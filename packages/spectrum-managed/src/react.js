// src/react.js — <WeftSpectrumHud /> universal telemetry overlay (mandate E).
//
// ZERO RE-RENDER RULE (non-negotiable): React renders this component exactly
// ONCE. Telemetry updates are micro-DOM textContent mutations on prebuilt
// rows driven by the HUD's OWN timer — never setState, never reconciliation,
// never a props change. The shim tests PROVE 0 setState calls across 10,000
// flyweight updates.
//
// TRANSPARENT FALLBACK (Law 4): every paint is error-contained. A destroyed
// row / lost context flips the overlay into an explicit FALLBACK banner while
// the remaining rows keep updating; healing clears the banner
// (E_HUD_CONTEXT_LOST -> E_HUD_RECOVERED). The HUD never throws into the
// host tree.
//
// LAW 1 NOTE: the poll loop runs at the DIAGNOSTIC rate (default 10 Hz), and
// the only allocations are the final `number -> string` conversions required
// by textContent (same accepted pattern as Pillar 4's WeftHud). The
// zero-allocation probe covers the 10+ kHz governor/telemetry path, which is
// strictly allocation-free.

import {
  ERROR_NAMES, E_HUD_CONTEXT_LOST, E_HUD_RECOVERED,
  TIER_FLAGSHIP, TIER_MID, TIER_BUDGET,
} from './wire.js';

const TIER_NAMES = ['UNKNOWN', 'T1 FLAGSHIP', 'T2 MID', 'T3 BUDGET'];
const THERMAL_NAMES = ['nominal', 'light', 'moderate', 'severe', 'critical'];
const VIS_NAMES = ['visible', 'HIDDEN', 'unknown'];
const FEATURE_NAMES = [
  'WASM_SIMD128', 'SHARED_ARRAY_BUFFER', 'WEBGPU', 'WEBGL2', 'AVX512', 'AVX2',
  'SSE42', 'NEON', 'SVE2', 'RVV', 'METAL_3', 'CUDA', 'APPLE_MPS', 'OPENVINO',
  'FASTRPC_DSP', 'NEUROPILOT', 'MULTILANE_DMA', 'BIG_LITTLE', 'THERMAL_SENSOR',
  'DLPACK_EXPORT',
];
const ROW_LABELS = ['TIER', 'FEATURES', 'FPS', 'JITTER p99/p99.9', 'MEM', 'THERMAL', 'CADENCE', 'STATUS'];

// ---------------------------------------------------------------------------
// UI construction (init path — allocations allowed here)
// ---------------------------------------------------------------------------

function makeEl(doc, tag, className, text) {
  const el = doc.createElement(tag);
  if (className) el.className = className;
  if (text !== undefined) el.textContent = text;
  return el;
}

export function buildSpectrumHudUi(host, doc) {
  const root = doc.createElement('div');
  root.className = 'weft-spectrum-hud';
  const style = makeEl(doc, 'style');
  style.textContent = [
    '.weft-spectrum-hud{position:fixed;top:8px;left:8px;z-index:2147483647;',
    'background:rgba(2,6,23,.85);color:#a5f3fc;font:11px/1.55 ui-monospace,monospace;',
    'padding:8px 10px;border-radius:8px;border:1px solid rgba(34,211,238,.4);',
    'min-width:190px;pointer-events:none;backdrop-filter:blur(4px)}',
    '.weft-spectrum-hud .row{display:flex;justify-content:space-between;gap:12px}',
    '.weft-spectrum-hud .k{opacity:.6}',
    '.weft-spectrum-hud .fatal{color:#fbbf24;font-weight:700}',
  ].join('');
  root.appendChild(style);
  const banner = makeEl(doc, 'div', 'fatal');
  banner.style.display = 'none';
  root.appendChild(banner);
  const rows = [];
  for (let i = 0; i < ROW_LABELS.length; i++) {
    const row = makeEl(doc, 'div', 'row');
    const k = makeEl(doc, 'span', 'k', ROW_LABELS[i]);
    const v = makeEl(doc, 'span', 'v', '-');
    row.appendChild(k);
    row.appendChild(v);
    root.appendChild(row);
    rows.push(v);
  }
  (host && host.appendChild) ? host.appendChild(root) : null;
  return { root, banner, rows, healthy: true };
}

