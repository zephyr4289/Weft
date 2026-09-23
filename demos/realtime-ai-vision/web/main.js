// web/main.js — browser twin of the terminal flight demo.
// Same wire: camera (synthetic | webcam via WebCodecs) -> WTR1 ring ->
// Canvas2DPlane (ONE ImageData) -> OverlayScratch boxes. React-free by
// design; the HUD reads the listener stats directly.
import {
  WeftTensorRing, Canvas2DPlane, RingListener, OverlayScratch,
  VideoFrameIngestor, DLPackCode,
} from '../../../packages/weft-tensor/src/index.js';

const W = 160, H = 90, FPS = 120;
const $ = (id) => document.getElementById(id);

const shared = typeof SharedArrayBuffer !== 'undefined' && crossOriginIsolated;
$('sab').textContent = shared ? 'SharedArrayBuffer: ON' : 'SharedArrayBuffer: off (plain ArrayBuffer — demo is single-threaded)';

const ring = WeftTensorRing.create({
  slotCount: 4, payloadCap: W * H * 4,
  dtype: { code: DLPackCode.UINT, bits: 8 },
  shape: [H, W, 4], tickHz: FPS, fourcc: 'RGBA', shared,
});

// -- synthetic camera (no permissions needed) --------------------------------
const frame = new Uint8Array(W * H * 4);
const BG = 30;
let seq = 0;
let cameraTimer = null;
function synthTick() {
  seq++;
  frame.fill(BG);
  for (let b = 0; b < 3; b++) {
    const ph = seq / (FPS * 1.7) + b * 2.1;
    const cx = (0.5 + 0.38 * Math.sin(ph * 1.3 + b)) * W;
    const cy = (0.5 + 0.36 * Math.cos(ph * 0.9 + b * 1.7)) * H;
    const r = 8 + 3 * Math.sin(ph * 2.2 + b);
    const x0 = Math.max(1, cx - r | 0), x1 = Math.min(W - 2, cx + r | 0);
    const y0 = Math.max(1, cy - r | 0), y1 = Math.min(H - 2, cy + r | 0);
    for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) {
      const dx = x - cx, dy = y - cy;
      const d2 = dx * dx + dy * dy;
      if (d2 <= r * r) {
        const v = 235 - b * 25 - (d2 / (r * r)) * 40;
        const o = (y * W + x) * 4;
        frame[o] = v; frame[o + 1] = v; frame[o + 2] = v; frame[o + 3] = 255;
      }
    }
  }
  ring.commit(frame, { timestampNs: seq * 8333333, fourcc: 'RGBA' });
}

// -- detector (browser twin: same preallocated-grid connected components) ----
const GW = 40, GH = 24;
const grid = new Uint8Array(GW * GH);
const labels = new Int32Array(GW * GH);
const stack = new Int32Array(GW * GH);
const scratch = new OverlayScratch(32);

function detect(view) {
  scratch.clear();
  const src = view.payloadView();
  const bx = W / GW | 0, by = H / GH | 0;
  for (let gy = 0; gy < GH; gy++) {
    for (let gx = 0; gx < GW; gx++) {
      const o = ((gy * by + (by >> 1)) * W + (gx * bx + (bx >> 1))) * 4;
      grid[gy * GW + gx] = ((src[o] * 77 + src[o + 1] * 150 + src[o + 2] * 29) >> 8) > 96 ? 1 : 0;
      labels[gy * GW + gx] = 0;
    }
  }
  let next = 0;
  for (let i = 0; i < grid.length; i++) {
    if (!grid[i] || labels[i]) continue;
    next++;
    let sp = 0; stack[sp++] = i; labels[i] = next;
    let minX = GW, maxX = -1, minY = GH, maxY = -1, size = 0, bright = 0;
    while (sp > 0) {
      const p = stack[--sp], px = p % GW, py = p / GW | 0;
      if (px < minX) minX = px; if (px > maxX) maxX = px;
      if (py < minY) minY = py; if (py > maxY) maxY = py;
      size++; bright += grid[p];
      if (px > 0 && grid[p - 1] && !labels[p - 1]) { labels[p - 1] = next; stack[sp++] = p - 1; }
      if (px < GW - 1 && grid[p + 1] && !labels[p + 1]) { labels[p + 1] = next; stack[sp++] = p + 1; }
      if (py > 0 && grid[p - GW] && !labels[p - GW]) { labels[p - GW] = next; stack[sp++] = p - GW; }
      if (py < GH - 1 && grid[p + GW] && !labels[p + GW]) { labels[p + GW] = next; stack[sp++] = p + GW; }
    }
    if (size >= 3) {
      const coverage = size / ((maxX - minX + 1) * (maxY - minY + 1));
      scratch.pushBox(minX * bx, minY * by, (maxX - minX + 1) * bx, (maxY - minY + 1) * by,
        Math.min(0.99, 0.5 * coverage + 0.5 * (bright / size / 255)), next % 6);
    }
  }
}

