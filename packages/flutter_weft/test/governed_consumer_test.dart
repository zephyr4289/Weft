// governed_consumer_test.dart — RFC-0009 Series 7: the composed display
// consumer conformance suite, Dart (the GovernedFanoutConsumerTest.kt /
// Swift twin): the exact-count steady-cadence regimes end to end over the
// real (single-isolate) fan-out ring, PC2 telescoping, the ladder's
// advisory classes on real drop accounting, and dispose-to-pool.
//
// Environment tag: flutter_test in flutter-packages CI; plain dart-test in
// the sandbox (the expect() shim). SINGLE-ISOLATE — the Dart port's
// honesty wall: no cross-thread claims are made or tested here.

import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:weft_flutter/weft_flutter.dart';

void main() {
  // --- C1: PACED 30-on-120 end to end — the steady raster ---

  test('C1 PACED 30-on-120 presents 398/400 end to end', () {
    final words = 64;
    final b = WeftFanoutBroadcaster(words * 4, 4);
    final reader = WeftFanoutReader(b.ringBytes(), words * 4, 4);
    final pool = WeftBufferRecycler(slotBytes: words * 4, maxFreeSlots: 1);
    final consumer = GovernedFanoutConsumer(reader,
        policyKind: CadencePolicyKind.pacedInterpolate, rasterPool: pool);
    var clock = 0;
    consumer.clock = () => clock;

    var presents = 0;
    for (var t = 1; t <= 400; t++) {
      if (t % 4 == 1) {
        final w = b.begin();
        for (var i = 0; i < words; i++) {
          w.setUint32(4 * i, (t * 31 + i * 2654435761) & 0xFFFFFFFF,
              Endian.little);
        }
        b.publish();
      }
      clock = t;
      if (consumer.tick().present) presents++;
    }
    // The PC5 battery's exact count for this regime.
    expect(presents, 398);
    expect(consumer.cadence.presents, presents);
    expect(consumer.cadence.interpFrames, greaterThan(0));
  });

  // --- C2: PC2 telescoping through the ring (identity exact) ---

  test('C2 telescoping holds through the ring (LATEST_WINS + BURST)', () {
    int xorshift32(int x) {
      var v = x & 0xFFFFFFFF;
      v = (v ^ ((v << 13) & 0xFFFFFFFF)) & 0xFFFFFFFF;
      v = (v ^ (v >>> 17)) & 0xFFFFFFFF;
      v = (v ^ ((v << 5) & 0xFFFFFFFF)) & 0xFFFFFFFF;
      return v & 0xFFFFFFFF;
    }

    for (final kind in [
      CadencePolicyKind.latestWins,
      CadencePolicyKind.burstCoalesce
    ]) {
      final words = 32;
      final b = WeftFanoutBroadcaster(words * 4, 4);
      final reader = WeftFanoutReader(b.ringBytes(), words * 4, 4);
      final consumer = GovernedFanoutConsumer(reader, policyKind: kind);
      consumer.clock = () => 0;
      var state = 0x00c0ffee;
      for (var t = 1; t <= 10000; t++) {
        state = xorshift32(state);
        final arrivals = state % 5;
        if (arrivals > 0 || t % 3 == 0) {
          final w = b.begin();
          for (var i = 0; i < words; i++) {
            w.setUint32(4 * i, (t * 31 + i * 2654435761) & 0xFFFFFFFF,
                Endian.little);
          }
          b.publish();
        }
        consumer.tick();
      }
      final c = consumer.cadence;
      expect(c.coalescedByDecision,
          c.lastPresentedSeqForTest() - c.presents,
          reason: 'C2 kind=$kind');
    }
  });

  // --- C3: the ladder sees the ring's REAL drop accounting ---

  test('C3 ladder classes from real drop accounting', () {
    final words = 16;
    final b = WeftFanoutBroadcaster(words * 4, 4);
    final reader = WeftFanoutReader(b.ringBytes(), words * 4, 4);
    final consumer =
        GovernedFanoutConsumer(reader, policyKind: CadencePolicyKind.latestWins);
    var clock = 0;
    consumer.clock = () => clock;

    void publish(int tag) {
      final w = b.begin();
      for (var i = 0; i < words; i++) {
        w.setUint32(4 * i, (tag * 31 + i * 2654435761) & 0xFFFFFFFF,
            Endian.little);
      }
      b.publish();
    }

    // Steady feed: FastPath (behind == 0).
    publish(1);
    consumer.tick();
    publish(2);
    final d = consumer.tick();
    expect(consumer.action.kind, GovernorActionKind.fastPath);
    expect(d.present, true);

    // Burst of 4 between ticks: dropped=3 -> Skip(2) ladder class.
    publish(3);
    publish(4);
    publish(5);
    publish(6);
    clock += 100;
    consumer.tick();
    expect(consumer.action.kind, GovernorActionKind.skip);
    expect(consumer.action.skipN, 2);
    expect(consumer.actionChanged, true);

    // Behind 0 again: back to FastPath (edge).
    clock += 100;
    consumer.tick();
    expect(consumer.action.kind, GovernorActionKind.fastPath);
    expect(consumer.actionChanged, true);

    // Massive burst: dropped=40 -> Reseed (post-cooldown).
    clock += 1000;
    for (var s = 7; s <= 47; s++) {
      publish(s);
    }
    consumer.tick();
    expect(consumer.action.kind, GovernorActionKind.reseed);
    expect(consumer.staleness.reseeds, 1);
  });

  // --- C4: dispose returns the raster slot to the pool ---

  test('C4 dispose returns the raster slot to the pool', () {
    final words = 8;
    final b = WeftFanoutBroadcaster(words * 4, 4);
    final reader = WeftFanoutReader(b.ringBytes(), words * 4, 4);
    final pool = WeftBufferRecycler(slotBytes: words * 4, maxFreeSlots: 2);
    final consumer = GovernedFanoutConsumer(reader,
        policyKind: CadencePolicyKind.latestWins, rasterPool: pool);
    expect(pool.liveNow, 1);
    expect(pool.pooledNow, 0);
    final r = consumer.raster;
    consumer.dispose();
    consumer.dispose(); // idempotent
    expect(pool.liveNow, 0);
    expect(pool.pooledNow, 1);
    // The slot is reusable (pooled identity).
    expect(identical(pool.acquire(), r), true);
  });
}
