// rmw_registry.c — cross-process topic registry (see rmw_registry.h).

#include "rmw_weft/rmw_registry.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define RMW_FUTEX_WAIT 0
#define RMW_FUTEX_WAKE 1

#define RMW_REGISTRY_PAGE0 4096u

// ---------------------------------------------------------------------------
// Cold-path topic-claim lock (CAS machine; see rmw_registry.h for the WHY)
// ---------------------------------------------------------------------------

static int rmw_pid_alive(uint32_t pid);

static void rmw_registry_claim_lock(rmw_registry_map_t *m) {
    _Atomic uint32_t *lk = &m->hdr->claim_lock;
    uint32_t pid = (uint32_t)getpid();
    for (;;) {
        uint32_t expected = 0;
        if (atomic_compare_exchange_strong_explicit(lk, &expected, pid,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            return;
        }
        if (expected == pid) return; /* same process: rcl serializes lifecycle
                                        calls per context (documented) */
        if (!rmw_pid_alive(expected)) {
            /* dead owner: steal (crash healing — same discipline as the
             * attach-slot sweep) */
            uint32_t dead = expected;
            if (atomic_compare_exchange_strong_explicit(lk, &dead, pid,
                                                        memory_order_acq_rel,
                                                        memory_order_acquire)) {
                atomic_fetch_add_explicit(&m->hdr->claim_steals, 1u,
                                          memory_order_relaxed);
                return;
            }
            continue;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000};
        (void)nanosleep(&ts, NULL);
    }
}

static void rmw_registry_claim_unlock(rmw_registry_map_t *m) {
    atomic_store_explicit(&m->hdr->claim_lock, 0, memory_order_release);
}

static inline void rmw_pause(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}

static inline int64_t rmw_now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

uint64_t rmw_weft_hash64(const char *s) {
    uint64_t h = 1469598103934665603ull;
    while (*s != '\0') {
        h ^= (uint64_t)(unsigned char)*s++;
        h *= 1099511628211ull;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------

static size_t rmw_registry_mapping_bytes(void) {
    return (size_t)RMW_REGISTRY_PAGE0 +
           (size_t)RMW_WEFT_MAX_TOPICS * sizeof(rmw_registry_topic_t);
}

static int rmw_pid_alive(uint32_t pid) {
    if (pid == 0) return 0;
    if (kill((pid_t)pid, 0) == 0) return 1;
    return (errno != ESRCH) ? 1 : 0;  /* EPERM => alive but not ours */
}

static int rmw_registry_claim_attach_slot(rmw_registry_header_t *h) {
    uint32_t pid = (uint32_t)getpid();
    for (int i = 0; i < 8; i++) {
        uint32_t expected = 0;
        if (atomic_compare_exchange_strong_explicit(
                &h->attach_pids[i], &expected, pid, memory_order_acq_rel,
                memory_order_relaxed)) {
            return i;
        }
    }
    return -1; /* table full: attach without a slot (count still honest-ish,
                  crash residue healed only when the table has room) */
}

static int rmw_registry_sweep_attachers(rmw_registry_header_t *h) {
    int freed = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t pid = atomic_load_explicit(&h->attach_pids[i],
                                            memory_order_acquire);
        if (pid == 0) continue;
        if (rmw_pid_alive(pid)) continue;
        uint32_t expected = pid;
        if (atomic_compare_exchange_strong_explicit(
                &h->attach_pids[i], &expected, 0, memory_order_acq_rel,
                memory_order_relaxed)) {
            atomic_fetch_sub_explicit(&h->attach_count, 1u,
                                      memory_order_acq_rel);
            freed++;
        }
    }
    return freed;
}

int rmw_registry_open(uint32_t domain_id, rmw_registry_map_t *out) {
    if (out == NULL) return -1;
    uint32_t d = rmw_weft_cfg_domain(domain_id);
    char name[80];
    snprintf(name, sizeof(name), "weft_rmw_d%u_registry", d);

    memset(out, 0, sizeof(*out));
    size_t mapping = rmw_registry_mapping_bytes();

    /* creator attempt */
    out->fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (out->fd >= 0) {
        if (ftruncate(out->fd, (off_t)mapping) != 0) {
            (void)close(out->fd);
            shm_unlink(name);
            return -1;
        }
        out->base = (uint8_t *)mmap(NULL, mapping, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, out->fd, 0);
        if (out->base == MAP_FAILED) {
            (void)close(out->fd);
            shm_unlink(name);
            return -1;
        }
        rmw_registry_header_t *h = (rmw_registry_header_t *)out->base;
        struct timespec ts;
        (void)clock_gettime(CLOCK_REALTIME, &ts);
        memset(out->base, 0, mapping);
        h->magic = RMW_WEFT_REGISTRY_MAGIC;
        h->version = RMW_WEFT_REGISTRY_VERSION;
        h->header_size = RMW_REGISTRY_PAGE0;
        h->max_topics = RMW_WEFT_MAX_TOPICS;
        h->max_subs = RMW_WEFT_MAX_SUBS_PER_TOPIC;
        h->max_pubs = RMW_WEFT_MAX_PUBS_PER_TOPIC;
        h->creator_pid = (uint32_t)getpid();
        h->created_unix_ns = (uint64_t)ts.tv_sec * 1000000000ull +
                             (uint64_t)ts.tv_nsec;
        atomic_init(&h->epoch, 1);
        atomic_init(&h->instance_seq, 1);
        atomic_init(&h->activity, 0);
        atomic_init(&h->attach_count, 1);
        atomic_init(&h->waiters, 0);
        atomic_init(&h->claim_lock, 0);
        atomic_init(&h->claim_steals, 0);
        out->hdr = h;
        out->topics = (rmw_registry_topic_t *)(out->base +
                                               RMW_REGISTRY_PAGE0);
        out->mapping_bytes = mapping;
        out->creator = 1;
        out->attach_slot = rmw_registry_claim_attach_slot(h);
        snprintf(out->name, sizeof(out->name), "%s", name);
        (void)rmw_registry_sweep_orphans(out);
        return 0;
    }

    /* attacher path — bounded retry while the creator fills the header.
     * BOTH the size check and the magic/version validation retry: the
     * creator's window between ftruncate() and the magic store passes the
     * size check while the header is still zero — a one-shot validation
     * there kills the attacher's whole rmw_init (observed as a dead bench
     * child and a parent spinning on borrow-retry; D-62 §C.2). The retry
     * is bounded (200 x 1 ms) and fails closed if the creator never
     * finishes. */
    if (errno != EEXIST) return -1;
    for (int attempt = 0; attempt < 200; attempt++) {
        out->fd = shm_open(name, O_RDWR, 0600);
        if (out->fd >= 0) {
            struct stat st;
            if (fstat(out->fd, &st) == 0 && st.st_size == (off_t)mapping) {
                out->base = (uint8_t *)mmap(NULL, mapping,
                                             PROT_READ | PROT_WRITE,
                                             MAP_SHARED, out->fd, 0);
                if (out->base == MAP_FAILED) {
                    (void)close(out->fd);
                    return -1;
                }
                rmw_registry_header_t *h =
                    (rmw_registry_header_t *)out->base;
                if (h->magic != RMW_WEFT_REGISTRY_MAGIC ||
                    h->version != RMW_WEFT_REGISTRY_VERSION ||
                    h->max_topics != RMW_WEFT_MAX_TOPICS ||
                    h->max_subs != RMW_WEFT_MAX_SUBS_PER_TOPIC ||
                    h->max_pubs != RMW_WEFT_MAX_PUBS_PER_TOPIC) {
                    /* creator still writing the header: unmap, wait, retry */
                    (void)munmap(out->base, mapping);
                    (void)close(out->fd);
                    out->fd = -1;
                    out->base = NULL;
                } else {
                    atomic_fetch_add_explicit(&h->attach_count, 1u,
                                              memory_order_acq_rel);
                    out->hdr = h;
                    out->topics = (rmw_registry_topic_t *)(out->base +
                                                           RMW_REGISTRY_PAGE0);
                    out->mapping_bytes = mapping;
                    out->creator = 0;
                    out->attach_slot = rmw_registry_claim_attach_slot(h);
                    snprintf(out->name, sizeof(out->name), "%s", name);
                    (void)rmw_registry_sweep_orphans(out);
                    return 0;
                }
            } else {
                /* size not ready yet (creator between shm_open and
                 * ftruncate) */
                (void)close(out->fd);
                out->fd = -1;
            }
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
        (void)nanosleep(&ts, NULL);
    }
    return -1;
}

void rmw_registry_close(rmw_registry_map_t *m) {
    if (m == NULL || m->base == NULL) return;
    /* heal crash residue first, then drop our own slot and count */
    (void)rmw_registry_sweep_attachers(m->hdr);
    if (m->attach_slot >= 0) {
        atomic_store_explicit(&m->hdr->attach_pids[m->attach_slot], 0,
                              memory_order_release);
    }
    uint32_t live = atomic_fetch_sub_explicit(&m->hdr->attach_count, 1u,
                                              memory_order_acq_rel);
    if (m->creator || live <= 1u) {
        shm_unlink(m->name);
    }
    (void)munmap(m->base, m->mapping_bytes);
    if (m->fd >= 0) (void)close(m->fd);
    memset(m, 0, sizeof(*m));
    m->fd = -1;
}

uint64_t rmw_registry_mint_instance(rmw_registry_map_t *m) {
    return atomic_fetch_add_explicit(&m->hdr->instance_seq, 1ull,
                                     memory_order_acq_rel);
}

uint64_t rmw_registry_epoch(const rmw_registry_map_t *m) {
    return atomic_load_explicit(&m->hdr->epoch, memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Topic / record claiming (CAS state machines — no mutexes)
// ---------------------------------------------------------------------------

static rmw_registry_topic_t *rmw_find_topic(rmw_registry_map_t *m,
                                            const char *topic,
                                            uint64_t type_hash) {
    (void)type_hash; /* name is the identity; hash is advisory metadata */
    uint64_t hash = rmw_weft_hash64(topic);
    for (uint32_t i = 0; i < RMW_WEFT_MAX_TOPICS; i++) {
        rmw_registry_topic_t *t = &m->topics[i];
        uint32_t st = atomic_load_explicit(&t->state, memory_order_acquire);
        if (st != RMW_WEFT_SLOT_ACTIVE && st != RMW_WEFT_SLOT_CLAIMED) {
            continue;
        }
        if (atomic_load_explicit(&t->name_hash, memory_order_relaxed) !=
            (uint32_t)hash) {
            continue;
        }
        if (strncmp(t->name, topic, RMW_WEFT_TOPIC_NAME_MAX) == 0) {
            return t;
        }
    }
    return NULL;
}

static rmw_registry_topic_t *rmw_claim_topic(rmw_registry_map_t *m,
                                             const char *topic) {
    if (strlen(topic) >= RMW_WEFT_TOPIC_NAME_MAX) return NULL;
    uint64_t hash = rmw_weft_hash64(topic);
    for (uint32_t i = 0; i < RMW_WEFT_MAX_TOPICS; i++) {
        rmw_registry_topic_t *t = &m->topics[i];
        uint32_t expected = RMW_WEFT_SLOT_FREE;
        if (!atomic_compare_exchange_strong_explicit(
                &t->state, &expected, RMW_WEFT_SLOT_CLAIMED,
                memory_order_acq_rel, memory_order_acquire)) {
            continue;
        }
        snprintf(t->name, RMW_WEFT_TOPIC_NAME_MAX, "%s", topic);
        atomic_store_explicit(&t->name_hash, (uint32_t)hash,
                              memory_order_relaxed);
        atomic_store_explicit(&t->subs_version, 1u, memory_order_relaxed);
        atomic_store_explicit(&t->pubs_version, 1u, memory_order_relaxed);
        atomic_store_explicit(&t->state, RMW_WEFT_SLOT_ACTIVE,
                              memory_order_release);
        atomic_fetch_add_explicit(&m->hdr->epoch, 1ull,
                                  memory_order_acq_rel);
        return t;
    }
    return NULL;
}

int rmw_registry_add_sub(rmw_registry_map_t *m, const char *topic,
                         uint64_t type_hash, uint32_t msg_size,
                         const char *ring_name, uint32_t ring_slots,
                         uint32_t ring_payload, uint32_t sub_instance,
                         uint32_t sub_pid,
                         rmw_registry_sub_record_t **out_record,
                         uint64_t *out_subs_version) {
    if (m == NULL || topic == NULL || ring_name == NULL || out_record == NULL)
        return -1;

    rmw_registry_claim_lock(m);

    rmw_registry_topic_t *t = rmw_find_topic(m, topic, type_hash);
    if (t == NULL) {
        t = rmw_claim_topic(m, topic);
        if (t == NULL) {
            rmw_registry_claim_unlock(m);
            return -1;
        }
        atomic_store_explicit(&t->type_hash, type_hash, memory_order_relaxed);
        atomic_store_explicit(&t->msg_size, msg_size, memory_order_relaxed);
    }

    for (uint32_t i = 0; i < RMW_WEFT_MAX_SUBS_PER_TOPIC; i++) {
        rmw_registry_sub_record_t *r = &t->subs[i];
        uint32_t expected = RMW_WEFT_SLOT_FREE;
        if (!atomic_compare_exchange_strong_explicit(
                &r->state, &expected, RMW_WEFT_SLOT_CLAIMED,
                memory_order_acq_rel, memory_order_acquire)) {
            continue;
        }
        r->sub_instance = sub_instance;
        atomic_store_explicit(&r->sub_pid, sub_pid, memory_order_relaxed);
        snprintf(r->ring_name, RMW_WEFT_RING_NAME_MAX, "%s", ring_name);
        r->ring_slots = ring_slots;
        r->ring_payload = ring_payload;
        atomic_store_explicit(&r->state, RMW_WEFT_SLOT_ACTIVE,
                              memory_order_release);
        *out_record = r;
        *out_subs_version = atomic_fetch_add_explicit(&t->subs_version, 1u,
                                                      memory_order_acq_rel) +
                            1u;
        atomic_fetch_add_explicit(&m->hdr->epoch, 1ull,
                                  memory_order_acq_rel);
        rmw_registry_claim_unlock(m);
        return 0;
    }
    rmw_registry_claim_unlock(m);
    return -1;  /* topic full: honest failure, no silent drop of the seam */
}

int rmw_registry_remove_sub(rmw_registry_map_t *m,
                            rmw_registry_sub_record_t *record) {
    if (m == NULL || record == NULL) return -1;
    rmw_registry_claim_lock(m);
    atomic_store_explicit(&record->state, RMW_WEFT_SLOT_CLOSED,
                          memory_order_release);
    /* the topic entry's subs_version bump happens via the record's owning
     * topic; find it by containment (fixed array => parent is derivable
     * only by scan — cold path, fine) */
    for (uint32_t i = 0; i < RMW_WEFT_MAX_TOPICS; i++) {
        rmw_registry_topic_t *t = &m->topics[i];
        if (record >= &t->subs[0] && record < &t->subs[RMW_WEFT_MAX_SUBS_PER_TOPIC]) {
            atomic_fetch_add_explicit(&t->subs_version, 1u,
                                      memory_order_acq_rel);
            /* close the topic when nothing remains alive in it */
            bool any = false;
            for (uint32_t k = 0; k < RMW_WEFT_MAX_SUBS_PER_TOPIC; k++) {
                uint32_t st = atomic_load_explicit(&t->subs[k].state,
                                                   memory_order_acquire);
                if (st == RMW_WEFT_SLOT_ACTIVE || st == RMW_WEFT_SLOT_CLAIMED)
                    any = true;
            }
            for (uint32_t k = 0; k < RMW_WEFT_MAX_PUBS_PER_TOPIC; k++) {
                uint32_t st = atomic_load_explicit(&t->pubs[k].state,
                                                   memory_order_acquire);
                if (st == RMW_WEFT_SLOT_ACTIVE || st == RMW_WEFT_SLOT_CLAIMED)
                    any = true;
            }
            if (!any) {
                atomic_store_explicit(&t->state, RMW_WEFT_SLOT_FREE,
                                      memory_order_release);
                memset(t->name, 0, sizeof(t->name));
            }
            break;
        }
    }
    atomic_fetch_add_explicit(&m->hdr->epoch, 1ull, memory_order_acq_rel);
    rmw_registry_claim_unlock(m);
    return 0;
}

int rmw_registry_add_pub(rmw_registry_map_t *m, const char *topic,
                         uint64_t type_hash, uint32_t msg_size,
                         uint32_t pub_instance, uint32_t pub_pid,
                         uint64_t gid_a, uint64_t gid_b,
                         rmw_registry_topic_t **out_topic,
                         rmw_registry_pub_record_t **out_record) {
    if (m == NULL || topic == NULL || out_topic == NULL || out_record == NULL)
        return -1;

    rmw_registry_claim_lock(m);

    rmw_registry_topic_t *t = rmw_find_topic(m, topic, type_hash);
    if (t == NULL) {
        t = rmw_claim_topic(m, topic);
        if (t == NULL) {
            rmw_registry_claim_unlock(m);
            return -1;
        }
        atomic_store_explicit(&t->type_hash, type_hash, memory_order_relaxed);
        atomic_store_explicit(&t->msg_size, msg_size, memory_order_relaxed);
    }

    for (uint32_t i = 0; i < RMW_WEFT_MAX_PUBS_PER_TOPIC; i++) {
        rmw_registry_pub_record_t *r = &t->pubs[i];
        uint32_t expected = RMW_WEFT_SLOT_FREE;
        if (!atomic_compare_exchange_strong_explicit(
                &r->state, &expected, RMW_WEFT_SLOT_CLAIMED,
                memory_order_acq_rel, memory_order_acquire)) {
            continue;
        }
        r->pub_instance = pub_instance;
        atomic_store_explicit(&r->pub_pid, pub_pid, memory_order_relaxed);
        r->gid_a = gid_a;
        r->gid_b = gid_b;
        atomic_store_explicit(&r->state, RMW_WEFT_SLOT_ACTIVE,
                              memory_order_release);
        *out_topic = t;
        *out_record = r;
        atomic_fetch_add_explicit(&t->pubs_version, 1u,
                                  memory_order_acq_rel);
        atomic_fetch_add_explicit(&m->hdr->epoch, 1ull,
                                  memory_order_acq_rel);
        rmw_registry_claim_unlock(m);
        return 0;
    }
    rmw_registry_claim_unlock(m);
    return -1;
}

int rmw_registry_remove_pub(rmw_registry_map_t *m,
                            rmw_registry_pub_record_t *record) {
    if (m == NULL || record == NULL) return -1;
    rmw_registry_claim_lock(m);
    atomic_store_explicit(&record->state, RMW_WEFT_SLOT_CLOSED,
                          memory_order_release);
    for (uint32_t i = 0; i < RMW_WEFT_MAX_TOPICS; i++) {
        rmw_registry_topic_t *t = &m->topics[i];
        if (record >= &t->pubs[0] &&
            record < &t->pubs[RMW_WEFT_MAX_PUBS_PER_TOPIC]) {
            atomic_fetch_add_explicit(&t->pubs_version, 1u,
                                      memory_order_acq_rel);
            bool any = false;
            for (uint32_t k = 0; k < RMW_WEFT_MAX_SUBS_PER_TOPIC; k++) {
                uint32_t st = atomic_load_explicit(&t->subs[k].state,
                                                   memory_order_acquire);
                if (st == RMW_WEFT_SLOT_ACTIVE || st == RMW_WEFT_SLOT_CLAIMED)
                    any = true;
            }
            for (uint32_t k = 0; k < RMW_WEFT_MAX_PUBS_PER_TOPIC; k++) {
                uint32_t st = atomic_load_explicit(&t->pubs[k].state,
                                                   memory_order_acquire);
                if (st == RMW_WEFT_SLOT_ACTIVE || st == RMW_WEFT_SLOT_CLAIMED)
                    any = true;
            }
            if (!any) {
                atomic_store_explicit(&t->state, RMW_WEFT_SLOT_FREE,
                                      memory_order_release);
                memset(t->name, 0, sizeof(t->name));
            }
            break;
        }
    }
    atomic_fetch_add_explicit(&m->hdr->epoch, 1ull, memory_order_acq_rel);
    rmw_registry_claim_unlock(m);
    return 0;
}

int rmw_registry_list_subs(rmw_registry_map_t *m,
                           rmw_registry_topic_t *topic, uint64_t known_version,
                           const char *names[], uint32_t slots[],
                           uint32_t payloads[], int max) {
    if (m == NULL || topic == NULL || names == NULL || max <= 0) return -1;
    uint64_t v = atomic_load_explicit(&topic->subs_version,
                                      memory_order_acquire);
    if (v == known_version) return -2;  /* no scan needed */
    int n = 0;
    for (uint32_t i = 0; i < RMW_WEFT_MAX_SUBS_PER_TOPIC && n < max; i++) {
        uint32_t st = atomic_load_explicit(&topic->subs[i].state,
                                           memory_order_acquire);
        if (st != RMW_WEFT_SLOT_ACTIVE) continue;
        names[n] = topic->subs[i].ring_name;
        if (slots != NULL) slots[n] = topic->subs[i].ring_slots;
        if (payloads != NULL) payloads[n] = topic->subs[i].ring_payload;
        n++;
    }
    return n;
}

uint64_t rmw_registry_subs_version(const rmw_registry_topic_t *topic) {
    return atomic_load_explicit(&topic->subs_version, memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Activity doorbell (wait-set parking)
// ---------------------------------------------------------------------------

void rmw_registry_activity_bump(rmw_registry_map_t *m) {
    if (m == NULL) return;
    atomic_fetch_add_explicit(&m->hdr->activity, 1u, memory_order_release);
    if (atomic_load_explicit(&m->hdr->waiters, memory_order_relaxed) != 0) {
        (void)syscall(SYS_futex, (uint32_t *)&m->hdr->activity,
                      RMW_FUTEX_WAKE, 1, NULL, NULL, 0);
    }
}

int rmw_registry_activity_wait(rmw_registry_map_t *m, int64_t deadline_ns) {
    if (m == NULL) return 0;
    uint32_t before = atomic_load_explicit(&m->hdr->activity,
                                           memory_order_acquire);
    if (deadline_ns < 0) {
        deadline_ns = rmw_now_ns() + 100000000;  /* 100 ms chunk */
    }
    const uint64_t spins = 64u;
    for (uint64_t i = 0; i < spins; i++) {
        if (atomic_load_explicit(&m->hdr->activity, memory_order_acquire) !=
            before) {
            return 1;
        }
        rmw_pause();
    }
    for (;;) {
        if (atomic_load_explicit(&m->hdr->activity, memory_order_acquire) !=
            before) {
            return 1;
        }
        if (rmw_now_ns() >= deadline_ns) return 0;
        atomic_fetch_add_explicit(&m->hdr->waiters, 1u,
                                  memory_order_acq_rel);
        if (atomic_load_explicit(&m->hdr->activity, memory_order_acquire) !=
            before) {
            atomic_fetch_sub_explicit(&m->hdr->waiters, 1u,
                                      memory_order_acq_rel);
            return 1;
        }
        uint32_t cur = atomic_load_explicit(&m->hdr->activity,
                                            memory_order_acquire);
        struct timespec ts;
        int64_t rem = deadline_ns - rmw_now_ns();
        if (rem <= 0) rem = 200000;
        if (rem > 1000000) rem = 1000000;
        ts.tv_sec = rem / 1000000000;
        ts.tv_nsec = (long)(rem % 1000000000);
        (void)syscall(SYS_futex, (uint32_t *)&m->hdr->activity,
                      RMW_FUTEX_WAIT, (uint32_t)cur, &ts, NULL, 0);
        atomic_fetch_sub_explicit(&m->hdr->waiters, 1u,
                                  memory_order_acq_rel);
    }
}

// ---------------------------------------------------------------------------
// Orphan sweep (crash healing)
// ---------------------------------------------------------------------------

int rmw_registry_sweep_orphans(rmw_registry_map_t *m) {
    if (m == NULL) return 0;
    rmw_registry_claim_lock(m);
    int reclaimed = 0;
    for (uint32_t i = 0; i < RMW_WEFT_MAX_TOPICS; i++) {
        rmw_registry_topic_t *t = &m->topics[i];
        uint32_t tst = atomic_load_explicit(&t->state, memory_order_acquire);
        if (tst == RMW_WEFT_SLOT_FREE) continue;

        for (uint32_t k = 0; k < RMW_WEFT_MAX_SUBS_PER_TOPIC; k++) {
            rmw_registry_sub_record_t *r = &t->subs[k];
            uint32_t st = atomic_load_explicit(&r->state,
                                               memory_order_acquire);
            if (st != RMW_WEFT_SLOT_ACTIVE && st != RMW_WEFT_SLOT_CLAIMED)
                continue;
            uint32_t pid = atomic_load_explicit(&r->sub_pid,
                                                memory_order_acquire);
            if (rmw_pid_alive(pid)) continue;
            /* dead owner: unlink the ring object and close the record */
            if (r->ring_name[0] != '\0') {
                shm_unlink(r->ring_name);
            }
            atomic_store_explicit(&r->state, RMW_WEFT_SLOT_CLOSED,
                                  memory_order_release);
            atomic_fetch_add_explicit(&t->subs_version, 1u,
                                      memory_order_acq_rel);
            reclaimed++;
        }
        for (uint32_t k = 0; k < RMW_WEFT_MAX_PUBS_PER_TOPIC; k++) {
            rmw_registry_pub_record_t *r = &t->pubs[k];
            uint32_t st = atomic_load_explicit(&r->state,
                                               memory_order_acquire);
            if (st != RMW_WEFT_SLOT_ACTIVE && st != RMW_WEFT_SLOT_CLAIMED)
                continue;
            uint32_t pid = atomic_load_explicit(&r->pub_pid,
                                                memory_order_acquire);
            if (rmw_pid_alive(pid)) continue;
            atomic_store_explicit(&r->state, RMW_WEFT_SLOT_CLOSED,
                                  memory_order_release);
            atomic_fetch_add_explicit(&t->pubs_version, 1u,
                                      memory_order_acq_rel);
            reclaimed++;
        }

        /* drop the topic entry if nothing is alive in it */
        bool any = false;
        for (uint32_t k = 0; k < RMW_WEFT_MAX_SUBS_PER_TOPIC; k++) {
            uint32_t st = atomic_load_explicit(&t->subs[k].state,
                                               memory_order_acquire);
            if (st == RMW_WEFT_SLOT_ACTIVE || st == RMW_WEFT_SLOT_CLAIMED)
                any = true;
        }
        for (uint32_t k = 0; k < RMW_WEFT_MAX_PUBS_PER_TOPIC; k++) {
            uint32_t st = atomic_load_explicit(&t->pubs[k].state,
                                               memory_order_acquire);
            if (st == RMW_WEFT_SLOT_ACTIVE || st == RMW_WEFT_SLOT_CLAIMED)
                any = true;
        }
        if (!any) {
            atomic_store_explicit(&t->state, RMW_WEFT_SLOT_FREE,
                                  memory_order_release);
            memset(t->name, 0, sizeof(t->name));
        }
    }
    rmw_registry_claim_unlock(m);
    return reclaimed;
}
