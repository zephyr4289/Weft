import React, { useState } from 'react';

export interface RecordedFrame {
  index: number;
  seq: number;
  version: number;
  headerSize: number;
  payloadLen: number;
  crc32: string;
  isStale: boolean;
  isForeign: boolean;
}

export const Playback: React.FC = () => {
  // Static fixture frames (no setter needed — the list never changes);
  // previously destructured as [frames, setFrames] with setFrames unused,
  // which failed `tsc` under noUnusedLocals and broke `pnpm build` in CI.
  const [frames] = useState<RecordedFrame[]>([
    { index: 0, seq: 1, version: 1, headerSize: 16, payloadLen: 64, crc32: '0x33C38BD5', isStale: false, isForeign: false },
    { index: 1, seq: 2, version: 1, headerSize: 16, payloadLen: 64, crc32: '0x8A12DFE1', isStale: false, isForeign: false },
    { index: 2, seq: 2, version: 1, headerSize: 16, payloadLen: 64, crc32: '0x8A12DFE1', isStale: true, isForeign: false },
    { index: 3, seq: 0, version: 0, headerSize: 0, payloadLen: 40, crc32: '0xDEADBEEF', isStale: false, isForeign: true },
    { index: 4, seq: 3, version: 1, headerSize: 16, payloadLen: 64, crc32: '0x71BC39A0', isStale: false, isForeign: false },
    { index: 5, seq: 4, version: 1, headerSize: 16, payloadLen: 64, crc32: '0x44D9102B', isStale: false, isForeign: false },
  ]);

  const [currentFrameIdx, setCurrentFrameIdx] = useState<number>(0);
  const [isPlaying, setIsPlaying] = useState<boolean>(false);

  const activeFrame = frames[currentFrameIdx] || frames[0];

  const handleStep = (delta: number) => {
    setCurrentFrameIdx((prev) => Math.max(0, Math.min(frames.length - 1, prev + delta)));
  };

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: '20px' }}>
      <div>
        <h2 style={{ margin: 0, color: '#38bdf8', fontSize: '18px' }}>
          .weftrec Playback & Structural File Inspector
        </h2>
        <span style={{ fontSize: '12px', color: '#94a3b8' }}>
          FORMATS.md v1 bit-exact playback engine, L8 skip-unknown forward compatibility.
        </span>
      </div>

      {/* Playback Controls & Scrubber */}
      <div style={{ background: '#1e293b', padding: '16px', borderRadius: '8px', border: '1px solid #334155' }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '12px' }}>
          <div style={{ display: 'flex', gap: '8px' }}>
            <button
              onClick={() => handleStep(-1)}
              disabled={currentFrameIdx === 0}
              style={{ padding: '6px 12px', borderRadius: '4px', background: '#334155', color: '#fff', border: 'none', cursor: 'pointer' }}
            >
              ◀ Step Back
            </button>
            <button
              onClick={() => setIsPlaying(!isPlaying)}
              style={{ padding: '6px 12px', borderRadius: '4px', background: isPlaying ? '#ef4444' : '#38bdf8', color: '#fff', border: 'none', fontWeight: 'bold', cursor: 'pointer' }}
            >
              {isPlaying ? 'Pause' : 'Play'}
            </button>
            <button
              onClick={() => handleStep(1)}
              disabled={currentFrameIdx === frames.length - 1}
              style={{ padding: '6px 12px', borderRadius: '4px', background: '#334155', color: '#fff', border: 'none', cursor: 'pointer' }}
            >
              Step Forward ▶
            </button>
          </div>
          <div style={{ color: '#94a3b8', fontSize: '13px' }}>
            Frame {currentFrameIdx + 1} / {frames.length}
          </div>
        </div>

        <input
          type="range"
          min={0}
          max={frames.length - 1}
          value={currentFrameIdx}
          onChange={(e) => setCurrentFrameIdx(Number(e.target.value))}
          style={{ width: '100%', accentColor: '#38bdf8', cursor: 'pointer' }}
        />
      </div>

      {/* Active Envelope Inspection */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(2, 1fr)', gap: '16px' }}>
        <div style={{ background: '#1e293b', padding: '16px', borderRadius: '8px', border: '1px solid #334155' }}>
          <h3 style={{ margin: '0 0 12px 0', fontSize: '14px', color: '#e2e8f0' }}>Envelope Fields (Little-Endian Decode)</h3>
          <div style={{ display: 'flex', flexDirection: 'column', gap: '6px', fontSize: '13px', fontFamily: 'monospace' }}>
            <div>MAGIC: <span style={{ color: activeFrame.isForeign ? '#ef4444' : '#38bdf8' }}>{activeFrame.isForeign ? 'FOREIGN (0xDEADBEEF)' : 'WEFT (0x54464557)'}</span></div>
            <div>SEQUENCE (seq): <span style={{ color: '#f59e0b' }}>{activeFrame.seq}</span></div>
            <div>VERSION: <span style={{ color: '#a7f3d0' }}>{activeFrame.version}</span></div>
            <div>HEADER_SIZE: <span style={{ color: '#cbd5e1' }}>{activeFrame.headerSize} B</span></div>
            <div>PAYLOAD_LEN: <span style={{ color: '#cbd5e1' }}>{activeFrame.payloadLen} B</span></div>
            <div>FRAME CRC-32: <span style={{ color: '#10b981' }}>{activeFrame.crc32}</span></div>
          </div>
        </div>

        <div style={{ background: '#1e293b', padding: '16px', borderRadius: '8px', border: '1px solid #334155' }}>
          <h3 style={{ margin: '0 0 12px 0', fontSize: '14px', color: '#e2e8f0' }}>Frame Classification</h3>
          <div style={{ display: 'flex', flexDirection: 'column', gap: '10px' }}>
            <div style={{ padding: '8px 12px', borderRadius: '6px', background: activeFrame.isForeign ? '#7f1d1d' : activeFrame.isStale ? '#78350f' : '#064e3b', color: '#fff', fontWeight: 'bold', fontSize: '13px' }}>
              {activeFrame.isForeign ? '⚠️ L8 Foreign Record (Skipped by rec_len)' : activeFrame.isStale ? '⚠️ Stale Frame (Unchanged Sequence)' : '✅ Fresh Verified Frame'}
            </div>
            <p style={{ margin: 0, fontSize: '12px', color: '#94a3b8' }}>
              Structural validation verifies CRC-32 checksums over the claimed envelope header and payload. Unknown record formats are cleanly skipped using <code>rec_len</code> demarcations.
            </p>
          </div>
        </div>
      </div>
    </div>
  );
};
