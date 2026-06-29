import 'dart:async';
import 'dart:convert';

import 'package:flutter_local_notifications/flutter_local_notifications.dart';
import '../call/incoming_call_handler.dart';
import 'package:vibration/vibration.dart';

/// Notification action identifiers used by the full-screen handler.
enum _Action {
  answer('answer'),
  reject('reject');

  const _Action(this.id);
  final String id;

  static _Action? fromId(String? id) {
    for (final a in values) {
      if (a.id == id) return a;
    }
    return null;
  }
}

/// Callback fired when the user taps a notification action.
typedef FullScreenCallTap = Future<void> Function(
    bool accepted, Map<String, dynamic> data);

/// Background entry-point required by flutter_local_notifications.
@pragma('vm:entry-point')
void _backgroundHandler(NotificationResponse response) {}

/// Implements [IncomingCallHandler] with full ringtone + vibration +
/// full-screen notification.
///
/// Uses [flutter_local_notifications] for a high-priority notification with
/// `fullScreenIntent`, Answer/Reject actions, and starts a vibration pattern
/// (0ms delay, 800ms vibrate, 600ms pause — repeating every 1400ms).
final class FullScreenCallHandler implements IncomingCallHandler {
  FullScreenCallHandler({
    FlutterLocalNotificationsPlugin? plugin,
    this.onTap,
  }) : _plugin = plugin ?? FlutterLocalNotificationsPlugin();

  final FlutterLocalNotificationsPlugin _plugin;
  final FullScreenCallTap? onTap;

  static const int _notificationId = 8189;

  // ── Notification channel (inlined from the old notification_channels.dart) ──

  static const _channel = AndroidNotificationChannel(
    'intercom_incoming_call',
    'Intercom calls',
    description: 'Incoming intercom calls',
    importance: Importance.max,
    playSound: true,
    enableVibration: true,
  );

  Timer? _vibrationTimer;

  // ── IncomingCallHandler ────────────────────────────────────────────────────

  @override
  Future<void> initialize() async {
    const android = AndroidInitializationSettings('@mipmap/ic_launcher');
    const ios = DarwinInitializationSettings();
    await _plugin.initialize(
      const InitializationSettings(android: android, iOS: ios),
      onDidReceiveNotificationResponse: _handleResponse,
      onDidReceiveBackgroundNotificationResponse: _backgroundHandler,
    );

    // Ensure the notification channel exists.
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
          importance: Importance.max,
          priority: Priority.max,
          category: AndroidNotificationCategory.call,
          fullScreenIntent: true,
          actions: [
            AndroidNotificationAction(_Action.answer.id, 'Answer',
                showsUserInterface: true),
            AndroidNotificationAction(_Action.reject.id, 'Reject',
                cancelNotification: true),
          ],
        ),
        iOS: const DarwinNotificationDetails(
          presentAlert: true,
          presentSound: true,
          categoryIdentifier: 'intercom_call',
        ),
      ),
      payload: payload,
    );

    // Start vibration pattern: 0ms delay, 800ms vibrate, 600ms pause.
    _startVibration();
  }

  @override
  Future<void> onCallDismissed() async {
    await _plugin.cancel(_notificationId);
    _stopVibration();
  }

  @override
  Future<void> dispose() async {
    await _plugin.cancel(_notificationId);
    _stopVibration();
  }

  // ── Private helpers ───────────────────────────────────────────────────────

  void _startVibration() {
    _stopVibration();
    _vibrate(); // first pulse immediately
    _vibrationTimer = Timer.periodic(
      const Duration(milliseconds: 1400),
      (_) => _vibrate(),
    );
  }

  void _vibrate() {
    Vibration.vibrate(duration: 800);
  }

  void _stopVibration() {
    _vibrationTimer?.cancel();
    _vibrationTimer = null;
    Vibration.cancel();
  }

  Future<void> _handleResponse(NotificationResponse response) async {
    final action = _Action.fromId(response.actionId);
    final accepted = action != _Action.reject;
    final data = response.payload == null
        ? <String, dynamic>{}
        : jsonDecode(response.payload!) as Map<String, dynamic>;
    _stopVibration();
    await onTap?.call(accepted, data);
  }
}
