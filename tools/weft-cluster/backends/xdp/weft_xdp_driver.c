// weft_xdp_driver.c — RFC-0019 §4 (see weft_xdp_driver.h for the contract).
//
// Three engineering stances worth reading before touching this file:
//
// 1. NO LIBBPF. Everything goes through the raw bpf(2) syscall — the
//    same five reasons uring_rx.c hand-rolls its rings apply here: zero
//    link-time deps, stable uapi, small surface, every refusal REPORTED
//    (Law 4) instead of errno-laundered, and a library would happily
//    paper over the capability ladder this driver exists to expose.
//
// 2. THE ASSEMBLER, NOT A TOOLCHAIN. The filter bytecode is EMITTED at
//    driver init from the config (cluster_id / schema_id / port / map
//    fd) — no clang, no ELF, no libbpf on the cluster node. The
//    kernel-side C source (bpf/weft_xdp_filter.c) is the deployment
//    reference; the two are kept in lockstep by the CL-X structural
//    gates (which assert the emitted program's shape: jump targets in
//    range, helper ids in {1,44,51}, immediates present, map fd at both
//    pseudo-load sites).
//
// 3. WCF1 AT CHUNK START, EVERYWHERE. The filter calls
//    bpf_xdp_adjust_head(ctx, 42) before redirecting, so the frame the
//    NIC DMAs into the WCR1 chunk starts at the WCF1 header — the SAME
//    layout the io_uring and loopback roads deliver. One placement law
//    across the whole fabric (RFC-0019 §2.2).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "weft_xdp_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/mman.h>

#include <linux/if_xdp.h>

// uapi fallback: the ring-flags "need wakeup" bit (1 << 3) is named
// XDP_NEED_WAKEUP in libxdp; some kernel-header vintages omit it.
#ifndef XDP_NEED_WAKEUP
#define XDP_NEED_WAKEUP (1 << 3)
#endif

// ---------------------------------------------------------------------------
// bpf(2) wrappers (raw, checked, honest)
// ---------------------------------------------------------------------------

static long sys_bpf(int cmd, union bpf_attr* attr, unsigned size) {
    return syscall(__NR_bpf, cmd, attr, size);
}

static int bpf_map_create_xskmap(uint32_t max_entries, char* err,
                                 size_t errlen) {
    union bpf_attr a;
    memset(&a, 0, sizeof(a));
    a.map_type = BPF_MAP_TYPE_XSKMAP;
    a.key_size = 4;
    a.value_size = 4;
    a.max_entries = max_entries;
    const int fd = (int)sys_bpf(BPF_MAP_CREATE, &a, sizeof(a));
    if (fd < 0 && err) {
        snprintf(err, errlen, "xdp: BPF_MAP_CREATE(XSKMAP) refused: %s",
                 strerror(errno));
    }
    return fd;
}

static int bpf_map_update(int map_fd, const void* key, const void* value,
                          uint64_t flags, char* err, size_t errlen) {
    union bpf_attr a;
    memset(&a, 0, sizeof(a));
    a.map_fd = map_fd;
    a.key = (uint64_t)(uintptr_t)key;
    a.value = (uint64_t)(uintptr_t)value;
    a.flags = flags;
    if (sys_bpf(BPF_MAP_UPDATE_ELEM, &a, sizeof(a)) != 0) {
        if (err) snprintf(err, errlen, "xdp: MAP_UPDATE refused: %s",
                          strerror(errno));
        return -1;
    }
    return 0;
}

static int bpf_prog_load_xdp(const struct bpf_insn* insns, uint32_t cnt,
                             char* err, size_t errlen) {
    char log[2048];
    union bpf_attr a;
    memset(&a, 0, sizeof(a));
    a.prog_type = BPF_PROG_TYPE_XDP;
    a.insns = (uint64_t)(uintptr_t)insns;
    a.insn_cnt = cnt;
    a.license = (uint64_t)(uintptr_t) "GPL";  // redirect/adjust_head are
                                              // gpl-only helpers
    a.log_buf = (uint64_t)(uintptr_t)log;
    a.log_size = sizeof(log);
    a.log_level = 1;
    const int fd = (int)sys_bpf(BPF_PROG_LOAD, &a, sizeof(a));
    if (fd < 0) {
        const int e = errno;
        if (err) {
            // EPERM is the unprivileged verdict — the honest PERMS rung.
            snprintf(err, errlen, "xdp: BPF_PROG_LOAD(XDP) refused: %s%s",
                     strerror(e),
                     e == EPERM ? " (CAP_BPF/CAP_NET_ADMIN missing — Law 4)"
                                : "");
            size_t used = strlen(err);
            if (used + 16 < errlen && log[0]) {
                snprintf(err + used, errlen - used, " | verifier: %.120s",
                         log);
            }
        }
        return -1;
    }
    return fd;
}

