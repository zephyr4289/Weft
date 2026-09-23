// weft_xdp_filter.c — RFC-0019 §4.2: the kernel-side XDP ingestion filter
// (REFERENCE SOURCE — compiled with clang -target bpf; see the shard's
// bpf leg and backends/xdp/weft_xdp_driver.c for the embedded-bytecode
// road that carries the same decisions without a BPF toolchain).
//
// WHY EXISTS: the cluster's ingress must classify .weft frames AT LINE
// RATE — before the Linux TCP/IP stack, before a single syscall, inside
// the NIC driver's RX path. An XDP program does exactly that: the kernel
// calls this function per packet with the packet bytes still in the NIC
// driver's page-fragment memory; matching frames are steered straight
// into the node's WCR1 UMEM (bpf_redirect_map on the xsks_map), and
// everything else continues its normal journey (XDP_PASS — the slow
// path is not harmed, it is the fallback).
//
// THE PARSER IS A MIRROR (the load-bearing invariant): this filter, the
// embedded bytecode the user-space driver assembles, and the
// user-space validator weft_wcf1_validate() must agree EXACTLY — same
// offsets, same rungs, same constants. The CL-X golden vectors drive
// the user-space pair on every CI run; the compiled-object identity of
// this source is the deployment-side leg (D-32 hardware checklist).
// The fast path deliberately handles ONLY: Eth/IPv4 (IHL=5, no frag)/
// UDP/dport/WCF1 magic+version+cluster+schema. Anything else — VLAN
// tags, IP options, fragments — PASSES to the stack, where the
// io_uring road ingests it correctly at a slower rate. Fast path
// narrowness is a FEATURE: every packet the filter is unsure about
// lands on the conservative road.
//
// BUILD (deployment; the sandbox has no BPF caps — the CL-X gates drive
// the embedded mirror instead):
//   clang -O2 -g -target bpf -c weft_xdp_filter.c -o weft_xdp_filter.o
//   # load via bpftool/ip on a CAP_NET_ADMIN/CAP_BPF runner:
//   ip link set dev eth0 xdp obj weft_xdp_filter.o sec xdp_weft
//
// LAW 1: the program allocates nothing (BPF cannot malloc — the
//        verifier enforces our law better than we do).
// LAW 2: bounded by construction — one linear pass, no loops.
// LAW 4: unknown/reserved bits PASS (conservative), never DROP silently.

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <stdint.h>

// ---- self-contained helper declarations (no libbpf dependency) ---------------

#define SEC(NAME) __attribute__((section(NAME), used))

static void* (*bpf_map_lookup_elem)(const void* map, const void* key) =
    (void*)1;  // BPF_FUNC_map_lookup_elem
static long (*bpf_redirect_map)(const void* map, uint32_t key,
                                uint64_t flags) = (void*)51;  // redirect_map

// The XSK steering map (queue_id -> xsk fd), populated by the user-space
// driver via bpf(BPF_MAP_UPDATE_ELEM) after socket bind.
struct {
    uint32_t type;        // BPF_MAP_TYPE_XSKMAP
    uint32_t key_size;    // 4
    uint32_t value_size;  // 4
    uint32_t max_entries; // 64
} xsks_map SEC(".maps") = {
    .type = BPF_MAP_TYPE_XSKMAP,
    .key_size = 4,
    .value_size = 4,
    .max_entries = 64,
};

// ---- WCF1 field offsets the filter matches (frozen; mirror of
//      weft_cluster_frame.h static asserts) -----------------------------------

#define WCF1_OFF_ETH_PROTO  12   // __be16 ethertype
#define WCF1_OFF_IP_IHL      14  // version/IHL byte
#define WCF1_OFF_IP_FRAG     20  // flags:frag_off (BE)
#define WCF1_OFF_IP_PROTO    23  // 17 = UDP
#define WCF1_OFF_UDP_DPORT   36  // 34 + 2, BE
#define WCF1_OFF_PAYLOAD     42  // UDP payload = WCF1 header
#define WCF1_OFF_VERSION     46  // +4
#define WCF1_OFF_CLUSTER     50  // +8
#define WCF1_OFF_SCHEMA      54  // +12

#ifndef WEFT_XDP_CLUSTER_ID
#define WEFT_XDP_CLUSTER_ID 1u  // set at load site (or patch the mirror)
#endif
#ifndef WEFT_XDP_SCHEMA_ID
#define WEFT_XDP_SCHEMA_ID 0u   // 0 = any schema (block compiled out)
#endif
#ifndef WEFT_XDP_UDP_PORT
#define WEFT_XDP_UDP_PORT 47911u
#endif

#define XDP_PASS 2

SEC("xdp_weft")
int weft_xdp_ingress(struct xdp_md* ctx) {
    void* data = (void*)(long)ctx->data;
    void* data_end = (void*)(long)ctx->data_end;

    // Bounds: one linear check for eth+ip+udp+wcf1 (14+20+8+64 = 106).
    uint8_t* p = (uint8_t*)data;
    if (p + 106 > (uint8_t*)data_end) return XDP_PASS;

    // Ethertype IPv4.
    if (((struct ethhdr*)data)->h_proto != __constant_htons(ETH_P_IP))
        return XDP_PASS;

    // IHL == 5 (no options on the fast path; options go to the stack).
    uint8_t ihl = ((struct iphdr*)p)->ihl;
    if (ihl != 5) return XDP_PASS;

    // No fragmentation (offset or MF set -> stack reassembly road).
    uint16_t frag = ((struct iphdr*)p)->frag_off;
    if (frag & __constant_htons(0x3FFF)) return XDP_PASS;

    // UDP.
    if (((struct iphdr*)p)->protocol != IPPROTO_UDP) return XDP_PASS;

    // Cluster dport.
    if (((struct udphdr*)(p + 34))->dest !=
        __constant_htons(WEFT_XDP_UDP_PORT))
        return XDP_PASS;

    // WCF1 magic ".wft" + version.
    const uint8_t* w = p + WCF1_OFF_PAYLOAD;
    if (w[0] != '.' || w[1] != 'w' || w[2] != 'f' || w[3] != 't')
        return XDP_PASS;
    uint16_t ver = (uint16_t)(w[4] | (w[5] << 8));
    if (ver != 1) return XDP_PASS;

    // Cluster steering key.
    uint32_t cid = (uint32_t)w[8] | ((uint32_t)w[9] << 8) |
                   ((uint32_t)w[10] << 16) | ((uint32_t)w[11] << 24);
    if (cid != WEFT_XDP_CLUSTER_ID) return XDP_PASS;

#if WEFT_XDP_SCHEMA_ID != 0
    uint32_t sid = (uint32_t)w[12] | ((uint32_t)w[13] << 8) |
                   ((uint32_t)w[14] << 16) | ((uint32_t)w[15] << 24);
    if (sid != WEFT_XDP_SCHEMA_ID) return XDP_PASS;
#endif

    // Steer: XSKMAP lookup validates the queue has a bound socket, then
    // redirect_map hands the frame to AF_XDP — NIC DMA into WCR1 UMEM.
    uint32_t queue = ctx->rx_queue_index;
    if (!bpf_map_lookup_elem(&xsks_map, &queue)) return XDP_PASS;
    return (int)bpf_redirect_map(&xsks_map, queue, 0);
}

char _license[] SEC("license") = "GPL";
