/**
 * HotDemo — the hero instrument. A live 240 Hz synthetic signal (waveform)
 * flowing through a real TriadEngine instance, rendered on canvas by the
 * reader side every animation frame. Shows publishes vs claims vs torn reads
 * live. This is not an animation — it is the actual protocol running.
 */
import { TriadEngine } from './triad-engine.js';

const el = document.getElementById('hot-canvas');
const statsEl = document.getElementById('hot-stats');
if (el) {
  const ctx = el.getContext('2d');
  const N = 128; // samples per frame
  const engine = new TriadEngine(N * 4);
  let t = 0;
  let dpr = Math.min(devicePixelRatio || 1, 2);

  function resize() {
    dpr = Math.min(devicePixelRatio || 1, 2);
    const r = el.getBoundingClientRect();
    el.width = r.width * dpr; el.height = r.height * dpr;
  }
  resize(); addEventListener('resize', resize);

  // writer: 240 Hz virtual cadence via accumulator (drop-not-queue)
  let lastPub = performance.now(), pubAcc = 0;
  const PUB_INTERVAL = 1000 / 240;

  function writeFrame(buf, off, len) {
    const f32 = new Float32Array(buf, off, N);
    for (let i = 0; i < N; i++) {
      const x = i / N;
      f32[i] =
        Math.sin((x * 9 + t * 0.06)) * 0.55 +
        Math.sin((x * 23 - t * 0.11)) * 0.28 +
        Math.sin((x * 57 + t * 0.19)) * 0.12;
    }
  }

  let phaseHue = 0;
  function frame(now) {
    t++;
    pubAcc += now - lastPub; lastPub = now;
    while (pubAcc >= PUB_INTERVAL) { pubAcc -= PUB_INTERVAL; engine.publish(writeFrame); }

    const c = engine.claim();
    const W = el.width, H = el.height;
    ctx.clearRect(0, 0, W, H);

    // grid
    ctx.strokeStyle = 'rgba(148,158,190,0.08)'; ctx.lineWidth = 1;
    for (let g = 1; g < 6; g++) { const y = H * g / 6; ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke(); }

    // waveform from claimed buffer (zero-copy read of the payload region)
    const f32 = new Float32Array(engine.buffer, c.offset, N);
    const grad = ctx.createLinearGradient(0, 0, W, 0);
    grad.addColorStop(0, '#f5a524'); grad.addColorStop(0.55, '#ffd58a'); grad.addColorStop(1, '#53d7fb');
    ctx.strokeStyle = grad; ctx.lineWidth = 2 * dpr; ctx.lineJoin = 'round';
    ctx.shadowColor = 'rgba(245,165,36,0.45)'; ctx.shadowBlur = 14 * dpr;
    ctx.beginPath();
    for (let i = 0; i < N; i++) {
      const x = (i / (N - 1)) * W;
      const y = H / 2 - f32[i] * H * 0.38;
      i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
    }
    ctx.stroke();
    ctx.shadowBlur = 0;

    // seq badge drawn into canvas too (draw-phase read → draw)
    ctx.font = `${11 * dpr}px 'JetBrains Mono', monospace`;
    ctx.fillStyle = 'rgba(236,231,221,0.55)';
    ctx.fillText(`seq ${c.seq}`, 10 * dpr, 16 * dpr);

    if (statsEl && t % 10 === 0) {
      const a = engine.audit();
      statsEl.innerHTML =
        `<span><b>${a.publishes.toLocaleString()}</b> publishes</span>` +
        `<span><b>${a.claims.toLocaleString()}</b> claims</span>` +
        `<span class="${a.tornReads ? 'bad' : 'good'}"><b>${a.tornReads}</b> torn reads</span>` +
        `<span class="${a.regressions ? 'bad' : 'good'}"><b>${a.regressions}</b> regressions</span>`;
    }
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
}
