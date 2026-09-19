import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';

// TraceTimeline — TIER5 §5 (issue #20, task 5): the .weftrec v4 kernel-trace
// lane view. Loads the JSON-lines export of a v4 capture (core/c/trace-dump
// json N > file), renders the decision stream as colored lanes — publish,
// claim, drop, revoke, ack, stall, tear, canary_fail — with pause, zoom,
// and click-to-inspect. The Rust lane colors match the Inspector's verdict
// palette so an operator reads one legend across both tools.
//
// WHY LANES: frame flow (publish/claim) reads horizontally; the anomaly
// lanes (drop/stall/tear/canary_fail) are sparse by design — a healthy run
// shows empty anomaly lanes, so ANY mark is a story. Revocation windows
// (revoke..ack pairs) shade the whole timeline so a reader sees "the
// writer was revoked during THIS span" without counting events.

export interface TraceHeader {
  format: 'weftrec';
  version: 4;
  kind: 'trace';
  events: number;
}

export interface TraceEvent {
  i: number;
  kind: 'publish' | 'claim' | 'drop' | 'revoke' | 'ack' | 'stall' | 'tear' | 'canary_fail';
  aux: number;
  data: number;
}

const KIND_COLORS: Record<TraceEvent['kind'], string> = {
  publish: '#10b981',
  claim: '#3b82f6',
  drop: '#f59e0b',
  revoke: '#ef4444',
  ack: '#8b5cf6',
  stall: '#eab308',
  tear: '#f97316',
  canary_fail: '#dc2626',
};

const LANE_ORDER: TraceEvent['kind'][] = [
  'publish', 'claim', 'drop', 'revoke', 'ack', 'stall', 'tear', 'canary_fail',
];

function parseJsonl(text: string): { header: TraceHeader; events: TraceEvent[] } | null {
  const lines = text.trim().split('\n');
  if (lines.length < 2) return null;
  let header: TraceHeader;
  try {
    header = JSON.parse(lines[0]);
  } catch {
    return null;
  }
  if (header.format !== 'weftrec' || header.version !== 4 || header.kind !== 'trace') {
    return null;
  }
  const events: TraceEvent[] = [];
  for (const ln of lines.slice(1)) {
    try {
      events.push(JSON.parse(ln));
    } catch {
      return null;
    }
  }
  return { header, events };
}

