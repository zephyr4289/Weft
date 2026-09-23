/* whp1_layout.h — the WHP1 (Weft Hot-Plane v1) memory contract, C twin.
 *
 * The byte-identical twin of packages/heddle-canvas/src/plane/whp1.ts
 * (RFC-0022 §3). test/layout_parity.test.ts parses THIS header and
 * asserts every number matches the TS constants — drift between the two
 * readings of the contract is a CI failure, not a debugging session.
 *
 * The native probe (vk_heddle_probe.c) builds a Hot-Plane-shaped host
 * buffer using these macros, binds it to a Vulkan compute pipeline
 * (shaders/vk/osc_decimate.comp) and reduces it against the C oracle —
 * proving the native Tier-1 road (direct storage-buffer compute over
 * producer memory) on a real ICD.
 *
 * LAW 2: 64-B plane header, 16 × 128-B lane descriptors at 0x80, data
 * region at the FIXED offset 0x880, all lanes 128-B aligned, little-endian.
 */
#ifndef WEFT_WHP1_LAYOUT_H
#define WEFT_WHP1_LAYOUT_H

#include <stdint.h>

/* Plane magic: bytes "WPL1" read as a little-endian u32. */
#define WHP1_MAGIC 0x314c5057u
/* Lane-descriptor magic: bytes "WNL1" read as a little-endian u32. */
#define WHP1_LANE_MAGIC 0x314c4e57u
#define WHP1_VERSION 1u
#define WHP1_HEADER_BYTES 64u
#define WHP1_MAX_LANES 16u
#define WHP1_LANE_TABLE 0x80u
#define WHP1_LANE_STRIDE_TABLE 128u
#define WHP1_DATA_START 0x880u
#define WHP1_MAX_LANE_CAPACITY (1u << 26)

/* Plane header offsets (bytes). */
#define WHP1_OFF_MAGIC 0x00u
#define WHP1_OFF_VERSION 0x04u
#define WHP1_OFF_HEADER_BYTES 0x08u
#define WHP1_OFF_LANE_COUNT 0x0cu
#define WHP1_OFF_EPOCH 0x10u          /* ATOMIC u32 liveness heartbeat */
#define WHP1_OFF_PRODUCER_SEQ 0x18u   /* ATOMIC u32 total publications */
#define WHP1_OFF_FLAGS 0x1cu          /* bit0 = teardown */
#define WHP1_OFF_DATA_START 0x20u     /* u64, must equal WHP1_DATA_START */
#define WHP1_OFF_PLANE_BYTES 0x28u    /* u64, full SAB size */

/* Lane descriptor offsets (bytes within one 128-B descriptor). */
#define WHP1_LANE_OFF_MAGIC 0x00u
#define WHP1_LANE_OFF_KIND 0x04u
#define WHP1_LANE_OFF_DTYPE 0x08u
#define WHP1_LANE_OFF_GRANULARITY 0x0cu /* elements per dirty bit */
#define WHP1_LANE_OFF_OFFSET 0x10u      /* u64 data offset (128-B aligned) */
#define WHP1_LANE_OFF_CAPACITY 0x18u    /* u64 element count */
#define WHP1_LANE_OFF_STRIDE 0x20u      /* u64 bytes per element */
#define WHP1_LANE_OFF_WRITE_POS 0x28u   /* ATOMIC u32 total published */
#define WHP1_LANE_OFF_SEQ 0x2cu         /* ATOMIC u32 publication seq */
#define WHP1_LANE_OFF_DIRTY_LO 0x30u    /* ATOMIC u32 dirty bits 0..31 */
#define WHP1_LANE_OFF_DIRTY_HI 0x34u    /* ATOMIC u32 dirty bits 32..63 */
#define WHP1_LANE_OFF_FLAGS 0x38u       /* bit0 = lane active */

/* Lane kinds (u32 at WHP1_LANE_OFF_KIND). */
#define HP_KIND_WAVEFORM_F32 0u        /* f32 samples, stride 4 */
#define HP_KIND_DEPTH_LADDER_F32 1u    /* price,size,side rows, stride 64 */
#define HP_KIND_CANDLE_OHLC_F32 2u     /* OHLC+volume rows, stride 64 */
#define HP_KIND_POINTCLOUD_QUAT_F32 3u /* pos+size,quat,rgba rows, stride 128 */

#define HP_DTYPE_F32 1u

static const uint32_t HP_KIND_STRIDE[4] = { 4u, 64u, 64u, 128u };

/* Ladder row words (u32 index within the 64-B row). */
#define HP_LADDER_W_PRICE 0
#define HP_LADDER_W_SIZE 1
#define HP_LADDER_W_SIDE 2
/* Candle row words. */
#define HP_CANDLE_W_OHLC 0  /* vec4: open, high, low, close */
#define HP_CANDLE_W_VOLUME 4
/* Point-cloud row words. */
#define HP_PC_W_POSSIZE 0 /* vec4: pos.xyz + size */
#define HP_PC_W_QUAT 4    /* vec4: xyzw */
#define HP_PC_W_COLOR 8   /* vec4: rgba */

#define WHP1_FLAG_TEARDOWN (1u << 0)
#define WHP1_LANE_FLAG_ACTIVE (1u << 0)

/* Plane total size for a lane set — mirrors whp1PlaneBytes() in TS. */
static inline uint64_t whp1_plane_bytes(const uint32_t *strides,
                                        const uint32_t *caps,
                                        uint32_t count) {
  uint64_t off = WHP1_DATA_START;
  for (uint32_t i = 0; i < count; i++) {
    off = (off + 127u) & ~127u;
    off += (uint64_t)strides[i] * caps[i];
  }
  return (off + 127u) & ~127u;
}

#endif /* WEFT_WHP1_LAYOUT_H */
