/**
 * DualPlane demo — "hot state breaks reactive UI".
 * Left: a simulated GC-bound reactive path (setState per tick → re-render churn,
 * random long tasks / dropped frames). Right: the Weft path (draw-phase read of
 * a claimed buffer; render tree untouched). Both draw the same signal.
 */

const N = 96;
function signal(t, i) {
  const x = i / N;
  return Math.sin(x * 12 + t * 0.05) * 0.5 + Math.sin(x * 31 - t * 0.09) * 0.3 + Math.sin(x * 71 + t * 0.15) * 0.14;
}

document.querySelectorAll('[data-dual]').forEach((wrap) => {
  const bad = wrap.querySelector('.dp-bad canvas');
  const good = wrap.querySelector('.dp-good canvas');
  if (!bad || !good) return;
  const bctx = bad.getContext('2d'), gctx = good.getContext('2d');
  const badgeBad = wrap.querySelector('.dp-badge-bad');
  const badgeGood = wrap.querySelector('.dp-badge-good');
  let dpr = Math.min(devicePixelRatio || 1, 2);

  function size(c) { const r = c.getBoundingClientRect(); c.width = r.width * dpr; c.height = r.height * dpr; }
  size(bad); size(good);
  addEventListener('resize', () => { size(bad); size(good); });

  // ---- "reactive" simulation model: virtual clock that randomly stalls ------
  let vt = 0, gt = 0, t = 0;
  let jankLeft = 0, renders = 0, dropped = 0, lastGt = 0;
  let gcPulse = 0;

  function drawWave(ctx, c, time, colorA, colorB, ghost) {
    const W = c.width, H = c.height;
    ctx.clearRect(0, 0, W, H);
    if (ghost) {
      ctx.globalAlpha = 0.16; ctx.strokeStyle = colorA; ctx.lineWidth = 1.6 * dpr;
      for (let k = 1; k <= 3; k++) {
        ctx.beginPath();
        for (let i = 0; i < N; i++) {
          const x = (i / (N - 1)) * W, y = H / 2 - signal(time - k * 7, i) * H * 0.36;
          i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
        }
        ctx.stroke();
      }
      ctx.globalAlpha = 1;
    }
    const grad = ctx.createLinearGradient(0, 0, W, 0);
    grad.addColorStop(0, colorA); grad.addColorStop(1, colorB);
    ctx.strokeStyle = grad; ctx.lineWidth = 2.2 * dpr; ctx.lineJoin = 'round';
    ctx.shadowColor = colorA; ctx.shadowBlur = 10 * dpr;
    ctx.beginPath();
    for (let i = 0; i < N; i++) {
      const x = (i / (N - 1)) * W, y = H / 2 - signal(time, i) * H * 0.36;
      i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
    }
    ctx.stroke(); ctx.shadowBlur = 0;
  }

  function loop() {
    t++;
    // writer advances truth at constant rate
    gt += 4;

    // --- BAD plane: setState-per-tick with GC pauses -------------------------
    if (jankLeft > 0) { jankLeft--; dropped++; }
    else {
      if (Math.random() < 0.012) { jankLeft = 8 + Math.floor(Math.random() * 22); gcPulse = 1; } // "GC pause"
      vt += 4; renders++;
    }
    if (gcPulse > 0) gcPulse -= 0.05;

    drawWave(bctx, bad, vt - (jankLeft > 0 ? 0 : 0), jankLeft > 0 ? '#f87171' : '#8b93a7', '#5b6377', false);
    if (jankLeft > 0) {
      // frozen frame overlay
      bctx.fillStyle = 'rgba(248,113,113,.08)'; bctx.fillRect(0, 0, bad.width, bad.height);
      bctx.font = `${12 * dpr}px 'JetBrains Mono', monospace`; bctx.fillStyle = '#f87171';
      bctx.fillText(`⚠ long task · frame ${Math.round(vt / 4)} held`, 10 * dpr, bad.height - 12 * dpr);
    }

    // --- GOOD plane: claim + draw --------------------------------------------
    drawWave(gctx, good, gt, '#f5a524', '#53d7fb', true);

    if (badgeBad && t % 6 === 0) {
      badgeBad.innerHTML = `<b>${renders.toLocaleString()}</b> renders · <b>${dropped}</b> dropped · gc ${(dropped / Math.max(1, renders) * 100).toFixed(1)}%`;
    }
    if (badgeGood && t % 6 === 0) {
      badgeGood.innerHTML = `<b>1</b> render · <b>${Math.round(gt / 4).toLocaleString()}</b> claims · <b>0</b> dropped`;
    }
    requestAnimationFrame(loop);
  }
  requestAnimationFrame(loop);
});
