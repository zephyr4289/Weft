// src/hud.js — <WeftHud /> Universal Fail-Safe HUD (charter §D).
//
// ISOLATION GUARANTEE (the point of this component):
//   * The live HUD lives in its own shadow root, OUTSIDE the host React tree.
//     It is driven by its OWN setInterval timer — not the host's rAF loop and
//     not React's reconciler. If the host application throws every frame, the
//     HUD keeps rendering telemetry.
//   * React never re-renders the HUD on telemetry: rows are prebuilt DOM nodes
//     whose textContent is mutated in place (≤ 10 Hz — diagnostic rate).
//   * Worker crash → EXPLICIT fallback banner (Law 4); the HUD itself stays
//     alive and says so.
//
// Displayed telemetry (charter): live FPS (producer + renderer), frame
// delivery latency (producer→lane0 publish lag, ms), memory throughput
// (MiB/s through the plane), dirty-mask bit activity, seqlock tear counts.

import { HPL1, HotPlaneView, Hpl1Error, makeLaneOut, makeHeaderOut } from '../../heddle-core/src/index.js';

const HUD_LABELS = ['FPS(prod)', 'FPS(ui)', 'LAG ms', 'MEM MiB/s', 'DIRTY', 'TEARS', 'STATUS'];

function makeEl(doc, tag) {
  return doc.createElement(tag);
}

function buildRows(root, doc) {
  const box = makeEl(doc, 'div');
  box.setAttribute('data-weft-hud', '1');
  const style = makeEl(doc, 'style');
  style.textContent = [
    '.weft-hud{position:fixed;top:8px;right:8px;z-index:2147483647;',
    'background:rgba(2,6,23,.82);color:#e2e8f0;font:11px/1.5 ui-monospace,monospace;',
    'padding:8px 10px;border-radius:8px;border:1px solid rgba(148,163,184,.35);',
    'min-width:150px;pointer-events:none;backdrop-filter:blur(4px)}',
    '.weft-hud .row{display:flex;justify-content:space-between;gap:10px}',
    '.weft-hud .k{opacity:.65}',
    '.weft-hud .fatal{color:#f87171;font-weight:700}',
  ].join('');
  box.appendChild(style);
  const inner = makeEl(doc, 'div');
  inner.className = 'weft-hud';
  box.appendChild(inner);
  root.appendChild(box);
  const rows = [];
  for (const label of HUD_LABELS) {
    const row = makeEl(doc, 'div');
    row.className = 'row';
    const k = makeEl(doc, 'span');
    k.className = 'k';
    k.textContent = label;
    const v = makeEl(doc, 'span');
    v.textContent = '—';
    row.appendChild(k);
    row.appendChild(v);
    inner.appendChild(row);
    rows.push(v);
  }
  return { box, inner, rows };
}

