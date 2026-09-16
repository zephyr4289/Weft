import React, { useState, useEffect, useRef } from 'react';
import { Weft } from '@weft/core';
import { FanoutTimeline } from './FanoutTimeline';

export interface TelemetrySample {
  timestamp: number;
  latest: number;
  w_work: number;
  r_work: number;
  t_publish: number;
  t_claim: number;
  t_drop: number;
  writer_fps: number;
  reader_fps: number;
  epoch: number;
  revoked: boolean;
  /** Per-slot envelope seq sampled from debugView (advisory). */
  bufSeqs: [number, number, number];
  midPublishSample: boolean;
}

interface InspectorProps {
  /** The LIVE Weft instance this inspector observes (debugView accessor). */
  weft: Weft;
}

/**
 * Telemetry Inspector — reads the sanctioned debugView() accessor of a LIVE
 * Weft every 100 ms. Every number on this tab comes from the kernel (AXIOM T:
 * advisory, read-only); the previous revision simulated sine-wave jitter with
 * setInterval and hardcoded "Steward reclaim sweep OK" strings — pure theater.
 * writer/reader FPS are DERIVED from real t_publish/t_claim deltas between
 * consecutive samples; reclaim events log actual epoch transitions observed
 * in the debug view, not fabricated ones.
 */
