// governor_test.dart — RFC-0009 FreshnessGovernor + §cadence policies
// conformance suite, Dart (Series 7).
//
// The G-series/PC-series counterpart of packages/core/test/governor.test.ts
// + cadence.test.ts, core/c/governor_test.c, core/rust/tests/governor_test.rs,
// GovernorTest.kt (Kotlin/JVM), and GovernorTests.swift: the ladder, the
// cooldown, the Law-4 counters, the three presentation policies, the
// exact-count steady-cadence regimes, and the identity-stable records.
//
// CROSS-LANGUAGE PARITY, LOCALLY (the Series-7 upgrade): the canonical
// xorshift32 traces (04-LITMUS §0.2) are pinned by FNV-1a-64 hashes
// computed from the TS reference (scripts/gen_trace_refs.mjs — ladder
// 0x3c33156204c7cfdf over 10,000 bytes, cadence (PC3 v2, four policies)
// 0x12eed7eec11b6a57 over 80,000 bytes, predictive-only 0xa6b942ef48046e0e over
// 60,000 bytes). Any arithmetic drift in THIS port fails HERE, without
// needing another toolchain; the fixtures (xlang-governor/vm,
// xlang-cadence) byte-compare all ports in CI as the standing proof.
//
// Environment tag: flutter_test in flutter-packages CI; plain dart-test
// in the sandbox (the expect() shim — the same honesty split as the
// verified/fanout batteries). The zero-allocation proof for this port is
// the identical() audit + pinned traces (Dart has no portable allocation
// counter; the JVM's allocated-bytes audit is the Kotlin leg's proof).
//
// SINGLE-ISOLATE: the governor is pure logic; no concurrency claims are
// made or tested here.

import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:weft_flutter/weft_flutter.dart';

// --- shared deterministic RNG (xorshift32, 04-LITMUS §0.2 — the exact
// step every port's emitter uses) ---

int _xorshift32(int x0) {
  var x = x0 & 0xFFFFFFFF;
  x = (x ^ ((x << 13) & 0xFFFFFFFF)) & 0xFFFFFFFF;
  x = (x ^ (x >>> 17)) & 0xFFFFFFFF;
  x = (x ^ ((x << 5) & 0xFFFFFFFF)) & 0xFFFFFFFF;
  return x & 0xFFFFFFFF;
}

/// FNV-1a 64 over the packed trace bytes. Wrapping arithmetic: Dart VM
/// ints are 64-bit two's complement and * / ^ wrap — the same bits every
/// port computes (the hex literal itself is the wrapped seed).
int _fnv1a64(Uint8List bytes) {
  var h = 0xcbf29ce484222325;
  for (final b in bytes) {
    h ^= b;
    h *= 0x100000001b3;
  }
  return h;
}

int _min(int a, int b) => a < b ? a : b;

