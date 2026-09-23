// lib/src/spectrum_wire.dart — SHP1 zero-copy decode (Pillar 5, Dart lane).
//
// Normative layout: docs/spectrum/SPECTRUM-WIRE-V1.md — offsets and codes are
// byte-frozen and MUST stay identical to packages/spectrum-managed/src/wire.js
// and python/weft_spectrum/wire.py (CI static audit + parity stage enforce).
//
// Law 1: ProfileFlyweight is a preallocated mutable object; decodeProfile()
// fills it in place. Steady-state telemetry allocates nothing.
// Law 2: every multi-byte read is explicit ByteData.get*(.., Endian.little).

import 'dart:typed_data';

// §6 Law 4 error taxonomy (frozen; identical across all managed runtimes)
const int eBadMagic = 1;
const int eBadVersion = 2;
const int eBadSize = 3;
const int eCrcMismatch = 4;
const int eReservedDirty = 5;
const int eProbeUnavailable = 6;
const int eDeviceLost = 7;
const int eFfiTimeout = 8;
const int eHeapPressure = 9;
const int eListenerLeak = 10;
const int eAlignInvalid = 11;
const int eHudContextLost = 12;
const int eUnmarshalFailed = 13;
const int eTierExhausted = 14;
const int eHudRecovered = 15;

const String eBadMagicName = 'E_BAD_MAGIC';
const String eBadVersionName = 'E_BAD_VERSION';
const String eBadSizeName = 'E_BAD_SIZE';
const String eCrcMismatchName = 'E_CRC_MISMATCH';
const String eReservedDirtyName = 'E_RESERVED_DIRTY';
const String eProbeUnavailableName = 'E_PROBE_UNAVAILABLE';
const String eDeviceLostName = 'E_DEVICE_LOST';
const String eFfiTimeoutName = 'E_FFI_TIMEOUT';
const String eHeapPressureName = 'E_HEAP_PRESSURE';
const String eListenerLeakName = 'E_LISTENER_LEAK';
const String eAlignInvalidName = 'E_ALIGN_INVALID';
const String eHudContextLostName = 'E_HUD_CONTEXT_LOST';
const String eUnmarshalFailedName = 'E_UNMARSHAL_FAILED';
const String eTierExhaustedName = 'E_TIER_EXHAUSTED';
const String eHudRecoveredName = 'E_HUD_RECOVERED';

// §2 record geometry (frozen)
const int shp1RecordSize = 192;
const int shp1CrcOffset = 188;
const int shp1LayoutVersion = 1;

// §3 feature bits
const int featWasmSimd128 = 0,
    featSharedArrayBuffer = 1,
    featWebgpu = 2,
    featWebgl2 = 3,
    featAvx512 = 4,
    featAvx2 = 5,
    featSse42 = 6,
    featNeon = 7,
    featSve2 = 8,
    featRvv = 9,
    featMetal3 = 10,
    featCuda = 11,
    featAppleMps = 12,
    featOpenvino = 13,
    featFastRpcDsp = 14,
    featNeuropilot = 15,
    featMultilaneDma = 16,
    featBigLittle = 17,
    featThermalSensor = 18,
    featDlpackExport = 19;

// Enum domains
const int tierUnknown = 0, tierFlagship = 1, tierMid = 2, tierBudget = 3;
const int thermalNominal = 0,
    thermalLight = 1,
    thermalModerate = 2,
    thermalSevere = 3,
    thermalCritical = 4;
const int visVisible = 0, visHidden = 1, visUnknown = 2;
const int chargingNo = 0, chargingYes = 1, chargingUnknown = 2;
const int batteryUnknown = 0xffff;

/// CRC-32 (IEEE 802.3, reflected) — identical arithmetic to wire.js /
/// wire.py / generate.mjs. Table is built once (init path).
final Uint32List _crcTable = () {
  final t = Uint32List(256);
  for (var n = 0; n < 256; n++) {
    var c = n;
    for (var k = 0; k < 8; k++) {
      c = (c & 1) != 0 ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    }
    t[n] = c;
  }
  return t;
}();

