import React, { useMemo, useState } from 'react';
import {
  parseWeftrecV2,
  fanoutTimeline,
  WeftrecV2ParseResult,
  FanoutTimelineSegment,
} from '../lib/weftrec';

/**
 * FanoutTimeline — the .weftrec v2 post-mortem view of the Inspector
 * (Series 6): load a fan-out flight-recorder capture (produced by
 * `tools/weft-fanout-rec capture|daemon --shm ...` — any port's ring) and
 * see the recorder's session as a timeline: claimed-frame runs (blue) and
 * the drop gaps it honestly recorded (red), with the telescoping badge the
 * lead's roadmap named — sum(dropped) == final_seq - records (RFC 0004
 * per-reader accounting, FORMATS.md §1.5).
 *
 * This is the counterpart of the LIVE telemetry above it: the live view
 * samples debugView(); this view loads the file the daemon wrote. Every
 * number comes from the parse (no mock data — the demo culture rule).
 */
export const FanoutTimeline: React.FC = () => {
  const [parse, setParse] = useState<WeftrecV2ParseResult | null>(null);
  const [fileName, setFileName] = useState<string>('');
  const [error, setError] = useState<string | null>(null);

  const segments: FanoutTimelineSegment[] = useMemo(
    () => (parse ? fanoutTimeline(parse.records) : []),
    [parse],
  );

  const onFile = async (f: File | null) => {
    if (!f) return;
    setError(null);
    try {
      const bytes = new Uint8Array(await f.arrayBuffer());
      setParse(parseWeftrecV2(bytes));
      setFileName(f.name);
    } catch (e) {
      setParse(null);
      setFileName('');
      setError(e instanceof Error ? e.message : String(e));
    }
  };

  // Timeline layout: each segment becomes a flex-grow block sized by its
  // claim/drop count; claims blue, drops red. Bounded to the last 500
  // segments so pathological files can't blow up the DOM (honest bound,
  // stated in the UI).
  const MAX_SEGS = 500;
  const shown = segments.length > MAX_SEGS ? segments.slice(-MAX_SEGS) : segments;
  const totalCount = useMemo(
    () => shown.reduce((a, s) => a + s.count, 0),
    [shown],
  );

  const badge = (ok: boolean, label: string) => (
    <span
      style={{
        display: 'inline-block', padding: '2px 10px', borderRadius: '999px',
        fontSize: '12px', fontWeight: 600, marginLeft: '8px',
        background: ok ? 'rgba(16,185,129,0.15)' : 'rgba(239,68,68,0.15)',
        color: ok ? '#10b981' : '#ef4444',
        border: `1px solid ${ok ? '#10b981' : '#ef4444'}`,
      }}
    >
      {ok ? '✓' : '✗'} {label}
    </span>
  );

  return (
    <div style={{ background: '#1e293b', padding: '16px', borderRadius: '8px', border: '1px solid #334155' }}>
      <h3 style={{ margin: '0 0 4px 0', fontSize: '14px', color: '#e2e8f0' }}>
        Fan-Out Flight-Recorder Timeline (.weftrec v2)
      </h3>
      <div style={{ fontSize: '12px', color: '#94a3b8', marginBottom: '10px' }}>
        Load a capture from <code>weft-fanout-rec capture|daemon --shm …</code> — claimed runs in blue, recorded
        drop gaps in red. Telescoping: sum(dropped) == final_seq − records (RFC 0004).
      </div>

      <input
        type="file"
        accept=".weftrec"
        onChange={(e) => onFile(e.target.files?.[0] ?? null)}
        style={{ fontSize: '13px', color: '#cbd5e1', marginBottom: '12px' }}
        aria-label="Load a .weftrec v2 capture file"
      />

      {error && (
        <div style={{ color: '#ef4444', fontSize: '13px', fontFamily: 'monospace' }}>
          {error}
        </div>
      )}

      {parse && !error && (
        <>
          <div style={{ display: 'flex', flexWrap: 'wrap', gap: '8px 16px', fontSize: '12px', color: '#94a3b8', marginBottom: '8px' }}>
            <span style={{ fontFamily: 'monospace', color: '#e2e8f0' }}>{fileName}</span>
            <span>records: <b style={{ color: '#38bdf8' }}>{parse.records.length}</b></span>
            <span>final seq: <b style={{ color: '#38bdf8' }}>{parse.records[parse.records.length - 1]?.seq ?? 0}</b></span>
            <span>sum(dropped): <b style={{ color: '#f59e0b' }}>{parse.sumDropped}</b></span>
            <span>
              telescoping
              {badge(parse.telescopingOk, `sum(dropped) ${parse.sumDropped} = seq − ${parse.records.length}`)}
            </span>
            <span>seqs strict {badge(parse.seqsIncreasing, 'increasing')}</span>
            <span>per-record gaps {badge(parse.perRecordGapsOk, 'dropped_i = gap_i')}</span>
            {parse.truncated && <span style={{ color: '#ef4444' }}>⚠ truncated (frame_count {'>'} records — scan path)</span>}
            {segments.length > MAX_SEGS && <span>showing last {MAX_SEGS}/{segments.length} segments</span>}
          </div>

          <div
            style={{
              display: 'flex', height: '48px', gap: '1px', background: '#0f172a',
              padding: '6px', borderRadius: '4px', overflow: 'hidden',
            }}
            role="img"
            aria-label={`Fan-out timeline: ${parse.records.length} claims, ${parse.sumDropped} dropped`}
          >
            {shown.map((s, i) => (
              <div
                key={i}
                title={`${s.kind === 'claim' ? 'claims' : 'dropped'} seq ${s.seq}..${s.seq + s.count - 1} (${s.count})`}
                style={{
                  flexGrow: Math.max(s.count, 1),
                  flexBasis: 0,
                  background: s.kind === 'claim' ? '#38bdf8' : '#ef4444',
                  opacity: s.kind === 'claim' ? 0.85 : 0.9,
                  borderRadius: '1px',
                  minWidth: s.count > 0 && totalCount > 0 && s.count / totalCount > 0 ? 1 : 0.5,
                }}
              />
            ))}
          </div>
          <div style={{ display: 'flex', gap: '16px', marginTop: '6px', fontSize: '12px', color: '#94a3b8' }}>
            <span style={{ display: 'flex', alignItems: 'center', gap: '6px' }}>
              <span style={{ width: '10px', height: '10px', background: '#38bdf8', display: 'inline-block' }} /> claimed (recorder observed)
            </span>
            <span style={{ display: 'flex', alignItems: 'center', gap: '6px' }}>
              <span style={{ width: '10px', height: '10px', background: '#ef4444', display: 'inline-block' }} /> dropped (accounted, never hidden)
            </span>
          </div>
        </>
      )}

      {!parse && !error && (
        <div style={{ color: '#64748b', fontSize: '13px' }}>
          Awaiting a capture file… (the live telemetry above is the in-session view; this is the post-mortem)
        </div>
      )}
    </div>
  );
};
