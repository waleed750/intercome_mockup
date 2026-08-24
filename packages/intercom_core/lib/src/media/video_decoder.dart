import 'dart:ui' as ui;

import 'package:flutter/services.dart';

final class VideoDecoder {
  VideoDecoder({this.channel = const MethodChannel('syncn_intercom/video')});

  final MethodChannel channel;
  int? _textureId;

  int? get textureId => _textureId;

  /// Reports the actual screen's physical pixel size so the native side can
  /// decode-and-scale directly to roughly that size instead of always doing
  /// full 1280x720 decode -- panels vary (4-inch vs 10-inch builds use
  /// different physical resolutions), so this is read from the live display
  /// at call time rather than assumed. Falls back to native (no scaling) if
  /// the display size can't be read for some reason.
  Future<int> start() async {
    int screenWidth = 0;
    int screenHeight = 0;
    final views = ui.PlatformDispatcher.instance.views;
    if (views.isNotEmpty) {
      final view = views.first;
      screenWidth = view.physicalSize.width.round();
      screenHeight = view.physicalSize.height.round();
    }
    _textureId = await channel.invokeMethod<int>('start', {
      'screenWidth': screenWidth,
      'screenHeight': screenHeight,
    });
    return _textureId ?? -1;
  }

  Future<void> submit(Uint8List frame) =>
      channel.invokeMethod<void>('submit', frame);

  Future<void> stop() async {
    await channel.invokeMethod<void>('stop');
    _textureId = null;
  }
}