export function createWeftHud(React, hooks) {
  // mountWeftHud(hostEl, plane, opts) → handle { tick, destroy, fatal }.
  // plane: PlaneContext (adds FPS(ui) from its scheduler) or a raw buffer.
  // opts.intervalMs: HUD refresh period (default 250); 0 = manual tick (tests).
  // opts.worker: optional Worker handle — crash/error becomes an explicit
  //   HPL1_WORKER_CRASH fallback banner (Law 4).
  function mountWeftHud(hostEl, plane, opts = {}) {
    if (hostEl === null || hostEl === undefined) {
      throw new Hpl1Error(HPL1.PLANE_DETACHED, 'WeftHud host element is null');
    }
    const view = plane instanceof HotPlaneView ? plane
      : (plane && plane.view instanceof HotPlaneView ? plane.view : new HotPlaneView(plane));
    const scheduler = plane && plane.scheduler ? plane.scheduler : null;
    const intervalMs = opts.intervalMs !== undefined ? opts.intervalMs : 250;
    const doc = hostEl.ownerDocument
      || (typeof document !== 'undefined' ? document : stubDoc());
    const root = typeof hostEl.attachShadow === 'function'
      ? hostEl.attachShadow({ mode: 'open' })
      : hostEl;
    const rootDoc = root.ownerDocument || doc;
    const { box, rows } = buildRows(root, rootDoc);

    // HUD state — preallocated scalars + one scratch (Law 1 discipline)
    let lastPubSeq = -1;
    let lastSamples = -1;
    let lastRendered = -1;
    let lastTears = -1;
    let lastNs = -1;
    let fatal = null;
    const laneOut = makeLaneOut();
    const hdrOut = makeHeaderOut();
    const changed = new Uint32Array(64);

    const onWorkerError = (evt) => {
      fatal = { code: HPL1.WORKER_CRASH, detail: (evt && evt.message) || 'worker error' };
    };
    if (opts.worker && typeof opts.worker.addEventListener === 'function') {
      opts.worker.addEventListener('error', onWorkerError);
      opts.worker.addEventListener('messageerror', onWorkerError);
    }

    function tick() {
      const now = Date.now();
      const dt = lastNs < 0 ? 0 : Math.max(1, now - lastNs);
      // header: producer publish rate
      if (view.readHeader(hdrOut) === HPL1.OK) {
        const pubSeq = hdrOut.publishSeqLo + hdrOut.publishSeqHi * 0x100000000;
        if (lastPubSeq >= 0 && dt > 0) {
          const fps = ((pubSeq - lastPubSeq) * 1000) / dt;
          rows[0].textContent = Number.isFinite(fps) ? fps.toFixed(1) : '—';
        }
        lastPubSeq = pubSeq;
      }
      // lane 0: memory throughput + producer→lane publish lag
      if (view.readLane(0, laneOut) === HPL1.OK) {
        const seen = laneOut.samplesSeenLo + laneOut.samplesSeenHi * 0x100000000;
        if (lastSamples >= 0 && dt > 0) {
          const perSec = ((seen - lastSamples) * 1000) / dt;
          rows[3].textContent = ((perSec * 8) / 1048576).toFixed(2);
        }
        lastSamples = seen;
        const lagNs = (hdrOut.lastPublishNsLo - laneOut.publishNsLo) >>> 0;
        rows[2].textContent = (lagNs / 1e6).toFixed(3);
      }
      // renderer rate (only when a PlaneContext scheduler is available)
      if (scheduler) {
        if (lastRendered >= 0 && dt > 0) {
          const fps = ((scheduler.rendered - lastRendered) * 1000) / dt;
          rows[1].textContent = Number.isFinite(fps) ? fps.toFixed(1) : '—';
        }
        lastRendered = scheduler.rendered;
      }
      // dirty-mask activity + seqlock tears (explicit, never silent)
      view.scanDirty(changed);
      rows[4].textContent = `${view.dirtyCount}`;
      if (lastTears >= 0 && view.tears > lastTears) {
        rows[6].textContent = `TEARS +${view.tears - lastTears}`;
      } else if (!fatal) {
        rows[6].textContent = 'OK';
      }
      lastTears = view.tears;
      rows[5].textContent = String(view.tears);
      if (fatal) {
        rows[6].textContent = `FALLBACK ${fatal.code}`;
        rows[6].className = 'fatal';
      }
      lastNs = now;
    }

    let timer = null;
    if (intervalMs > 0 && typeof setInterval === 'function') {
      timer = setInterval(tick, intervalMs);
      if (typeof timer === 'object' && timer && typeof timer.unref === 'function') timer.unref();
    }

    return {
      tick,
      get fatal() { return fatal; },
      rows,
      destroy() {
        if (timer !== null) clearInterval(timer);
        timer = null;
        if (opts.worker && typeof opts.worker.removeEventListener === 'function') {
          opts.worker.removeEventListener('error', onWorkerError);
          opts.worker.removeEventListener('messageerror', onWorkerError);
        }
        if (box.parentNode) box.parentNode.removeChild(box);
      },
    };
  }

  // <WeftHud /> — React wrapper. The HUD's live region is a shadow root the
  // host tree never touches; only mount/unmount lifecycle flows through React.
  function WeftHud({ plane, worker, intervalMs = 250, host, style, ...pass }) {
    const hostRef = React.useRef(null);
    React.useEffect(() => {
      const el = host !== undefined ? host : hostRef.current;
      if (el === null || el === undefined) return undefined;
      const handle = mountWeftHud(el, plane, { intervalMs, worker });
      return () => handle.destroy();
    }, [plane, worker, intervalMs, host]);
    if (host !== undefined) return null; // external host mode
    return React.createElement('div', { ref: hostRef, 'data-weft-hud-host': '1', style, ...pass });
  }

  return { WeftHud, mountWeftHud };
}

// node/test fallback document: minimal element factory
function stubDoc() {
  function make(tag) {
    const node = {
      tagName: tag, children: [], className: '', textContent: '', _attrs: {},
      _parentNode: null,
      setAttribute(k, v) { this._attrs[k] = v; },
      getAttribute(k) { return this._attrs[k]; },
      appendChild(c) { this.children.push(c); c._parentNode = this; return c; },
      removeChild(c) {
        const i = this.children.indexOf(c);
        if (i >= 0) this.children.splice(i, 1);
        c._parentNode = null;
        return c;
      },
      get parentNode() { return this._parentNode; },
      ownerDocument: null,
    };
    return node;
  }
  const doc = make('#document');
  doc.ownerDocument = doc;
  doc.createElement = (tag) => {
    const n = make(tag);
    n.ownerDocument = doc;
    return n;
  };
  return doc;
}
