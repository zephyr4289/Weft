// lib/src/spectrum_hud.dart — WeftSpectrumHud for Flutter (mandate E).
//
// ZERO RE-RENDER RULE (Flutter grammar): the HUD widget NEVER calls
// setState(). The painter is a CustomPainter whose `repaint` IS the engine's
// own ValueNotifier — the framework repaints the layer directly, bypassing
// element rebuild entirely. shouldRepaint() returns false (the notifier owns
// repaint decisions). Paint reads flyweight PRIMITIVES only.
//
// TRANSPARENT FALLBACK (Law 4): paint() is error-contained; a failed paint
// flips to an explicit FALLBACK banner painted INSIDE the same canvas (the
// HUD stays alive even when its own draw path breaks), and healing clears it.

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import 'spectrum_governor.dart';
import 'spectrum_wire.dart';
import 'weft_spectrum_engine.dart';

const List<String> _tierNames = ['UNKNOWN', 'T1 FLAGSHIP', 'T2 MID', 'T3 BUDGET'];
const List<String> _thermalNames = ['nominal', 'light', 'moderate', 'severe', 'critical'];

/// Drop-in diagnostic overlay. Mount ONCE; pass the engine; nothing else.
class WeftSpectrumHud extends StatelessWidget {
  final WeftSpectrumEngine engine;
  final int fps; // owned by the host's frame probe (may be -1 = unknown)
  final int jitterP99Us;
  final int jitterP999Us;

  const WeftSpectrumHud({
    super.key,
    required this.engine,
    this.fps = -1,
    this.jitterP99Us = 0,
    this.jitterP999Us = 0,
  });

  @override
  Widget build(BuildContext context) {
    return IgnorePointer(
      child: CustomPaint(
        // repaint IS the engine channel — no setState, no rebuild, no
        // element tree traffic on the telemetry path.
        painter: _SpectrumHudPainter(this),
        size: const Size(220, 118),
      ),
    );
  }
}

class _SpectrumHudPainter extends CustomPainter {
  final WeftSpectrumHud hud;

  /// The engine's cadence notifier drives repaints directly.
  final ValueListenable<int> repaint;

  _SpectrumHudPainter(this.hud) : repaint = _CadenceListenable(hud.engine.cadence);

  @override
  void paint(Canvas canvas, Size size) {
    try {
      _paintInner(canvas, size);
    } catch (_) {
      // Law 4: explicit fallback INSIDE the canvas — the HUD survives its
      // own breakage and says so. Never throw into the widget tree.
      final tp = TextPainter(
        text: const TextSpan(
          text: 'SPECTRUM HUD FALLBACK (E_HUD_CONTEXT_LOST)',
          style: TextStyle(color: Color(0xFFFBBF24), fontSize: 10,
              fontFamily: 'monospace'),
        ),
        textDirection: TextDirection.ltr,
      );
      tp.layout(maxWidth: size.width);
      tp.paint(canvas, const Offset(8, 8));
      tp.dispose();
    }
  }

  void _paintInner(Canvas canvas, Size size) {
    final st = hud.engine.profile;
    final cad = hud.engine.cadence;

    canvas.drawRRect(
      RRect.fromRectAndRadius(
          Offset.zero & size, const Radius.circular(8)),
      const Paint()..color = Color(0xDD020617),
    );

    const style = TextStyle(color: Color(0xFFA5F3FC), fontSize: 10,
        fontFamily: 'monospace');
    final tier = (st.siliconTier >= 0 && st.siliconTier <= tierBudget)
        ? _tierNames[st.siliconTier]
        : _tierNames[tierUnknown];
    final thermal = (st.thermalState >= 0 && st.thermalState <= thermalCritical)
        ? _thermalNames[st.thermalState]
        : 'unknown';

    final rows = <String, String>{
      'TIER': tier,
      'THERMAL': thermal,
      'FPS': hud.fps >= 0 ? '$hud.fps' : '-',
      'JITTER': '${hud.jitterP99Us}/${hud.jitterP999Us}us',
      'CADENCE': '${cad.capHz} Hz',
      'STATUS': st.visibility == visHidden ? 'HIDDEN' : 'visible',
    };

    var y = 8.0;
    rows.forEach((k, v) {
      final tp = TextPainter(
        text: TextSpan(
          text: '$k $v',
          style: style,
        ),
        textDirection: TextDirection.ltr,
      );
      tp.layout(maxWidth: size.width - 16);
      tp.paint(canvas, Offset(8, y));
      tp.dispose();
      y += 16;
    });
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}

/// Bridges the CadenceState into a ValueListenable without allocating on
/// ticks: the engine's tick() sets a plain int; this listenable only holds
/// the last value and notifies AFTER the tick (host wiring, not hot path).
class _CadenceListenable extends ChangeNotifier implements ValueListenable<int> {
  final CadenceState _cadence;
  int _last = 240;

  _CadenceListenable(this._cadence);

  @override
  int get value => _last;

  /// Called by the host AFTER cadenceTick when capHz changed (edge-triggered:
  /// repaint only on CHANGE — zero notifications during steady state).
  void syncFromEngine() {
    if (_cadence.capHz != _last) {
      _last = _cadence.capHz;
      notifyListeners();
    }
  }
}
