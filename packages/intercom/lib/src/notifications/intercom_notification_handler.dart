import 'dart:convert';

import 'package:flutter_local_notifications/flutter_local_notifications.dart';

import 'notification_actions.dart';
import 'notification_channels.dart';

typedef IntercomNotificationTap = Future<void> Function(
    IntercomNotificationAction action, Map<String, dynamic> data);

final class IntercomNotificationHandler {
  IntercomNotificationHandler({
    FlutterLocalNotificationsPlugin? plugin,
    this.onTap,
  }) : plugin = plugin ?? FlutterLocalNotificationsPlugin();

  final FlutterLocalNotificationsPlugin plugin;
  final IntercomNotificationTap? onTap;

  Future<void> initialize() async {
    const android = AndroidInitializationSettings('@mipmap/ic_launcher');
    const ios = DarwinInitializationSettings();
    await plugin.initialize(
      const InitializationSettings(android: android, iOS: ios),
      onDidReceiveNotificationResponse: _handleResponse,
      onDidReceiveBackgroundNotificationResponse: notificationTapBackground,
    );
    await IntercomNotificationChannels.ensure(plugin);
  }

  Future<void> handleFcmData(Map<String, dynamic> data) async {
    final doorName = data['doorName']?.toString() ??
        data['door_name']?.toString() ??
        'Front Door';
    final doorIp =
        data['doorIp']?.toString() ?? data['door_ip']?.toString() ?? '';
    final payload =
        jsonEncode({...data, 'doorName': doorName, 'doorIp': doorIp});
    await plugin.show(
      8189,
      doorName,
      'Incoming intercom call',
      NotificationDetails(
        android: AndroidNotificationDetails(
          IntercomNotificationChannels.incomingCall.id,
          IntercomNotificationChannels.incomingCall.name,
          channelDescription:
              IntercomNotificationChannels.incomingCall.description,
          importance: Importance.max,
          priority: Priority.max,
          category: AndroidNotificationCategory.call,
          fullScreenIntent: true,
          actions: [
            AndroidNotificationAction(
                IntercomNotificationAction.answer.id, 'Answer',
                showsUserInterface: true),
            AndroidNotificationAction(
                IntercomNotificationAction.reject.id, 'Reject',
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
  }

  Future<void> showIncomingCall({
    required String doorName,
    String doorIp = '',
  }) {
    return handleFcmData({
      'doorName': doorName,
      'doorIp': doorIp,
    });
  }

  Future<void> dismissIncomingCall() => plugin.cancel(8189);

  Future<void> _handleResponse(NotificationResponse response) async {
    final action = IntercomNotificationAction.fromId(response.actionId) ??
        IntercomNotificationAction.answer;
    final payload = response.payload == null
        ? <String, dynamic>{}
        : jsonDecode(response.payload!) as Map<String, dynamic>;
    await onTap?.call(action, payload);
  }
}

@pragma('vm:entry-point')
void notificationTapBackground(NotificationResponse response) {}
