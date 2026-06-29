import 'package:flutter/services.dart';

final class AndroidForegroundService {
  const AndroidForegroundService._();

  static const _channel = MethodChannel('syncn_intercom/foreground_service');

  static Future<void> startPanelService() {
    return _channel.invokeMethod<void>('startPanelService');
  }

  static Future<void> stopPanelService() {
    return _channel.invokeMethod<void>('stopPanelService');
  }

  static Future<bool> takePendingIncomingCall() async {
    return await _channel.invokeMethod<bool>('takePendingIncomingCall') ??
        false;
  }
}
