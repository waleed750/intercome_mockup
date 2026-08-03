import 'dart:async';
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:vibration/vibration.dart';

final class Ringer {
  Ringer({this.channel = const MethodChannel('syncn_intercom/ringer')});

  final MethodChannel channel;
  Timer? _timer;

  // The `vibration` package has no desktop backend; a haptic motor doesn't
  // exist on Linux anyway, so the native `channel` sound below is Linux's
  // ring signal instead.
  static bool get _canVibrate => Platform.isAndroid || Platform.isIOS;

  Future<void> start() async {
    await channel.invokeMethod<void>('start').catchError((_) {});
    if (!_canVibrate) return;
    _timer?.cancel();
    _timer = Timer.periodic(const Duration(milliseconds: 1400), (_) {
      Vibration.vibrate(pattern: const [0, 800, 600]);
    });
    await Vibration.vibrate(pattern: const [0, 800, 600]);
  }

  Future<void> stop() async {
    _timer?.cancel();
    _timer = null;
    if (_canVibrate) await Vibration.cancel();
    await channel.invokeMethod<void>('stop').catchError((_) {});
  }
}
