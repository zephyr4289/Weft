// recycler_test.dart — RFC-0009 Series 7: 0-GC buffer recycler +
// memory-pressure backstop conformance suite, Dart.
//
// The RecyclerTest.kt / RecyclerTests.swift twin: pool discipline, bounded
// steady state, the trim backstop's LIVE-slot safety, realloc accounting,
// the level mapping, the center fan-out — plus the drawing-loop churn leg
// (claim -> cadence step -> blend -> pool round-trip over the fan-out
// reader's zero-alloc view). Dart has no portable allocation counter (the
// per-port honesty wall — the JVM's allocated-bytes audit is the Kotlin
// leg's proof): the identity discipline (identical() on a
// released-then-reacquired slot) and the exact counter equations pin the
// contract here.
//
// Environment tag: flutter_test in flutter-packages CI; plain dart-test in
// the sandbox (the expect() shim).

import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:weft_flutter/weft_flutter.dart';

void main() {
  // --- R1: pool reuse + counter discipline ---

  test('R1 pool reuse identity + counters', () {
    final pool = WeftBufferRecycler(slotBytes: 256, maxFreeSlots: 2);
    expect(pool.pooledNow, 0);
    expect(pool.liveNow, 0);

    final a = pool.acquire();
    expect(a.length, 256);
    expect(pool.liveNow, 1);
    expect(pool.pooledNow, 0);
    expect(pool.reallocs, 0); // first fill is not a realloc

    expect(pool.release(a), true);
    expect(pool.liveNow, 0);
    expect(pool.pooledNow, 1);

    // Reuse: the SAME slot identity comes back (zero allocation).
    final b = pool.acquire();
    expect(identical(a, b), true, reason: 'pooled slot identity reused');
    expect(pool.acquires, 2);
    expect(pool.releases, 1);
    expect(pool.reallocs, 0);
  });

  // --- R2: bounded steady state (excess releases drop to the GC) ---

  test('R2 maxFreeSlots bounds steady state', () {
    final pool = WeftBufferRecycler(slotBytes: 64, maxFreeSlots: 2);
    final x = pool.acquire();
    final y = pool.acquire();
    final z = pool.acquire();
    expect(pool.release(x), true);
    expect(pool.release(y), true);
    expect(pool.release(z), false, reason: 'pool full — z drops to the GC');
    expect(pool.pooledNow, 2);
    expect(pool.liveNow, 0);
  });

  // --- R3: the backstop touches FREE slots only, never LIVE ---

  test('R3 trim drops only free slots; live survives; next acquire reallocates', () {
    final pool = WeftBufferRecycler(slotBytes: 128, maxFreeSlots: 2);
    final live = pool.acquire(); // LIVE: mid-frame raster buffer
    final free1 = pool.acquire(); // two DISTINCT slots pool up
    final free2 = pool.acquire();
    pool.release(free1);
    pool.release(free2);
    expect(pool.pooledNow, 2);

    pool.trim(0);
    expect(pool.pooledNow, 0);
    expect(pool.trimmedSlots, 2);
    expect(pool.trims, 1);
    expect(pool.liveNow, 1, reason: 'live slot untouched by the backstop');

    // The LIVE slot is still perfectly usable (no frame dropped).
    live[0] = 0xAB;
    expect(live[0], 0xAB);

    // Next acquire: fresh allocation, COUNTED (pressure's visible cost).
    final fresh = pool.acquire();
    expect(identical(free1, fresh), false);
    expect(pool.reallocs, 1);
  });

  // --- R4: realloc accounting ---

  test('R4 realloc accounting (first fill != realloc; keep-all trims inert)', () {
    final pool = WeftBufferRecycler(slotBytes: 32, maxFreeSlots: 4);
    for (var i = 0; i < 4; i++) {
      pool.acquire();
    }
    expect(pool.reallocs, 0);
    expect(pool.liveNow, 4);

    // UI_HIDDEN-class with an empty free list: NOT an effective trim.
    pool.onLowMemory(TrimLevel.uiHidden);
    expect(pool.trims, 1);
    expect(pool.trimmedSlots, 0);

    // Release all, then a keep-all trim again — still not effective.
    for (var i = 0; i < 4; i++) {
      pool.release(Uint8List(32));
    }
    expect(pool.pooledNow, 4);
    pool.onLowMemory(TrimLevel.uiHidden);
    expect(pool.pooledNow, 4);
    expect(pool.trimmedSlots, 0);
    expect(pool.reallocs, 0);

    // COMPLETE: drops all four; the next four acquires are reallocs.
    pool.onLowMemory(TrimLevel.complete);
    expect(pool.pooledNow, 0);
    expect(pool.trimmedSlots, 4);
    for (var i = 0; i < 4; i++) {
      pool.acquire();
    }
    expect(pool.reallocs, 4);
  });

  // --- R5: the documented level mapping ---

  test('R5 level mapping', () {
    int keepAfter(int level) {
      final p = WeftBufferRecycler(slotBytes: 16, maxFreeSlots: 4);
      final slots = List<Uint8List>.generate(4, (_) => p.acquire());
      for (final s in slots) {
        p.release(s);
      }
      expect(p.pooledNow, 4);
      p.onLowMemory(level);
      return p.pooledNow;
    }

    expect(keepAfter(TrimLevel.complete), 0);
    expect(keepAfter(TrimLevel.runningCritical), 0);
    expect(keepAfter(TrimLevel.moderate), 2);
    expect(keepAfter(TrimLevel.background), 2);
    expect(keepAfter(TrimLevel.runningLow), 2);
    expect(keepAfter(TrimLevel.uiHidden), 4);
    expect(keepAfter(TrimLevel.runningModerate), 4);
    expect(keepAfter(0), 4); // unknown -> conservative keep
  });

  // --- R6: the center fan-out ---

  test('R6 center fan-out + unregister', () {
    final a = WeftBufferRecycler(slotBytes: 8, maxFreeSlots: 2);
    final b = WeftBufferRecycler(slotBytes: 8, maxFreeSlots: 2);
    a.release(a.acquire());
    b.release(b.acquire());
    final center = WeftRecyclerCenter.shared;
    final n0 = center.registered;
    center.register(a);
    center.register(b);
    expect(center.registered, n0 + 2);

    center.handleMemoryWarning(); // hard event == COMPLETE class
    expect(a.pooledNow, 0);
    expect(b.pooledNow, 0);
    expect(a.trims, 1);
    expect(b.trims, 1);

    center.unregister(a);
    center.unregister(b);
    expect(center.registered, n0);
  });

  // --- R7: the drawing-loop churn (fan-out reader + policy + pool) ---
  //
  // The Dart leg of the Kotlin R8 audit: the SAME per-tick path over the
  // zero-alloc fan-out reader (single isolate — the port's honesty wall),
  // with identity discipline + counter equations standing in for the JVM's
  // byte counter (declared).

  test('R7 drawing-loop churn: pool stable, counters exact, no reallocs', () {
    final words = 64;
    final b = WeftFanoutBroadcaster(words * 4, 4);
    final reader = WeftFanoutReader(b.ringBytes(), words * 4, 4);
    final pool = WeftBufferRecycler(slotBytes: words * 4, maxFreeSlots: 2);
    final policy = CadencePolicy(CadenceConfig(CadencePolicyKind.pacedInterpolate));
    var rasterSlot = pool.acquire();
    final prevWords = Uint32List(words);
    final newWords = Uint32List(words);

    var lastObs = 0;
    var presents = 0;
    var poolMisses = 0;
    for (var tick = 1; tick <= 12000; tick++) {
      // Producer: publish a frame every 4th tick (30-on-120 feed).
      if (tick % 4 == 1) {
        final w = b.begin();
        for (var i = 0; i < words; i++) {
          // word i = (seq*31 + i*golden) as u32 — monotonic word 0.
          w.setUint32(4 * i,
              (tick * 31 + i * 2654435761) & 0xFFFFFFFF, Endian.little);
        }
        b.publish();
      }
      // Consumer: the drawing loop.
      final rec = reader.claim();
      var arrived = false;
      if (rec.fresh) {
        arrived = true;
        final v = reader.view();
        for (var i = 0; i < words; i++) {
          newWords[i] = v[i];
        }
      }
      if (rec.fresh) lastObs = rec.seq;
      final d = policy.step(lastObs);
      if (d.present && d.interp) {
        final alpha = d.alphaQ12;
        final inv = 4096 - alpha;
        final r = rasterSlot;
        for (var i = 0; i < words; i++) {
          // Per-channel u32 blend in Q12 (packed LE into the byte slot).
          final p = _blendQ12(prevWords[i], newWords[i], alpha, inv);
          r[4 * i] = p & 0xFF;
          r[4 * i + 1] = (p >>> 8) & 0xFF;
          r[4 * i + 2] = (p >>> 16) & 0xFF;
          r[4 * i + 3] = (p >>> 24) & 0xFF;
        }
        presents++;
      }
      if (arrived) {
        for (var i = 0; i < words; i++) {
          prevWords[i] = newWords[i];
        }
      }
      // The raster slot round-trips the pool's zero-alloc path; the SAME
      // identity must come back every time (the pool never reallocates).
      pool.release(rasterSlot);
      final again = pool.acquire();
      if (!identical(again, rasterSlot)) poolMisses++;
      rasterSlot = again;
    }
    expect(poolMisses, 0, reason: 'pool slot identity stable across the churn');
    expect(pool.reallocs, 0);
    expect(pool.trimmedSlots, 0);
    expect(presents, greaterThan(9000), reason: 'PACED presented most ticks');
    expect(policy.interpFrames, greaterThan(0), reason: 'synthesis exercised');
    // PC2 telescoping held across the churn.
    expect(
      policy.coalescedByDecision,
      policy.newestSeqForTest() - policy.arrivalTicks,
    );
  });
}

/// Per-channel u32 blend in Q12 — the drawing-loop raster op.
int _blendQ12(int a, int b, int alpha, int inv) {
  final r = ((a & 0xff) * inv + (b & 0xff) * alpha) >>> 12;
  final g = ((a >>> 8 & 0xff) * inv + (b >>> 8 & 0xff) * alpha) >>> 12;
  final bl = ((a >>> 16 & 0xff) * inv + (b >>> 16 & 0xff) * alpha) >>> 12;
  final al = ((a >>> 24 & 0xff) * inv + (b >>> 24 & 0xff) * alpha) >>> 12;
  return r | (g << 8) | (bl << 16) | (al << 24);
}
