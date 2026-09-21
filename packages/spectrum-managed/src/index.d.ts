// index.d.ts — @weft/spectrum API surface (Pillar 5).
// Normative SHP1 layout: docs/spectrum/SPECTRUM-WIRE-V1.md

/** §6 Law 4 error taxonomy (frozen codes, cross-language) */
export const E_BAD_MAGIC: 1; export const E_BAD_VERSION: 2; export const E_BAD_SIZE: 3;
export const E_CRC_MISMATCH: 4; export const E_RESERVED_DIRTY: 5;
export const E_PROBE_UNAVAILABLE: 6; export const E_DEVICE_LOST: 7; export const E_FFI_TIMEOUT: 8;
export const E_HEAP_PRESSURE: 9; export const E_LISTENER_LEAK: 10; export const E_ALIGN_INVALID: 11;
export const E_HUD_CONTEXT_LOST: 12; export const E_UNMARSHAL_FAILED: 13;
export const E_TIER_EXHAUSTED: 14; export const E_HUD_RECOVERED: 15;

export const RECORD_SIZE: 192;
export const CRC_OFFSET: 188;

export const FEAT: {
  readonly WASM_SIMD128: 0; readonly SHARED_ARRAY_BUFFER: 1; readonly WEBGPU: 2;
  readonly WEBGL2: 3; readonly AVX512: 4; readonly AVX2: 5; readonly SSE42: 6;
  readonly NEON: 7; readonly SVE2: 8; readonly RVV: 9; readonly METAL_3: 10;
  readonly CUDA: 11; readonly APPLE_MPS: 12; readonly OPENVINO: 13;
  readonly FASTRPC_DSP: 14; readonly NEUROPILOT: 15; readonly MULTILANE_DMA: 16;
  readonly BIG_LITTLE: 17; readonly THERMAL_SENSOR: 18; readonly DLPACK_EXPORT: 19;
};

/** Zero-copy window over one 192-byte SHP1 record. Getters are primitive reads. */
export class ProfileView {
  constructor(buffer: ArrayBuffer | DataView, byteOffset?: number);
  validate(): number; // 0 = intact, else §6 code
  readonly layoutVersion: number; readonly recordSize: number;
  readonly featureFlagsLo: number; readonly featureFlagsHi: number;
  readonly siliconTier: number; readonly thermalState: number;
  readonly perfCores: number; readonly effCores: number; readonly gpuFamily: number;
  readonly cacheLineBytes: number; readonly cpuMaxClockKhz: number;
  readonly memoryTotalBytes: number; readonly memoryBudgetBytes: number;
  readonly simdWidthBits: number; readonly frameBudgetUs: number;
  readonly maxFrameRateMilliHz: number; readonly batteryPermille: number;
  readonly batteryCharging: number; readonly visibility: number;
  readonly dmaLaneCount: number; readonly vendorId: number; readonly deviceId: number;
  readonly crc32: number;
  hasFeatureBit(bit: number): boolean;
  featureName(bit: number): string | null;
  u64(field: 'cpuMaxClockKhz' | 'memoryTotalBytes' | 'memoryBudgetBytes' | 'maxFrameRateMilliHz'): number;
  snapshotInto(dst: ProfileFlyweight): ProfileFlyweight;
}

export interface ProfileFlyweight {
  layoutVersion: number; recordSize: number; featureFlagsLo: number; featureFlagsHi: number;
  siliconTier: number; thermalState: number; perfCores: number; effCores: number;
  gpuFamily: number; cacheLineBytes: number; cpuMaxClockKhz: number;
  memoryTotalBytes: number; memoryBudgetBytes: number; simdWidthBits: number;
  frameBudgetUs: number; maxFrameRateMilliHz: number; batteryPermille: number;
  batteryCharging: number; visibility: number; dmaLaneCount: number;
  vendorId: number; deviceId: number;
}
export function makeProfileFlyweight(): ProfileFlyweight;
export function decodeProfile(buffer: ArrayBuffer | DataView, byteOffset?: number):
  { ok: boolean; code: number; name: string | null; view: ProfileView };

/** Init-time detection (one-shot; the telemetry loop afterwards only reads). */
export function detectWasmSimd128(): boolean;
export function detectSharedArrayBuffer(): boolean;
export function detectWebGPU(navigatorLike?: Navigator): boolean;
export function confirmWebGPUAdapter(navigatorLike?: Navigator): Promise<boolean>;
export function detectWebGL2(doc?: Document): boolean;
export function detectWorkerCount(navigatorLike?: Navigator): number;
export function simdWidthFromFeatures(flagsLo: number): number;
export function detectInto(dst: ProfileFlyweight, opts?: object): ProfileFlyweight;
export function makeFallbackProfile(opts?: object): ProfileFlyweight;

/** Battery + Page Visibility wiring. Every handler writes primitives only. */
export interface PowerHandle {
  readonly state: ProfileFlyweight;
  readonly leaks: { count: number };
  liveCount(): number;
  detach(): void;
}
export function attachPowerSources(state: ProfileFlyweight, navigatorLike?: Navigator, doc?: Document): PowerHandle;

/** Reactive Cadence Governor (normative §4 integer state machine). */
export const CADENCE_LADDER: readonly [240, 120, 60, 30];
export const SUSTAINED_TICKS: 10;
export const RECOVERY_TICKS: 50;
export const BACKGROUND_CAP: 30;
export const LOW_BATTERY_PERMILLE: 150;
export const MAX_TIER_STAGES: 2;
export interface CadenceState {
  rung: number; tierStage: number; severeStreak: number;
  moderateStreak: number; coolStreak: number;
  capHz: number; effTier: number; budgetBytes: number;
}
export interface GovernorInput {
  thermalState: number; batteryPermille: number; batteryCharging: number;
  visibility: number; heapPressure: 0 | 1; tierMaxHz?: number;
  profileBudgetBytes?: number;
}
export function createCadenceState(): CadenceState;
export function cadenceTick(st: CadenceState, inp: GovernorInput): number;
export function tierTick(st: CadenceState, inp: GovernorInput): number;
export function effectiveBudgetBytes(profileBudget: number, tierStage: number): number;

/** <WeftSpectrumHud /> — zero-re-render universal telemetry overlay. */
export function createWeftSpectrumHud(React: object): React.ComponentType<WeftSpectrumHudProps>;
export interface WeftSpectrumHudProps {
  state: ProfileFlyweight;
  gov?: CadenceState;
  metrics?: { fps: number; jitterP99Us: number; jitterP999Us: number; memBytes: number };
  pollMs?: number;
  doc?: Document;
  label?: string;
}
export function buildSpectrumHudUi(host: object, doc: object): object;
export function paintSpectrumHud(ui: object, props: WeftSpectrumHudProps): number;
