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

  Future<void> start({required bool captureEnabled}) {
    return methodChannel
        .invokeMethod<void>('start', {'captureEnabled': captureEnabled});
  }

  Future<void> playDownlink(Uint8List alaw) =>
      methodChannel.invokeMethod<void>('playDownlink', alaw);

  Future<void> setMuted(bool muted) =>
      methodChannel.invokeMethod<void>('setMuted', muted);

  Future<void> stop() => methodChannel.invokeMethod<void>('stop');
}
