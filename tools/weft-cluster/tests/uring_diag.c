// uring_diag.c — opcode-level capability diagnostic (the D-32 §5 tool:
// which of SEND / SEND_ZC / WRITE_FIXED / READ / READ_FIXED this kernel
// honors for connected UDP loopback). Standalone build:
//   cc -O1 -g -std=c11 -D_GNU_SOURCE -o /tmp/uring_diag tests/uring_diag.c
// Not part of the gate batteries — it is the evidence tool that found
// the ZC-drop defect; preserved for hardware-runner diagnosis.
// uring_diag.c — opcode-level diagnosis for the cluster uring road:
// which of SEND / SEND_ZC / WRITE_FIXED / READ / READ_FIXED actually
// work on THIS kernel for connected UDP loopback sockets.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <linux/io_uring.h>

#ifndef IORING_OP_SEND_ZC
#define IORING_OP_SEND_ZC 38
#endif
#ifndef IORING_CQE_F_NOTIFY
#define IORING_CQE_F_NOTIFY (1u << 3)
#endif

static int ring_fd = -1;
static struct io_uring_params P;
static uint32_t *sq_tail, *sq_head, *sq_array, *sq_mask;
static uint32_t *cq_head, *cq_tail, *cq_mask;
static void *cqes, *sqes;
static uint32_t entries;

static int setup_ring(void) {
    memset(&P, 0, sizeof(P));
    ring_fd = (int)syscall(__NR_io_uring_setup, 16, &P);
    if (ring_fd < 0) { perror("setup"); return -1; }
    entries = P.sq_entries;
    size_t sql = (size_t)P.sq_off.array + entries * 4;
    size_t cql = (size_t)P.cq_off.cqes + entries * sizeof(struct io_uring_cqe);
    void* sq = mmap(NULL, sql, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, ring_fd, IORING_OFF_SQ_RING);
    void* cq = mmap(NULL, cql, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, ring_fd, IORING_OFF_CQ_RING);
    sqes = mmap(NULL, entries * sizeof(struct io_uring_sqe), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, ring_fd, IORING_OFF_SQES);
    if (sq == MAP_FAILED || cq == MAP_FAILED || sqes == MAP_FAILED) { perror("mmap"); return -1; }
    sq_tail = (uint32_t*)((char*)sq + P.sq_off.tail);
    sq_head = (uint32_t*)((char*)sq + P.sq_off.head);
    sq_array = (uint32_t*)((char*)sq + P.sq_off.array);
    sq_mask = (uint32_t*)((char*)sq + P.sq_off.ring_mask);
    cq_head = (uint32_t*)((char*)cq + P.cq_off.head);
    cq_tail = (uint32_t*)((char*)cq + P.cq_off.tail);
    cq_mask = (uint32_t*)((char*)cq + P.cq_off.ring_mask);
    cqes = (char*)cq + P.cq_off.cqes;
    return 0;
}

static struct io_uring_sqe* next_sqe(void) {
    uint32_t head = __atomic_load_n(sq_head, __ATOMIC_ACQUIRE);
    if (*sq_tail - head >= entries) return NULL;
    uint32_t idx = (*sq_tail) & (*sq_mask);
    struct io_uring_sqe* s = (struct io_uring_sqe*)sqes + idx;
    memset(s, 0, sizeof(*s));
    sq_array[idx] = idx;
    __atomic_store_n(sq_tail, *sq_tail + 1, __ATOMIC_RELEASE);
    return s;
}

static long enter_submit(void) {
    uint32_t pending = *sq_tail - __atomic_load_n(sq_head, __ATOMIC_ACQUIRE);
    if (!pending) return 0;
    return syscall(__NR_io_uring_enter, ring_fd, pending, 0, 0, NULL);
}

static int harvest(char* tag, int wait_ms) {
    for (int spin = 0; spin < wait_ms * 100; spin++) {
        uint32_t h = __atomic_load_n(cq_head, __ATOMIC_RELAXED);
        uint32_t t = __atomic_load_n(cq_tail, __ATOMIC_ACQUIRE);
        while (h != t) {
            struct io_uring_cqe* c = (struct io_uring_cqe*)((char*)cqes +
                (h & (*cq_mask)) * sizeof(struct io_uring_cqe));
            h++;
            __atomic_store_n(cq_head, h, __ATOMIC_RELEASE);
            printf("  [%s] cqe res=%d user_data=%llx flags=%x\n", tag,
                   c->res, (unsigned long long)c->user_data, c->flags);
            if (c->res < 0) printf("       errno-name: %s\n", strerror(-c->res));
            return c->res;
        }
        usleep(100);
    }
    printf("  [%s] NO cqe (timeout %d ms)\n", tag, wait_ms);
    return -999;
}

