// draw.ts — Shared Canvas draw module for W1–W6
//
// FAIRNESS PIN:
// Exactly ONE draw implementation exists in the entire codebase for each workload.
// All four modes (A, B, C, D) invoke this identical draw routine.

export function drawWorkload(
  ctx: CanvasRenderingContext2D,
  width: number,
  height: number,
  wid: string,
  data: Float32Array
): void {
  ctx.clearRect(0, 0, width, height);

  switch (wid) {
    case 'W1':
      drawW1Spectrum(ctx, width, height, data);
      break;
    case 'W2':
      drawW2Particles(ctx, width, height, data);
      break;
    case 'W3':
      drawW3Spectrogram(ctx, width, height, data);
      break;
    case 'W4':
      drawW4EEG(ctx, width, height, data);
      break;
    case 'W5':
      drawW5OrderBook(ctx, width, height, data);
      break;
    case 'W6':
      drawW6FeedLadder(ctx, width, height, data);
      break;
  }
}

function drawW1Spectrum(
  ctx: CanvasRenderingContext2D,
  width: number,
  height: number,
  data: Float32Array
): void {
  const barCount = Math.min(data.length, Math.floor(width / 2));
  const barWidth = width / barCount;
  ctx.fillStyle = '#38bdf8';

  for (let i = 0; i < barCount; i++) {
    const val = data[i];
    const barHeight = val * height;
    ctx.fillRect(i * barWidth, height - barHeight, barWidth - 1, barHeight);
  }
}

function drawW2Particles(
  ctx: CanvasRenderingContext2D,
  width: number,
  height: number,
  data: Float32Array
): void {
  const count = data.length / 6;
  const cx = width / 2;
  const cy = height / 2;
  const scale = Math.min(width, height) * 0.35;

  ctx.fillStyle = '#818cf8';
  for (let p = 0; p < count; p++) {
    const idx = p * 6;
    const x = cx + data[idx + 0] * scale;
    const y = cy + data[idx + 1] * scale;
    ctx.fillRect(x - 1.5, y - 1.5, 3, 3);
  }
}

function drawW3Spectrogram(
  ctx: CanvasRenderingContext2D,
  width: number,
  height: number,
  data: Float32Array
): void {
  const cols = 256;
  const rows = 64;
  const cellW = width / cols;
  const cellH = height / rows;

  for (let r = 0; r < rows; r++) {
    for (let c = 0; c < cols; c++) {
      const val = data[r * cols + c];
      const brightness = Math.floor(val * 255);
      ctx.fillStyle = `rgb(${brightness}, ${Math.floor(brightness * 0.4)}, ${Math.floor(255 - brightness)})`;
      ctx.fillRect(c * cellW, r * cellH, cellW, cellH);
    }
  }
}

function drawW4EEG(
  ctx: CanvasRenderingContext2D,
  width: number,
  height: number,
  data: Float32Array
): void {
  ctx.strokeStyle = '#4ade80';
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  const step = width / data.length;
  for (let i = 0; i < data.length; i++) {
    const y = height * 0.5 + ((data[i] - 500) / 1000) * height * 0.8;
    if (i === 0) ctx.moveTo(0, y);
    else ctx.lineTo(i * step, y);
  }
  ctx.stroke();
}

function drawW5OrderBook(
  ctx: CanvasRenderingContext2D,
  width: number,
  height: number,
  data: Float32Array
): void {
  const levels = Math.min(50, data.length / 10);
  const midX = width / 2;
  const rowH = height / levels;

  // Bids (green, left)
  ctx.fillStyle = '#22c55e';
  for (let lvl = 0; lvl < levels; lvl++) {
    const size = data[lvl * 10 + 1];
    const barW = (size / 10.0) * (midX - 10);
    ctx.fillRect(midX - barW, lvl * rowH, barW, rowH - 1);
  }

  // Asks (red, right)
  ctx.fillStyle = '#ef4444';
  for (let lvl = 0; lvl < levels; lvl++) {
    const size = data[lvl * 10 + 4];
    const barW = (size / 10.0) * (midX - 10);
    ctx.fillRect(midX + 2, lvl * rowH, barW, rowH - 1);
  }
}

// W6 layout (l2feed.ts): header 8 floats, then 64 levels × 6
// (bidPx, bidSz, bidN, askPx, askSz, askN), then 64 tape entries × 3
// (px, sz, side). The FAIRNESS PIN applies to this routine exactly as to
// W1–W5: one implementation, shared by every mode. The fan-out feed views
// (RFC-0004 panel) reuse drawW6FeedLadder for their ladder canvas — the
// same workload, the same draw code — and draw their tape/stats views with
// view-specific routines local to the panel component.
const W6_LADDER_SHOWN = 48;

/// Exported for the RFC-0004 fan-out feed views panel (App.tsx): the
/// ladder canvas reuses THIS routine — same workload, same draw code
/// (FAIRNESS PIN discipline extended across surfaces).
export function drawW6FeedLadder(
  ctx: CanvasRenderingContext2D,
  width: number,
  height: number,
  data: Float32Array
): void {
  const LADDER_BASE = 8; // header floats
  const TAPE_BASE = 8 + 64 * 6;
  const midX = width / 2;
  const ladderH = height * 0.85;
  const rowH = ladderH / W6_LADDER_SHOWN;
  // Demo scale for bar normalization: sizes vary widely under the delta
  // stream; clamp at the same fixed scale for bids and asks (fairness).
  const sizeScale = 40.0;

  // Bids (green, left)
  ctx.fillStyle = '#22c55e';
  for (let lvl = 0; lvl < W6_LADDER_SHOWN; lvl++) {
    const o = LADDER_BASE + lvl * 6;
    const size = data[o + 1];
    const barW = Math.min(size / sizeScale, 1) * (midX - 10);
    ctx.fillRect(midX - barW, lvl * rowH, barW, rowH - 1);
  }

  // Asks (red, right)
  ctx.fillStyle = '#ef4444';
  for (let lvl = 0; lvl < W6_LADDER_SHOWN; lvl++) {
    const o = LADDER_BASE + lvl * 6;
    const size = data[o + 4];
    const barW = Math.min(size / sizeScale, 1) * (midX - 10);
    ctx.fillRect(midX + 2, lvl * rowH, barW, rowH - 1);
  }

  // Trade tape strip (bottom 15%): last trades as colored marks — green
  // = buy aggressor (lifted the ask), red = sell aggressor (hit the bid).
  // Mark width encodes trade size (1–8 lots → 2–8 px).
  const tapeTop = ladderH + 2;
  const tapeH = height - tapeTop;
  ctx.fillStyle = '#334155';
  ctx.fillRect(0, tapeTop, width, 1);
  for (let j = 0; j < 64; j++) {
    const o = TAPE_BASE + j * 3;
    const side = data[o + 2];
    if (side === 0) continue; // unfilled tape slot
    const sz = data[o + 1];
    const x = (width * (j + 1)) / 65;
    ctx.fillStyle = side > 0 ? '#4ade80' : '#f87171';
    ctx.fillRect(x, tapeTop + 2 + (tapeH - 4) * 0.5, 2 + Math.min(sz, 6), Math.max(tapeH - 4, 2));
  }
}