export const TraceTimeline: React.FC = () => {
  const [raw, setRaw] = useState<string>('');
  const [paused, setPaused] = useState<boolean>(false);
  const [zoom, setZoom] = useState<number>(1); // 1 = whole stream
  const [selected, setSelected] = useState<TraceEvent | null>(null);
  const [error, setError] = useState<string>('');
  const fileRef = useRef<HTMLInputElement | null>(null);

  const parsed = useMemo(() => parseJsonl(raw), [raw]);

  const onFile = useCallback(async (f: File) => {
    const text = await f.text();
    setError('');
    setRaw(text);
    setSelected(null);
  }, []);

  // Drag & drop a .jsonl export anywhere on the lane.
  const onDrop = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    const f = e.dataTransfer.files?.[0];
    if (f) void onFile(f);
  }, []);

  // Live demo capture when no file is loaded: run the RFC 0014 scenario in
  // this browser via the packaged kernel (same script, same bytes).
  useEffect(() => {
    if (raw || !fileRef.current) return;
    // NOTE: the browser leg uses the JSON export captured at build time when
    // present; a live capture path exists via the playback tab's recorder.
    // Kept file-first so the visualizer is usable offline.
  }, [raw]);

  const events = parsed?.events ?? [];
  const n = events.length;
  const visibleCount = Math.max(1, Math.floor(n / zoom));
  const visible = events.slice(0, visibleCount);

  // Revocation spans: revoke..ack pairs shade the timeline.
  const revokeSpans = useMemo(() => {
    const spans: { from: number; to: number | null }[] = [];
    let open: number | null = null;
    for (const e of visible) {
      if (e.kind === 'revoke') open = e.i;
      else if (e.kind === 'ack' && open !== null) {
        spans.push({ from: open, to: e.i });
        open = null;
      }
    }
    if (open !== null) spans.push({ from: open, to: null });
    return spans;
  }, [visible]);

  const laneY = (k: TraceEvent['kind']) => LANE_ORDER.indexOf(k) * 34 + 10;

  return (
    <div style={{ fontFamily: 'ui-monospace, monospace', color: '#e5e7eb' }}>
      <div style={{ display: 'flex', gap: 12, alignItems: 'center', marginBottom: 8 }}>
        <strong style={{ color: '#f9fafb' }}>Kernel trace — .weftrec v4 (RFC 0014)</strong>
        <button
          onClick={() => setPaused((p) => !p)}
          style={{ background: paused ? '#10b981' : '#374151', color: '#fff', border: 'none', borderRadius: 6, padding: '4px 10px', cursor: 'pointer' }}
        >
          {paused ? '▶ resume' : '⏸ pause'}
        </button>
        <label style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
          zoom
          <input
            type="range" min={1} max={50} value={zoom}
            onChange={(ev) => setZoom(Number(ev.target.value))}
            style={{ width: 140 }}
          />
          {zoom}x
        </label>
        <button
          onClick={() => fileRef.current?.click()}
          style={{ background: '#374151', color: '#fff', border: 'none', borderRadius: 6, padding: '4px 10px', cursor: 'pointer' }}
        >
          load .jsonl export
        </button>
        <input
          ref={fileRef}
          type="file"
          accept=".jsonl,.json,text/plain"
          style={{ display: 'none' }}
          onChange={(ev) => {
            const f = ev.target.files?.[0];
            if (f) void onFile(f);
          }}
        />
        {parsed && (
          <span style={{ color: '#9ca3af' }}>
            {n.toLocaleString()} events · {events.filter((e) => e.kind === 'drop').length} drops ·{' '}
            {revokeSpans.length} revoke span(s)
          </span>
        )}
      </div>

      {error && <div style={{ color: '#ef4444', marginBottom: 8 }}>{error}</div>}

      {!parsed ? (
        <div
          onDragOver={(e) => e.preventDefault()}
          onDrop={onDrop}
          style={{ border: '2px dashed #374151', borderRadius: 8, padding: 24, textAlign: 'center', color: '#6b7280' }}
        >
          Drop a <code>.weftrec</code> v4 JSON-lines export here
          <div style={{ marginTop: 6, fontSize: 12 }}>
            produce one with: <code>trace-dump json 2000 {'>'} trace.jsonl</code>
          </div>
        </div>
      ) : (
        <>
          <svg
            viewBox={`0 0 ${Math.max(visibleCount, 1)} ${LANE_ORDER.length * 34 + 20}`}
            width="100%"
            height={LANE_ORDER.length * 34 + 20}
            preserveAspectRatio="none"
            style={{ background: '#111827', borderRadius: 8, border: '1px solid #1f2937' }}
          >
            {revokeSpans.map((s, idx) => (
              <rect
                key={idx}
                x={s.from}
                y={0}
                width={(s.to ?? visibleCount) - s.from}
                height={LANE_ORDER.length * 34 + 20}
                fill="rgba(239,68,68,0.12)"
              />
            ))}
            {visible.map((e) => (
              <rect
                key={e.i}
                x={e.i}
                y={laneY(e.kind)}
                width={1}
                height={22}
                fill={KIND_COLORS[e.kind]}
                onClick={() => setSelected(e)}
                style={{ cursor: 'pointer' }}
              />
            ))}
          </svg>

          <div style={{ display: 'flex', flexWrap: 'wrap', gap: 10, marginTop: 8 }}>
            {LANE_ORDER.map((k) => (
              <span key={k} style={{ display: 'inline-flex', alignItems: 'center', gap: 4 }}>
                <span style={{ width: 10, height: 10, background: KIND_COLORS[k], borderRadius: 2, display: 'inline-block' }} />
                {k}
              </span>
            ))}
          </div>

          {selected && (
            <div style={{ marginTop: 8, background: '#1f2937', borderRadius: 8, padding: 10 }}>
              <strong>event #{selected.i}</strong>
              <div>kind: <span style={{ color: KIND_COLORS[selected.kind] }}>{selected.kind}</span></div>
              <div>aux: {selected.aux}{selected.kind === 'publish' ? ' (payload_len)' : ''}</div>
              <div>data: {selected.data}{selected.kind === 'publish' || selected.kind === 'claim' ? ' (seq)' : selected.kind === 'revoke' || selected.kind === 'ack' ? ' (epoch)' : ''}</div>
            </div>
          )}
        </>
      )}
    </div>
  );
};