int main(void) {
    if (setup_ring() != 0) return 1;
    printf("io_uring params: sq_entries=%u features=0x%x\n", P.sq_entries, P.features);

    int a = socket(AF_INET, SOCK_DGRAM, 0);
    int b = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(a, (struct sockaddr*)&sa, sizeof(sa));
    bind(b, (struct sockaddr*)&sa, sizeof(sa));
    socklen_t sl = sizeof(sa);
    getsockname(a, (struct sockaddr*)&sa, &sl);
    struct sockaddr_in aa = sa;
    getsockname(b, (struct sockaddr*)&sa, &sl);
    struct sockaddr_in bb = sa;
    connect(a, (struct sockaddr*)&bb, sizeof(bb));
    connect(b, (struct sockaddr*)&aa, sizeof(aa));
    printf("a=%d -> port %u, b=%d -> port %u\n", a, ntohs(aa.sin_port), b, ntohs(bb.sin_port));

    static char bufA[4096] __attribute__((aligned(4096)));
    static char bufB[4096] __attribute__((aligned(4096)));
    strcpy(bufA, "hello-weft");

    // 1. plain SEND -> plain READ
    printf("[1] plain SEND then plain READ\n");
    {
        struct io_uring_sqe* r = next_sqe();
        r->opcode = IORING_OP_READ; r->fd = b;
        r->addr = (uint64_t)(uintptr_t)bufB; r->len = 4096; r->user_data = 0x11;
        struct io_uring_sqe* s = next_sqe();
        s->opcode = IORING_OP_SEND; s->fd = a;
        s->addr = (uint64_t)(uintptr_t)bufA; s->len = 11; s->msg_flags = MSG_DONTWAIT; s->user_data = 0x22;
        enter_submit();
        harvest("send-plain", 50);
        int res = harvest("read-plain", 100);
        if (res > 0) printf("  -> recv '%.*s'\n", res, bufB);
    }

    // 2. SEND_ZC
    printf("[2] SEND_ZC\n");
    {
        struct io_uring_sqe* r = next_sqe();
        r->opcode = IORING_OP_READ; r->fd = b;
        r->addr = (uint64_t)(uintptr_t)bufB; r->len = 4096; r->user_data = 0x33;
        struct io_uring_sqe* s = next_sqe();
        s->opcode = IORING_OP_SEND_ZC; s->fd = a;
        s->addr = (uint64_t)(uintptr_t)bufA; s->len = 11; s->msg_flags = MSG_DONTWAIT; s->user_data = 0x44;
        long rc = enter_submit();
        printf("  enter rc=%ld errno=%d(%s)\n", rc, errno, strerror(errno));
        harvest("send-zc", 100);
        harvest("send-zc-notif", 100);
        harvest("read-after-zc", 100);
    }

    // 3. registered buffers + FIXED ops
    printf("[3] REGISTER_BUFFERS + WRITE_FIXED / READ_FIXED\n");
    {
        struct iovec iov[2] = { { bufA, 4096 }, { bufB, 4096 } };
        long rc = syscall(__NR_io_uring_register, ring_fd, IORING_REGISTER_BUFFERS, iov, 2);
        printf("  register rc=%ld errno=%d(%s)\n", rc, errno, rc ? strerror(errno) : "ok");
        if (rc == 0) {
            struct io_uring_sqe* r = next_sqe();
            r->opcode = IORING_OP_READ_FIXED; r->fd = b;
            r->addr = (uint64_t)(uintptr_t)bufB; r->len = 4096; r->buf_index = 1; r->user_data = 0x55;
            struct io_uring_sqe* s = next_sqe();
            s->opcode = IORING_OP_WRITE_FIXED; s->fd = a;
            s->addr = (uint64_t)(uintptr_t)bufA; s->len = 11; s->buf_index = 0; s->user_data = 0x66;
            enter_submit();
            harvest("write-fixed", 100);
            int res = harvest("read-fixed", 100);
            if (res > 0) printf("  -> recv '%.*s'\n", res, bufB);
        }
    }

    printf("[4] SEND_ZC with fixed buffer (ioprio bit)\n");
    {
        struct io_uring_sqe* r = next_sqe();
        r->opcode = IORING_OP_READ_FIXED; r->fd = b;
        r->addr = (uint64_t)(uintptr_t)bufB; r->len = 4096; r->buf_index = 1; r->user_data = 0x77;
        struct io_uring_sqe* s = next_sqe();
        s->opcode = IORING_OP_SEND_ZC; s->fd = a;
        s->addr = (uint64_t)(uintptr_t)bufA; s->len = 11; s->buf_index = 0;
        s->ioprio |= (1 << 4); s->msg_flags = MSG_DONTWAIT; s->user_data = 0x88;
        long rc = enter_submit();
        printf("  enter rc=%ld\n", rc);
        harvest("zc-fixed", 200);
        harvest("zc-fixed-2", 100);
        harvest("read-after-zc-fixed", 100);
    }
    return 0;
}
