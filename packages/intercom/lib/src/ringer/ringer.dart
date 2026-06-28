import 'dart:async';

import 'package:flutter/services.dart';
import 'package:vibration/vibration.dart';

final class Ringer {
  Ringer({this.channel = const MethodChannel('syncn_intercom/ringer')});

  final MethodChannel channel;
  Timer? _timer;

  Future<void> start() async {
    await channel.invokeMethod<void>('start').catchError((_) {});
    _timer?.cancel();
    _timer = Timer.periodic(const Duration(milliseconds: 1400), (_) {
      Vibration.vibrate(pattern: const [0, 800, 600]);
    });
    await Vibration.vibrate(pattern: const [0, 800, 600]);
  }

  Future<void> stop() async {
    _timer?.cancel();
    _timer = null;
    await Vibration.cancel();
    await channel.invokeMethod<void>('stop').catchError((_) {});
  }
}
