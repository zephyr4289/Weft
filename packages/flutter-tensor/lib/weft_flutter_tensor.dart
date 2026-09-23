/// weft_flutter_tensor — real-time AI tensor widgets over WTR1 rings.
///
/// Pillar 2, Deliverable D (Flutter sub-build). Exports:
///
/// * [ring_header] — PURE-DART WTR1 constants, header parse/validate,
///   CRC-32/IEEE (runs on the plain VM; no flutter/ffi imports).
/// * [weft_ring] — [WeftRing]: dart:ffi attach + seqlock acquire, zero alloc.
/// * [weft_tensor_view] — [WeftTensorView]: reused frame flyweight.
/// * [weft_tensor_notifier] — [WeftTensorNotifier]: fixed-capacity Listenable
///   (mirror of Pillar 1's WeftNotifier zero-GC discipline).
/// * [weft_tensor_painter] — [WeftTensorPainter] + [OverlayScratch] +
///   COCO-17 skeleton edges (zero-GC overlay painting).
/// * [weft_video_overlay] — [WeftVideoOverlay]: one Ticker at display
///   refresh driving acquire -> notify -> paint with no setState.
library weft_flutter_tensor;

export 'src/ring_header.dart';
export 'src/weft_ring.dart';
export 'src/weft_tensor_view.dart';
export 'src/weft_tensor_notifier.dart';
export 'src/weft_tensor_painter.dart';
export 'src/weft_video_overlay.dart';
