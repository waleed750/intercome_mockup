import 'package:flutter/services.dart';

/// Thin wrapper around the `syncn/video` platform channel implemented by
/// `flutterpi/src/syncn_video.c`.
///
/// Unlike this package's older `VideoDecoder` (which pushed each H.264 NAL
/// from Dart into a native GStreamer appsrc via `submit()`), this plugin
/// pulls already-decoded frames directly from `syncn-intercomd`'s own Unix
/// socket -- the daemon decodes H.264 in hardware (Rockchip MPP) and hands
/// finished DMA-BUF frames straight to the plugin. Dart's only job is
/// `start()` (which returns a texture id to give a `Texture` widget) and
/// `stop()`; there is no `submit()` because Dart never sees frame bytes at
/// all with this backend.
final class IntercomdVideo {
  IntercomdVideo({this.channel = const MethodChannel('syncn/video')});

  final MethodChannel channel;
  int? _textureId;

  int? get textureId => _textureId;

  Future<int> start() async {
    final result =
        await channel.invokeMethod<Map<Object?, Object?>>('start');
    _textureId = (result?['textureId'] as num?)?.toInt();
    return _textureId ?? -1;
  }

  Future<void> stop() async {
    await channel.invokeMethod<void>('stop');
    _textureId = null;
  }
}
