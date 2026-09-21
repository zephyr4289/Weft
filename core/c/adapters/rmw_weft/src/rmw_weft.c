// rmw_weft.c — the ROS 2 RMW C-API implementation over Weft SHM loan-rings.
//
// WHY EXISTS: this is the drop-in surface a ROS 2 graph links against
// (rmw_init .. rmw_wait, publishers/subscriptions/wait-sets, plus the
// official loaned-message zero-copy ABI). Every entry point here is thin:
// geometry and lifecycle on cold paths, pure shared-memory atomics on the
// hot paths (publish/take/loan/wait make no syscalls in the steady state
// and touch no heap — Law 1; loans alias ring slots directly — Law 2;
// full rings, torn writers, and dead peers return honest codes within the
// caller's deadline — Law 3).
//
// Lifecycle contract (documented, rcl-compatible):
//   rmw_init_options_init -> rmw_init -> rmw_create_node ->
//     rmw_create_publisher / rmw_create_subscription ->
//     rmw_publish / rmw_borrow_loaned_message + rmw_publish_loaned_message /
//     rmw_take / rmw_take_loaned_message + rmw_return_loaned_message /
//     rmw_wait -> rmw_destroy_* -> rmw_shutdown -> rmw_fini.
//
// Threading contract (SPSC per entity, same as every fast RMW): one
// publisher thread per publisher, one consumer thread per subscription.
// The wait-set thread is the subscription's consumer thread (or parks on
// the registry activity doorbell).

#include "rmw_weft/rmw_weft.h"
#include "rmw_weft/rmw_registry.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <stdatomic.h>

// ---------------------------------------------------------------------------
// Impl data blocks (the opaque structs the vendored ABI forward-declares)
// ---------------------------------------------------------------------------

struct rmw_context_data_t {
    rmw_registry_map_t registry;
    uint32_t domain_id;
    atomic_int shutdown;
};

struct rmw_node_data_t {
    rmw_context_t *context;
    char *name;    /* owned */
    char *ns;      /* owned */
    uint32_t pid;
    uint32_t domain_id;
};

struct rmw_publisher_data_t {
    rmw_node_t *node;
    struct rmw_context_data_t *ctx;
    rmw_registry_map_t *registry;
    rmw_registry_topic_t *topic;
    rmw_registry_pub_record_t *record;
    char *topic_name; /* owned */
    uint64_t type_hash;
    uint32_t msg_size;
    int reliable;
    uint32_t pub_instance;
    uint64_t gid_a, gid_b;
    /* fan-out rings: one per ACTIVE subscriber (cold attach/detach) */
    rmw_ring_map_t rings[RMW_WEFT_MAX_SUBS_PER_TOPIC];
    uint32_t ring_count;
    uint64_t known_subs_version;
    /* single outstanding publisher loan (documented contract) */
    rmw_ring_map_t *loan_ring;
    rmw_ring_slot_t *loan_slot;
    uint8_t *loan_payload;
    /* honest counters */
    uint64_t published;
    uint64_t failed;
};

struct rmw_subscription_data_t {
    rmw_node_t *node;
    struct rmw_context_data_t *ctx;
    rmw_registry_map_t *registry;
    rmw_registry_topic_t *topic;
    rmw_registry_sub_record_t *record;
    char *topic_name; /* owned */
    uint64_t type_hash;
    uint32_t msg_size;
    int reliable;
    uint32_t sub_instance;
    rmw_ring_map_t ring;      /* created here (tail owner) */
    uint64_t cursor;          /* absolute next-to-read index */
    uint64_t expected_seq;    /* next expected seq_id */
    /* single outstanding subscriber loan (documented contract) */
    rmw_ring_slot_t *loan_slot;
    void *loan_ptr;
    uint32_t loan_size;
    /* honest counters */
    uint64_t taken;
    uint64_t missed;
};

struct rmw_wait_set_data_t {
    rmw_context_t *context;
    const rmw_subscription_t *subs[64];
    size_t count;
    size_t max_conditions;
};

// ---------------------------------------------------------------------------
// Error state + identity
// ---------------------------------------------------------------------------

static _Thread_local char rmw_err_buf[1024];

void rmw_weft_set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(rmw_err_buf, sizeof(rmw_err_buf), fmt, ap);
    va_end(ap);
}

rmw_error_string_t rmw_get_error_string(void) {
    rmw_error_string_t s;
    snprintf(s.str, sizeof(s.str), "%s", rmw_err_buf);
    return s;
}

const char *rmw_get_implementation_identifier(void) {
    return RMW_WEFT_IMPLEMENTATION_ID;
}

const char *rmw_weft_identifier(void) {
    return RMW_WEFT_IMPLEMENTATION_ID;
}

const rmw_qos_profile_t RMW_QOS_PROFILE_DEFAULT = {
    RMW_QOS_POLICY_HISTORY_KEEP_LAST, 10,
    RMW_QOS_POLICY_RELIABILITY_RELIABLE, RMW_QOS_POLICY_DURABILITY_VOLATILE,
    RMW_QOS_DEADLINE_DEFAULT, RMW_QOS_LIFESPAN_DEFAULT,
    RMW_QOS_POLICY_LIVELINESS_AUTOMATIC,
    RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT,
};

const rmw_qos_profile_t RMW_QOS_PROFILE_SENSOR_DATA = {
    RMW_QOS_POLICY_HISTORY_KEEP_LAST, 5,
    RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT, RMW_QOS_POLICY_DURABILITY_VOLATILE,
    RMW_QOS_DEADLINE_DEFAULT, RMW_QOS_LIFESPAN_DEFAULT,
    RMW_QOS_POLICY_LIVELINESS_AUTOMATIC,
    RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT,
};

// ---------------------------------------------------------------------------
// POD typesupport
// ---------------------------------------------------------------------------

static const rosidl_message_type_support_t *rmw_pod_ts_create(
    size_t message_size, const char *name);

struct pod_ts_data {
    rmw_weft_pod_ts_t pod;
};

static const rosidl_message_type_support_t *
    rmw_pod_ts_get(const void *data, const char *identifier);

