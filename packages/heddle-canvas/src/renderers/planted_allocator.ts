// planted_allocator.ts — Law-1 INSTRUMENT CALIBRATION (test-only).
//
// An allocator that DELIBERATELY allocates from inside the engine's
// attribution scope (this file lives in src/). test/law1.test.ts uses it
// to prove the sampling heap profiler actually catches an engine-path
// allocator — an instrument that cannot fail proves nothing (the house
// "must-bite" discipline, cf. tools/guardian).
export function plantedAllocator(sink: unknown[]): void {
  for (let i = 0; i < 5; i++) {
    sink.push({ planted: true, stamp: performance.now(), payload: [i, i + 1, i + 2, i + 3] });
  }
}