void main() {
  // ------------------------------------------------------------------
  // G-series — the ladder
  // ------------------------------------------------------------------

  test('G1 ladder: every behind 0..64 maps to the documented action', () {
    final gov = FreshnessGovernor();
    // now advances past the cooldown every step so Reseed is never
    // suppressed (G1 tests the LADDER, not the rate limit).
    for (var behind = 0; behind <= 64; behind++) {
      final a = gov.step(behind, behind * 1000);
      if (behind <= governorDefaults.fastPathBehind) {
        expect(a.kind, GovernorActionKind.fastPath);
        expect(a.skipN, 0);
      } else if (behind <= governorDefaults.skipBehind) {
        expect(a.kind, GovernorActionKind.skip);
        expect(a.skipN, behind - governorDefaults.fastPathBehind);
      } else if (behind <= governorDefaults.snapshotBehind) {
        expect(a.kind, GovernorActionKind.snapshot);
        expect(a.skipN, 0);
      } else {
        expect(a.kind, GovernorActionKind.reseed);
        expect(a.skipN, 0);
      }
    }
  });

  test('G2 monotone: larger behind never yields a fresher-class action', () {
    final gov = FreshnessGovernor();
    var prevClass = -1;
    for (var behind = 0; behind <= 64; behind++) {
      final a = gov.step(behind, behind * 1000);
      expect(a.kind >= prevClass, true, reason: 'G2 monotone at behind=$behind');
      prevClass = a.kind;
    }
  });

  test('G3 reseed flap: 10k spike trace, bound + spacing', () {
    final gov = FreshnessGovernor();
    var state = 0x00c0ffee;
    var reseeds = 0;
    var lastReseedAt = -1;
    for (var i = 0; i < 10000; i++) {
      state = _xorshift32(state);
      final behind = state % 128;
      final a = gov.step(behind, i);
      if (a.kind == GovernorActionKind.reseed) {
        reseeds++;
        if (lastReseedAt >= 0) {
          expect(i - lastReseedAt >= governorDefaults.reseedCooldownMs, true,
              reason: 'G3 cooldown spacing at i=$i');
        }
        lastReseedAt = i;
      }
    }
    final bound = (10000 + governorDefaults.reseedCooldownMs - 1) ~/
        governorDefaults.reseedCooldownMs;
    expect(reseeds <= bound, true, reason: 'G3 flap bound ($reseeds <= $bound)');
    expect(reseeds > 0, true, reason: 'G3 exercised');
    expect(reseeds, gov.reseeds);
  });

  test('G3b suppressed Reseed degrades to Snapshot', () {
    final gov = FreshnessGovernor();
    expect(gov.step(64, 1000).kind, GovernorActionKind.reseed);
    expect(gov.step(64, 1050).kind, GovernorActionKind.snapshot);
    expect(gov.step(64, 1300).kind, GovernorActionKind.reseed);
    expect(gov.reseeds, 2);
  });

  test('G4 identity-stable record + counters (no per-step allocation)', () {
    final gov = FreshnessGovernor();
    final a1 = gov.step(0, 0);
    for (var i = 0; i < 1000000; i++) {
      gov.step(i % 128, i);
    }
    final a2 = gov.step(0, 2000000);
    expect(identical(a1, a2), true, reason: 'G4 identity-stable record');
    expect(gov.steps, 1000002);
  });

  test('Law 4: decidedDrops distinct from ring counters', () {
    final gov = FreshnessGovernor();
    // behind 2 -> Skip(1); behind 4 -> Skip(3): 1+3 = 4 decided drops.
    gov.step(2, 0);
    gov.step(4, 1);
    expect(gov.decidedDrops, 4);
    // A Snapshot and a FastPath add nothing.
    gov.step(8, 2);
    gov.step(0, 3);
    expect(gov.decidedDrops, 4);
  });

  test('custom ladder + reset', () {
    const cfg = GovernorConfig(
        fastPathBehind: 0, skipBehind: 2, snapshotBehind: 8,
        reseedCooldownMs: 100);
    final gov = FreshnessGovernor(config: cfg);
    expect(gov.step(0, 0).kind, GovernorActionKind.fastPath);
    expect(gov.step(1, 1).kind, GovernorActionKind.skip);
    expect(gov.step(1, 1).skipN, 1);
    expect(gov.step(3, 2).kind, GovernorActionKind.snapshot);
    expect(gov.step(9, 3).kind, GovernorActionKind.reseed);
    expect(gov.step(9, 50).kind, GovernorActionKind.snapshot); // suppressed
    gov.reset();
    expect(gov.steps, 0);
    expect(gov.reseeds, 0);
    expect(gov.step(0, 0).kind, GovernorActionKind.fastPath);
  });

  test('G5 local ladder trace hash pin (TS reference)', () {
    // The canonical (behind, nowMs) trace, packed identically to
    // fixtures/xlang-governor: byte = (kind << 6) | min(skipN, 63).
    final gov = FreshnessGovernor();
    final bytes = Uint8List(10000);
    var state = 0x00c0ffee;
    for (var i = 0; i < 10000; i++) {
      state = _xorshift32(state);
      final behind = state % 128;
      final a = gov.step(behind, i);
      bytes[i] = (a.kind << 6) | _min(a.skipN, 63);
    }
    expect(_fnv1a64(bytes), 0x3c33156204c7cfdf,
        reason: 'G5 local trace hash (TS reference pin)');
  });

  // ------------------------------------------------------------------
  // PC-series — the cadence policies
  // ------------------------------------------------------------------

  test('PC1 LATEST_WINS presents iff seq advanced; jumps coalesced', () {
    final p = CadencePolicy(CadenceConfig(CadencePolicyKind.latestWins));
    const seqs = [0, 1, 1, 4, 4, 4, 5];
    const expectPresent = [false, true, false, true, false, false, true];
    const expectCoalesced = [0, 0, 0, 2, 0, 0, 0];
    final presented = <int>[];
    for (var i = 0; i < seqs.length; i++) {
      final a = p.step(seqs[i]);
      expect(a.present, expectPresent[i], reason: 'present@$i');
      expect(a.coalesced, expectCoalesced[i], reason: 'coalesced@$i');
      expect(a.interp, false);
      if (a.present) presented.add(a.presentSeq);
    }
    expect(presented, [1, 4, 5]);
    final held = p.step(5);
    expect(held.present, false);
    expect(held.presentSeq, 5);
  });

  test('PC5 PACED 30-on-120 presents every tick after warmup (398/400)', () {
    final p =
        CadencePolicy(CadenceConfig(CadencePolicyKind.pacedInterpolate));
    var latest = 0;
    var presents = 0;
    for (var t = 1; t <= 400; t++) {
      if (t % 4 == 1) latest += 1; // 30 Hz on a 120 Hz ticker
      final a = p.step(latest);
      expect(a.interp, true);
      if (a.present) presents++;
    }
    // First window period 1 saturates at tick 2 (ticks 3-4 elide);
    // from tick 5 the ladder presents EVERY tick: 2 + 396 = 398 of 400.
    expect(presents, 398);
    expect(p.presents, presents);
  });

  test('PC5 PACED 240-on-120 presents every tick, one period behind', () {
    final p =
        CadencePolicy(CadenceConfig(CadencePolicyKind.pacedInterpolate));
    var latest = 0;
    for (var t = 1; t <= 400; t++) {
      latest += 2;
      final a = p.step(latest);
      expect(a.present, true);
      expect(a.alphaQ12, 0);
      expect(a.presentSeq, latest);
    }
    expect(p.presents, 400);
  });

  test('PC5 PACED stall saturates and elides, never extrapolates', () {
    final p =
        CadencePolicy(CadenceConfig(CadencePolicyKind.pacedInterpolate));
    p.step(1);
    p.step(2);
    final a3 = p.step(2);
    expect(a3.present, true);
    expect(a3.alphaQ12, cadenceAlphaOneQ12);
    final a4 = p.step(2);
    expect(a4.present, false);
    expect(a4.alphaQ12, cadenceAlphaOneQ12);
    for (var i = 0; i < 200; i++) {
      p.step(2);
    }
    expect(p.interpFrames, 0); // endpoints only — zero true blends
  });

  test('PC6 PACED alpha ladder is the period ladder', () {
    final p =
        CadencePolicy(CadenceConfig(CadencePolicyKind.pacedInterpolate));
    final alphas = <int>[];
    for (var t = 1; t <= 8; t++) {
      final int latest;
      if (t == 1) {
        latest = 1;
      } else if (t == 5) {
        latest = 2;
      } else {
        latest = t <= 4 ? 1 : 2;
      }
      final a = p.step(latest);
      if (t >= 5) alphas.add(a.alphaQ12);
    }
    expect(alphas, [0, 1024, 2048, 3072]);
  });

  test('PC5 BURST 30-on-120 locks K on the content beat (K=4)', () {
    final p = CadencePolicy(CadenceConfig(CadencePolicyKind.burstCoalesce));
    var latest = 0;
    var settledOk = true;
    for (var t = 1; t <= 400; t++) {
      if (t % 4 == 1) latest += 1;
      final a = p.step(latest);
      if (t > 200 && a.k != 4) settledOk = false;
    }
    expect(settledOk, true, reason: 'K locked at 4 (last 200 ticks)');
  });

  test('PC5 BURST 240-on-120 degrades to newest-wins (K=1)', () {
    final p = CadencePolicy(CadenceConfig(CadencePolicyKind.burstCoalesce));
    var latest = 0;
    var settledOk = true;
    var presents = 0;
    for (var t = 1; t <= 400; t++) {
      latest += 2;
      final a = p.step(latest);
      if (a.present) presents++;
      if (t > 200 && a.k != 1) settledOk = false;
    }
    expect(settledOk, true);
    expect(presents, 400);
    expect(p.coalescedByDecision, 400);
  });

  test('PC5 BURST feed paces one present per burst', () {
    final p = CadencePolicy(CadenceConfig(CadencePolicyKind.burstCoalesce));
    var latest = 0;
    var presents = 0;
    var coalescedSeen = 0;
    for (var t = 1; t <= 400; t++) {
      if (t % 10 == 1) latest += 24; // 24-frame burst every 10 ticks
      final a = p.step(latest);
      if (a.present) {
        presents++;
        coalescedSeen += a.coalesced;
      }
    }
    expect(presents >= 38 && presents <= 41, true,
        reason: 'presents per burst in 38..41 (got $presents)');
    expect(coalescedSeen, 960 - presents);
    expect(p.missedPresentTicks + p.elided + presents, 400);
  });

  test('PC5 BURST stall freezes cadence; recovery immediate', () {
    final p = CadencePolicy(CadenceConfig(CadencePolicyKind.burstCoalesce));
    var latest = 0;
    for (var t = 1; t <= 200; t++) {
      latest += 2;
      p.step(latest);
    }
    final before = p.presents;
    for (var t = 1; t <= 100; t++) {
      p.step(latest);
    }
    expect(p.presents, before); // nothing newer -> no presents
    expect(p.missedPresentTicks, 100);
    latest += 5;
    final a = p.step(latest);
    expect(a.present, true);
    expect(a.coalesced, 4);
  });

  test('PC2 telescoping identities over volatile traces', () {
    // LATEST_WINS / BURST: sum(coalesced) == lastPresentedSeq - presents
    for (final kind in [
      CadencePolicyKind.latestWins,
      CadencePolicyKind.burstCoalesce
    ]) {
      final p = CadencePolicy(CadenceConfig(kind));
      var state = 0x1234abcd;
      var latest = 0;
      for (var i = 0; i < 10000; i++) {
        state = _xorshift32(state);
        latest += state % 5;
        p.step(latest);
      }
      expect(p.coalescedByDecision,
          p.lastPresentedSeqForTest() - p.presents,
          reason: 'PC2 kind=$kind');
    }
    // PACED: sum(coalesced) == newestSeq - arrivalTicks
    {
      final p =
          CadencePolicy(CadenceConfig(CadencePolicyKind.pacedInterpolate));
      var state = 0xfeedface;
      var latest = 0;
      for (var i = 0; i < 10000; i++) {
        state = _xorshift32(state);
        latest += state % 5;
        p.step(latest);
      }
      expect(p.coalescedByDecision, p.newestSeqForTest() - p.arrivalTicks,
          reason: 'PC2 PACED');
    }
  });

  test('PC4 cadence identity-stable record', () {
    final p =
        CadencePolicy(CadenceConfig(CadencePolicyKind.pacedInterpolate));
    final first = p.step(1);
    var state = 0xabcdef01;
    var latest = 1;
    for (var i = 0; i < 10000; i++) {
      state = _xorshift32(state);
      latest += state % 5;
      final a = p.step(latest);
      if (!identical(a, first)) {
        fail('PC4: SAME object expected, mutated in place');
      }
    }
  });

  test('PC6 policy switch mid-trace: monotone presentSeq', () {
    final p = CadencePolicy(CadenceConfig(CadencePolicyKind.latestWins));
    var latest = 0;
    var lastPresented = 0;
    final kinds = [
      CadencePolicyKind.latestWins,
      CadencePolicyKind.pacedInterpolate,
      CadencePolicyKind.burstCoalesce,
      CadencePolicyKind.latestWins,
    ];
    for (var i = 0; i < 4000; i++) {
      if (i % 1000 == 0) p.reset(kinds[i ~/ 1000]);
      latest += (i % 3 == 0 ? 1 : 0) + (i % 7 == 0 ? 2 : 0);
      final a = p.step(latest);
      if (a.present && !a.interp) {
        expect(a.presentSeq >= lastPresented, true,
            reason: 'PC6 monotone at i=$i');
        lastPresented = a.presentSeq;
      }
    }
    expect(p.presents > 0, true);
  });

  test('PC3 v2 local cadence trace hash pin (TS reference)', () {
    // The canonical arrival trace, packed identically to
    // fixtures/xlang-cadence (PC3 v2): per tick, 4 policies in kind
    // order, each 2 bytes: b1 = present<<7 | interp<<6 | alphaQ12>>7,
    // b2 = min(coalesced, 255). Hash pinned from the TS reference
    // (RFC-0012 added PREDICTIVE_PACED as kind 3 — the v1 3-policy
    // substream is byte-preserved; the pin covers the extended stream).
    final pols = [
      CadencePolicy(CadenceConfig(CadencePolicyKind.latestWins)),
      CadencePolicy(CadenceConfig(CadencePolicyKind.pacedInterpolate)),
      CadencePolicy(CadenceConfig(CadencePolicyKind.burstCoalesce)),
      CadencePolicy(CadenceConfig(CadencePolicyKind.predictivePaced)),
    ];
    final bytes = Uint8List(80000);
    var state = 0x00c0ffee;
    var latest = 0;
    var out = 0;
    for (var i = 0; i < 10000; i++) {
      state = _xorshift32(state);
      latest += state % 5;
      for (final pol in pols) {
        final a = pol.step(latest);
        final b1 = (a.present ? 1 : 0) << 7 |
            (a.interp ? 1 : 0) << 6 |
            (a.alphaQ12 >> 7);
        bytes[out] = b1;
        out++;
        bytes[out] = _min(a.coalesced, 255);
        out++;
      }
    }
    expect(_fnv1a64(bytes), 0x12eed7eec11b6a57,
        reason: 'PC3 v2 local trace hash (TS reference pin)');
  });

  test('PC7 predictive: drift-freedom on the 12/5 beat + reactive gate clears', () {
    final p = CadencePolicy(CadenceConfig(CadencePolicyKind.predictivePaced));
    var m = 1;
    var latest = 0;
    var satHolds = 0;
    for (var t = 1; t <= 10000; t++) {
      if (t >= (2.4 * m).toInt()) { latest++; m++; }
      final d = p.step(latest);
      if (d.present && d.alphaQ12 == cadenceAlphaOneQ12) satHolds++;
    }
    final dev = (p.gapQ16ForTest() - (2.4 * cadenceOneQ16).toInt()).abs();
    expect(dev <= 8192, true, reason: 'PC7a gap EWMA orbit: $dev');
    expect(satHolds <= 10, true, reason: 'PC7b saturated holds: $satHolds');
    final r0 = p.reactiveTicks;
    for (var i = 0; i < 5000; i++) { p.step(latest); }
    expect(p.reactiveTicks - r0, 0, reason: 'PC7b gate clear');
  });
}