// -- render plane + listener -------------------------------------------------
const canvas = $('stage');
const plane = new Canvas2DPlane(canvas, ring);
const PALETTE = ['#00e5ff', '#ff4081', '#76ff03', '#ffd740', '#e040fb', '#ff6e40'];
const stats = { frames: 0, stalls: 0, t0: 0 };
let listener = null;

function startFlight() {
  if (listener) return;
  const ctx = plane.ctx;
  listener = new RingListener(ring, {
    pump: { request(cb) { return requestAnimationFrame(cb); }, cancel(id) { cancelAnimationFrame(id); } },
    onFrame(view) {
      detect(view);
      plane.draw(view);
      // Vector overlay ON TOP of the blit — same ctx, zero allocations.
      const bx = W / GW, by = H / GH;
      ctx.lineWidth = 1;
      ctx.font = '6px monospace';
      for (let i = 0; i < scratch.count; i++) {
        const o = i * 6;
        ctx.strokeStyle = PALETTE[(scratch.boxes[o + 5] | 0) % PALETTE.length];
        ctx.strokeRect(scratch.boxes[o] + 0.5, scratch.boxes[o + 1] + 0.5,
          scratch.boxes[o + 2], scratch.boxes[o + 3]);
        ctx.fillStyle = '#fff';
        ctx.fillText(`${(scratch.boxes[o + 4] * 100) | 0}`, scratch.boxes[o] + 1, scratch.boxes[o + 1] + 5);
      }
      stats.frames++;
    },
    onStall() { stats.stalls++; plane.draw(null); },
  });
  listener.start();
  cameraTimer = setInterval(synthTick, 1000 / FPS);
  stats.t0 = performance.now();
  setInterval(() => {
    const s = listener.stats;
    const fps = stats.frames / ((performance.now() - stats.t0) / 1000);
    $('hud').textContent =
      `seq ${ring.producerSeq} | canvas frames ${stats.frames} (~${fps.toFixed(1)} fps) | ` +
      `listener stalls ${stats.stalls} | boxes ${scratch.count} | ring: ${ring.describe()}`;
  }, 500);
}

$('start').addEventListener('click', startFlight);

// Optional: real webcam through WebCodecs into the SAME ring (fast path).
$('cam').addEventListener('click', async () => {
  if (listener === null) startFlight();
  try {
    const stream = await navigator.mediaDevices.getUserMedia({ video: { width: W, height: H } });
    const video = document.createElement('video');
    video.srcObject = stream; video.muted = true; await video.play();
    const ingestor = new VideoFrameIngestor(ring, { fourcc: 'RGBA' });
    const processor = new VideoFrameCallbackMeter(video, ingestor);
    $('cam').textContent = 'webcam live';
  } catch (e) {
    $('cam').textContent = 'webcam unavailable';
  }
});

/** Drives VideoFrame.copyTo -> ring at the camera's own cadence. */
class VideoFrameCallbackMeter {
  constructor(video, ingestor) {
    this.video = video; this.ingestor = ingestor; this.ingest();
  }
  async ingest() {
    if (this.video.readyState < 2) { setTimeout(() => this.ingest(), 50); return; }
    const vf = new VideoFrame(this.video);
    this.ingestor.ingestWebCodecs(vf, { timestampNs: vf.timestamp * 1000 });
    vf.close();
    // Track the camera's native cadence (typically 30) — ring is 120-capable.
    setTimeout(() => this.ingest(), 33);
  }
}