static int bpf_link_create_xdp(int prog_fd, unsigned ifindex,
                               uint32_t flags, char* err, size_t errlen) {
    union bpf_attr a;
    memset(&a, 0, sizeof(a));
    a.link_create.prog_fd = prog_fd;
    a.link_create.target_ifindex = ifindex;
    a.link_create.attach_type = BPF_XDP;
    a.link_create.flags = flags;
    const int fd = (int)sys_bpf(BPF_LINK_CREATE, &a, sizeof(a));
    if (fd < 0) {
        const int e = errno;
        if (err) {
            if (e == EPERM) {
                snprintf(err, errlen,
                         "xdp: BPF_LINK_CREATE refused: %s "
                         "(CAP_NET_ADMIN missing — Law 4)", strerror(e));
            } else if (e == EINVAL || e == ENOSYS) {
                snprintf(err, errlen,
                         "xdp: bpf_link XDP attach unsupported (kernel "
                         "< 5.9?); attach manually: "
                         "ip link set dev <if> xdp fd %d", prog_fd);
            } else {
                snprintf(err, errlen, "xdp: BPF_LINK_CREATE refused: %s",
                         strerror(e));
            }
        }
        return -1;
    }
    return fd;
}

// ---------------------------------------------------------------------------
// The bytecode assembler (init-time; Law 1 exempts setup, the EMITTED
// program itself cannot allocate by construction)
// ---------------------------------------------------------------------------

// eBPF encodings (uapi-stable; mirrors of linux/bpf.h's classes).
#define BPF_CLS_LD   0x00
#define BPF_CLS_LDX  0x01
#define BPF_CLS_JMP  0x05
#define BPF_CLS_ALU64 0x07
#define BPF_OP_MOV   0xb0
#define BPF_OP_ADD   0x00
#define BPF_OP_AND   0x50
#define BPF_OP_JEQ   0x10
#define BPF_OP_JNE   0x50
#define BPF_OP_JGT   0x20
#define BPF_SRC_K    0x00
#define BPF_SRC_X    0x08
#define BPF_MODE_MEM 0x60
#define BPF_SIZE_W   0x00
#define BPF_SIZE_H   0x08
#define BPF_SIZE_B   0x10
#define BPF_IMM      0x00
#define BPF_DW       0x18

#define EMIT_MOV64_IMM(d, imm)  emit(&a, BPF_CLS_ALU64|BPF_OP_MOV|BPF_SRC_K, d, 0, 0, imm)
#define EMIT_MOV64_REG(d, s)    emit(&a, BPF_CLS_ALU64|BPF_OP_MOV|BPF_SRC_X, d, s, 0, 0)
#define EMIT_ADD64_IMM(d, imm)  emit(&a, BPF_CLS_ALU64|BPF_OP_ADD|BPF_SRC_K, d, 0, 0, imm)
#define EMIT_AND64_IMM(d, imm)  emit(&a, BPF_CLS_ALU64|BPF_OP_AND|BPF_SRC_K, d, 0, 0, imm)
#define EMIT_LDX_W(d, s, off)   emit(&a, BPF_CLS_LDX|BPF_MODE_MEM|BPF_SIZE_W, d, s, off, 0)
#define EMIT_LDX_H(d, s, off)   emit(&a, BPF_CLS_LDX|BPF_MODE_MEM|BPF_SIZE_H, d, s, off, 0)
#define EMIT_LDX_B(d, s, off)   emit(&a, BPF_CLS_LDX|BPF_MODE_MEM|BPF_SIZE_B, d, s, off, 0)
#define EMIT_JEQ_IMM(d, imm)    emit_jump(&a, BPF_CLS_JMP|BPF_OP_JEQ|BPF_SRC_K, d, 0, imm)
#define EMIT_JNE_IMM(d, imm)    emit_jump(&a, BPF_CLS_JMP|BPF_OP_JNE|BPF_SRC_K, d, 0, imm)
#define EMIT_JGT_REG(d, s)      emit_jump(&a, BPF_CLS_JMP|BPF_OP_JGT|BPF_SRC_X, d, s, 0)
#define EMIT_CALL(fn)           emit(&a, BPF_CLS_JMP|0x80, 0, 0, 0, fn)
#define EMIT_EXIT()             emit(&a, BPF_CLS_JMP|0x90, 0, 0, 0, 0)
#define EMIT_LD_IMM64_FD(d, fd) emit_ldimm64_fd(&a, d, fd)

#define XDP_PASS_ACTION 2

typedef struct {
    struct bpf_insn* insns;
    uint32_t cnt, max;
    int failed;
    uint16_t pass_fixups[32];   // jump insns awaiting the PASS target
    uint32_t n_pass;
} weft_bpf_asm_t;

