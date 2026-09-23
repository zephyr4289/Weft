// main.dart — minimal runnable sketch: synthetic WTR1 ring @120 Hz + fake
// detector + WeftVideoOverlay (zero-GC AI overlay).
//
// Producer and consumer run on one isolate for brevity — the WTR1 wire
// protocol exercised here (commit ordering, seqlock acquire, flyweight
// bind) is byte-identical to the cross-isolate / cross-process case where
// the producer is a native adapter writing shared memory.
//
// The ONLY setState in this file runs at 1 Hz (stats line) — lifecycle-level
// UI, never per frame (Law 1 / weft_notifier contract).
import 'dart:async';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:weft_flutter_tensor/weft_flutter_tensor.dart';

void main() {
  runApp(const WeftTensorExampleApp());
}

// ---------------------------------------------------------------------------
// Synthetic ring builder (demo/dev only — real producers write the same
// bytes from native code; the header writer below mirrors ring.js create()).
// ---------------------------------------------------------------------------

int _align64(int n) => (n + 63) & ~63;

/// Build a valid WTR1 ring header (+ full ring storage) in memory.
ByteData buildSyntheticRing({
  required int slotCount,
  required int payloadCap,
  required List<int> shape,
  int tickHz = 120,
}) {
  final stride = _align64(slotHeaderSize + payloadCap);
  final bd = ByteData(ringHeaderSize + slotCount * stride);
  for (var i = 0; i < 4; i++) {
    bd.setUint8(offMagic + i, ringMagicBytes[i]); // "WEFT"
  }
  bd.setUint16(offLayoutVersion, layoutVersion, Endian.little);
  bd.setUint16(offHeaderSize, ringHeaderSize, Endian.little);
  bd.setUint32(offSlotCount, slotCount, Endian.little);
  bd.setUint32(offSlotStride, stride, Endian.little);
  bd.setUint8(offDtypeCode, dlPackUInt);
  bd.setUint8(offDtypeBits, 8);
  bd.setUint16(offLanes, 1, Endian.little);
  bd.setUint32(offElemSize, 1, Endian.little);
  for (var d = 0; d < shape.length; d++) {
    bd.setUint32(offShape + 4 * d, shape[d], Endian.little);
  }
  var acc = 1;
  for (var d = shape.length - 1; d >= 0; d--) {
    bd.setUint32(offStrides + 4 * d, acc, Endian.little); // row-major, ELEMENTS
    acc *= shape[d];
  }
  bd.setUint32(offTickHz, tickHz, Endian.little);
  bd.setUint32(offFlags, ringFlagLittleEndian, Endian.little);
  bd.setUint32(offHeaderCrc, ringHeaderCrc(bd), Endian.little);
  return bd;
}

// ---------------------------------------------------------------------------
// App
// ---------------------------------------------------------------------------

class WeftTensorExampleApp extends StatelessWidget {
  const WeftTensorExampleApp({super.key});

  @override
  Widget build(BuildContext context) {
    return const MaterialApp(
      home: WeftTensorOverlayScreen(),
    );
  }
}

class WeftTensorOverlayScreen extends StatefulWidget {
  const WeftTensorOverlayScreen({super.key});

  @override
  State<WeftTensorOverlayScreen> createState() =>
      _WeftTensorOverlayScreenState();
}

class _WeftTensorOverlayScreenState extends State<WeftTensorOverlayScreen> {
  static const int _slotCount = 4;
  static const int _payloadCap = 64 * 1024;

  late final WeftRing _ring;
  late final OverlayScratch _scratch;
  late final Uint8List _framePayload;
  Timer? _producer;
  Timer? _stats;
  String _statsLine = 'starting…';

  @override
  void initState() {
    super.initState();
    final header = buildSyntheticRing(
      slotCount: _slotCount,
      payloadCap: _payloadCap,
      shape: const [64, 256, 4], // synthetic RGBA-ish plane
    );
    _ring = WeftRing.attachByteData(header); // full Law-4 validation
    _scratch = OverlayScratch(maxBoxes: 16);
    _framePayload = Uint8List(_payloadCap);
    for (var i = 0; i < _payloadCap; i++) {
      _framePayload[i] = (i * 7 + 13) & 0xff;
    }
    // 120 Hz simulated producer (tick hint mirrors the header's tick_hz).
    _producer = Timer.periodic(const Duration(microseconds: 8333), (_) {
      final seq = _ring.producerSeq + 1;
      _ring.commit(
        _framePayload,
        timestampNs: seq * 1000000, // 1 ms cadence, monotonic ns
        durationUs: 8333,
        fourccCode: fourccFromString('RGBA'),
      );
    });
    // Cold-path stats refresh (setState allowed OUTSIDE the frame loop).
    _stats = Timer.periodic(const Duration(seconds: 1), (_) {
      if (!mounted) return;
      setState(() {
        _statsLine = 'seq=${_ring.producerSeq}  commits=${_ring.stats.commits} '
            'acquires=${_ring.stats.acquireCalls} torn=${_ring.stats.tornReads} '
            'overruns=${_ring.stats.overruns}';
      });
    });
  }

  @override
  void dispose() {
    _producer?.cancel();
    _stats?.cancel();
    _ring.dispose();
    super.dispose();
  }

  /// Fake detector: deterministic moving boxes keyed off the frame sequence.
  /// ZERO allocation — fills the reused scratch in place (Law 1).
  void _fillScratch(WeftTensorView view) {
    _scratch.clear();
    final t = (view.seq % 3600) / 3600.0;
    final x1 = 40 + 120 * (0.5 + 0.5 * math.sin(2 * math.pi * t));
    final y1 = 60 + 80 * (0.5 + 0.5 * math.cos(2 * math.pi * t * 2));
    final x2 = 200 + 60 * (0.5 + 0.5 * math.cos(2 * math.pi * t * 3));
    final y2 = 140 + 40 * (0.5 + 0.5 * math.sin(2 * math.pi * t));
    _scratch
      ..pushBox(x1, y1, 64, 48, 0.91, 0)
      ..pushBox(x2, y2, 80, 60, 0.76, 1);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      appBar: AppBar(title: const Text('weft_flutter_tensor — zero-GC overlay')),
      body: Column(
        children: [
          Expanded(
            child: WeftVideoOverlay(
              ring: _ring,
              scratch: _scratch,
              onFrame: _fillScratch,
            ),
          ),
          Padding(
            padding: const EdgeInsets.all(8),
            child: Text(
              '${_ring.layout.describe()}  $_statsLine',
              style: const TextStyle(fontFamily: 'monospace', fontSize: 11),
            ),
          ),
        ],
      ),
    );
  }
}
