// @weft/fintech — zero-copy market-data engine + managed UI bindings.
//
// Exports the managed FinTech surface (Pillar 6 deliverable A):
//   ItchEngine / ItchView — ITCH 5.0 flyweight parser (big-endian wire)
//   OrderBook             — O(1) ring-allocated L2/L3 book
//   packSnapshot / Mdp1View — MDP1 snapshot wire (parity + UI binding)
//   SbeDecoder            — schema-driven SBE flyweight decoder
//   MarketTelemetry       — zero-allocation msgs/sec + latency stats
//   createOrderBookCanvas — zero-re-render 240 FPS depth-ladder renderer

export * from './itch.js';
export * from './book.js';
export * from './mdp1.js';
export * from './sbe.js';
export * from './telemetry.js';
export * from './orderbook-canvas.js';
