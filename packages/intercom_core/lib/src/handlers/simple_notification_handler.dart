import 'dart:async';
import 'dart:convert';

import 'package:flutter_local_notifications/flutter_local_notifications.dart';
import '../call/incoming_call_handler.dart';

/// Callback fired when the user taps a notification action.
typedef SimpleCallTap = Future<void> Function(
    bool accepted, Map<String, dynamic> data);

/// Background entry-point required by flutter_local_notifications.
@pragma('vm:entry-point')
void _backgroundHandler(NotificationResponse response) {}

/// Implements [IncomingCallHandler] with a simple push notification
/// (Answer/Cancel buttons).
///
/// No ringtone, no vibration, no full-screen intent. Suitable for
/// secondary devices or companion apps where a subtle notification
/// is preferred.
final class SimpleNotificationHandler implements IncomingCallHandler {
  SimpleNotificationHandler({
    FlutterLocalNotificationsPlugin? plugin,
    this.onTap,
  }) : _plugin = plugin ?? FlutterLocalNotificationsPlugin();

  final FlutterLocalNotificationsPlugin _plugin;
  final SimpleCallTap? onTap;

  static const int _notificationId = 8190;

  static const _channel = AndroidNotificationChannel(
    'intercom_simple_call',
    'Intercom notifications',
    description: 'Simple intercom call notifications',
    importance: Importance.defaultImportance,
    playSound: false,
    enableVibration: false,
  );

  @override
  Future<void> initialize() async {
    const android = AndroidInitializationSettings('@mipmap/ic_launcher');
    const ios = DarwinInitializationSettings();
    await _plugin.initialize(
      const InitializationSettings(android: android, iOS: ios),
      onDidReceiveNotificationResponse: _handleResponse,
      onDidReceiveBackgroundNotificationResponse: _backgroundHandler,
    );

    final androidImpl = _plugin.resolvePlatformSpecificImplementation<
        AndroidFlutterLocalNotificationsPlugin>();
    await androidImpl?.createNotificationChannel(_channel);
  }

  @override
  Future<void> onIncomingCall({
    required String doorName,
    Map<String, dynamic>? data,
  }) async {
    final payload = jsonEncode({
      ...?data,
      'doorName': doorName,
    });

    await _plugin.show(
      _notificationId,
      doorName,
      'Incoming intercom call',
      NotificationDetails(
        android: AndroidNotificationDetails(
          _channel.id,
          _channel.name,
          channelDescription: _channel.description,
          importance: Importance.defaultImportance,
          priority: Priority.defaultPriority,
          category: AndroidNotificationCategory.call,
          actions: [
            const AndroidNotificationAction('answer', 'Answer',
                showsUserInterface: true),
            const AndroidNotificationAction('cancel', 'Cancel',
                cancelNotification: true),
          ],
        ),
        iOS: const DarwinNotificationDetails(
          presentAlert: true,
          presentSound: false,
          categoryIdentifier: 'intercom_call',
        ),
      ),
      payload: payload,
    );
  }

  @override
  Future<void> onCallDismissed() async {
    await _plugin.cancel(_notificationId);
  }

  @override
  Future<void> dispose() async {
    await _plugin.cancel(_notificationId);
  }

  Future<void> _handleResponse(NotificationResponse response) async {
    final accepted = response.actionId != 'cancel';
    final data = response.payload == null
        ? <String, dynamic>{}
        : jsonDecode(response.payload!) as Map<String, dynamic>;
    await onTap?.call(accepted, data);
  }
}
