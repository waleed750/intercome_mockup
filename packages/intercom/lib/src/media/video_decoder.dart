import 'dart:collection';

import 'package:flutter/services.dart';

final class VideoDecoder {
  VideoDecoder({this.channel = const MethodChannel('syncn_intercom/video')});

  final MethodChannel channel;
  int? _textureId;
  final Queue<Uint8List> _pending = Queue<Uint8List>();
  static const _maxPending = 60;

  int? get textureId => _textureId;

  Future<int> start() async {
    _textureId = await channel.invokeMethod<int>('start');
    await _flushPending();
    return _textureId ?? -1;
  }

  Future<void> submit(Uint8List frame) async {
    if (_textureId != null) {
      await channel.invokeMethod<void>('submit', frame);
      return;
    }
    if (_pending.length >= _maxPending) _pending.removeFirst();
    _pending.add(frame);
  }

  Future<void> stop() async {
    _pending.clear();
    await channel.invokeMethod<void>('stop');
    _textureId = null;
  }

  Future<void> _flushPending() async {
    while (_pending.isNotEmpty && _textureId != null) {
      await channel.invokeMethod<void>('submit', _pending.removeFirst());
    }
    _pending.clear();
  }
}
