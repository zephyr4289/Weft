// detector.mjs — reference blob detector: real connected-component analysis
// on a preallocated downsample grid. Zero allocation per frame (Law 1):
// grid, labels, stack and the box scratch all live for the detector's life.
//
// This is a REAL detector (threshold + 4-neighbour flood fill + bbox
// extraction), not a mock — it finds the synthetic blobs and would find
// bright objects in a real luma plane too.

import { OverlayScratch } from '../../../packages/weft-tensor/src/index.js';

export class BlobDetector {
  /**
   * @param {object} opts { width, height, gridW, gridH, threshold }
   */
  constructor(opts = {}) {
    const {
      width = 160, height = 90,
      gridW = 40, gridH = 24,   // 4x4 downsample blocks
      threshold = 96,           // luma threshold (0..255)
      maxBoxes = 32,
    } = opts;
    this.width = width;
    this.height = height;
    this.gridW = gridW;
    this.gridH = gridH;
    this.threshold = threshold;
    this.grid = new Uint8Array(gridW * gridH);   // downsampled luma
    this.labels = new Int32Array(gridW * gridH); // component labels (0 = none)
    this.stack = new Int32Array(gridW * gridH);  // flood-fill stack
    this.scratch = new OverlayScratch(maxBoxes); // output boxes (reused)
    this.stats = { runs: 0, boxes: 0, lastUs: 0 };
  }

  /**
   * Detect on a ring frame view. Writes boxes into this.scratch (reused).
   * @returns {OverlayScratch}
   */
  detect(frameView) {
    const t0 = process.hrtime.bigint();
    const { gridW, gridH, grid, labels, stack, scratch } = this;
    const src = frameView.payloadView(); // preallocated per-slot view (zero-alloc)
    const W = this.width, H = this.height;
    const bx = Math.floor(W / gridW), by = Math.floor(H / gridH);

    // 1. Downsample luma (sample block centers — grid-cell cost, not pixel cost).
    scratch.clear();
    for (let gy = 0; gy < gridH; gy++) {
      const sy = (gy * by + (by >> 1)) * W;
      for (let gx = 0; gx < gridW; gx++) {
        const sx = gx * bx + (bx >> 1);
        const o = (sy + sx) * 4;
        // luma ~ (R*0.299 + G*0.587 + B*0.114) — integer approximation
        const l = (src[o] * 77 + src[o + 1] * 150 + src[o + 2] * 29) >> 8;
        grid[gy * gridW + gx] = l > this.threshold ? 1 : 0;
        labels[gy * gridW + gx] = 0;
      }
    }

    // 2. Connected components (4-neighbour, iterative flood fill).
    let next = 0;
    for (let i = 0; i < grid.length; i++) {
      if (grid[i] === 0 || labels[i] !== 0) continue;
      next++;
      let sp = 0;
      stack[sp++] = i;
      labels[i] = next;
      let minX = gridW, maxX = -1, minY = gridH, maxY = -1, size = 0, bright = 0;
      while (sp > 0) {
        const p = stack[--sp];
        const px = p % gridW, py = (p / gridW) | 0;
        if (px < minX) minX = px; if (px > maxX) maxX = px;
        if (py < minY) minY = py; if (py > maxY) maxY = py;
        size++;
        bright += grid[p];
        // 4 neighbours
        if (px > 0 && grid[p - 1] && !labels[p - 1]) { labels[p - 1] = next; stack[sp++] = p - 1; }
        if (px < gridW - 1 && grid[p + 1] && !labels[p + 1]) { labels[p + 1] = next; stack[sp++] = p + 1; }
        if (py > 0 && grid[p - gridW] && !labels[p - gridW]) { labels[p - gridW] = next; stack[sp++] = p - gridW; }
        if (py < gridH - 1 && grid[p + gridW] && !labels[p + gridW]) { labels[p + gridW] = next; stack[sp++] = p + gridW; }
      }
      if (size >= 3) { // reject specks
        // Score: coverage of bbox + mean brightness, both 0..1
        const bw = (maxX - minX + 1) * bx, bh = (maxY - minY + 1) * by;
        const coverage = size / ((maxX - minX + 1) * (maxY - minY + 1));
        const meanBright = bright / size / 255;
        const score = Math.min(0.99, 0.5 * coverage + 0.5 * meanBright);
        scratch.pushBox(minX * bx, minY * by, bw, bh, score, next % 6);
      }
    }

    const t1 = process.hrtime.bigint();
    this.stats.runs++;
    this.stats.boxes = scratch.count;
    this.stats.lastUs = Number(t1 - t0) / 1000;
    return scratch;
  }
}
