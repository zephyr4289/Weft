import { describe, it, expect } from 'vitest';
import { WORKLOAD_INFO } from '../src/workloads/generators';
import { createModeRunner, ModeType } from '../src/modes/runner';

describe('Showcase Demos 5×4 Matrix & Hot-Switching', () => {
  const workloads = ['W1', 'W2', 'W3', 'W4', 'W5'];
  const modes: ModeType[] = ['A', 'B', 'C', 'D'];

  // Test 1: 5×4 Matrix Execution
  for (const wid of workloads) {
    for (const mode of modes) {
      it(`Workload ${wid} · Mode ${mode} steady-state execution (1000 frames)`, () => {
        const info = WORKLOAD_INFO[wid];
        const runner = createModeRunner(mode, wid, info.floatCount);
        const target = new Float32Array(info.floatCount);

        for (let frame = 1; frame <= 1000; frame++) {
          runner.produceFrame(frame);
          const ok = runner.consumeFrame(target);
          expect(ok).toBe(true);
        }

        if (mode === 'C' || mode === 'D') {
          expect(runner.getDropCount()).toBe(0);
        }

        runner.dispose();
      });
    }
  }

  // Test 2: Mode Hot-Switching (100 switches without leaks)
  it('Hot-switches modes across 100 transitions without error', () => {
    let currentMode: ModeType = 'A';
    let currentWid = 'W1';
    let runner = createModeRunner(currentMode, currentWid, WORKLOAD_INFO[currentWid].floatCount);
    const target = new Float32Array(16384);

    for (let cycle = 1; cycle <= 100; cycle++) {
      currentMode = modes[cycle % 4];
      currentWid = workloads[cycle % 5];
      runner.dispose();
      runner = createModeRunner(currentMode, currentWid, WORKLOAD_INFO[currentWid].floatCount);

      runner.produceFrame(cycle);
      const ok = runner.consumeFrame(target.subarray(0, WORKLOAD_INFO[currentWid].floatCount));
      expect(ok).toBe(true);
    }
    runner.dispose();
  });
});