export const Inspector: React.FC<InspectorProps> = ({ weft }) => {
  const [history, setHistory] = useState<TelemetrySample[]>([]);
  const [currentSample, setCurrentSample] = useState<TelemetrySample | null>(null);
  const [isAttached, setIsAttached] = useState<boolean>(true);
  const [reclaimEvents, setReclaimEvents] = useState<string[]>([]);
  const prevRef = useRef<{ publish: bigint; claim: bigint; epoch: number } | null>(null);

  useEffect(() => {
    if (!isAttached) return;

    const interval = setInterval(() => {
      // THE kernel accessor — read-only, wait-free, allocation-free-ish
      // (cold path; never called from the hot loop per Law 2).
      const v = weft.debugView();
      const prev = prevRef.current;

      // Real FPS: delta of the advisory counters over the sample window.
      const dt = prev ? 0.1 : 0; // interval is 100 ms
      const writerFps = prev && dt > 0
        ? Math.max(0, Math.round(Number(v.tPublish - prev.publish) / dt))
        : 0;
      const readerFps = prev && dt > 0
        ? Math.max(0, Math.round(Number(v.tClaim - prev.claim) / dt))
        : 0;

      prevRef.current = { publish: v.tPublish, claim: v.tClaim, epoch: v.epoch };

      if (prev && v.epoch !== prev.epoch) {
        setReclaimEvents((r) => [
          `[${new Date().toLocaleTimeString()}] Epoch ${prev.epoch} → ${v.epoch} (writer ACK observed by kernel)`,
          ...r.slice(0, 4),
        ]);
      }

      const sample: TelemetrySample = {
        timestamp: Date.now(),
        latest: v.latest,
        w_work: v.wWork,
        r_work: v.rWork,
        t_publish: Number(v.tPublish),
        t_claim: Number(v.tClaim),
        t_drop: Number(v.tDrop),
        writer_fps: writerFps,
        reader_fps: readerFps,
        epoch: v.epoch,
        revoked: v.revoked,
        bufSeqs: [v.bufs[0].seq, v.bufs[1].seq, v.bufs[2].seq],
        midPublishSample: v.midPublishSample,
      };

      setCurrentSample(sample);
      setHistory((h) => [...h.slice(-40), sample]);
    }, 100);

    return () => clearInterval(interval);
  }, [isAttached, weft]);

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '20px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
        <div>
          <h2 style={{ margin: 0, color: '#38bdf8', fontSize: '18px' }}>
            Telemetry Inspector — Live debugView() Accessor
          </h2>
          <span style={{ fontSize: '12px', color: '#94a3b8' }}>
            AXIOM T: every value below is a real kernel sample (advisory, read-only). FPS derived from t_publish/t_claim deltas.
          </span>
        </div>
        <button
          onClick={() => setIsAttached(!isAttached)}
          style={{
            padding: '8px 16px',
            borderRadius: '6px',
            border: 'none',
            background: isAttached ? '#ef4444' : '#22c55e',
            color: '#fff',
            fontWeight: 'bold',
            cursor: 'pointer',
          }}
        >
          {isAttached ? 'Detach Inspector' : 'Attach Live Stream'}
        </button>
      </div>

      {/* Primary Metrics Grid */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(4, 1fr)', gap: '16px' }}>
        <div style={{ background: '#1e293b', padding: '16px', borderRadius: '8px', border: '1px solid #334155' }}>
          <div style={{ fontSize: '12px', color: '#94a3b8' }}>LATEST / W_WORK / R_WORK</div>
          <div style={{ fontSize: '20px', fontWeight: 'bold', color: '#38bdf8', marginTop: '4px' }}>
            {currentSample ? `${currentSample.latest} / ${currentSample.w_work} / ${currentSample.r_work}` : '--'}
          </div>
        </div>
        <div style={{ background: '#1e293b', padding: '16px', borderRadius: '8px', border: '1px solid #334155' }}>
          <div style={{ fontSize: '12px', color: '#94a3b8' }}>WRITER vs READER FPS (from counters)</div>
          <div style={{ fontSize: '20px', fontWeight: 'bold', color: '#f59e0b', marginTop: '4px' }}>
            {currentSample ? `${currentSample.writer_fps} W / ${currentSample.reader_fps} R` : '--'}
          </div>
        </div>
        <div style={{ background: '#1e293b', padding: '16px', borderRadius: '8px', border: '1px solid #334155' }}>
          <div style={{ fontSize: '12px', color: '#94a3b8' }}>T_DROP (kernel counter)</div>
          <div style={{ fontSize: '20px', fontWeight: 'bold', color: '#ec4899', marginTop: '4px' }}>
            {currentSample ? `${currentSample.t_drop} drops` : '--'}
          </div>
        </div>
        <div style={{ background: '#1e293b', padding: '16px', borderRadius: '8px', border: '1px solid #334155' }}>
          <div style={{ fontSize: '12px', color: '#94a3b8' }}>EPOCH / REVOKED</div>
          <div style={{ fontSize: '20px', fontWeight: 'bold', color: currentSample?.revoked ? '#ef4444' : '#10b981', marginTop: '4px' }}>
            {currentSample ? `Epoch ${currentSample.epoch}${currentSample.revoked ? ' (REVOKED)' : ' (OK)'}` : '--'}
          </div>
        </div>
      </div>

      {/* Strip Chart: Slot Transitions */}
      <div style={{ background: '#1e293b', padding: '16px', borderRadius: '8px', border: '1px solid #334155' }}>
        <h3 style={{ margin: '0 0 12px 0', fontSize: '14px', color: '#e2e8f0' }}>
          Live Slot Index Transition Timeline (Last 40 Samples)
        </h3>
        <div style={{ display: 'flex', alignItems: 'flex-end', height: '80px', gap: '4px', background: '#0f172a', padding: '8px', borderRadius: '4px' }}>
          {history.map((s, idx) => (
            <div key={idx} style={{ flex: 1, display: 'flex', flexDirection: 'column', gap: '2px', height: '100%', justifyContent: 'flex-end' }}>
              <div style={{ height: `${(s.latest + 1) * 20}%`, background: '#38bdf8', borderRadius: '2px' }} title={`Latest: ${s.latest}`} />
              <div style={{ height: `${(s.w_work + 1) * 20}%`, background: '#f59e0b', borderRadius: '2px' }} title={`W_Work: ${s.w_work}`} />
            </div>
          ))}
        </div>
        <div style={{ display: 'flex', gap: '16px', marginTop: '8px', fontSize: '12px', color: '#94a3b8' }}>
          <span style={{ display: 'flex', alignItems: 'center', gap: '6px' }}><span style={{ width: '10px', height: '10px', background: '#38bdf8', display: 'inline-block' }} /> Latest Slot</span>
          <span style={{ display: 'flex', alignItems: 'center', gap: '6px' }}><span style={{ width: '10px', height: '10px', background: '#f59e0b', display: 'inline-block' }} /> Writer Work Slot</span>
        </div>
        <div style={{ marginTop: '10px', fontSize: '12px', fontFamily: 'monospace', color: '#94a3b8' }}>
          {currentSample && (
            <>slot seqs: [{currentSample.bufSeqs.join(', ')}] · mid-publish sample: {String(currentSample.midPublishSample)}</>
          )}
        </div>
      </div>

      {/* Epoch Handshake Log — REAL epoch transitions only */}
      <div style={{ background: '#1e293b', padding: '16px', borderRadius: '8px', border: '1px solid #334155' }}>
        <h3 style={{ margin: '0 0 8px 0', fontSize: '14px', color: '#e2e8f0' }}>
          Epoch Handshake Log (observed, not simulated)
        </h3>
        {reclaimEvents.length === 0 ? (
          <div style={{ color: '#64748b', fontSize: '13px' }}>Awaiting epoch transitions...</div>
        ) : (
          <div style={{ display: 'flex', flexDirection: 'column', gap: '4px', fontFamily: 'monospace', fontSize: '12px', color: '#a7f3d0' }}>
            {reclaimEvents.map((ev, i) => (
              <div key={i}>{ev}</div>
            ))}
          </div>
        )}
      </div>

      {/* Series 6: the post-mortem counterpart — .weftrec v2 fan-out
          flight-recorder captures (daemon output) with the telescoping
          badge the roadmap named. */}
      <FanoutTimeline />
    </div>
  );
};
