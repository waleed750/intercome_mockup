import 'dart:collection';

import 'package:flutter/services.dart';

final class VideoDecoder {
  VideoDecoder({this.channel = const MethodChannel('syncn_intercom/video')});

  final MethodChannel channel;
  int? _textureId;
  bool _starting = false;
  final Queue<Uint8List> _pending = Queue<Uint8List>();
  static const _maxPending = 60;

  int? get textureId => _textureId;

  Future<int> start() async {
    _starting = true;
    _textureId = await channel.invokeMethod<int>('start');
    _starting = false;
    await _flushPending();
    return _textureId ?? -1;
  }

  Future<void> submit(Uint8List frame) async {
    if (_starting) {
      if (_pending.length >= _maxPending) _pending.removeFirst();
      _pending.add(frame);
      return;
    }
    if (_textureId == null) return;
    await channel.invokeMethod<void>('submit', frame);
  }

  Future<void> stop() async {
    _starting = false;
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