const rosidl_message_type_support_t * rmw_weft_create_pod_type_support(
    size_t message_size, const char * name) {
    return rmw_pod_ts_create(message_size, name);
}

static const rosidl_message_type_support_t *rmw_pod_ts_create(
    size_t message_size, const char *name) {
    if (message_size == 0 || name == NULL || strlen(name) >= 64) return NULL;
    rosidl_message_type_support_t *ts =
        calloc(1, sizeof(rosidl_message_type_support_t));
    struct pod_ts_data *d = calloc(1, sizeof(*d));
    if (ts == NULL || d == NULL) {
        free(ts);
        free(d);
        return NULL;
    }
    d->pod.message_size = message_size;
    snprintf(d->pod.name, sizeof(d->pod.name), "%s", name);
    d->pod.type_hash = rmw_weft_hash64(name);
    ts->typesupport_identifier = RMW_WEFT_TYPESUPPORT_ID;
    ts->data = d;
    ts->function = rmw_pod_ts_get;
    return ts;
}

static const rosidl_message_type_support_t *
    rmw_pod_ts_get(const void *data, const char *identifier) {
    (void)data;
    (void)identifier;
    return NULL; /* single-level typesupport: no nested identifiers */
}

void rmw_weft_destroy_pod_type_support(
    const rosidl_message_type_support_t * ts) {
    if (ts == NULL) return;
    if (ts->typesupport_identifier == NULL ||
        strcmp(ts->typesupport_identifier, RMW_WEFT_TYPESUPPORT_ID) != 0) {
        return;
    }
    free((void *)ts->data);
    free((void *)ts);
}

static const rmw_weft_pod_ts_t *rmw_ts_as_pod(
    const rosidl_message_type_support_t *ts) {
    if (ts == NULL || ts->typesupport_identifier == NULL ||
        strcmp(ts->typesupport_identifier, RMW_WEFT_TYPESUPPORT_ID) != 0) {
        return NULL;
    }
    return (const rmw_weft_pod_ts_t *)ts->data;
}

// ---------------------------------------------------------------------------
// Init / fini / shutdown
// ---------------------------------------------------------------------------

rmw_ret_t rmw_init_options_init(rmw_init_options_t *init_options, int argc,
                                const char *const *argv) {
    (void)argc;
    (void)argv;
    if (init_options == NULL) {
        rmw_weft_set_error("rmw_init_options_init: init_options is NULL");
        return RMW_RET_INVALID_ARGUMENT;
    }
    memset(init_options, 0, sizeof(*init_options));
    init_options->fini = rmw_init_options_fini;
    init_options->implementation_identifier = RMW_WEFT_IMPLEMENTATION_ID;
    init_options->instance_id = 0;
    init_options->enclave = NULL;
    init_options->security_options.enforce_security =
        RMW_SECURITY_ENFORCEMENT_NOT_SET;
    init_options->security_options.security_root_path = NULL;
    return RMW_RET_OK;
}