static void emit(weft_bpf_asm_t* a, uint8_t code, uint8_t dst, uint8_t src,
                 int16_t off, int32_t imm) {
    if (a->failed) return;
    if (a->cnt >= a->max) { a->failed = 1; return; }
    struct bpf_insn* i = &a->insns[a->cnt++];
    i->code = code;
    i->dst_reg = dst & 0xf;
    i->src_reg = src & 0xf;
    i->off = off;
    i->imm = imm;
}

static void emit_jump(weft_bpf_asm_t* a, uint8_t code, uint8_t dst,
                      uint8_t src, int32_t imm) {
    emit(a, code, dst, src, 0, imm);
    if (a->failed) return;
    if (a->n_pass >= 32) { a->failed = 1; return; }
    a->pass_fixups[a->n_pass++] = (uint16_t)(a->cnt - 1);
}

static void emit_ldimm64_fd(weft_bpf_asm_t* a, uint8_t dst, int fd) {
    emit(a, BPF_CLS_LD|BPF_IMM|BPF_DW, dst, BPF_PSEUDO_MAP_FD, 0, fd);
    emit(a, 0, 0, 0, 0, 0);  // the mandatory second half
}

static void patch_pass(weft_bpf_asm_t* a) {
    for (uint32_t k = 0; k < a->n_pass; k++) {
        const uint16_t at = a->pass_fixups[k];
        a->insns[at].off = (int16_t)(a->cnt - at - 1);
    }
}

int weft_xdp_build_filter(const weft_xdp_config_t* cfg,
                          struct bpf_insn* insns, uint32_t max, int map_fd) {
    if (!cfg || !insns || max < 48) return -1;
    // eBPF immediate compares are SIGN-EXTENDED: steering keys must fit
    // the u31 law or the filter could never match (named refusal).
    if (cfg->cluster_id > 0x7fffffffu || cfg->schema_id > 0x7fffffffu) {
        return -1;
    }
    weft_bpf_asm_t a = { .insns = insns, .cnt = 0, .max = max };

    // dport on the wire is BE; the program compares the LE load — swap.
    const uint32_t port = (cfg->udp_port ? cfg->udp_port
                                         : WEFT_CLUSTER_UDP_PORT);
    const int32_t port_le = (int32_t)(((port & 0xff) << 8) | (port >> 8));

    // r6 = ctx, r7 = data, r8 = data_end
    EMIT_MOV64_REG(6, 1);
    EMIT_LDX_W(7, 6, 0);
    EMIT_LDX_W(8, 6, 4);

    // one bounds check for eth(14)+ip(20)+udp(8)+wcf1(64)
    EMIT_MOV64_REG(2, 7);
    EMIT_ADD64_IMM(2, 14 + 20 + 8 + 64);
    EMIT_JGT_REG(2, 8);

    // ethertype == IPv4 (wire BE 08 00 -> LE u16 0x0008)
    EMIT_LDX_H(3, 7, 12);
    EMIT_JNE_IMM(3, 0x0008);

    // IHL == 5 (options take the stack road)
    EMIT_LDX_B(3, 7, 14);
    EMIT_AND64_IMM(3, 0x0f);
    EMIT_JNE_IMM(3, 5);

    // no fragmentation: BE flags:frag lives at +20; LE-load mask 0xFF3F
    // covers frag-offset bits AND the MF bit (DF/reserved excluded) —
    // the derivation is in RFC-0019 §4.4 (the derivation matters: this
    // mask is the one bit of the filter that is NOT readable by eye).
    EMIT_LDX_H(3, 7, 20);
    EMIT_AND64_IMM(3, (int32_t)0xFF3F);
    EMIT_JNE_IMM(3, 0);

    // protocol == UDP
    EMIT_LDX_B(3, 7, 23);
    EMIT_JNE_IMM(3, 17);

    // dport == cluster port (LE-swapped immediate)
    EMIT_LDX_H(3, 7, 36);
    EMIT_JNE_IMM(3, port_le);

    // WCF1: magic ".wft" (LE u32 0x7466772E), version 1, cluster, schema
    EMIT_LDX_W(3, 7, 42);
    EMIT_JNE_IMM(3, 0x7466772e);
    EMIT_LDX_H(3, 7, 46);
    EMIT_JNE_IMM(3, 1);
    EMIT_LDX_W(3, 7, 50);
    EMIT_JNE_IMM(3, (int32_t)cfg->cluster_id);
    if (cfg->schema_id != 0) {
        EMIT_LDX_W(3, 7, 54);
        EMIT_JNE_IMM(3, (int32_t)cfg->schema_id);
    }

    // strip eth+ip+udp (42) so the chunk receives WCF1 AT OFFSET ZERO —
    // the uniform placement law across all transports.
    EMIT_MOV64_REG(1, 6);
    EMIT_MOV64_IMM(2, 42);
    EMIT_CALL(44);  // bpf_xdp_adjust_head
    EMIT_JNE_IMM(0, 0);  // r0 = verdict; != 0 -> bail to PASS

    // XSKMAP lookup validates the queue has a bound socket
    EMIT_LD_IMM64_FD(1, map_fd);
    EMIT_LDX_W(2, 6, 16);  // ctx->rx_queue_index
    EMIT_CALL(1);          // bpf_map_lookup_elem
    EMIT_JEQ_IMM(0, 0);    // socket absent -> PASS (no stealing)

    // redirect: the NIC DMAs the (head-adjusted) frame into WCR1 UMEM
    EMIT_LD_IMM64_FD(1, map_fd);
    EMIT_LDX_W(2, 6, 16);
    EMIT_MOV64_IMM(3, 0);
    EMIT_CALL(51);         // bpf_redirect_map
    EMIT_EXIT();

    // PASS label: everything unmatched continues its normal stack road
    patch_pass(&a);
    EMIT_MOV64_IMM(0, XDP_PASS_ACTION);
    EMIT_EXIT();

    return a.failed ? -1 : (int)a.cnt;
}