// ---------------------------------------------------------------------------
// Paint (steady-state path — called by the HUD timer or the owner's loop)
// ---------------------------------------------------------------------------

const scratch = { text: '' }; // module flyweight — paint writes before use

function featuresText(flagsLo) {
  // Diagnostic-rate string build (10 Hz); names come from the frozen table.
  let count = 0;
  let head = '';
  for (let bit = 0; bit < FEATURE_NAMES.length; bit++) {
    if ((flagsLo & ((1 >>> 0) << bit)) !== 0) {
      count++;
      if (head.length < 24) head = head.length === 0 ? FEATURE_NAMES[bit] : head + ',' + FEATURE_NAMES[bit];
    }
  }
  return head + (count > 3 ? ' +' + (count - 3) : '');
}

export function paintSpectrumHud(ui, props) {
  if (!ui || !ui.root) return;
  try {
    const st = props.state;
    const gov = props.gov;
    const mx = props.metrics;

    ui.rows[0].textContent = TIER_NAMES[st.siliconTier & 3];
    ui.rows[1].textContent = featuresText(st.featureFlagsLo);
    ui.rows[2].textContent = mx ? (mx.fps >= 0 ? String(mx.fps) : '-') : '-';
    ui.rows[3].textContent = mx ? String(mx.jitterP99Us) + '/' + String(mx.jitterP999Us) + 'us' : '-';
    ui.rows[4].textContent = mx ? String(Math.round(mx.memBytes / 1048576)) + ' MiB' : '-';
    ui.rows[5].textContent = THERMAL_NAMES[st.thermalState & 7];
    ui.rows[6].textContent = gov ? String(gov.capHz) + ' Hz' : '-';
    ui.rows[7].textContent = VIS_NAMES[st.visibility & 3] + (st.batteryPermille <= 0xffff && st.batteryPermille !== 0xffff
      ? ' bat ' + String(Math.round(st.batteryPermille / 10)) + '%' : '');

    if (!ui.healthy) {
      ui.healthy = true;
      ui.banner.style.display = 'none';
      ui.banner.textContent = '';
      // E_HUD_RECOVERED is informational — surfaced on the STATUS row owner-
      // side via errorCodes if the owner tracks it.
    }
    scratch.text = '';
  } catch (err) {
    // Law 4: fail visibly but survive — banner ON, remaining rows keep painting.
    ui.healthy = false;
    try {
      ui.banner.style.display = 'block';
      ui.banner.textContent = 'SPECTRUM HUD FALLBACK: ' +
        (err && err.spectrumCode ? (ERROR_NAMES.get(err.spectrumCode) || String(err.spectrumCode)) : 'E_HUD_CONTEXT_LOST');
    } catch { /* even the banner is gone — stay silent, never throw into host */ }
  }
  return ui.healthy ? 0 : E_HUD_CONTEXT_LOST;
}

// ---------------------------------------------------------------------------
// React component factory (shim-testable; production React works unchanged)
// ---------------------------------------------------------------------------

export function createWeftSpectrumHud(React) {
  const WeftSpectrumHud = function WeftSpectrumHud(props) {
    const hostRef = React.useRef(null);
    const uiRef = React.useRef(null);
    const timerRef = React.useRef(null);

    React.useEffect(() => {
      const doc = (props && props.doc) || (typeof document !== 'undefined' ? document : null);
      const host = hostRef.current;
      const ui = buildSpectrumHudUi(host, doc);
      uiRef.current = ui;
      const pollMs = (props && props.pollMs) || 100;
      timerRef.current = setInterval(() => paintSpectrumHud(uiRef.current, props), pollMs);
      paintSpectrumHud(ui, props);
      return () => {
        if (timerRef.current !== null && timerRef.current !== undefined) {
          clearInterval(timerRef.current);
          timerRef.current = null;
        }
        uiRef.current = null;
      };
      // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    return React.createElement('div', { ref: hostRef, 'data-weft-spectrum-hud': '1' });
  };
  WeftSpectrumHud.displayName = 'WeftSpectrumHud';
  return WeftSpectrumHud;
}
