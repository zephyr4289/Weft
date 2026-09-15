import React, { useState, useEffect, useRef } from 'react';
import { Weft } from '@weft/core';
import { WORKLOAD_INFO, WorkloadMetadata } from './workloads/generators';
import { drawWorkload } from './workloads/draw';
import { createModeRunner, ModeType, ModeRunner } from './modes/runner';
import { Inspector } from './components/Inspector';
import { Playback } from './components/Playback';

type TabType = 'showcase' | 'inspector' | 'playback';

export const App: React.FC = () => {
  const [activeTab, setActiveTab] = useState<TabType>('showcase');
  const [selectedWid, setSelectedWid] = useState<string>('W1');
  const [selectedMode, setSelectedMode] = useState<ModeType>('C');
  const [isRunning, setIsRunning] = useState<boolean>(true);

  const [fps, setFps] = useState<number>(60);
  const [p50, setP50] = useState<number>(16.6);
  const [p99, setP99] = useState<number>(16.7);
  const [tDrop, setTDrop] = useState<number>(0);
  const [heapMb, setHeapMb] = useState<number>(0);

  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const runnerRef = useRef<ModeRunner | null>(null);
  const frameTimesRef = useRef<number[]>([]);
  const frameIdxRef = useRef<number>(0);
  const consumeBufRef = useRef<Float32Array | null>(null);

  // Live kernel instance backing the Inspector and Playback tabs (previously
  // those tabs were hardcoded mocks; they now observe/record a REAL Weft).
  const [inspectorWeft, setInspectorWeft] = useState<Weft | null>(null);
  useEffect(() => {
    const w = new Weft(256);
    setInspectorWeft(w);
    let seq = 0;
    let raf = 0;
    let t0 = performance.now();
    const produce = (now: number) => {
      const f32 = w.wBeginFloat32();
      f32[0] = Math.sin((now - t0) * 0.002);
      f32[1] = Math.cos((now - t0) * 0.0013);
      f32[2] = (now - t0) / 1000;
      w.publish(++seq, 256);
      raf = requestAnimationFrame(produce);
    };
    raf = requestAnimationFrame(produce);
    return () => cancelAnimationFrame(raf);
  }, []);

  const currentWorkload: WorkloadMetadata = WORKLOAD_INFO[selectedWid] || WORKLOAD_INFO['W1'];

  // Hot-switch runner on workload or mode change
  useEffect(() => {
    if (runnerRef.current) {
      runnerRef.current.dispose();
    }
    const floatCount = currentWorkload.floatCount;
    consumeBufRef.current = new Float32Array(floatCount);
    runnerRef.current = createModeRunner(selectedMode, selectedWid, floatCount);
    frameTimesRef.current = [];
    frameIdxRef.current = 0;
  }, [selectedWid, selectedMode, currentWorkload.floatCount]);

  // Main animation loop
  useEffect(() => {
    let animationId: number;
    let lastTime = performance.now();

    const loop = (now: number) => {
      if (isRunning && runnerRef.current && canvasRef.current && consumeBufRef.current) {
        const delta = now - lastTime;
        lastTime = now;
        frameTimesRef.current.push(delta);
        if (frameTimesRef.current.length > 60) {
          frameTimesRef.current.shift();
        }

        // 1. Produce frame
        frameIdxRef.current++;
        runnerRef.current.produceFrame(frameIdxRef.current);

        // 2. Consume frame (Paint Phase)
        const ok = runnerRef.current.consumeFrame(consumeBufRef.current);
        if (ok) {
          const ctx = canvasRef.current.getContext('2d');
          if (ctx) {
            drawWorkload(
              ctx,
              canvasRef.current.width,
              canvasRef.current.height,
              selectedWid,
              consumeBufRef.current
            );
          }
        }

        // Compute metrics every 15 frames
        if (frameIdxRef.current % 15 === 0 && frameTimesRef.current.length > 0) {
          const sorted = [...frameTimesRef.current].sort((a, b) => a - b);
          const currentP50 = sorted[Math.floor(sorted.length * 0.5)];
          const currentP99 = sorted[Math.floor(sorted.length * 0.99)];
          setP50(Number(currentP50.toFixed(1)));
          setP99(Number(currentP99.toFixed(1)));
          setFps(Math.round(1000 / (currentP50 || 16.6)));
          setTDrop(runnerRef.current.getDropCount());

          // Estimate heap if performance.memory is available
          if ((performance as any).memory) {
            const used = (performance as any).memory.usedJSHeapSize / (1024 * 1024);
            setHeapMb(Number(used.toFixed(1)));
          }
        }
      }
      animationId = requestAnimationFrame(loop);
    };

    animationId = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(animationId);
  }, [isRunning, selectedWid, selectedMode]);

  return (
    <div style={{ maxWidth: '1000px', margin: '0 auto', padding: '24px' }}>
      <header style={{ marginBottom: '24px', borderBottom: '1px solid #334155', paddingBottom: '16px' }}>
        <h1 style={{ margin: '0 0 8px 0', fontSize: '24px', color: '#38bdf8' }}>
          Weft Protocol Suite — Showcase & DevTools
        </h1>
        <div style={{ display: 'flex', gap: '8px', marginTop: '16px' }}>
          <button
            onClick={() => setActiveTab('showcase')}
            style={{
              padding: '8px 16px',
              borderRadius: '6px',
              border: 'none',
              background: activeTab === 'showcase' ? '#0284c7' : '#1e293b',
              color: '#fff',
              cursor: 'pointer',
              fontWeight: activeTab === 'showcase' ? 'bold' : 'normal',
            }}
          >
            Workload Matrix (W1–W5)
          </button>
          <button
            onClick={() => setActiveTab('inspector')}
            style={{
              padding: '8px 16px',
              borderRadius: '6px',
              border: 'none',
              background: activeTab === 'inspector' ? '#0284c7' : '#1e293b',
              color: '#fff',
              cursor: 'pointer',
              fontWeight: activeTab === 'inspector' ? 'bold' : 'normal',
            }}
          >
            Telemetry Inspector
          </button>
          <button
            onClick={() => setActiveTab('playback')}
            style={{
              padding: '8px 16px',
              borderRadius: '6px',
              border: 'none',
              background: activeTab === 'playback' ? '#0284c7' : '#1e293b',
              color: '#fff',
              cursor: 'pointer',
              fontWeight: activeTab === 'playback' ? 'bold' : 'normal',
            }}
          >
            .weftrec Playback
          </button>
        </div>
      </header>

      {activeTab === 'inspector' && inspectorWeft && <Inspector weft={inspectorWeft} />}
      {activeTab === 'playback' && inspectorWeft && <Playback weft={inspectorWeft} />}

      {activeTab === 'showcase' && (
        <>
          {/* Control Bar */}
          <div style={{ display: 'flex', gap: '16px', flexWrap: 'wrap', marginBottom: '20px' }}>
            <div>
              <label style={{ display: 'block', fontSize: '12px', color: '#94a3b8', marginBottom: '4px' }}>
                Workload (W1–W5)
              </label>
              <select
                value={selectedWid}
                onChange={(e) => setSelectedWid(e.target.value)}
                style={{
                  background: '#1e293b',
                  color: '#f8fafc',
                  border: '1px solid #475569',
                  padding: '8px 12px',
                  borderRadius: '6px',
                }}
              >
                {Object.keys(WORKLOAD_INFO).map((k) => (
                  <option key={k} value={k}>
                    {WORKLOAD_INFO[k].title}
                  </option>
                ))}
              </select>
            </div>

            <div>
              <label style={{ display: 'block', fontSize: '12px', color: '#94a3b8', marginBottom: '4px' }}>
                Buffer Protocol Mode (A/B/C/D)
              </label>
              <div style={{ display: 'flex', gap: '4px' }}>
                {(['A', 'B', 'C', 'D'] as ModeType[]).map((m) => (
                  <button
                    key={m}
                    onClick={() => setSelectedMode(m)}
                    style={{
                      background: selectedMode === m ? '#0284c7' : '#1e293b',
                      color: '#f8fafc',
                      border: '1px solid #475569',
                      padding: '8px 16px',
                      borderRadius: '6px',
                      cursor: 'pointer',
                      fontWeight: selectedMode === m ? 'bold' : 'normal',
                    }}
                  >
                    Mode {m}
                  </button>
                ))}
              </div>
            </div>

            <div style={{ display: 'flex', alignItems: 'flex-end' }}>
              <button
                onClick={() => setIsRunning(!isRunning)}
                style={{
                  background: isRunning ? '#ef4444' : '#22c55e',
                  color: '#f8fafc',
                  border: 'none',
                  padding: '8px 20px',
                  borderRadius: '6px',
                  cursor: 'pointer',
                  fontWeight: 'bold',
                }}
              >
                {isRunning ? 'Pause' : 'Resume'}
              </button>
            </div>
          </div>

          {/* Mode Description Banner */}
          <div
            style={{
              background: '#1e293b',
              borderLeft: '4px solid #38bdf8',
              padding: '12px 16px',
              marginBottom: '20px',
              borderRadius: '0 6px 6px 0',
              fontSize: '13px',
            }}
          >
            <strong>
              Active: Workload {selectedWid} · Mode {selectedMode}{' '}
              {selectedMode === 'A' && '(Reactive Naive — per-frame allocation)'}
              {selectedMode === 'B' && '(Pooled Best-Practice — main-thread pool)'}
              {selectedMode === 'C' && '(Weft Kernel — zero-copy atomic exchange)'}
              {selectedMode === 'D' && '(Hand-Rolled Triple-Buffer with I6)'}
            </strong>
            <div style={{ color: '#94a3b8', marginTop: '4px' }}>{currentWorkload.description}</div>
          </div>

          {/* Canvas Viewport */}
          <div
            style={{
              background: '#020617',
              border: '1px solid #334155',
              borderRadius: '8px',
              overflow: 'hidden',
              position: 'relative',
              marginBottom: '20px',
            }}
          >
            <canvas ref={canvasRef} width={950} height={360} style={{ display: 'block', width: '100%', height: '360px' }} />

            {/* Burned-in telemetry overlay */}
            <div
              style={{
                position: 'absolute',
                top: '12px',
                right: '12px',
                background: 'rgba(15, 23, 42, 0.85)',
                border: '1px solid #475569',
                padding: '8px 12px',
                borderRadius: '6px',
                fontSize: '12px',
                fontFamily: 'monospace',
              }}
            >
              <div>FPS: <span style={{ color: fps >= 55 ? '#4ade80' : '#f87171' }}>{fps}</span></div>
              <div>p50: {p50} ms | p99: {p99} ms</div>
              <div>Drops (t_drop): {tDrop}</div>
              {heapMb > 0 && <div>JS Heap: {heapMb} MB</div>}
            </div>
          </div>
        </>
      )}

      {/* Honesty Disclosure Footer */}
      <footer
        style={{
          background: '#1e293b',
          padding: '16px',
          borderRadius: '8px',
          fontSize: '12px',
          color: '#94a3b8',
          borderTop: '1px solid #334155',
          marginTop: '20px',
        }}
      >
        <div style={{ fontWeight: 'bold', color: '#f8fafc', marginBottom: '4px' }}>
          Environment Tag: chromium / linux-sandbox (or host browser)
        </div>
        <div>
          <strong>Honesty Disclosure</strong>: Hardware frame pacing and thermal persistence on physical consumer devices
          are unverified by design in this program. All metrics represent host execution. Modes A and B exhibit expected
          GC allocation pressure under high particle/matrix volume.
        </div>
      </footer>
    </div>
  );
};
export default App;
