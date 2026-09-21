// example/lib/main.dart — heddle-2.0 flight example: a 240 FPS live chart fed
// by a WeftHotPlane, with ZERO widget rebuilds on the telemetry path.
//
// The native shm attach is a drop-in swap: WeftHotPlane.fromBytes(
//   Pointer<Uint8>.asTypedList(...)) over the mapped region — the widget tree
// below is unchanged.
//
// NOTE: CI-gated on the flutter lane (no Flutter SDK in the authoring
// sandbox); the mechanical structural audit is test/static_audit.mjs.

import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'package:weft_flutter/weft_flutter.dart';

void main() => runApp(const WeftHeddleDemo());

class WeftHeddleDemo extends StatelessWidget {
  const WeftHeddleDemo({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'weft_flutter — heddle-2.0',
      theme: ThemeData.dark(),
      home: const PlaneChartPage(),
    );
  }
}

class PlaneChartPage extends StatefulWidget {
  const PlaneChartPage({super.key});

  @override
  State<PlaneChartPage> createState() => _PlaneChartPageState();
}

class _PlaneChartPageState extends State<PlaneChartPage> {
  late final WeftHotPlane plane;
  late final _ProducerRig _rig;

  @override
  void initState() {
    super.initState();
    plane = _rigPlane();
    _rig = _ProducerRig(plane);
    _rig.start();
  }

  @override
  void dispose() {
    _rig.stop();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('heddle-2.0 — WeftCanvasWidget')),
      body: WeftCanvasWidget(plane: plane, hz: 240),
    );
  }
}

WeftHotPlane _rigPlane() {
  final geo = Hpl1.deriveGeometry(4, 256);
  final bytes = Uint8List(geo.totalBytes);
  final dv = ByteData.sublistView(bytes);
  dv.setUint32(Hpl1.hdrMagic, Hpl1.magicU32, Endian.little);
  dv.setUint32(Hpl1.hdrVersion, Hpl1.version, Endian.little);
  dv.setUint32(Hpl1.hdrFlags, Hpl1.flagLeRequired, Endian.little);
  dv.setUint32(Hpl1.hdrLaneCount, 4, Endian.little);
  dv.setUint32(Hpl1.hdrSamplesPerLane, 256, Endian.little);
  dv.setUint32(Hpl1.hdrRingMask, 255, Endian.little);
  dv.setUint32(Hpl1.hdrDirtyWords, geo.dirtyWords, Endian.little);
  dv.setUint32(Hpl1.hdrLaneCtrlStride, Hpl1.laneCtrlStride, Endian.little);
  dv.setUint32(Hpl1.hdrRingBase, geo.ringBase, Endian.little);
  dv.setUint32(Hpl1.hdrTotalBytes, geo.totalBytes, Endian.little);
  return WeftHotPlane.fromBytes(bytes);
}

/// Minimal single-threaded producer rig: publishes a sine wave on lane 0 at
/// ~1 kHz so the demo shows live data without native dependencies.
class _ProducerRig {
  final WeftHotPlane plane;
  bool _running = false;
  int _seq = 0;
  int _seen = 0;

  _ProducerRig(this.plane);

  void start() {
    _running = true;
    _loop();
  }

  void stop() => _running = false;

  void _loop() async {
    final geo = plane.geo;
    while (_running) {
      await Future<void>.delayed(const Duration(milliseconds: 1));
      final dv = ByteData.sublistView(plane.bytes);
      final ctrl = geo.laneCtrl(0);
      final v = 100 + 40 * _sin(_seen / 20);
      dv.setUint32(ctrl + Hpl1.laneSeq, _seq * 2 + 1, Endian.little); // odd
      dv.setFloat64(ctrl + Hpl1.laneCurrent, v, Endian.little);
      dv.setFloat64(ctrl + Hpl1.laneMin, 60, Endian.little);
      dv.setFloat64(ctrl + Hpl1.laneMax, 140, Endian.little);
      dv.setFloat64(ctrl + Hpl1.laneAvg, 100, Endian.little);
      dv.setUint32(ctrl + Hpl1.laneHead, (_seen + 1) & 255, Endian.little);
      dv.setUint32(ctrl + Hpl1.laneSamplesSeen, _seen + 1, Endian.little);
      dv.setFloat64(geo.laneRing(0) + (_seen & 255) * 8, v, Endian.little);
      dv.setUint32(ctrl + Hpl1.laneSeq, _seq * 2 + 2, Endian.little); // even
      dv.setUint32(Hpl1.headerSize, 1, Endian.little); // dirty bit lane 0
      _seq++;
      _seen++;
    }
  }

  static double _sin(num x) {
    // tiny sine approximation — keeps the example dependency-free
    var t = x.toDouble() % 6.283185307;
    var acc = t;
    var term = t;
    for (var k = 3; k < 13; k += 2) {
      term *= -t * t / (k * (k - 1));
      acc += term;
    }
    return acc;
  }
}