int crc32Shp1(Uint8List bytes, int len) {
  var c = 0xFFFFFFFF;
  for (var i = 0; i < len; i++) {
    c = _crcTable[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
  }
  return (c ^ 0xFFFFFFFF) & 0xFFFFFFFF;
}

/// Reusable decode target (Law 1). One instance, mutated in place forever.
class ProfileFlyweight {
  int layoutVersion = 0;
  int recordSize = 0;
  int featureFlagsLo = 0;
  int featureFlagsHi = 0;
  int siliconTier = tierUnknown;
  int thermalState = thermalNominal;
  int perfCores = 0;
  int effCores = 0;
  int gpuFamily = 0;
  int cacheLineBytes = 64;
  int cpuMaxClockKhz = 0;
  int memoryTotalBytes = 0;
  int memoryBudgetBytes = 0;
  int simdWidthBits = 0;
  int frameBudgetUs = 0;
  int maxFrameRateMilliHz = 0;
  int batteryPermille = batteryUnknown;
  int batteryCharging = chargingUnknown;
  int visibility = visUnknown;
  int dmaLaneCount = 0;
  int vendorId = 0;
  int deviceId = 0;

  bool hasFeatureBit(int bit) => bit < 32
      ? (featureFlagsLo & (1 << bit)) != 0
      : (featureFlagsHi & (1 << (bit - 32))) != 0;
}

/// Zero-copy window over ONE 192-byte SHP1 record.
class ProfileView {
  final ByteData _dv;
  final Uint8List _bytes;

  /// [data] must expose at least 192 bytes at [offset].
  ProfileView(Uint8List data, [int offset = 0])
      : _bytes = Uint8List.sublistView(data, offset),
        _dv = ByteData.sublistView(data, offset);

  /// Law 4 gate: 0 when intact, else a §6 code (cheapest checks first).
  int validate() {
    if (_bytes.length < shp1RecordSize) return eBadSize;
    if (_bytes[0] != 0x53 || _bytes[1] != 0x48 ||
        _bytes[2] != 0x50 || _bytes[3] != 0x31) {
      return eBadMagic;
    }
    if (_dv.getUint16(4, Endian.little) != shp1LayoutVersion) return eBadVersion;
    if (_dv.getUint16(6, Endian.little) != shp1RecordSize) return eBadSize;
    for (var i = 104; i < shp1CrcOffset; i++) {
      if (_bytes[i] != 0) return eReservedDirty;
    }
    if (crc32Shp1(_bytes, shp1CrcOffset) !=
        _dv.getUint32(shp1CrcOffset, Endian.little)) {
      return eCrcMismatch;
    }
    return 0;
  }

  int get featureFlagsLo => _dv.getUint32(8, Endian.little);
  int get featureFlagsHi => _dv.getUint32(12, Endian.little);
  int get siliconTier => _dv.getUint32(16, Endian.little);
  int get thermalState => _dv.getUint32(20, Endian.little);
  int get perfCores => _dv.getUint32(24, Endian.little);
  int get effCores => _dv.getUint32(28, Endian.little);
  int get gpuFamily => _dv.getUint32(32, Endian.little);
  int get cacheLineBytes => _dv.getUint32(36, Endian.little);
  int get cpuMaxClockKhz => _dv.getUint64(40, Endian.little);
  int get memoryTotalBytes => _dv.getUint64(48, Endian.little);
  int get memoryBudgetBytes => _dv.getUint64(56, Endian.little);
  int get simdWidthBits => _dv.getUint32(64, Endian.little);
  int get frameBudgetUs => _dv.getUint32(68, Endian.little);
  int get maxFrameRateMilliHz => _dv.getUint64(72, Endian.little);
  int get batteryPermille => _dv.getUint32(80, Endian.little);
  int get batteryCharging => _dv.getUint32(84, Endian.little);
  int get visibility => _dv.getUint32(88, Endian.little);
  int get dmaLaneCount => _dv.getUint32(92, Endian.little);
  int get vendorId => _dv.getUint32(96, Endian.little);
  int get deviceId => _dv.getUint32(100, Endian.little);
  int get crc32 => _dv.getUint32(shp1CrcOffset, Endian.little);

  /// Copies all fields into the caller-owned flyweight (no allocation).
  ProfileFlyweight snapshotInto(ProfileFlyweight dst) {
    dst.layoutVersion = _dv.getUint16(4, Endian.little);
    dst.recordSize = _dv.getUint16(6, Endian.little);
    dst.featureFlagsLo = featureFlagsLo;
    dst.featureFlagsHi = featureFlagsHi;
    dst.siliconTier = siliconTier;
    dst.thermalState = thermalState;
    dst.perfCores = perfCores;
    dst.effCores = effCores;
    dst.gpuFamily = gpuFamily;
    dst.cacheLineBytes = cacheLineBytes;
    dst.cpuMaxClockKhz = cpuMaxClockKhz;
    dst.memoryTotalBytes = memoryTotalBytes;
    dst.memoryBudgetBytes = memoryBudgetBytes;
    dst.simdWidthBits = simdWidthBits;
    dst.frameBudgetUs = frameBudgetUs;
    dst.maxFrameRateMilliHz = maxFrameRateMilliHz;
    dst.batteryPermille = batteryPermille;
    dst.batteryCharging = batteryCharging;
    dst.visibility = visibility;
    dst.dmaLaneCount = dmaLaneCount;
    dst.vendorId = vendorId;
    dst.deviceId = deviceId;
    return dst;
  }
}

/// Init-path convenience: validate + snapshot. Returns the §6 code.
int decodeProfile(Uint8List data, ProfileFlyweight dst, [int offset = 0]) {
  final view = ProfileView(data, offset);
  final code = view.validate();
  if (code == 0) view.snapshotInto(dst);
  return code;
}

String errorName(int code) {
  switch (code) {
    case eBadMagic: return eBadMagicName;
    case eBadVersion: return eBadVersionName;
    case eBadSize: return eBadSizeName;
    case eCrcMismatch: return eCrcMismatchName;
    case eReservedDirty: return eReservedDirtyName;
    case eProbeUnavailable: return eProbeUnavailableName;
    case eDeviceLost: return eDeviceLostName;
    case eFfiTimeout: return eFfiTimeoutName;
    case eHeapPressure: return eHeapPressureName;
    case eListenerLeak: return eListenerLeakName;
    case eAlignInvalid: return eAlignInvalidName;
    case eHudContextLost: return eHudContextLostName;
    case eUnmarshalFailed: return eUnmarshalFailedName;
    case eTierExhausted: return eTierExhaustedName;
    case eHudRecovered: return eHudRecoveredName;
  }
  return 'E_UNKNOWN_$code';
}
