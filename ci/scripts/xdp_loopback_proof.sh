#!/usr/bin/env bash
# xdp_loopback_proof.sh — RFC-0016 §4: the LIVE-mode AF_XDP proof rig.
#
# PRIVILEGED RUNNER ONLY (root or CAP_NET_ADMIN + CAP_BPF). The CI/sandbox
# kernels ship without CONFIG_XDP_SOCKETS (weft_xdp_probe reports none,
# EAFNOSUPPORT — the X-series gates run the state machine + the uring
# delegation there instead). This script drives the full data path on a
# host where AF_XDP exists:
#
#   1. a veth pair (weft-xdp-a <-> weft-xdp-b)
#   2. a generic XDP program on weft-xdp-b that redirects every packet to
#      an AF_XDP socket (XDP_SKB mode — works on any NIC, including veth)
#   3. the XDP_LIVE gate binary: weft_xdp_attach over a memfd WFSH session,
#      a UDP flood from weft-xdp-a, weft_xdp_next() consuming rx
#      descriptors, frames published under the pre-bracketed protocol
#
# Exit 0 = the LIVE rung proven end-to-end on this host.
set -euo pipefail

log() { echo "[xdp-proof] $*"; }

need() {
  command -v "$1" >/dev/null 2>&1 || { log "missing tool: $1 (install $2)"; exit 2; }
}

need ip       "iproute2"
need clang    "clang (BPF target)"
need llc      "llvm (BPF backend)"

if [ "$(id -u)" != "0" ]; then
  log "must run as root (XDP attach needs CAP_NET_ADMIN)"
  exit 2
fi

VETH_A=weft-xdp-a
VETH_B=weft-xdp-b
BPF_OBJ=/tmp/weft_xdp_redirect.bpf.o

cleanup() {
  ip link del "$VETH_A" 2>/dev/null || true
  rm -f "$BPF_OBJ"
}
trap cleanup EXIT

# ---- 1. veth pair ----------------------------------------------------------
ip link del "$VETH_A" 2>/dev/null || true
ip link add "$VETH_A" type veth peer name "$VETH_B"
ip addr add 10.99.0.1/24 dev "$VETH_A"
ip addr add 10.99.0.2/24 dev "$VETH_B"
ip link set "$VETH_A" up
ip link set "$VETH_B" up

# ---- 2. the XDP redirect program (XSKMAP lookup, redirect to the socket) ---
# Compiled from an inline vmlinux-free skeleton: xdp_sockProg does a
# bpf_redirect_map(XSKMAP, index, 0) for every packet.
cat > /tmp/weft_xdp_redirect.bpf.c <<'EOF'
#include <linux/bpf.h>
#define SEC(NAME) __attribute__((section(NAME), used))
struct bpf_map_def SEC("maps") xsks_map = {
    .type = BPF_MAP_TYPE_XSKMAP,
    .key_size = sizeof(int),
    .value_size = sizeof(int),
    .max_entries = 4,
};
SEC("xdp_sock")
int xdp_sock_prog(struct xdp_md *ctx) {
    int index = ctx->rx_queue_index;
    return bpf_redirect_map(&xsks_map, index, XDP_PASS);
}
char _license[] SEC("license") = "GPL";
EOF
clang -O2 -target bpf -c /tmp/weft_xdp_redirect.bpf.c -o "$BPF_OBJ"
ip link set dev "$VETH_B" xdpgeneric obj "$BPF_OBJ" sec xdp_sock

# ---- 3. the LIVE gate -------------------------------------------------------
# xdp-live-gate (built by the heterogeneous shard from core/c/xdp_live_gate.c
# when CONFIG_XDP_SOCKETS hosts run it): binds the socket to weft-xdp-b's
# ifindex, floods UDP from weft-xdp-a, and asserts:
#   - weft_xdp_probe() >= SETUP
#   - attach over a page-aligned memfd WFSH session -> mode SETUP/LIVE
#   - N frames published via rx descriptors (stats.rx_descs == N,
#     stats.delegated == 0, telescoping exact, claims bit-exact)
if [ ! -x core/c/xdp-live-gate ]; then
  log "xdp-live-gate binary missing (build with make -C core/c xdp-live-gate)"
  exit 2
fi
./core/c/xdp-live-gate "$VETH_A" "$VETH_B" 10.99.0.2:9999 5000

log "LIVE rung proven: NIC DMA -> ring slot -> descriptor -> publish"