// ---------------------------------------------------------------------------
// probe / config
// ---------------------------------------------------------------------------

static weft_xdp_cap_t g_probe_cap = WEFT_XDP_CAP_NONE;
static int g_probe_done = 0;
static char g_probe_detail[192];

weft_xdp_cap_t weft_xdp_probe(char* detail, size_t detail_len) {
    if (!g_probe_done) {
        g_probe_done = 1;
        g_probe_cap = WEFT_XDP_CAP_NONE;
        g_probe_detail[0] = '\0';

        // The capability ladder's honest order: caps first (the common
        // cloud-runner state), then the family, then a real map create.
        if (!weft_cap_effective(WEFT_CAP_BPF) &&
            !weft_cap_effective(WEFT_CAP_NET_ADMIN) &&
            !weft_cap_effective(WEFT_CAP_SYS_ADMIN)) {
            snprintf(g_probe_detail, sizeof(g_probe_detail),
                     "xdp: no CAP_BPF/CAP_NET_ADMIN — filter load will "
                     "refuse (Law 4); io_uring road carries the traffic");
        } else {
            const int fd = socket(AF_XDP, SOCK_RAW | SOCK_CLOEXEC, 0);
            if (fd < 0) {
                snprintf(g_probe_detail, sizeof(g_probe_detail),
                         "xdp: AF_XDP family absent (%s) — CONFIG_XDP_"
                         "SOCKETS kernel required", strerror(errno));
            } else {
                close(fd);
                snprintf(g_probe_detail, sizeof(g_probe_detail),
                         "xdp: caps + AF_XDP family present (SETUP "
                         "candidate)");
                g_probe_cap = WEFT_XDP_CAP_SETUP;
            }
        }
    }
    if (detail && detail_len) {
        snprintf(detail, detail_len, "%.150s (rung=%d)", g_probe_detail,
                 (int)g_probe_cap);
    }
    return g_probe_cap;
}

size_t weft_xdp_report(char* buf, size_t buflen) {
    char d[192];
    weft_xdp_probe(d, sizeof(d));
    int n = snprintf(buf, buflen, "%s\n", d);
    return n > 0 ? (size_t)n : 0;
}

weft_xdp_config_t weft_xdp_config_default(void) {
    weft_xdp_config_t c = {
        .ifname = "lo",
        .cluster_id = 1,
        .schema_id = 0,
        .udp_port = WEFT_CLUSTER_UDP_PORT,
        .queue_id = 0,
        .fill_depth = 64,
        .comp_depth = 64,
        .rx_depth = 64,
        .xdp_flags = 0,
        .poll_timeout_ns = 50ull * 1000 * 1000,
    };
    return c;
}

// ---------------------------------------------------------------------------
// ring primitives (kernel-shared memory; acquire/release as documented)
// ---------------------------------------------------------------------------

static uint32_t ring_ld_acq(volatile uint32_t* p) {
    return __atomic_load_n((uint32_t*)p, __ATOMIC_ACQUIRE);
}
static void ring_st_rel(volatile uint32_t* p, uint32_t v) {
    __atomic_store_n((uint32_t*)p, v, __ATOMIC_RELEASE);
}

static int pow2_u32(uint32_t v) { return v && !(v & (v - 1)); }

// ---------------------------------------------------------------------------
// init (xsk + UMEM over the WCR1 region + rings + bind)
// ---------------------------------------------------------------------------

