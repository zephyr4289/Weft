// render/overlay.js — AI overlay scratch: boxes / skeletons, zero allocation.
//
// Detectors write into ONE preallocated Float32Array; renderers read it.
// No {x,y,w,h} object churn, no arrays-of-objects, no GC (Law 1).

export const MAX_BOXES = 256;
/** Stride of one box record: x, y, w, h, score, classId. */
export const BOX_STRIDE = 6;

/**
 * Detection scratch — the detector's output buffer AND the renderer's input.
 * Reused every frame; `count` says how many records are live.
 */
export class OverlayScratch {
  constructor(maxBoxes = MAX_BOXES) {
    this.boxes = new Float32Array(maxBoxes * BOX_STRIDE);
    this.maxBoxes = maxBoxes;
    this.count = 0;
  }

  clear() { this.count = 0; }

  /**
   * Append one box. Returns false when full (never throws in the hot loop).
   */
  pushBox(x, y, w, h, score, classId) {
    if (this.count >= this.maxBoxes) return false;
    const o = this.count * BOX_STRIDE;
    const b = this.boxes;
    b[o] = x; b[o + 1] = y; b[o + 2] = w; b[o + 3] = h; b[o + 4] = score; b[o + 5] = classId;
    this.count++;
    return true;
  }

  boxAt(i, out) {
    // Copies record i into a caller-owned 6-slot array (readable API without
    // allocation); hot renderers index .boxes directly instead.
    const o = i * BOX_STRIDE;
    out[0] = this.boxes[o]; out[1] = this.boxes[o + 1]; out[2] = this.boxes[o + 2];
    out[3] = this.boxes[o + 3]; out[4] = this.boxes[o + 4]; out[5] = this.boxes[o + 5];
    return out;
  }
}

/**
 * Draw boxes onto a Canvas2D context. Zero allocation: style object and
 * scratch are caller-owned/reused.
 */
export function drawBoxes2D(ctx, scratch, style = DEFAULT_STYLE) {
  const b = scratch.boxes;
  const n = scratch.count;
  if (n === 0) return 0;
  ctx.lineWidth = style.lineWidth;
  ctx.font = style.font;
  ctx.textBaseline = 'top';
  for (let i = 0; i < n; i++) {
    const o = i * BOX_STRIDE;
    const score = b[o + 4], cls = b[o + 5] | 0;
    ctx.strokeStyle = style.colorFor(cls);
    ctx.strokeRect(b[o], b[o + 1], b[o + 2], b[o + 3]);
    if (style.drawLabels) {
      ctx.fillStyle = style.labelColor;
      ctx.fillText(`${cls} ${(score * 100) | 0}`, b[o] + 2, b[o + 1] + 2);
    }
  }
  return n;
}

const PALETTE = ['#00e5ff', '#ff4081', '#76ff03', '#ffd740', '#e040fb', '#ff6e40'];
const DEFAULT_STYLE = {
  lineWidth: 2,
  font: '12px monospace',
  drawLabels: true,
  labelColor: '#ffffff',
  colorFor: (cls) => PALETTE[cls % PALETTE.length],
};

/** Keypoint skeleton helper (COCO-17 order) — pairs into ONE Int8Array. */
export const COCO17_EDGES = new Int8Array([
  0, 1, 0, 2, 1, 3, 2, 4,        // face
  5, 6, 5, 7, 7, 9, 6, 8, 8, 10, // arms
  5, 11, 6, 12, 11, 12,          // torso
  11, 13, 13, 15, 12, 14, 14, 16 // legs
]);

/**
 * Draw a keypoint skeleton from a preallocated Float32Array (x,y,score triplets).
 * Zero allocation.
 */
export function drawSkeleton2D(ctx, keypoints, count, confidence = 0.25) {
  ctx.lineWidth = 2;
  ctx.strokeStyle = '#00e5ff';
  for (let e = 0; e < COCO17_EDGES.length; e += 2) {
    const a = COCO17_EDGES[e], b = COCO17_EDGES[e + 1];
    if (a >= count || b >= count) continue;
    const ax = keypoints[a * 3], ay = keypoints[a * 3 + 1], as = keypoints[a * 3 + 2];
    const bx = keypoints[b * 3], by = keypoints[b * 3 + 1], bs = keypoints[b * 3 + 2];
    if (as < confidence || bs < confidence) continue;
    ctx.beginPath();
    ctx.moveTo(ax, ay);
    ctx.lineTo(bx, by);
    ctx.stroke();
  }
}
