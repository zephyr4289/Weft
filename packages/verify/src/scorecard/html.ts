// html.ts — self-contained dark-theme verification scorecard (Deep Slate).

import type { Scorecard } from './schema.ts';

const C = {
  bg: '#1E1F22',
  panel: '#2B2D30',
  panelEdge: '#393B40',
  accent: '#3574F0',
  ok: '#499C54',
  err: '#E06C75',
  warn: '#DCA855',
  text: '#BCBEC4',
  dim: '#868791',
  mono: "'JetBrains Mono', 'SFMono-Regular', Consolas, 'Liberation Mono', monospace",
};

function esc(s: string): string {
  return s
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

function chip(text: string, color: string): string {
  return `<span class="chip" style="color:${color};border-color:${color}">${esc(text)}</span>`;
}

function svgCurve(
  pts: Array<{ x: number; y: number }>,
  opts: { w?: number; h?: number; color?: string; labelX?: string; labelY?: string },
): string {
  const w = opts.w ?? 280;
  const h = opts.h ?? 120;
  const pad = 8;
  if (pts.length < 2) return '<svg></svg>';
  const xs = pts.map((p) => p.x);
  const ys = pts.map((p) => p.y);
  const minX = Math.min(...xs);
  const maxX = Math.max(...xs);
  const minY = Math.min(...ys);
  const maxY = Math.max(...ys);
  const sx = (v: number): number => pad + ((v - minX) / (maxX - minX || 1)) * (w - 2 * pad);
  const sy = (v: number): number => h - pad - ((v - minY) / (maxY - minY || 1)) * (h - 2 * pad);
  const line = pts.map((p) => `${sx(p.x).toFixed(1)},${sy(p.y).toFixed(1)}`).join(' ');
  const area = `${pad},${h - pad} ${line} ${sx(maxX).toFixed(1)},${h - pad}`;
  const color = opts.color ?? C.accent;
  return `<svg width="${w}" height="${h}" viewBox="0 0 ${w} ${h}" role="img">
  <polygon points="${area}" fill="${color}" opacity="0.12"/>
  <polyline points="${line}" fill="none" stroke="${color}" stroke-width="2"/>
  <text x="${pad}" y="${12}" font-size="9" fill="${C.dim}">max ${maxY.toFixed(1)}</text>
  <text x="${pad}" y="${h - pad + 10}" font-size="9" fill="${C.dim}">min ${minY.toFixed(1)}</text>
</svg>`;
}

export function renderScorecardHtml(card: Scorecard): string {
  const verdictColor = card.verdict === 'PASS' ? C.ok : C.err;
  const bundle = card.package !== null ? `${card.package.bundleKiB.toFixed(1)} KiB payload` : 'payload n/a';
  const deps = card.package !== null ? `${card.package.runtimeDependencies} runtime deps` : 'deps n/a';

  const theorems = card.formal.theorems
    .map(
      (t) => `<div class="thm">
  <div class="thm-head"><span class="tid">${esc(t.id)}</span> ${chip(t.verdict, t.verdict === 'PROVED' ? C.ok : C.err)}</div>
  <div class="thm-name">${esc(t.name)}</div>
  <div class="thm-basis">${esc(t.basis)}</div>
</div>`,
    )
    .join('\n');

  const allocRows = card.allocation.scanned
    .map(
      (r) => `<tr><td class="mono">${esc(r.path)}</td><td>${esc(r.lang)}</td><td class="num">${r.hotFunctions}</td><td class="num">${r.findings}</td><td>${chip(
        r.findings === 0 ? '0B ALLOC' : `${r.findings} FINDINGS`,
        r.findings === 0 ? C.ok : C.err,
      )}</td></tr>`,
    )
    .join('\n');

  const thermalPts = card.chaos.thermal.curve.map((p) => ({ x: p.mhz, y: p.nsPerOp }));
  const busPts = card.chaos.bus.curve.map((p) => ({ x: p.utilization * 100, y: p.nsPerOp }));
  const netRows = card.chaos.network.rates
    .map(
      (r) =>
        `<tr><td class="num">${(r.dropRate * 100).toFixed(1)}%</td><td class="num">${r.sent}</td><td class="num">${r.delivered}</td><td class="num">${r.dropped}</td><td class="num">${(r.deliveredRatio * 100).toFixed(2)}%</td><td class="num">${r.avgRecoveryTicks}</td></tr>`,
    )
    .join('\n');

  const rb = card.resilience.breakdown;
  const score = card.resilience.score;
  const bar = (label: string, v: number): string =>
    `<div class="bar-row"><span class="bar-label">${esc(label)}</span><div class="bar"><div class="fill" style="width:${Math.max(0, Math.min(100, v)).toFixed(1)}%"></div></div><span class="bar-val">${v.toFixed(1)}</span></div>`;

  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>weft-verify scorecard — ${esc(card.verdict)}</title>
<style>
  :root { --bg:${C.bg}; --panel:${C.panel}; --edge:${C.panelEdge}; --accent:${C.accent}; --ok:${C.ok}; --err:${C.err}; --text:${C.text}; --dim:${C.dim}; }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--text); font:14px/1.5 -apple-system,'Segoe UI',Roboto,sans-serif; }
  .wrap { max-width: 980px; margin: 0 auto; padding: 28px 20px 60px; }
  h1 { font-size: 20px; margin: 0 0 4px; color:#DFE1E5; }
  h2 { font-size: 13px; letter-spacing: .12em; text-transform: uppercase; color: var(--dim); border-bottom: 1px solid var(--edge); padding-bottom: 6px; margin: 34px 0 14px; }
  .sub { color: var(--dim); font-size: 12px; margin-bottom: 18px; }
  .badge { display:inline-block; padding:3px 12px; border-radius:12px; font-weight:700; color:${verdictColor}; border:1px solid ${verdictColor}; margin-left:8px; }
  .chip { display:inline-block; border:1px solid; border-radius:10px; padding:0 8px; font-size:11px; margin-left:6px; }
  .cards { display:grid; grid-template-columns:repeat(auto-fill,minmax(280px,1fr)); gap:10px; }
  .thm, .panelbox { background:var(--panel); border:1px solid var(--edge); border-radius:8px; padding:12px 14px; }
  .thm-head { display:flex; align-items:center; gap:8px; }
  .tid { color: var(--accent); font-weight:700; font-size:12px; }
  .thm-name { font-weight:600; margin-top:4px; color:#DFE1E5; font-size:13px; }
  .thm-basis { color: var(--dim); font-size:11px; margin-top:3px; font-family:${C.mono}; }
  table { width:100%; border-collapse:collapse; font-size:12px; }
  th { text-align:left; color:var(--dim); font-weight:600; border-bottom:1px solid var(--edge); padding:6px 8px; }
  td { border-bottom:1px solid var(--edge); padding:6px 8px; }
  td.num { text-align:right; font-family:${C.mono}; }
  .mono { font-family:${C.mono}; font-size:11px; }
  .charts { display:grid; grid-template-columns:repeat(auto-fill,minmax(300px,1fr)); gap:14px; }
  .chart-title { font-size:12px; color:#DFE1E5; margin-bottom:6px; }
  .bar-row { display:flex; align-items:center; gap:10px; margin:8px 0; }
  .bar-label { width: 150px; font-size:12px; color:var(--dim); }
  .bar { flex:1; height:8px; background:#17181A; border-radius:4px; overflow:hidden; }
  .fill { height:100%; background:var(--accent); }
  .bar-val { width:48px; text-align:right; font-family:${C.mono}; font-size:11px; }
  .big { font-size:30px; font-weight:800; color:${score >= 85 ? C.ok : C.err}; }
  footer { margin-top:40px; color:var(--dim); font-size:11px; border-top:1px solid var(--edge); padding-top:12px; }
</style>
</head>
<body><div class="wrap">
  <h1>Weft Verification Scorecard <span class="badge">${esc(card.verdict)}</span></h1>
  <div class="sub">${esc(card.tool.name)} v${esc(card.tool.version)} &middot; node ${esc(card.node)} &middot; ${esc(card.platform)}
    ${chip(deps, C.ok)} ${chip(bundle, C.accent)}
  </div>

  <h2 id="formal-hud">Formal Theorems HUD</h2>
  <div class="cards">
${theorems}
  </div>

  <h2 id="alloc-matrix">Allocation Audit Matrix — hot paths, 0B heap compliance</h2>
  <div class="panelbox">
    <table>
      <tr><th>File</th><th>Lang</th><th style="text-align:right">Hot fns</th><th style="text-align:right">Findings</th><th>Status</th></tr>
${allocRows}
    </table>
    <div class="sub" style="margin-top:10px">${card.allocation.summary.files} files &middot; ${card.allocation.summary.hotFunctions} hot functions &middot; ${card.allocation.summary.findings} findings &middot; ${esc(card.allocation.summary.compliant ? 'COMPLIANT' : 'VIOLATIONS')}</div>
  </div>

  <h2 id="silicon-curves">Synthetic Silicon Performance Curves</h2>
  <div class="charts">
    <div class="panelbox"><div class="chart-title">Thermal throttling — ns/op vs core frequency (3.2 GHz &rarr; 800 MHz)</div>${svgCurve(thermalPts, { color: C.warn })}</div>
    <div class="panelbox"><div class="chart-title">Memory-bus saturation — ns/op vs utilization</div>${svgCurve(busPts, { color: C.accent })}</div>
  </div>
  <div class="panelbox" style="margin-top:14px">
    <div class="chart-title">Cluster resilience — packet drop tolerance &amp; split-brain recovery</div>
    <table>
      <tr><th>Drop rate</th><th style="text-align:right">Sent</th><th style="text-align:right">Delivered</th><th style="text-align:right">Dropped</th><th style="text-align:right">Delivery</th><th style="text-align:right">Recovery ticks</th></tr>
${netRows}
    </table>
  </div>

  <h2 id="resilience">Cluster Resilience Score</h2>
  <div class="panelbox">
    <div class="big">${score} / 100</div>
${bar('delivery @1% drop', rb.delivery1pct)}
${bar('delivery @5% drop', rb.delivery5pct)}
${bar('split-brain recovery', rb.recovery)}
  </div>

  <footer>
    schema ${esc(card.schema)} &middot; deterministic report (no wall-clock inputs; formal timing stripped) &middot;
    generated by weft-verify — the fail-closed developer verification suite for the Weft zero-copy ecosystem.
  </footer>
</div></body>
</html>
`;
}
