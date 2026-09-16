import React, { useState, useEffect, useRef, useCallback } from 'react';
import { Weft, WEFT_VERSION_1 } from '@weft/core';
import { parseWeftrec, writeWeftrec, WeftrecParseResult, WeftrecRecord } from '../lib/weftrec';

export interface RecordedFrame {
  index: number;
  seq: number;
  version: number;
  headerSize: number;
  payloadLen: number;
  magic: number;
  crc32: string;
  isStale: boolean;
  isForeign: boolean;
  kind: WeftrecRecord['kind'];
}

interface PlaybackProps {
  /** The LIVE Weft to record from (claims real frames, serializes real bytes). */
  weft: Weft;
}

/**
 * .weftrec Playback — REAL bit-exact capture and replay.
 *
 * Flow: record N frames from the live Weft (claim → serialize envelope+payload
 * verbatim per FORMATS.md §1.2) → write a .weftrec byte stream → parse it back
 * with the bit-exact parser (CRC-32/zlib over header and per-record) → scrub
 * and inspect the parsed records. Also accepts external .weftrec files (e.g.
 * litmus/evidence/soak-b2/*.weftrec from the C/Rust recorders) via upload —
 * cross-language round-trip, exactly what the format promises.
 */
export const Playback: React.FC<PlaybackProps> = ({ weft }) => {
  const [frames, setFrames] = useState<RecordedFrame[]>([]);
  const [fileInfo, setFileInfo] = useState<string>('');
  const [currentFrameIdx, setCurrentFrameIdx] = useState<number>(0);
  const [isPlaying, setIsPlaying] = useState<boolean>(false);
  const [recordCount, setRecordCount] = useState<number>(64);
  const fileInputRef = useRef<HTMLInputElement | null>(null);

  const ingest = useCallback((bytes: Uint8Array, label: string) => {
    const parsed: WeftrecParseResult = parseWeftrec(bytes);
    const mapped: RecordedFrame[] = parsed.records.map((r) => ({
      index: r.index,
      seq: r.seq,
      version: r.version,
      headerSize: r.headerSize,
      payloadLen: r.payloadLen,
      magic: r.magic,
      crc32: `0x${r.payloadCrc.toString(16).toUpperCase().padStart(8, '0')}`,
      isStale: r.kind === 'stale',
      isForeign: r.kind === 'foreign',
      kind: r.kind,
    }));
    setFrames(mapped);
    setCurrentFrameIdx(0);
    setFileInfo(
      `${label} · v${parsed.header.formatVersion} · envelope v${parsed.header.envelopeVersion} · ` +
      `header CRC ${parsed.header.headerCrcOk ? 'OK' : 'BAD'} · decoded ${parsed.decodedCount}/${parsed.records.length}` +
      (parsed.truncated ? ' · TRUNCATED (crash-tolerant scan)' : '')
    );
  }, []);

  // Record REAL frames from the live Weft on mount / recordCount change.
  useEffect(() => {
    const payloadMax = weft.payloadMax;
    const captured: Array<{ header: Uint8Array; payload: Uint8Array }> = [];
    for (let i = 0; i < recordCount; i++) {
      weft.claim();
      // Envelope header, verbatim as claimed (16 B at buffer offset 0).
      const slice = weft.rReadSlice(0, 16 + payloadMax);
      captured.push({
        header: slice.slice(0, 16),
        payload: slice.slice(16, 16 + payloadMax),
      });
    }
    const bytes = writeWeftrec(WEFT_VERSION_1, captured);
    ingest(bytes, `live capture (${captured.length} frames)`);
  }, [weft, recordCount, ingest]);

  // Playback ticker.
  useEffect(() => {
    if (!isPlaying || frames.length === 0) return;
    const id = setInterval(() => {
      setCurrentFrameIdx((prev) => {
        if (prev >= frames.length - 1) {
          setIsPlaying(false);
          return prev;
        }
        return prev + 1;
      });
    }, 33); // ~30 fps scrub rate
    return () => clearInterval(id);
  }, [isPlaying, frames.length]);

  const handleFile = (file: File) => {
    const reader = new FileReader();
    reader.onload = () => ingest(new Uint8Array(reader.result as ArrayBuffer), file.name);
    reader.readAsArrayBuffer(file);
  };

  const activeFrame = frames[currentFrameIdx] ?? frames[0];

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
          FORMATS.md v1 bit-exact capture → parse → replay, CRC-32/zlib verified, L8 skip-unknown. External files welcome (C/Rust recorder output parses identically).
        </span>
      </div>

      {/* Capture controls */}
      <div style={{ background: '#1e293b', padding: '16px', borderRadius: '8px', border: '1px solid #334155', display: 'flex', gap: '12px', alignItems: 'center', flexWrap: 'wrap' }}>
        <label style={{ fontSize: '13px', color: '#94a3b8' }}>
          Capture frames:
          <input
            type="number"
            min={1}
            max={4096}
            value={recordCount}
            onChange={(e) => setRecordCount(Math.max(1, Math.min(4096, Number(e.target.value) || 1)))}
            style={{ width: 80, marginLeft: 8, background: '#0f172a', color: '#f8fafc', border: '1px solid #475569', padding: '4px 8px', borderRadius: 4 }}
          />
        </label>
        <button
          onClick={() => fileInputRef.current?.click()}
          style={{ padding: '6px 12px', borderRadius: 4, background: '#334155', color: '#fff', border: 'none', cursor: 'pointer' }}
        >
          Open external .weftrec…
        </button>
        <input
          ref={fileInputRef}
          type="file"
          accept=".weftrec"
          style={{ display: 'none' }}
          onChange={(e) => {
            const f = e.target.files?.[0];
            if (f) handleFile(f);
          }}
        />
        {fileInfo && <span style={{ fontSize: '12px', color: '#a7f3d0', fontFamily: 'monospace' }}>{fileInfo}</span>}
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
              disabled={currentFrameIdx >= frames.length - 1}
              style={{ padding: '6px 12px', borderRadius: '4px', background: '#334155', color: '#fff', border: 'none', cursor: 'pointer' }}
            >
              Step Forward ▶
            </button>
          </div>
          <div style={{ color: '#94a3b8', fontSize: '13px' }}>
            Frame {frames.length ? currentFrameIdx + 1 : 0} / {frames.length}
          </div>
        </div>

        <input
          type="range"
          min={0}
          max={Math.max(0, frames.length - 1)}
          value={currentFrameIdx}
          onChange={(e) => setCurrentFrameIdx(Number(e.target.value))}
          style={{ width: '100%', accentColor: '#38bdf8', cursor: 'pointer' }}
        />
      </div>

      {/* Active Envelope Inspection */}
      {activeFrame && (
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(2, 1fr)', gap: '16px' }}>
          <div style={{ background: '#1e293b', padding: '16px', borderRadius: '8px', border: '1px solid #334155' }}>
            <h3 style={{ margin: '0 0 12px 0', fontSize: '14px', color: '#e2e8f0' }}>Envelope Fields (Little-Endian Decode)</h3>
            <div style={{ display: 'flex', flexDirection: 'column', gap: '6px', fontSize: '13px', fontFamily: 'monospace' }}>
              <div>MAGIC: <span style={{ color: activeFrame.isForeign ? '#ef4444' : '#38bdf8' }}>{activeFrame.isForeign ? `FOREIGN (0x${activeFrame.magic.toString(16).toUpperCase()})` : 'WEFT (0x54464557)'}</span></div>
              <div>SEQUENCE (seq): <span style={{ color: '#f59e0b' }}>{activeFrame.seq}</span></div>
              <div>VERSION: <span style={{ color: '#a7f3d0' }}>{activeFrame.version}</span></div>
              <div>HEADER_SIZE: <span style={{ color: '#cbd5e1' }}>{activeFrame.headerSize} B</span></div>
              <div>PAYLOAD_LEN: <span style={{ color: '#cbd5e1' }}>{activeFrame.payloadLen} B</span></div>
              <div>FRAME CRC-32: <span style={{ color: activeFrame.kind === 'corrupt' ? '#ef4444' : '#10b981' }}>{activeFrame.crc32} {activeFrame.kind === 'corrupt' ? '(MISMATCH)' : '(VERIFIED)'}</span></div>
            </div>
          </div>

          <div style={{ background: '#1e293b', padding: '16px', borderRadius: '8px', border: '1px solid #334155' }}>
            <h3 style={{ margin: '0 0 12px 0', fontSize: '14px', color: '#e2e8f0' }}>Frame Classification</h3>
            <div style={{ display: 'flex', flexDirection: 'column', gap: '10px' }}>
              <div style={{ padding: '8px 12px', borderRadius: '6px', background: activeFrame.kind === 'foreign' ? '#7f1d1d' : activeFrame.kind === 'corrupt' ? '#7f1d1d' : activeFrame.kind === 'stale' ? '#78350f' : '#064e3b', color: '#fff', fontWeight: 'bold', fontSize: '13px' }}>
                {activeFrame.kind === 'foreign' && '⚠️ Foreign Record (skipped by rec_len — L8 forward compatibility)'}
                {activeFrame.kind === 'corrupt' && '⚠️ Corrupt Record (CRC mismatch — reported, never thrown)'}
                {activeFrame.kind === 'stale' && '⚠️ Stale Frame (unchanged sequence — honest capture keeps it)'}
                {activeFrame.kind === 'fresh' && '✅ Fresh Verified Frame (CRC-32/zlib match)'}
              </div>
              <p style={{ margin: 0, fontSize: '12px', color: '#94a3b8' }}>
                Structural validation verifies CRC-32/zlib checksums over the claimed envelope header and payload (reflected poly 0xEDB88320, init/final 0xFFFFFFFF). Unknown record formats are cleanly skipped using <code>rec_len</code> demarcations.
              </p>
            </div>
          </div>
        </div>
      )}
    </div>
  );
};