weft_cluster_status_t weft_xdp_driver_init(const weft_xdp_config_t* cfg,
                                           const weft_wcr1_region_t* r,
                                           weft_xdp_socket_t* s) {
    if (!cfg || !r || !s) return WEFT_CLUSTER_E_INVALID_ARG;
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    s->xsk_fd = -1;
    s->map_fd = -1;
    s->prog_fd = -1;
    s->link_fd = -1;

    // 1. placement law FIRST (pure logic — testable on every runner,
    //    hardware or not; stricter than the WCR1 floor — see header)
    char why[128];
    if (weft_wcr1_validate(r, why, sizeof(why)) != WEFT_WCR1_OK) {
        snprintf(s->err, sizeof(s->err), "xdp: region refused — %s", why);
        return WEFT_CLUSTER_E_INVALID_ARG;
    }
    if (r->chunk0_offset % 4096u != 0 || r->chunk_size % 4096u != 0) {
        snprintf(s->err, sizeof(s->err),
                 "xdp: UMEM placement law refuses chunk0=%llu chunk=%u "
                 "(need page multiples — the dedicated DMA ring road)",
                 (unsigned long long)r->chunk0_offset, r->chunk_size);
        return WEFT_CLUSTER_E_INVALID_ARG;
    }
    if (!pow2_u32(cfg->fill_depth) || !pow2_u32(cfg->comp_depth) ||
        !pow2_u32(cfg->rx_depth)) {
        snprintf(s->err, sizeof(s->err),
                 "xdp: ring depths must be powers of two");
        return WEFT_CLUSTER_E_INVALID_ARG;
    }

    // 2. capability gate (honest order: refuse before side effects)
    const weft_xdp_cap_t cap = weft_xdp_probe(s->err, sizeof(s->err));
    if (cap == WEFT_XDP_CAP_NONE) {
        s->stats.last_errno = EPERM;
        return WEFT_CLUSTER_E_PERMS;
    }


    // 3. the socket
    s->xsk_fd = socket(AF_XDP, SOCK_RAW | SOCK_CLOEXEC, 0);
    if (s->xsk_fd < 0) {
        s->stats.last_errno = errno;
        snprintf(s->err, sizeof(s->err), "xdp: socket(AF_XDP) refused: %s",
                 strerror(errno));
        return (errno == EAFNOSUPPORT) ? WEFT_CLUSTER_E_SYS
                                       : WEFT_CLUSTER_E_IO;
    }

    // 4. UMEM registration over the WHOLE region span (the xdp_rx
    //    precedent: the session header rides inside the UMEM; chunks
    //    are addressed by offset).
    struct xdp_umem_reg um;
    memset(&um, 0, sizeof(um));
    um.addr = (uint64_t)(uintptr_t)r->base;
    um.len = r->span;
    um.chunk_size = r->chunk_size;
    um.headroom = 0;   // WCF1 lands at chunk start (adjust_head did it)
    um.flags = XDP_UMEM_UNALIGNED_CHUNK_FLAG;  // chunk0 may sit at a
                                               // page-multiple offset
    if (setsockopt(s->xsk_fd, SOL_XDP, XDP_UMEM_REG, &um, sizeof(um)) != 0) {
        const int e = errno;
        s->stats.last_errno = e;
        if (e == EINVAL &&
            ((r->chunk0_offset % r->chunk_size) == 0)) {
            // retry the aligned-chunk road (older kernels): power-of-two
            // chunk sizes with chunk-aligned offsets
            if (pow2_u32(r->chunk_size)) {
                um.flags = 0;
                if (setsockopt(s->xsk_fd, SOL_XDP, XDP_UMEM_REG, &um,
                               sizeof(um)) == 0) {
                    goto umem_ok;
                }
            }
        }
        snprintf(s->err, sizeof(s->err),
                 "xdp: XDP_UMEM_REG(%llu bytes) refused: %s "
                 "(RLIMIT_MEMLOCK caps pinned pages — see RFC-0019 §4.6)",
                 (unsigned long long)r->span, strerror(errno));
        close(s->xsk_fd);
        s->xsk_fd = -1;
        return (e == EPERM || e == ENOMEM) ? WEFT_CLUSTER_E_PERMS
                                           : WEFT_CLUSTER_E_IO;
    }
umem_ok:
    s->region = *r;   // borrowed view

    // 5. ring sizes + mmap (fixed uapi pgoffs — the xdp_rx finding)
    const size_t page = 4096;
    s->ring_len_fr = (size_t)cfg->fill_depth * sizeof(uint64_t) + 2 * page;
    s->ring_len_cr = (size_t)cfg->comp_depth * sizeof(uint64_t) + 2 * page;
    s->ring_len_rx = (size_t)cfg->rx_depth * sizeof(struct xdp_desc) +
                     2 * page;

    void* fr = mmap(NULL, s->ring_len_fr, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE, s->xsk_fd,
                    XDP_UMEM_PGOFF_FILL_RING);
    void* cr = mmap(NULL, s->ring_len_cr, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE, s->xsk_fd,
                    XDP_UMEM_PGOFF_COMPLETION_RING);
    void* rx = mmap(NULL, s->ring_len_rx, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE, s->xsk_fd, XDP_PGOFF_RX_RING);
    if (fr == MAP_FAILED || cr == MAP_FAILED || rx == MAP_FAILED) {
        s->stats.last_errno = errno;
        snprintf(s->err, sizeof(s->err),
                 "xdp: ring mmap refused: %s", strerror(errno));
        if (fr != MAP_FAILED) munmap(fr, s->ring_len_fr);
        if (cr != MAP_FAILED) munmap(cr, s->ring_len_cr);
        if (rx != MAP_FAILED) munmap(rx, s->ring_len_rx);
        close(s->xsk_fd);
        s->xsk_fd = -1;
        return WEFT_CLUSTER_E_IO;
    }
    // ring layout: [pad page][producer u32][consumer u32][flags u32]
    // [desc array] — producer/consumer/flags on the second page.
    s->fr_prod = (uint32_t*)((char*)fr + page);
    s->fr_cons = s->fr_prod + 1;
    s->fr_desc = (uint64_t*)((char*)fr + 2 * page);
    s->cr_prod = (uint32_t*)((char*)cr + page);
    s->cr_cons = s->cr_prod + 1;
    s->cr_desc = (uint64_t*)((char*)cr + 2 * page);
    s->rx_prod = (uint32_t*)((char*)rx + page);
    s->rx_cons = s->rx_prod + 1;
    s->rx_flags = s->rx_prod + 2;
    s->rx_desc = (uint64_t*)((char*)rx + 2 * page);  // xdp_desc[] via cast
    s->ring_mask = cfg->rx_depth - 1;

    // 6. bind to the interface + queue (NEED_WAKEUP: the kernel asks
    //    for a kick only when it matters — bounded by design)
    s->ifindex = if_nametoindex(cfg->ifname ? cfg->ifname : "lo");
    if (s->ifindex == 0) {
        snprintf(s->err, sizeof(s->err), "xdp: interface '%s' absent",
                 cfg->ifname ? cfg->ifname : "lo");
        weft_xdp_driver_shutdown(s);
        return WEFT_CLUSTER_E_INVALID_ARG;
    }
    struct sockaddr_xdp sa;
    memset(&sa, 0, sizeof(sa));
    sa.sxdp_family = AF_XDP;
    sa.sxdp_ifindex = s->ifindex;
    sa.sxdp_queue_id = cfg->queue_id;
    sa.sxdp_flags = XDP_USE_NEED_WAKEUP | XDP_ZEROCOPY;
    if (bind(s->xsk_fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        const int e = errno;
        s->stats.last_errno = e;
        // XDP_ZEROCOPY needs driver support; COPY is the honest
        // [FALLBACK-COPY] rung (one copy, still stack-bypassed).
        if (e == EINVAL) {
            sa.sxdp_flags = XDP_USE_NEED_WAKEUP | XDP_COPY;
            if (bind(s->xsk_fd, (struct sockaddr*)&sa, sizeof(sa)) == 0) {
                s->copy_mode = 1;
                goto bound;
            }
        }
        snprintf(s->err, sizeof(s->err), "xdp: bind(if %s q %u) refused: "
                 "%s", cfg->ifname ? cfg->ifname : "lo", cfg->queue_id,
                 strerror(errno));
        weft_xdp_driver_shutdown(s);
        return (e == EPERM) ? WEFT_CLUSTER_E_PERMS : WEFT_CLUSTER_E_IO;
    }
bound:
    // 7. free-chunk stack (the recycle currency — Law 1)
    s->free_stack = (uint32_t*)malloc(sizeof(uint32_t) * r->chunk_count);
    if (!s->free_stack) {
        weft_xdp_driver_shutdown(s);
        return WEFT_CLUSTER_E_NO_MEMORY;
    }
    for (uint32_t k = 0; k < r->chunk_count; k++) {
        s->free_stack[k] = r->chunk_count - 1 - k;
    }
    s->free_count = r->chunk_count;

    s->cap = WEFT_XDP_CAP_SETUP;
    s->err[0] = '\0';
    return WEFT_CLUSTER_OK;
}

// ---------------------------------------------------------------------------
// filter load + attach
// ---------------------------------------------------------------------------

weft_cluster_status_t weft_xdp_load_and_attach(weft_xdp_socket_t* s) {
    if (!s || s->xsk_fd < 0) {
        if (s) snprintf(s->err, sizeof(s->err), "xdp: attach before init");
        return WEFT_CLUSTER_E_STATE;
    }

    // map create (also the caps tripwire for unprivileged runners)
    s->map_fd = bpf_map_create_xskmap(64, s->err, sizeof(s->err));
    if (s->map_fd < 0) {
        s->stats.last_errno = errno;
        return (errno == EPERM) ? WEFT_CLUSTER_E_PERMS : WEFT_CLUSTER_E_IO;
    }

    // assemble + load the filter
    struct bpf_insn insns[64];
    const int cnt = weft_xdp_build_filter(&s->cfg, insns, 64, s->map_fd);
    if (cnt < 0) {
        snprintf(s->err, sizeof(s->err),
                 "xdp: filter assembly refused (u31/config law)");
        return WEFT_CLUSTER_E_INVALID_ARG;
    }
    s->prog_fd = bpf_prog_load_xdp(insns, (uint32_t)cnt, s->err,
                                   sizeof(s->err));
    if (s->prog_fd < 0) {
        s->stats.last_errno = errno;
        return (errno == EPERM) ? WEFT_CLUSTER_E_PERMS : WEFT_CLUSTER_E_IO;
    }

    // steering: queue -> socket fd
    const uint32_t key = s->cfg.queue_id;
    const uint32_t val = (uint32_t)s->xsk_fd;
    if (bpf_map_update(s->map_fd, &key, &val, BPF_ANY, s->err,
                       sizeof(s->err)) != 0) {
        s->stats.last_errno = errno;
        return WEFT_CLUSTER_E_IO;
    }

    // attach (bpf_link road; manual-ip fallback named in the refusal)
    s->link_fd = bpf_link_create_xdp(s->prog_fd, s->ifindex,
                                     s->cfg.xdp_flags, s->err,
                                     sizeof(s->err));
    if (s->link_fd < 0) {
        s->stats.last_errno = errno;
        return (errno == EPERM) ? WEFT_CLUSTER_E_PERMS
                                : WEFT_CLUSTER_E_UNSUPPORTED;
    }

    s->cap = WEFT_XDP_CAP_LIVE;
    s->err[0] = '\0';
    return WEFT_CLUSTER_OK;
}

// ---------------------------------------------------------------------------
// fill / rx / recycle (the data path — zero allocations)
// ---------------------------------------------------------------------------

weft_cluster_status_t weft_xdp_populate_fill(weft_xdp_socket_t* s,
                                             uint32_t n) {
    if (!s || s->xsk_fd < 0) return WEFT_CLUSTER_E_STATE;
    if (n > s->free_count) n = s->free_count;

    const uint32_t depth = s->cfg.fill_depth;
    uint32_t prod = ring_ld_acq(s->fr_prod);
    const uint32_t cons = ring_ld_acq(s->fr_cons);
    const uint32_t used = prod - cons;
    if (used >= depth) return WEFT_CLUSTER_E_BUSY;
    uint32_t space = depth - used;
    if (n > space) n = space;

    for (uint32_t i = 0; i < n; i++) {
        const uint32_t chunk = s->free_stack[--s->free_count];
        // page-aligned UMEM offset; UNALIGNED encoding degenerates to
        // the raw offset when the in-page part is zero (our placement
        // law guarantees it)
        s->fr_desc[prod & (depth - 1)] =
            weft_wcr1_chunk_offset(&s->region, chunk);
        prod++;
        s->stats.fills++;
    }
    ring_st_rel(s->fr_prod, prod);
    return (n > 0) ? WEFT_CLUSTER_OK : WEFT_CLUSTER_E_BUSY;
}

weft_cluster_status_t weft_xdp_rx(weft_xdp_socket_t* s,
                                  uint64_t timeout_ns,
                                  weft_xdp_rx_ent_t* out, uint32_t cap,
                                  uint32_t* out_n) {
    if (!s || !out || !out_n) return WEFT_CLUSTER_E_INVALID_ARG;
    if (s->xsk_fd < 0) {
        snprintf(s->err, sizeof(s->err), "xdp: rx before init");
        return WEFT_CLUSTER_E_STATE;
    }
    *out_n = 0;
    const uint64_t deadline = weft_deadline_after(timeout_ns);
    const uint32_t depth = s->cfg.rx_depth;

    for (;;) {
        const uint32_t prod = ring_ld_acq(s->rx_prod);
        uint32_t cons = ring_ld_acq(s->rx_cons);
        const uint32_t avail = prod - cons;

        for (uint32_t i = 0; i < avail && *out_n < cap; i++, cons++) {
            const struct xdp_desc* d =
                (const struct xdp_desc*)&s->rx_desc[cons & (depth - 1)];
            s->stats.rx_descs++;

            // placement decode: the descriptor's addr IS the fill addr
            // we posted (page-aligned chunk offset)
            const uint64_t off = d->addr & 0x0000ffffffffffffull;
            if (off < s->region.chunk0_offset ||
                (off - s->region.chunk0_offset) % s->region.chunk_size ||
                (off - s->region.chunk0_offset) / s->region.chunk_size >=
                    s->region.chunk_count) {
                // foreign/stale address: count and drop (bounded, named)
                s->stats.refused_frames++;
                continue;
            }
            const uint32_t chunk = (uint32_t)((off -
                s->region.chunk0_offset) / s->region.chunk_size);

            // WCF1 validation (defense in depth — the filter already
            // matched; this is the second opinion + the exact-length law)
            weft_wcf1_t* hdr = (weft_wcf1_t*)weft_wcr1_chunk(&s->region,
                                                             chunk);
            const weft_wcf1_refusal_t rr = weft_wcf1_validate(
                hdr, d->len, s->cfg.cluster_id, s->cfg.schema_id,
                s->region.chunk_size, NULL, NULL);
            if (rr != WEFT_WCF1_OK ||
                d->len != WEFT_WCF1_HDR_BYTES + hdr->payload_len) {
                s->stats.refused_frames++;
                // refused: return the chunk to the fill currency
                if (s->free_count < s->region.chunk_count) {
                    s->free_stack[s->free_count++] = chunk;
                }
                continue;
            }
            out[*out_n].chunk_idx = chunk;
            out[*out_n].hdr = hdr;
            out[*out_n].payload = (const uint8_t*)hdr + WEFT_WCF1_HDR_BYTES;
            out[*out_n].len = d->len - WEFT_WCF1_HDR_BYTES;
            (*out_n)++;
            s->stats.steered++;
        }
        ring_st_rel(s->rx_cons, cons);

        if (*out_n > 0) return WEFT_CLUSTER_OK;
        if (weft_deadline_remaining(deadline) == 0) {
            return WEFT_CLUSTER_E_TIMEOUT;
        }
        // need_wakeup: the kernel asks for a kick via recvfrom(0)
        if (s->rx_flags && (ring_ld_acq(s->rx_flags) & XDP_NEED_WAKEUP)) {
            recvfrom(s->xsk_fd, NULL, 0, MSG_DONTWAIT, NULL, NULL);
            s->stats.wakeups++;
        }
        if (weft_deadline_remaining(deadline) == 0) {
            return WEFT_CLUSTER_E_TIMEOUT;
        }
        weft_poll_yield();
    }
}

weft_cluster_status_t weft_xdp_recycle(weft_xdp_socket_t* s,
                                       const uint32_t* chunks, uint32_t n) {
    if (!s || !chunks) return WEFT_CLUSTER_E_INVALID_ARG;
    if (s->xsk_fd < 0) return WEFT_CLUSTER_E_STATE;
    for (uint32_t i = 0; i < n; i++) {
        if (chunks[i] >= s->region.chunk_count) {
            return WEFT_CLUSTER_E_INVALID_ARG;
        }
        if (s->free_count >= s->region.chunk_count) {
            return WEFT_CLUSTER_E_BUSY;  // double recycle — bounded refuse
        }
        s->free_stack[s->free_count++] = chunks[i];
        s->stats.recycled++;
    }
    return weft_xdp_populate_fill(s, n);
}

void weft_xdp_driver_shutdown(weft_xdp_socket_t* s) {
    if (!s) return;
    if (s->link_fd >= 0) close(s->link_fd);
    if (s->prog_fd >= 0) close(s->prog_fd);
    if (s->map_fd >= 0) close(s->map_fd);
    if (s->fr_desc) munmap((void*)((char*)s->fr_desc - 2 * 4096),
                           s->ring_len_fr);
    if (s->cr_desc) munmap((void*)((char*)s->cr_desc - 2 * 4096),
                           s->ring_len_cr);
    if (s->rx_desc) munmap((void*)((char*)s->rx_desc - 2 * 4096),
                           s->ring_len_rx);
    if (s->xsk_fd >= 0) close(s->xsk_fd);
    free(s->free_stack);
    memset(s, 0, sizeof(*s));
    s->xsk_fd = s->map_fd = s->prog_fd = s->link_fd = -1;
}

const weft_xdp_stats_t* weft_xdp_stats(const weft_xdp_socket_t* s) {
    return s ? &s->stats : NULL;
}

const char* weft_xdp_last_error(const weft_xdp_socket_t* s) {
    return s ? s->err : "null ctx";
}
