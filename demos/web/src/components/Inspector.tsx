import React, { useState, useEffect, useRef } from 'react';

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
}

export const Inspector: React.FC = () => {
  const [history, setHistory] = useState<TelemetrySample[]>([]);
  const [currentSample, setCurrentSample] = useState<TelemetrySample | null>(null);
  const [isAttached, setIsAttached] = useState<boolean>(true);
  const [reclaimEvents, setReclaimEvents] = useState<string[]>([]);
  const sampleRef = useRef<number>(0);

  useEffect(() => {
    if (!isAttached) return;

    let wFps = 120;
    let rFps = 60;
    let tDrop = 0;
    let seq = 0;
    let latest = 0;
    let wWork = 1;
    let rWork = 2;
    let epoch = 0;

    const interval = setInterval(() => {
      sampleRef.current++;
      seq += 2;
      latest = (latest + 1) % 3;
      wWork = (wWork + 1) % 3;
      rWork = (rWork + 1) % 3;

      // Simulate slight jitter in disparity
      wFps = 120 + Math.floor(Math.sin(sampleRef.current * 0.1) * 4);
      rFps = 60 + Math.floor(Math.cos(sampleRef.current * 0.1) * 2);
      if (sampleRef.current % 25 === 0) {
        tDrop += 1;
      }

      if (sampleRef.current % 100 === 0) {
        epoch++;
        setReclaimEvents((prev) => [
          `[${new Date().toLocaleTimeString()}] Epoch ${epoch}: Steward reclaim sweep OK (0 leaked handles)`,
          ...prev.slice(0, 4),
        ]);
      }

      const sample: TelemetrySample = {
        timestamp: Date.now(),
        latest,
        w_work: wWork,
        r_work: rWork,
        t_publish: seq,
        t_claim: Math.floor(seq / 2),
        t_drop: tDrop,
        writer_fps: wFps,
        reader_fps: rFps,
        epoch,
        revoked: false,
      };

      setCurrentSample(sample);
      setHistory((prev) => [...prev.slice(-40), sample]);
    }, 100);

    return () => clearInterval(interval);
  }, [isAttached]);

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '20px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
        <div>
          <h2 style={{ margin: 0, color: '#38bdf8', fontSize: '18px' }}>
            Telemetry Inspector — Sanctioned Kernel Debug Accessor
          </h2>
          <span style={{ fontSize: '12px', color: '#94a3b8' }}>
            AXIOM T: Telemetry is strictly advisory and read-only. Zero write syscalls.
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
          <div style={{ fontSize: '12px', color: '#94a3b8' }}>WRITER vs READER FPS</div>
          <div style={{ fontSize: '20px', fontWeight: 'bold', color: '#f59e0b', marginTop: '4px' }}>
            {currentSample ? `${currentSample.writer_fps} W / ${currentSample.reader_fps} R` : '--'}
          </div>
        </div>
        <div style={{ background: '#1e293b', padding: '16px', borderRadius: '8px', border: '1px solid #334155' }}>
          <div style={{ fontSize: '12px', color: '#94a3b8' }}>T_DROP (ACCUMULATED)</div>
          <div style={{ fontSize: '20px', fontWeight: 'bold', color: '#ec4899', marginTop: '4px' }}>
            {currentSample ? `${currentSample.t_drop} drops` : '--'}
          </div>
        </div>
        <div style={{ background: '#1e293b', padding: '16px', borderRadius: '8px', border: '1px solid #334155' }}>
          <div style={{ fontSize: '12px', color: '#94a3b8' }}>STEWARD EPOCH / STATE</div>
          <div style={{ fontSize: '20px', fontWeight: 'bold', color: '#10b981', marginTop: '4px' }}>
            {currentSample ? `Epoch ${currentSample.epoch} (OK)` : '--'}
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
      </div>

      {/* Steward GC Leak Traces */}
      <div style={{ background: '#1e293b', padding: '16px', borderRadius: '8px', border: '1px solid #334155' }}>
        <h3 style={{ margin: '0 0 8px 0', fontSize: '14px', color: '#e2e8f0' }}>
          Steward GC Reclaim Events & Epoch Handshake Log
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
    </div>
  );
};
