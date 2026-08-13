import 'package:flutter/services.dart';

typedef EncodedAudioSink = void Function(Uint8List alaw);

final class AudioPipeline {
  AudioPipeline({
    this.methodChannel = const MethodChannel('syncn_intercom/audio'),
    this.eventChannel = const EventChannel('syncn_intercom/audio_uplink'),
  });

  final MethodChannel methodChannel;
  final EventChannel eventChannel;
  Stream<Uint8List>? _uplink;

  Stream<Uint8List> get uplink {
    return _uplink ??= eventChannel
        .receiveBroadcastStream()
        .where((event) => event is Uint8List)
        .cast<Uint8List>();
  }

  Future<void> start({required bool captureEnabled, bool headsetMode = false}) {
    return methodChannel.invokeMethod<void>('start', {
      'captureEnabled': captureEnabled,
      'headsetMode': headsetMode,
    });
  }

  Future<void> playDownlink(Uint8List alaw) =>
      methodChannel.invokeMethod<void>('playDownlink', alaw);

  Future<void> setMuted(bool muted) =>
      methodChannel.invokeMethod<void>('setMuted', muted);

  /// Manually selects headset (wired headphone) vs speaker output routing
  /// for platforms that can't detect a jack insertion themselves (the
  /// flutter-pi/Linux panel target has no kernel jack-detect device for its
  /// rk809 codec -- see syncn_intercom_audio.c). No-op / ignored on
  /// platforms (e.g. Android) that already route automatically.
  Future<void> setHeadsetMode(bool headsetMode) =>
      methodChannel.invokeMethod<void>('setHeadsetMode', headsetMode);

  Future<void> stop() => methodChannel.invokeMethod<void>('stop');
}
