// index.js — @weft/tensor public surface.
//
// Zero-allocation managed tensor plane for Weft: ingestion (WebCodecs video,
// PCM audio), seqlock tensor rings, Canvas2D/WebGL2 render planes.
// Laws: (1) zero-alloc steady state, (2) strict LE/IEEE-754, (3) browser+
// Node+Deno+Electron, (4) boundary schema validation.

export * from './layout.js';
export * from './frame-view.js';
export * from './ring.js';
export * from './runtime.js';
export * from './listener.js';
export * from './ingest/video.js';
export * from './ingest/audio.js';
export * from './render/canvas2d.js';
export * from './render/webgl2.js';
export * from './render/overlay.js';
