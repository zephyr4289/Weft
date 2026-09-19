#!/usr/bin/env bash
# build.sh — compile the Weft C core to WebAssembly (issue #18-1).
#
# Flavors:
#   ./build.sh              single-threaded (default; runs in every browser
#                           and node without COOP/COEP)
#   ./build.sh pthreads     -pthread flavor (SharedArrayBuffer; serve the
#                           host with COOP/COEP headers — same requirements
#                           as the TS SAB port)
#
# Output: core/wasm/dist/{libweft.js, libweft.wasm}  (commit-averse: the
# repo does not track built binaries; CI and consumers build from source).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE="$HERE/../c"
OUT="$HERE/dist"
mkdir -p "$OUT"

FLAVOR="${1:-single}"

SOURCES="$CORE/weft.c $CORE/fanout.c $CORE/fanout_simd.c $CORE/fanout_batch.c $HERE/weft_wasm_glue.c"

EXPORTS="_wweft_new,_wweft_free,_wweft_publish,_wweft_claim_seq,_wweft_payload_ptr,_wweft_payload_len,_wweft_epoch,_wweft_revoke,_wweft_reclaim,_wweft_t_publish,_wweft_t_claim,_wweft_t_drop,_wweft_t_wsteps,_wweft_t_rsteps,_wfan_new,_wfan_free,_wfan_publish,_wfan_publish_batch,_wfan_ring,_wfan_ring_bytes,_wfr_new,_wfr_free,_wfr_claim,_wfr_view,_malloc,_free"

COMMON_FLAGS="-O2 -std=c11 -D_GNU_SOURCE -I$CORE \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=16MB \
  -s MAXIMUM_MEMORY=256MB \
  -s EXPORTED_FUNCTIONS=$EXPORTS \
  -s EXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32 \
  -s MODULARIZE=1 -s EXPORT_NAME=initWeftWasm \
  -s EXPORT_ES6=1 \
  -s ENVIRONMENT=web,node,worker"

case "$FLAVOR" in
  single)
    emcc $COMMON_FLAGS -o "$OUT/libweft.js" $SOURCES
    ;;
  pthreads)
    # SAB flavor: COOP/COEP required on the serving host.
    emcc $COMMON_FLAGS -pthread -s PTHREAD_POOL_SIZE=4 \
      -o "$OUT/libweft-pthreads.js" $SOURCES
    ;;
  *)
    echo "usage: $0 [single|pthreads]" >&2; exit 2 ;;
esac

ls -la "$OUT"
echo "OK ($FLAVOR)"
