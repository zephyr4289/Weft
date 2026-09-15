import React, { useMemo, useEffect, useState } from 'react';
import { Weft } from '@weft/core';
import { WeftCanvas } from '@weft/react';

export function App() {
  const payloadMax = 256;
  const weft = useMemo(() => new Weft(payloadMax), [payloadMax]);
  const [fps, setFps] = useState(0);

  // Writer thread simulation at 120Hz
  useEffect(() => {
    let seq = 1;
    let running = true;
    const interval = setInterval(() => {
      if (!running) return;
      weft.fillPayload(seq, payloadMax);
      weft.publish(seq, payloadMax);
      seq++;
    }, 8);

    return () => {
      running = false;
      clearInterval(interval);
    };
  }, [weft, payloadMax]);

  const handleDraw = (ctx: CanvasRenderingContext2D, buf: Uint8Array) => {
    ctx.clearRect(0, 0, 400, 300);
    ctx.fillStyle = '#0f172a';
    ctx.fillRect(0, 0, 400, 300);

    // Draw bars from payload
    const barWidth = 400 / 32;
    for (let i = 0; i < 32; i++) {
      const val = buf[i] || 0;
      const height = (val / 255) * 200;
      ctx.fillStyle = `hsl(${val * 1.4}, 70%, 60%)`;
      ctx.fillRect(i * barWidth, 300 - height, barWidth - 1, height);
    }
  };

  return (
    <div style={{ fontFamily: 'sans-serif', padding: '20px' }}>
      <h2>Weft + React Triad Repaint Fixture</h2>
      <p>Zero-copy state synchronization via SharedArrayBuffer</p>
      <WeftCanvas
        weft={weft}
        draw={handleDraw}
        width={400}
        height={300}
        style={{ border: '1px solid #334155', borderRadius: '4px' }}
      />
    </div>
  );
}
