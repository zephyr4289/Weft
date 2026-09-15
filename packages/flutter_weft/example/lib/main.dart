import 'dart:ffi';
import 'package:ffi/ffi.dart';
import 'package:flutter/material.dart';
import 'package:weft_flutter/weft_flutter.dart';

void main() {
  runApp(const WeftExampleApp());
}

class WeftExampleApp extends StatelessWidget {
  const WeftExampleApp({super.key});

  @override
  Widget build(BuildContext context) {
    return const MaterialApp(
      home: WeftDesktopScreen(),
    );
  }
}

class WeftDesktopScreen extends StatefulWidget {
  const WeftDesktopScreen({super.key});

  @override
  State<WeftDesktopScreen> createState() => _WeftDesktopScreenState();
}

class _WeftDesktopScreenState extends State<WeftDesktopScreen> {
  late final WeftNativeBindings _bindings;
  late final WeftFFI _weft;
  late final Pointer<Uint8> _readBuf;

  @override
  void initState() {
    super.initState();
    final dylib = WeftNativeBindings.openLibrary();
    _bindings = WeftNativeBindings(dylib);
    _weft = WeftFFI.allocate(_bindings, 256);
    _readBuf = calloc<Uint8>(256);
  }

  @override
  void dispose() {
    calloc.free(_readBuf);
    _weft.destroy();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Weft Flutter/FFI Desktop Example'),
      ),
      body: Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            CustomPaint(
              size: const Size(300, 300),
              painter: WeftPainter(
                weft: _weft,
                readBuffer: _readBuf,
                onPaint: (canvas, size, buffer) {
                  final paint = Paint()..color = Colors.blue;
                  canvas.drawCircle(
                    Offset(size.width / 2, size.height / 2),
                    80.0,
                    paint,
                  );
                },
              ),
            ),
            const SizedBox(height: 20),
            Text('Published: ${_weft.tPublishCount} | Claimed: ${_weft.tClaimCount}'),
          ],
        ),
      ),
    );
  }
}