rmw_ret_t rmw_init_options_fini(rmw_init_options_t *init_options) {
    if (init_options == NULL) {
        rmw_weft_set_error("rmw_init_options_fini: init_options is NULL");
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (init_options->implementation_identifier == NULL ||
        strcmp(init_options->implementation_identifier,
               RMW_WEFT_IMPLEMENTATION_ID) != 0) {
        rmw_weft_set_error(
            "rmw_init_options_fini: not an rmw_weft options object");
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    /* enclave/security_root_path are CALLER-owned per the rmw spec */
    memset(init_options, 0, sizeof(*init_options));
    return RMW_RET_OK;
}

rmw_ret_t rmw_init(const rmw_init_options_t *options, rmw_context_t *context) {
    if (options == NULL || context == NULL) {
        rmw_weft_set_error("rmw_init: NULL argument");
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (options->implementation_identifier == NULL ||
        strcmp(options->implementation_identifier,
               RMW_WEFT_IMPLEMENTATION_ID) != 0) {
        rmw_weft_set_error("rmw_init: options built for a different rmw");
        return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
    }
    struct rmw_context_data_t *d = calloc(1, sizeof(*d));
    if (d == NULL) return RMW_RET_BAD_ALLOC;
    atomic_init(&d->shutdown, 0);
    d->domain_id = 0;

    if (rmw_registry_open(d->domain_id, &d->registry) != 0) {
        rmw_weft_set_error("rmw_init: registry open failed (domain %u)",
                           d->domain_id);
        free(d);
        return RMW_RET_ERROR;
    }
    if (options->instance_id == 0) {
        context->instance_id = rmw_registry_mint_instance(&d->registry);
    } else {
        context->instance_id = options->instance_id;
    }
    context->options = NULL; /* caller retains ownership per rmw semantics */
    context->implementation_identifier = RMW_WEFT_IMPLEMENTATION_ID;
    context->data = d;
    return RMW_RET_OK;
}

rmw_ret_t rmw_shutdown(rmw_context_t *context) {
    if (context == NULL || context->data == NULL) {
        rmw_weft_set_error("rmw_shutdown: context is NULL or uninitialized");
        return RMW_RET_INVALID_ARGUMENT;
    }
    struct rmw_context_data_t *d = context->data;
    atomic_store_explicit(&d->shutdown, 1, memory_order_release);
    return RMW_RET_OK;
}

rmw_ret_t rmw_fini(rmw_context_t *context) {
    if (context == NULL || context->data == NULL) {
        rmw_weft_set_error("rmw_fini: context is NULL or uninitialized");
        return RMW_RET_INVALID_ARGUMENT;
    }
    struct rmw_context_data_t *d = context->data;
    rmw_registry_close(&d->registry);
    free(d);
    context->data = NULL;
    context->options = NULL;
    context->implementation_identifier = NULL;
    context->instance_id = 0;
    return RMW_RET_OK;
}

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

rmw_node_t *rmw_create_node(rmw_context_t *context, const char *name,
                            const char *namespace_, size_t domain_id,
                            const rmw_node_security_options_t *security_options) {
    if (context == NULL || context->data == NULL || name == NULL) {
        rmw_weft_set_error("rmw_create_node: NULL context/name");
        return NULL;
    }
    if (security_options != NULL &&
        security_options->enforce_security ==
            RMW_SECURITY_ENFORCEMENT_ENFORCE) {
        rmw_weft_set_error(
            "rmw_create_node: ENFORCE security is not implemented; "
            "PERMISSIVE/NOT_SET only (honest refusal, D-62 §C.4)");
        return NULL;
    }
    (void)namespace_;
    struct rmw_context_data_t *ctx = context->data;
    if (atomic_load_explicit(&ctx->shutdown, memory_order_acquire) != 0) {
        rmw_weft_set_error("rmw_create_node: context is shut down");
        return NULL;
    }
    if (domain_id != (size_t)ctx->domain_id) {
        rmw_weft_set_error(
            "rmw_create_node: one domain per context is supported "
            "(context domain %u, requested %zu) — declared limit",
            ctx->domain_id, domain_id);
        return NULL;
    }
    struct rmw_node_data_t *d = calloc(1, sizeof(*d));
    if (d == NULL) return NULL;
    d->name = strdup(name);
    d->ns = strdup(namespace_ == NULL ? "/" : namespace_);
    if (d->name == NULL || d->ns == NULL) {
        free(d->name);
        free(d->ns);
        free(d);
        return NULL;
    }
    d->context = context;
    d->pid = (uint32_t)getpid();
    d->domain_id = ctx->domain_id;

    rmw_node_t *n = calloc(1, sizeof(*n));
    if (n == NULL) {
        free(d->name);
        free(d->ns);
        free(d);
        return NULL;
    }
    n->context = context;
    n->implementation_identifier = RMW_WEFT_IMPLEMENTATION_ID;
    n->data = d;
    n->name = d->name;
    n->namespace_ = d->ns;
    return n;
}

rmw_ret_t rmw_destroy_node(rmw_node_t *node) {
    if (node == NULL || node->data == NULL) {
        rmw_weft_set_error("rmw_destroy_node: NULL node");
        return RMW_RET_INVALID_ARGUMENT;
    }
    struct rmw_node_data_t *d = node->data;
    free(d->name);
    free(d->ns);
    free(d);
    free(node);
    return RMW_RET_OK;
}

// ---------------------------------------------------------------------------
// Publisher fan-out management (cold paths only)
// ---------------------------------------------------------------------------

static int pub_attach_missing_rings(struct rmw_publisher_data_t *p) {
    const char *names[RMW_WEFT_MAX_SUBS_PER_TOPIC];
    uint32_t slots[RMW_WEFT_MAX_SUBS_PER_TOPIC];
    uint32_t payloads[RMW_WEFT_MAX_SUBS_PER_TOPIC];

    /* Lost-update guard (seqcount reader discipline, read version BEFORE
     * the scan): known_subs_version may only ever be set to a version whose
     * FULL record state this scan observed. Re-reading the version after
     * the scan would let a subscription that turned ACTIVE inside the scan
     * window (state stored before the version bump) masquerade as already
     * seen — every later scan would take the -2 "unchanged" fast path and
     * the subscriber ring would NEVER attach (first observed as the
     * bench's instant pp-borrow failure; fixed D-62 §C.2). */
    uint64_t v_before = rmw_registry_subs_version(p->topic);
    if (v_before == p->known_subs_version) return 0; /* steady state */

    int n = rmw_registry_list_subs(p->registry, p->topic,
                                   p->known_subs_version, names, slots,
                                   payloads, RMW_WEFT_MAX_SUBS_PER_TOPIC);
    if (n == -2) return 0; /* unchanged since the last scan */
    if (n < 0) return -1;

    /* detach rings whose subscription left */
    for (uint32_t i = 0; i < p->ring_count;) {
        bool alive = false;
        for (int k = 0; k < n; k++) {
            if (strcmp(p->rings[i].name, names[k]) == 0) alive = true;
        }
        if (!alive) {
            rmw_ring_destroy(&p->rings[i]);
            p->rings[i] = p->rings[p->ring_count - 1];
            p->ring_count--;
        } else {
            i++;
        }
    }
    /* attach new rings */
    for (int k = 0; k < n; k++) {
        bool have = false;
        for (uint32_t i = 0; i < p->ring_count; i++) {
            if (strcmp(p->rings[i].name, names[k]) == 0) have = true;
        }
        if (have || p->ring_count >= RMW_WEFT_MAX_SUBS_PER_TOPIC) continue;
        rmw_ring_map_t m;
        if (rmw_ring_attach(names[k], slots[k], payloads[k], &m) != 0) {
            continue; /* subscriber died between scan and attach; sweep heals */
        }
        rmw_ring_stamp_publisher(&m, p->gid_a, p->gid_b);
        p->rings[p->ring_count++] = m;
    }
    /* claim ONLY the version we actually scanned: a concurrent mutation
     * bumps the version past v_before and the next publish rescans */
    p->known_subs_version = v_before;
    return 0;
}

int rmw_weft_pub_refresh(rmw_publisher_t *pub) {
    if (pub == NULL || pub->data == NULL) return -1;
    return pub_attach_missing_rings((struct rmw_publisher_data_t *)pub->data);
}

// ---------------------------------------------------------------------------
// Publishers
// ---------------------------------------------------------------------------

rmw_publisher_t *rmw_create_publisher(
    const rmw_node_t *node, const rosidl_message_type_support_t *type_supports,
    const char *topic_name, const rmw_qos_profile_t *qos_policies,
    const rmw_publisher_options_t *publisher_options) {
    if (node == NULL || node->data == NULL || type_supports == NULL ||
        topic_name == NULL) {
        rmw_weft_set_error("rmw_create_publisher: NULL argument");
        return NULL;
    }
    if (publisher_options != NULL &&
        publisher_options->require_unique_network_flow_endpoints ==
            RMW_UNIQUE_NETWORK_FLOW_ENDPOINTS_REQUIRED) {
        rmw_weft_set_error(
            "rmw_create_publisher: unique network flow endpoints are a "
            "networking concern; intra-host SHM transport ignores them "
            "(honest refusal)");
        return NULL;
    }
    const rmw_weft_pod_ts_t *pod = rmw_ts_as_pod(type_supports);
    if (pod == NULL) {
        rmw_weft_set_error(
            "rmw_create_publisher: typesupport '%s' is not the rmw_weft "
            "POD/.weft descriptor ('%s') — no generated-code path exists",
            type_supports->typesupport_identifier == NULL
                ? "(null)"
                : type_supports->typesupport_identifier,
            RMW_WEFT_TYPESUPPORT_ID);
        return NULL;
    }
    if (qos_policies == NULL) qos_policies = &RMW_QOS_PROFILE_DEFAULT;

    struct rmw_node_data_t *nd = node->data;
    struct rmw_context_data_t *ctx = nd->context->data;
    struct rmw_publisher_data_t *d = calloc(1, sizeof(*d));
    if (d == NULL) return NULL;
    d->node = (rmw_node_t *)node;
    d->ctx = ctx;
    d->registry = &ctx->registry;
    d->topic_name = strdup(topic_name);
    if (d->topic_name == NULL) {
        free(d);
        return NULL;
    }
    d->type_hash = pod->type_hash;
    d->msg_size = (uint32_t)pod->message_size;
    d->reliable = (qos_policies->reliability ==
                   RMW_QOS_POLICY_RELIABILITY_RELIABLE)
                      ? 1
                      : 0;
    d->pub_instance = (uint32_t)rmw_registry_mint_instance(&ctx->registry);
    uint64_t mix = rmw_weft_hash64(topic_name);
    d->gid_a = ((uint64_t)nd->pid << 32) ^ d->pub_instance ^ mix;
    d->gid_b = rmw_weft_hash64(RMW_WEFT_IMPLEMENTATION_ID) ^ mix;

    if (rmw_registry_add_pub(&ctx->registry, topic_name, d->type_hash,
                             d->msg_size, d->pub_instance, nd->pid, d->gid_a,
                             d->gid_b, &d->topic, &d->record) != 0) {
        rmw_weft_set_error(
            "rmw_create_publisher: registry full for topic '%s' "
            "(max %u pubs, %u topics)",
            topic_name, RMW_WEFT_MAX_PUBS_PER_TOPIC, RMW_WEFT_MAX_TOPICS);
        free(d->topic_name);
        free(d);
        return NULL;
    }
    d->known_subs_version = 0; /* force first scan */
    (void)pub_attach_missing_rings(d);

    rmw_publisher_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        (void)rmw_registry_remove_pub(&ctx->registry, d->record);
        free(d->topic_name);
        free(d);
        return NULL;
    }
    p->implementation_identifier = RMW_WEFT_IMPLEMENTATION_ID;
    p->data = d;
    p->topic_name = d->topic_name;
    p->type_support_ = type_supports;
    p->options.require_unique_network_flow_endpoints =
        RMW_UNIQUE_NETWORK_FLOW_ENDPOINTS_NOT_REQUIRED;
    p->can_loan_messages = true;
    return p;
}

rmw_ret_t rmw_destroy_publisher(rmw_node_t *node, rmw_publisher_t *publisher) {
    (void)node;
    if (publisher == NULL || publisher->data == NULL) {
        rmw_weft_set_error("rmw_destroy_publisher: NULL publisher");
        return RMW_RET_INVALID_ARGUMENT;
    }
    struct rmw_publisher_data_t *d = publisher->data;
    (void)rmw_registry_remove_pub(d->registry, d->record);
    for (uint32_t i = 0; i < d->ring_count; i++) {
        rmw_ring_destroy(&d->rings[i]);
    }
    free(d->topic_name);
    free(d);
    free(publisher);
    return RMW_RET_OK;
}

// ---------------------------------------------------------------------------
// Publish paths (hot: zero heap, zero syscalls steady-state)
// ---------------------------------------------------------------------------

rmw_ret_t rmw_publish(const rmw_publisher_t *publisher, const void *ros_message,
                      rmw_publisher_allocation_t *allocation) {
    (void)allocation;
    if (publisher == NULL || publisher->data == NULL || ros_message == NULL) {
        rmw_weft_set_error("rmw_publish: NULL argument");
        return RMW_RET_INVALID_ARGUMENT;
    }
    struct rmw_publisher_data_t *d = publisher->data;
    if (atomic_load_explicit(&d->ctx->shutdown, memory_order_acquire) != 0) {
        rmw_weft_set_error("rmw_publish: context is shut down");
        return RMW_RET_ERROR;
    }
    (void)pub_attach_missing_rings(d); /* one atomic when unchanged */

    int rc = RMW_RET_OK;
    for (uint32_t i = 0; i < d->ring_count; i++) {
        int r = rmw_ring_publish(&d->rings[i], ros_message, d->msg_size,
                                 d->reliable, -1);
        if (r == 0) {
            rmw_registry_activity_bump(d->registry);
        } else if (r == -2) {
            /* dead subscriber ring: the next refresh/sweep detaches it */
        } else if (r == -3) {
            rc = RMW_RET_TIMEOUT;
            d->failed++;
        } else if (r == -4) {
            d->failed++;
        } else {
            rc = RMW_RET_ERROR;
            d->failed++;
        }
    }
    if (rc == RMW_RET_OK) d->published++;
    return rc;
}

rmw_ret_t rmw_borrow_loaned_message(
    const rmw_publisher_t *publisher,
    const rosidl_message_type_support_t *type_support,
    void **loaned_message) {
    if (publisher == NULL || publisher->data == NULL ||
        loaned_message == NULL) {
        rmw_weft_set_error("rmw_borrow_loaned_message: NULL argument");
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (type_support != publisher->type_support_) {
        rmw_weft_set_error(
            "rmw_borrow_loaned_message: typesupport differs from the "
            "publisher's");
        return RMW_RET_INVALID_ARGUMENT;
    }
    struct rmw_publisher_data_t *d = publisher->data;
    if (d->loan_slot != NULL) {
        rmw_weft_set_error(
            "rmw_borrow_loaned_message: one outstanding loan per publisher "
            "(documented contract)");
        return RMW_RET_ERROR;
    }
    (void)pub_attach_missing_rings(d);
    if (d->ring_count == 0) {
        /* No subscriber yet: borrow against the pending slot of a shadow
         * commit is impossible — we honestly refuse instead of handing out
         * memory that would never be published. */
        rmw_weft_set_error(
            "rmw_borrow_loaned_message: no subscriber ring attached yet");
        return RMW_RET_TIMEOUT;
    }
    rmw_ring_map_t *ring = &d->rings[0];
    rmw_ring_slot_t *slot = NULL;
    uint8_t *payload = NULL;
    int r = rmw_ring_borrow(ring, d->reliable, -1, &slot, &payload);
    if (r != 0) {
        rmw_weft_set_error(
            "rmw_borrow_loaned_message: ring full (%s)",
            r == -3 ? "RELIABLE ladder timed out" : "BEST_EFFORT dropped");
        return r == -3 ? RMW_RET_TIMEOUT : RMW_RET_ERROR;
    }
    d->loan_ring = ring;
    d->loan_slot = slot;
    d->loan_payload = payload;
    *loaned_message = payload;
    return RMW_RET_OK;
}

rmw_ret_t rmw_publish_loaned_message(const rmw_publisher_t *publisher,
                                     void *loaned_message,
                                     rmw_publisher_allocation_t *allocation) {
    (void)allocation;
    if (publisher == NULL || publisher->data == NULL || loaned_message == NULL) {
        rmw_weft_set_error("rmw_publish_loaned_message: NULL argument");
        return RMW_RET_INVALID_ARGUMENT;
    }
    struct rmw_publisher_data_t *d = publisher->data;
    if (d->loan_slot == NULL || loaned_message != (void *)d->loan_payload) {
        rmw_weft_set_error(
            "rmw_publish_loaned_message: pointer is not the outstanding "
            "publisher loan");
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (rmw_ring_commit(d->loan_ring, d->loan_slot, d->msg_size) != 0) {
        return RMW_RET_ERROR;
    }
    rmw_registry_activity_bump(d->registry);
    d->loan_ring = NULL;
    d->loan_slot = NULL;
    d->loan_payload = NULL;
    d->published++;

    /* fan-out: replicate to the remaining rings (N-1 copies, declared) */
    for (uint32_t i = 1; i < d->ring_count; i++) {
        int r = rmw_ring_publish(&d->rings[i], loaned_message, d->msg_size,
                                 d->reliable, -1);
        if (r == 0) {
            rmw_registry_activity_bump(d->registry);
        } else {
            d->failed++;
        }
    }
    return RMW_RET_OK;
}

rmw_ret_t rmw_return_loaned_message_to_publisher(const rmw_publisher_t *publisher,
                                                 void *loaned_message) {
    if (publisher == NULL || publisher->data == NULL ||
        loaned_message == NULL) {
        rmw_weft_set_error("rmw_return_loaned_message_to_publisher: NULL");
        return RMW_RET_INVALID_ARGUMENT;
    }
    struct rmw_publisher_data_t *d = publisher->data;
    if (loaned_message != (void *)d->loan_payload) {
        rmw_weft_set_error(
            "rmw_return_loaned_message_to_publisher: not the outstanding "
            "loan");
        return RMW_RET_INVALID_ARGUMENT;
    }
    d->loan_ring = NULL;
    d->loan_slot = NULL;
    d->loan_payload = NULL;
    return RMW_RET_OK;
}

// ---------------------------------------------------------------------------
// Subscriptions
// ---------------------------------------------------------------------------

static uint32_t sub_ring_payload_for(uint32_t msg_size) {
    uint32_t p = rmw_weft_cfg_payload();
    uint32_t need = (msg_size + 63u) & ~63u;
    return need > p ? need : p;
}

rmw_subscription_t *rmw_create_subscription(
    const rmw_node_t *node, const rosidl_message_type_support_t *type_supports,
    const char *topic_name, const rmw_qos_profile_t *qos_policies,
    const rmw_subscription_options_t *subscription_options) {
    if (node == NULL || node->data == NULL || type_supports == NULL ||
        topic_name == NULL) {
        rmw_weft_set_error("rmw_create_subscription: NULL argument");
        return NULL;
    }
    if (subscription_options != NULL &&
        subscription_options->ignore_local_publications) {
        rmw_weft_set_error(
            "rmw_create_subscription: ignore_local_publications is "
            "intra-host-only semantics in rmw_weft (everything is local) — "
            "refused rather than silently ignored (D-62 §C.4)");
        return NULL;
    }
    const rmw_weft_pod_ts_t *pod = rmw_ts_as_pod(type_supports);
    if (pod == NULL) {
        rmw_weft_set_error(
            "rmw_create_subscription: typesupport is not the rmw_weft "
            "POD/.weft descriptor");
        return NULL;
    }
    if (qos_policies == NULL) qos_policies = &RMW_QOS_PROFILE_SENSOR_DATA;

    struct rmw_node_data_t *nd = node->data;
    struct rmw_context_data_t *ctx = nd->context->data;
    struct rmw_subscription_data_t *d = calloc(1, sizeof(*d));
    if (d == NULL) return NULL;
    d->node = (rmw_node_t *)node;
    d->ctx = ctx;
    d->registry = &ctx->registry;
    d->topic_name = strdup(topic_name);
    if (d->topic_name == NULL) {
        free(d);
        return NULL;
    }
    d->type_hash = pod->type_hash;
    d->msg_size = (uint32_t)pod->message_size;
    d->reliable = (qos_policies->reliability ==
                   RMW_QOS_POLICY_RELIABILITY_RELIABLE)
                      ? 1
                      : 0;
    d->sub_instance = (uint32_t)rmw_registry_mint_instance(&ctx->registry);
    d->cursor = 0;
    d->expected_seq = 0;

    uint32_t slots = rmw_weft_cfg_slots();
    uint32_t payload = sub_ring_payload_for(d->msg_size);
    char ring_name[RMW_WEFT_RING_NAME_MAX];
    snprintf(ring_name, sizeof(ring_name), "weft_rmw_d%u_t%08x_s%u",
             ctx->domain_id, (uint32_t)rmw_weft_hash64(topic_name),
             d->sub_instance);
    uint64_t epoch = rmw_registry_epoch(&ctx->registry);
    if (rmw_ring_create(ring_name, slots, payload, d->sub_instance,
                        (uint32_t)(d->reliable ? 1 : 0), epoch, &d->ring) != 0) {
        rmw_weft_set_error(
            "rmw_create_subscription: ring create failed for '%s' "
            "(geometry %u x %u)",
            ring_name, slots, payload);
        free(d->topic_name);
        free(d);
        return NULL;
    }

    uint64_t subs_v = 0;
    if (rmw_registry_add_sub(&ctx->registry, topic_name, d->type_hash,
                             d->msg_size, ring_name, slots, payload,
                             d->sub_instance, nd->pid, &d->record,
                             &subs_v) != 0) {
        rmw_weft_set_error(
            "rmw_create_subscription: registry full for topic '%s'",
            topic_name);
        rmw_ring_destroy(&d->ring);
        free(d->topic_name);
        free(d);
        return NULL;
    }

    rmw_subscription_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        (void)rmw_registry_remove_sub(&ctx->registry, d->record);
        rmw_ring_destroy(&d->ring);
        free(d->topic_name);
        free(d);
        return NULL;
    }
    s->implementation_identifier = RMW_WEFT_IMPLEMENTATION_ID;
    s->data = d;
    s->topic_name = d->topic_name;
    s->type_support_ = type_supports;
    s->options.ignore_local_publications = false;
    s->options.require_unique_network_flow_endpoints =
        RMW_UNIQUE_NETWORK_FLOW_ENDPOINTS_NOT_REQUIRED;
    s->is_cft_enabled = false;
    return s;
}

rmw_ret_t rmw_destroy_subscription(rmw_node_t *node,
                                   rmw_subscription_t *subscription) {
    (void)node;
    if (subscription == NULL || subscription->data == NULL) {
        rmw_weft_set_error("rmw_destroy_subscription: NULL subscription");
        return RMW_RET_INVALID_ARGUMENT;
    }
    struct rmw_subscription_data_t *d = subscription->data;
    (void)rmw_registry_remove_sub(d->registry, d->record);
    rmw_ring_destroy(&d->ring); /* creator: unlinks the shm object */
    free(d->topic_name);
    free(d);
    free(subscription);
    return RMW_RET_OK;
}

// ---------------------------------------------------------------------------
// Take paths (hot)
// ---------------------------------------------------------------------------

static rmw_ret_t sub_take_locked(rmw_subscription_t *sub, void **loan_or_buf,
                                 int loaned, bool *taken,
                                 rmw_message_info_t *info) {
    struct rmw_subscription_data_t *d = sub->data;
    if (loaned && d->loan_ptr != NULL) {
        rmw_weft_set_error(
            "rmw_take_loaned_message: one outstanding loan per "
            "subscription (documented contract)");
        *taken = false;
        return RMW_RET_ERROR;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        rmw_ring_slot_t *slot = NULL;
        uint64_t seq = 0, ts = 0;
        uint32_t size = 0, crc = 0;
        int r = rmw_ring_try_take(&d->ring, d->cursor, &slot, &seq, &size,
                                  &crc, &ts);
        if (r == 0) {
            *taken = false;
            return RMW_RET_OK;
        }
        if (r < 0 || seq != d->cursor) {
            /* torn or abandoned slot: skip, count, never deliver */
            d->cursor++;
            d->missed++;
            rmw_ring_advance_tail(&d->ring, d->cursor);
            continue;
        }
        if (seq > d->expected_seq) {
            d->missed += seq - d->expected_seq;
        }
        d->expected_seq = seq + 1;

        uint64_t v1 = atomic_load_explicit(&slot->version,
                                           memory_order_acquire);
        uint64_t ga = atomic_load_explicit(&d->ring.ctrl->pub_gid_a,
                                           memory_order_acquire);
        uint64_t gb = atomic_load_explicit(&d->ring.ctrl->pub_gid_b,
                                           memory_order_acquire);
        if (loaned) {
            uint8_t *p = rmw_ring_slot_payload(slot);
            if (v1 & 1u) continue;
            d->loan_slot = slot;
            d->loan_ptr = p;
            d->loan_size = size;
            d->cursor++;
            d->taken++;
            *loan_or_buf = p;
            *taken = true;
            if (info != NULL) {
                info->source_timestamp.sec = (int64_t)(ts / 1000000000ull);
                info->source_timestamp.nsec = ts % 1000000000ull;
                struct timespec now;
                (void)clock_gettime(CLOCK_REALTIME, &now);
                info->received_timestamp.sec = now.tv_sec;
                info->received_timestamp.nsec = (uint64_t)now.tv_nsec;
                memset(&info->publisher_gid, 0, sizeof(info->publisher_gid));
                snprintf(info->publisher_gid.implementation_identifier,
                         RMW_IMPLEMENTATION_ID_MAX_SIZE, "%s",
                         RMW_WEFT_IMPLEMENTATION_ID);
                memcpy(info->publisher_gid.data, &ga, sizeof(ga));
                memcpy(info->publisher_gid.data + 8, &gb, sizeof(gb));
                info->from_intra_process = true;
            }
            return RMW_RET_OK;
        }
        /* copy-out path: the tail invariant guarantees exclusivity while
         * we hold the window; the version re-check is defense in depth */
        memcpy(*loan_or_buf, rmw_ring_slot_payload(slot), size);
        atomic_thread_fence(memory_order_acquire);
        uint64_t v2 = atomic_load_explicit(&slot->version,
                                           memory_order_acquire);
        if ((v2 & 1u) || v2 != v1) {
            d->cursor++;
            d->missed++;
            rmw_ring_advance_tail(&d->ring, d->cursor);
            continue;
        }
        (void)crc;
        d->cursor++;
        d->taken++;
        rmw_ring_advance_tail(&d->ring, d->cursor);
        *taken = true;
        if (info != NULL) {
            info->source_timestamp.sec = (int64_t)(ts / 1000000000ull);
            info->source_timestamp.nsec = ts % 1000000000ull;
            struct timespec now;
            (void)clock_gettime(CLOCK_REALTIME, &now);
            info->received_timestamp.sec = now.tv_sec;
            info->received_timestamp.nsec = (uint64_t)now.tv_nsec;
            memset(&info->publisher_gid, 0, sizeof(info->publisher_gid));
            snprintf(info->publisher_gid.implementation_identifier,
                     RMW_IMPLEMENTATION_ID_MAX_SIZE, "%s",
                     RMW_WEFT_IMPLEMENTATION_ID);
            memcpy(info->publisher_gid.data, &ga, sizeof(ga));
            memcpy(info->publisher_gid.data + 8, &gb, sizeof(gb));
            info->from_intra_process = true;
        }
        return RMW_RET_OK;
    }
    *taken = false;
    return RMW_RET_SUBSCRIPTION_TAKE_FAILED;
}

rmw_ret_t rmw_take(const rmw_subscription_t *subscription, void *ros_message,
                   bool *taken, rmw_subscription_allocation_t *allocation) {
    (void)allocation;
    if (subscription == NULL || subscription->data == NULL ||
        ros_message == NULL || taken == NULL) {
        rmw_weft_set_error("rmw_take: NULL argument");
        return RMW_RET_INVALID_ARGUMENT;
    }
    return sub_take_locked((rmw_subscription_t *)subscription, &ros_message, 0,
                           taken, NULL);
}

rmw_ret_t rmw_take_with_info(const rmw_subscription_t *subscription,
                             void *ros_message, bool *taken,
                             rmw_message_info_t *message_info,
                             rmw_subscription_allocation_t *allocation) {
    (void)allocation;
    if (subscription == NULL || subscription->data == NULL ||
        ros_message == NULL || taken == NULL) {
        rmw_weft_set_error("rmw_take_with_info: NULL argument");
        return RMW_RET_INVALID_ARGUMENT;
    }
    if (message_info == NULL) {
        rmw_weft_set_error("rmw_take_with_info: message_info is NULL");
        return RMW_RET_INVALID_ARGUMENT;
    }
    return sub_take_locked((rmw_subscription_t *)subscription, &ros_message, 0,
                           taken, message_info);
}

rmw_ret_t rmw_take_loaned_message(const rmw_subscription_t *subscription,
                                  void **loaned_message, bool *taken,
                                  rmw_message_info_t *message_info) {
    if (subscription == NULL || subscription->data == NULL ||
        loaned_message == NULL || taken == NULL) {
        rmw_weft_set_error("rmw_take_loaned_message: NULL argument");
        return RMW_RET_INVALID_ARGUMENT;
    }
    return sub_take_locked((rmw_subscription_t *)subscription, loaned_message,
                           1, taken, message_info);
}

rmw_ret_t rmw_return_loaned_message(const rmw_subscription_t *subscription,
                                    void *loaned_message) {
    if (subscription == NULL || subscription->data == NULL ||
        loaned_message == NULL) {
        rmw_weft_set_error("rmw_return_loaned_message: NULL argument");
        return RMW_RET_INVALID_ARGUMENT;
    }
    struct rmw_subscription_data_t *d = subscription->data;
    if (loaned_message != d->loan_ptr) {
        rmw_weft_set_error(
            "rmw_return_loaned_message: pointer is not the outstanding "
            "subscription loan");
        return RMW_RET_INVALID_ARGUMENT;
    }
    rmw_ring_advance_tail(&d->ring, d->cursor);
    d->loan_slot = NULL;
    d->loan_ptr = NULL;
    d->loan_size = 0;
    return RMW_RET_OK;
}

// ---------------------------------------------------------------------------
// Wait sets
// ---------------------------------------------------------------------------

rmw_wait_set_t *rmw_create_wait_set(rmw_context_t *context,
                                    size_t max_conditions) {
    if (context == NULL || context->data == NULL) {
        rmw_weft_set_error("rmw_create_wait_set: NULL context");
        return NULL;
    }
    if (max_conditions == 0 || max_conditions > 64) {
        rmw_weft_set_error(
            "rmw_create_wait_set: max_conditions must be 1..64 (got %zu)",
            max_conditions);
        return NULL;
    }
    struct rmw_wait_set_data_t *d = calloc(1, sizeof(*d));
    if (d == NULL) return NULL;
    d->context = context;
    d->max_conditions = max_conditions;

    rmw_wait_set_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        free(d);
        return NULL;
    }
    w->implementation_identifier = RMW_WEFT_IMPLEMENTATION_ID;
    w->data = d;
    return w;
}

rmw_ret_t rmw_destroy_wait_set(rmw_wait_set_t *wait_set) {
    if (wait_set == NULL || wait_set->data == NULL) {
        rmw_weft_set_error("rmw_destroy_wait_set: NULL wait_set");
        return RMW_RET_INVALID_ARGUMENT;
    }
    free(wait_set->data);
    free(wait_set);
    return RMW_RET_OK;
}

rmw_ret_t rmw_wait_set_add_subscription(rmw_wait_set_t *wait_set,
                                        const rmw_subscription_t *subscription) {
    if (wait_set == NULL || wait_set->data == NULL || subscription == NULL) {
        rmw_weft_set_error("rmw_wait_set_add_subscription: NULL argument");
        return RMW_RET_INVALID_ARGUMENT;
    }
    struct rmw_wait_set_data_t *d = wait_set->data;
    if (d->count >= d->max_conditions) return RMW_RET_ERROR;
    for (size_t i = 0; i < d->count; i++) {
        if (d->subs[i] == subscription) return RMW_RET_OK;
    }
    d->subs[d->count++] = subscription;
    return RMW_RET_OK;
}

rmw_ret_t rmw_wait_set_remove_subscription(
    rmw_wait_set_t *wait_set, const rmw_subscription_t *subscription) {
    if (wait_set == NULL || wait_set->data == NULL || subscription == NULL) {
        rmw_weft_set_error("rmw_wait_set_remove_subscription: NULL");
        return RMW_RET_INVALID_ARGUMENT;
    }
    struct rmw_wait_set_data_t *d = wait_set->data;
    for (size_t i = 0; i < d->count; i++) {
        if (d->subs[i] == subscription) {
            d->subs[i] = d->subs[d->count - 1];
            d->count--;
            return RMW_RET_OK;
        }
    }
    return RMW_RET_ERROR;
}

static int sub_is_ready(const rmw_subscription_t *sub) {
    struct rmw_subscription_data_t *d = sub->data;
    uint64_t head = atomic_load_explicit(&d->ring.ctrl->head,
                                         memory_order_acquire);
    return head != d->cursor;
}

static int64_t rmw_mono_now(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

rmw_ret_t rmw_wait(rmw_wait_set_t *wait_set,
                   const rmw_subscriptions_t *subscriptions,
                   const rmw_guard_conditions_t *guard_conditions,
                   const rmw_services_t *services,
                   const rmw_clients_t *clients,
                   const rmw_events_t *events,
                   rmw_wait_set_entry_t *wait_set_info,
                   const rmw_time_point_value_t *wait_timeout) {
    (void)guard_conditions;
    (void)services;
    (void)clients;
    (void)events;
    (void)wait_set_info;
    if (wait_set == NULL || wait_set->data == NULL) {
        rmw_weft_set_error("rmw_wait: NULL wait_set");
        return RMW_RET_INVALID_ARGUMENT;
    }
    struct rmw_wait_set_data_t *d = wait_set->data;
    struct rmw_context_data_t *ctx =
        (struct rmw_context_data_t *)d->context->data;

    /* the passed array wins when present (rcl semantics); otherwise the
     * wait-set storage added via rmw_wait_set_add_subscription */
    const rmw_subscription_t **subs = NULL;
    size_t n = 0;
    if (subscriptions != NULL && subscriptions->subscribers != NULL) {
        subs = (const rmw_subscription_t **)subscriptions->subscribers;
        n = subscriptions->subscriber_count;
    } else {
        subs = d->subs;
        n = d->count;
    }

    int64_t deadline_mono = -1; /* -1: wait forever (100 ms chunks) */
    if (wait_timeout != NULL) {
        struct timespec now_r, now_m;
        (void)clock_gettime(CLOCK_REALTIME, &now_r);
        (void)clock_gettime(CLOCK_MONOTONIC, &now_m);
        int64_t now_r_ns = (int64_t)now_r.tv_sec * 1000000000 +
                           (int64_t)now_r.tv_nsec;
        int64_t now_m_ns = (int64_t)now_m.tv_sec * 1000000000 +
                           (int64_t)now_m.tv_nsec;
        int64_t tgt_ns = (int64_t)wait_timeout->sec * 1000000000 +
                         (int64_t)wait_timeout->nsec;
        deadline_mono = now_m_ns + (tgt_ns - now_r_ns);
        if (tgt_ns == 0) deadline_mono = now_m_ns; /* {0,0} = poll */
    }

    for (;;) {
        for (size_t i = 0; i < n; i++) {
            if (subs[i] != NULL && sub_is_ready(subs[i])) {
                return RMW_RET_OK;
            }
        }
        if (atomic_load_explicit(&ctx->shutdown, memory_order_acquire) != 0) {
            return RMW_RET_OK;
        }
        if (wait_timeout != NULL && wait_timeout->sec == 0 &&
            wait_timeout->nsec == 0) {
            return RMW_RET_TIMEOUT;
        }
        /* park on the registry-wide activity doorbell: infinite waits run
         * in 100 ms chunks, finite waits park to their deadline */
        int64_t now = rmw_mono_now();
        int64_t chunk = (deadline_mono < 0)
                            ? (now + 100000000)
                            : (deadline_mono < now ? now : deadline_mono);
        (void)rmw_registry_activity_wait(&ctx->registry, chunk);
        if (deadline_mono > 0 && rmw_mono_now() >= deadline_mono) {
            for (size_t i = 0; i < n; i++) {
                if (subs[i] != NULL && sub_is_ready(subs[i])) {
                    return RMW_RET_OK;
                }
            }
            return RMW_RET_TIMEOUT;
        }
    }
}

// ---------------------------------------------------------------------------
// Test seams
// ---------------------------------------------------------------------------

const rmw_ring_map_t *rmw_weft_sub_ring(const rmw_subscription_t *sub) {
    if (sub == NULL || sub->data == NULL) return NULL;
    return &((struct rmw_subscription_data_t *)sub->data)->ring;
}

const rmw_ring_map_t *rmw_weft_pub_ring0(const rmw_publisher_t *pub) {
    if (pub == NULL || pub->data == NULL) return NULL;
    struct rmw_publisher_data_t *d = pub->data;
    return d->ring_count > 0 ? &d->rings[0] : NULL;
}

int rmw_weft_sub_ring_name(const rmw_subscription_t *sub, char *buf,
                           size_t buflen) {
    if (sub == NULL || sub->data == NULL || buf == NULL || buflen == 0)
        return -1;
    struct rmw_subscription_data_t *d = sub->data;
    snprintf(buf, buflen, "%s", d->ring.name);
    return 0;
}

uint64_t rmw_weft_pub_published(const rmw_publisher_t *pub) {
    if (pub == NULL || pub->data == NULL) return 0;
    return ((struct rmw_publisher_data_t *)pub->data)->published;
}

uint64_t rmw_weft_pub_failed(const rmw_publisher_t *pub) {
    if (pub == NULL || pub->data == NULL) return 0;
    return ((struct rmw_publisher_data_t *)pub->data)->failed;
}

uint32_t rmw_weft_pub_ring_count(const rmw_publisher_t *pub) {
    if (pub == NULL || pub->data == NULL) return 0;
    return ((struct rmw_publisher_data_t *)pub->data)->ring_count;
}

uint64_t rmw_weft_sub_taken(const rmw_subscription_t *sub) {
    if (sub == NULL || sub->data == NULL) return 0;
    return ((struct rmw_subscription_data_t *)sub->data)->taken;
}

uint64_t rmw_weft_sub_missed(const rmw_subscription_t *sub) {
    if (sub == NULL || sub->data == NULL) return 0;
    return ((struct rmw_subscription_data_t *)sub->data)->missed;
}

uint64_t rmw_weft_pub_dropped(const rmw_publisher_t *pub) {
    if (pub == NULL || pub->data == NULL) return 0;
    struct rmw_publisher_data_t *d = pub->data;
    uint64_t total = 0;
    for (uint32_t i = 0; i < d->ring_count; i++) {
        total += atomic_load_explicit(&d->rings[i].ctrl->dropped_total,
                                      memory_order_acquire);
    }
    return total;
}
