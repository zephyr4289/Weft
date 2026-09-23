// @weft/heddle-core — framework-agnostic HPL1 Hot-Plane engine.
// Normative layout: docs/heddle2/HPL1-LAYOUT-V1.md
export { HPL1, HPL1_NAME, Hpl1Error } from './errors.js';
export {
  HEADER_SIZE, LANE_CTRL_STRIDE, MAGIC_U32, MAGIC_BYTES, VERSION,
  FLAG_LE_REQUIRED, FLAG_EPOCH_STABLE, LANE_FLAG_ACTIVE, LANE_FLAG_MANUAL,
  MAX_LANES, HDR, LANE, align8, deriveGeometry, isPow2, validatePlane, initHeader,
} from './layout.js';
export { HotPlaneView, makeLaneOut, makeHeaderOut } from './plane.js';
export { HotPlaneProducer } from './producer.js';
export { FrameScheduler } from './scheduler.js';
