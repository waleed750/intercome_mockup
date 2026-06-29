import 'package:flutter/services.dart';

final class VideoDecoder {
  VideoDecoder({this.channel = const MethodChannel('syncn_intercom/video')});

  final MethodChannel channel;
  int? _textureId;

  int? get textureId => _textureId;

  Future<int> start() async {
    _textureId = await channel.invokeMethod<int>('start');
    return _textureId ?? -1;
  }

  Future<void> submit(Uint8List frame) =>
      channel.invokeMethod<void>('submit', frame);

  Future<void> stop() async {
    await channel.invokeMethod<void>('stop');
    _textureId = null;
  }
}
