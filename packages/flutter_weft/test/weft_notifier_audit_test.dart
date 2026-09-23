// weft_notifier static audit — no Dart/Flutter toolchain in the codegen
// sandbox, so the Pillar 1 §2.E laws are enforced STRUCTURALLY (the same
// gate style as tools/weftc/codegen/dart/test). Runtime verification rides
// the flutter-packages CI lane.
//
// Gates:
//   Zero-alloc notify path   frame() is an indexed loop over a fixed array;
//                            no List growth, no iterators, no closures
//   Cold-path honesty        growth happens ONLY in addListener (doubling)
//   Listenable contract      implements Listenable (addListener/removeListener)
//   Pump                     one view instance reused; validator consulted
//                            before bind; rejected/accepted counters
//   Window cache             identity-cached ByteData re-derivation
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

void main() {
  final file = File('lib/src/weft_notifier.dart').existsSync()
      ? File('lib/src/weft_notifier.dart')
      : (File('packages/flutter_weft/lib/src/weft_notifier.dart').existsSync()
          ? File('packages/flutter_weft/lib/src/weft_notifier.dart')
          : File('../../packages/flutter_weft/lib/src/weft_notifier.dart'));
  final src = file.readAsStringSync().replaceAll('\r\n', '\n');

  test('WeftNotifier implements Listenable with the full contract', () {
    expect(src, contains('class WeftNotifier implements Listenable'));
    expect(src, contains('@override\n  void addListener(VoidCallback listener)'));
    expect(src, contains('@override\n  void removeListener(VoidCallback listener)'));
    expect(src, contains('void dispose()'));
  });

  test('frame() hot path: indexed loop, no allocation, no iterators', () {
    final frameBody = src.split('void frame() {')[1].split('void addListener')[0];
    expect(frameBody, isNot(contains('map(')));
    expect(frameBody, isNot(contains('where(')));
    expect(frameBody, isNot(contains('for (final'))); // iterator loops allocate
    expect(frameBody, contains('for (var i = 0; i < n; i++)'));
    expect(frameBody, isNot(contains('List(')));
    expect(frameBody, isNot(contains('[]'))); // no literal list construction
  });

  test('growth is confined to addListener (doubling, cold path)', () {
    final addBody = src.split('void addListener(VoidCallback listener) {')[1].split('void removeListener')[0];
    expect(addBody, contains('_listeners.length * 2'));
    final frameBody = src.split('void frame() {')[1].split('void addListener')[0];
    expect(frameBody, isNot(contains('* 2')));
  });

  test('removeListener uses swap-remove (no list rebuild)', () {
    final rmBody = src.split('void removeListener(VoidCallback listener) {')[1].split('void dispose')[0];
    expect(rmBody, contains('swap-remove'));
    expect(rmBody, isNot(contains('remove(')));
    expect(rmBody, isNot(contains('where(')));
  });

  test('WeftFramePump reuses one view; validates before binding', () {
    final pump = src.split('class WeftFramePump<V> {')[1];
    expect(pump, contains('final V view;'));
    expect(pump, contains('if (validator != null && !validator(view, bd, byteOffset, avail))'));
    expect(pump, contains('bind(view, bd, byteOffset)'));
    expect(pump, contains('notifier.frame()'));
    expect(pump, contains("rejected++"));
    expect(pump, contains("accepted++"));
  });

  test('WeftWindowCache re-derives only on source identity change', () {
    final cache = src.split('class WeftWindowCache {')[1].split('/// Binds a frame window')[0];
    expect(cache, contains('identical(_source, window)'));
    expect(cache, contains('ByteData.view('));
    expect(cache, isNot(contains('Uint8List.fromList')));
    expect(cache, isNot(contains('asUint8List()'))); // no copies
  });

  test('api surface versioned for the parity scorecard', () {
    expect(src, contains('weftNotifierApiVersion = 1'));
  });
}
