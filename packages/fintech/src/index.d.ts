// Type declarations for @weft/fintech (Pillar 6 deliverable A).

export declare const E_TRUNC = 1;
export declare const T_SYSTEM = 0x53, T_ADD = 0x41, T_ADD_MPID = 0x46,
  T_EXEC = 0x45, T_EXEC_PRICE = 0x43, T_CANCEL = 0x58, T_DELETE = 0x44,
  T_REPLACE = 0x55, T_TRADE = 0x50;

export declare const OK = 0, E_DUP_REF = 1, E_UNKNOWN_REF = 2,
  E_POOL_FULL = 3, E_PRICE_RANGE = 4, E_BAD_SIZE = 5;

export declare class ItchView {
  constructor(buffer: ArrayBuffer | DataView);
  bind(off: number): this;
  readonly type: number;
  readonly locate: number;
  readonly tracking: number;
  readonly ts: number;
  readonly refLo: number;
  readonly refHi: number;
  readonly ref2Lo: number;
  readonly ref2Hi: number;
  readonly side: number;
  readonly shares: number;
  readonly price: number;
  readonly match: number;
  eventCode(): string;
  stockInto(u8scratch: Uint8Array): Uint8Array;
}

export declare class OrderBook {
  constructor(opts?: {
    poolCapacity?: number;
    baseTick?: number;
    tickCount?: number;
    topLevels?: number;
  });
  readonly msgsApplied: number;
  readonly skipped: number;
  readonly liveOrders: number;
  readonly tradeCount: number;
  readonly lastTs: number;
  readonly lastMatch: number;
  readonly lastExecPrice: number;
  readonly rejects: Uint32Array;
  applyView(view: ItchView): boolean;
  bestBid(): number;
  bestAsk(): number;
  totalRejects(): number;
  topLevelsOf(side: 0 | 1, out: Int32Array): number;
}

export declare class ItchEngine {
  constructor(book: OrderBook, opts?: { buffer?: ArrayBuffer; clock?: () => number });
  readonly truncated: number;
  readonly bytesProcessed: number;
  readonly parseNs: number;
  process(buffer: ArrayBuffer | DataView, byteLength: number): number;
}

export declare const MDP1_SIZE = 304, MDP1_TOP_LEVELS = 10;
export declare const F_BOOK_VALID = 1, F_CROSSED = 2, F_LOCKED = 4;

export declare function packSnapshot(
  book: OrderBook,
  out: Uint8Array | DataView,
  scratchBids: Int32Array,
  scratchAsks: Int32Array,
): Uint8Array | DataView;
export declare function mdp1Crc32(u8: Uint8Array, start?: number, end?: number): number;

export declare class Mdp1View {
  constructor(buffer: ArrayBuffer | DataView);
  valid(): boolean;
  crcOk(): boolean;
  readonly version: number;
  readonly flags: number;
  readonly seq: number;
  readonly lastTs: number;
  readonly bestBid: number;
  readonly bestAsk: number;
  bidPrice(i: number): number;
  bidSize(i: number): number;
  bidOrders(i: number): number;
  askPrice(i: number): number;
  askSize(i: number): number;
  askOrders(i: number): number;
  readonly msgCount: number;
  readonly tradeCount: number;
}

export declare class SbeDecoder {
  constructor(schema: object);
  readonly blockLength: number;
  readonly templateId: number;
  readonly truncated: number;
  readonly skipped: number;
  process(
    buffer: ArrayBuffer | DataView,
    byteLength: number,
    visit: (templateId: number, vector: Float64Array, fieldCount: number) => void,
  ): number;
}

export declare class MarketTelemetry {
  readonly slots: Float64Array;
  readonly msgsTotal: number;
  readonly bytesTotal: number;
  readonly rateEwma: number;
  readonly latEwmaNs: number;
  record(bytes: number, nowNs: number): void;
  recordLatency(ns: number): void;
  percentiles(): [number, number, number];
}

export declare const DEFAULT_THEME: Record<string, string>;

export declare function createOrderBookController(opts: {
  mdp1: ArrayBuffer | DataView | Mdp1View;
  ctx?: CanvasRenderingContext2D | null;
  telemetry?: MarketTelemetry | null;
  clock?: () => number;
  width?: number;
  height?: number;
  rows?: number;
  theme?: Record<string, string>;
  frameRate?: number;
  frameBudgetNs?: number;
  drawLabels?: boolean;
}): {
  state: {
    frames: number; drops: number; torn: number; fallback: boolean;
    lastCostNs: number; maxCostNs: number; drawCalls: number;
  };
  mdp1: Mdp1View;
  render(nowNs: number): boolean;
  degrade(reason?: string): string;
  restore(): boolean;
};

export declare function bindOrderBookCanvas(
  canvas: HTMLCanvasElement,
  controller: ReturnType<typeof createOrderBookController>,
  opts?: { frameRate?: number; clock?: () => number },
): () => void;

export declare function createWeftOrderBook(React: object): (props: object) => object;
export declare function createWeftTelemetryBar(React: object): (props: object) => object;
