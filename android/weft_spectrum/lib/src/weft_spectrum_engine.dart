// lib/src/weft_spectrum_engine.dart — WeftSpectrumEngine (P5, mandate C).
//
// Dart FFI bindings into the native hardware-probing seam:
//   * Engineer 1 authors core/c/include/weft_spectrum.h — the
//     weft_hw_profile_t descriptor + runtime micro-probing engine. This
//     engine CONSUMES it through DynamicLibrary symbol lookup; it NEVER
//     authors native C internals (Pillar boundary, no exceptions).
//   * Engineer 2's acceleration drivers (FastRPC/NeuroPilot) are reached
//     through their dynamic dispatch symbols via the same seam.
//
// Law 4: absent symbols are an HONEST down-grade — the engine surfaces
// E_PROBE_UNAVAILABLE, builds a runtime-detected fallback profile from
// Platform-level facts, and the host UI keeps running (no crash, no throw
// into widget code). When E1's authoritative symbol names land, only
// _SpectrumSymbols changes; the SHP1 wire contract does not.

import 'dart:ffi';
import 'dart:io' show Platform;
import 'dart:typed_data';

import 'spectrum_wire.dart';
import 'spectrum_governor.dart';

/// Resolvable native symbols (E1 seam). Absent entries are null — the seam
/// is contract-first and tolerates a partially-landed native layer.
class SpectrumSymbols {
  /// Pointer to a 192-byte SHP1 record maintained by the native prober.
  final Pointer<Uint8> Function() readProfile;

  /// Native cadence tick (optional fast path); managed governor is the
  /// source of truth for parity, this is an acceleration seam only.
  final int Function(int, int, int, int, int)? nativeTick;

  SpectrumSymbols(this.readProfile, this.nativeTick);
}

typedef _ReadProfileC = Pointer<Uint8> Function();

/// Attempts symbol resolution against a list of candidate libraries
/// (process, executable, libweft.so). Every failure is fail-soft.
SpectrumSymbols? tryResolveSymbols(List<String> libraryNames) {
  final libs = <DynamicLibrary>[];
  try {
    libs.add(DynamicLibrary.process());
  } catch (_) {}
  try {
    libs.add(DynamicLibrary.executable());
  } catch (_) {}
  for (final name in libraryNames) {
    try {
      libs.add(DynamicLibrary.open(name));
    } catch (_) {}
  }
  for (final lib in libs) {
    try {
      final read = lib.lookupFunction<_ReadProfileC, _ReadProfileC>(
          'weft_hw_profile_read');
      return SpectrumSymbols(read, null);
    } catch (_) {
      // symbol not present — keep trying candidates (E1 seam not landed)
    }
  }
  return null;
}

/// The engine: owns the profile flyweight, the governor state, and the
/// Platform-facts fallback. Steady-state [tick] allocates nothing.
class WeftSpectrumEngine {
  final ProfileFlyweight profile = ProfileFlyweight();
  final CadenceState cadence = CadenceState();
  final GovernorInput input = GovernorInput();
  final Uint8List _record = Uint8List(shp1RecordSize);

  int lastErrorCode = 0; // §6 code of the last seam event (0 = none)
  bool get probeAvailable => _symbols != null;

  SpectrumSymbols? _symbols;
  ProfileView? _view;

  /// Attach: native seam first; on E_PROBE_UNAVAILABLE fall back to
  /// Platform-detected facts. Never throws (Law 4).
  WeftSpectrumEngine attach({List<String> libraries = const ['libweft.so', 'libweft_spectrum.so']}) {
    try {
      _symbols = tryResolveSymbols(libraries);
    } catch (_) {
      _symbols = null;
    }
    if (_symbols != null) {
      try {
        final rec = _symbols!.readProfile();
        final bytes = rec.asTypedList(shp1RecordSize);
        _record.setRange(0, shp1RecordSize, bytes);
        _view = ProfileView(_record);
        lastErrorCode = decodeProfile(_record, profile);
        if (lastErrorCode != 0) {
          _fallbackProfile(); // torn/invalid native record: fail-soft
        }
        return this;
      } catch (_) {
        lastErrorCode = eUnmarshalFailed;
      }
    }
    if (_symbols == null || _view == null) {
      lastErrorCode = eProbeUnavailable;
      _fallbackProfile();
    }
    return this;
  }

  /// Platform-facts fallback profile (honest down-spec, no native code).
  void _fallbackProfile() {
    _view = null;
    profile.siliconTier = _detectTier();
    profile.thermalState = thermalNominal;
    profile.perfCores = (Platform.numberOfProcessors / 2).ceil();
    profile.effCores = Platform.numberOfProcessors - profile.perfCores;
    profile.cacheLineBytes = 64;
    profile.simdWidthBits = 128; // ARM NEON baseline on Flutter targets
    profile.frameBudgetUs =
        profile.siliconTier == tierFlagship ? 4166 : (profile.siliconTier == tierMid ? 8333 : 16666);
    profile.maxFrameRateMilliHz = profile.siliconTier == tierFlagship
        ? 240000
        : (profile.siliconTier == tierMid ? 120000 : 60000);
    profile.dmaLaneCount = Platform.numberOfProcessors >= 6 ? 2 : 1;
  }

  int _detectTier() {
    final cores = Platform.numberOfProcessors;
    if (cores >= 10) return tierFlagship;
    if (cores >= 6) return tierMid;
    return tierBudget;
  }

  /// One steady-state telemetry tick: flyweight + governor, zero allocation.
  /// Returns the cadence cap (Hz).
  int tick({
    required int thermalState,
    required int batteryPermille,
    required int batteryCharging,
    required int visibility,
    int heapPressure = 0,
  }) {
    input.thermalState = thermalState;
    input.batteryPermille = batteryPermille;
    input.batteryCharging = batteryCharging;
    input.visibility = visibility;
    input.heapPressure = heapPressure;
    input.tierMaxHzCap = tierMaxHz[profile.siliconTier] ?? 60;
    return cadenceTick(cadence, input);
  }
}
