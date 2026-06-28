import 'package:flutter_local_notifications/flutter_local_notifications.dart';

final class IntercomNotificationChannels {
  static const incomingCall = AndroidNotificationChannel(
    'intercom_incoming_call',
    'Intercom calls',
    description: 'Incoming intercom calls',
    importance: Importance.max,
    playSound: true,
    enableVibration: true,
  );

  static Future<void> ensure(FlutterLocalNotificationsPlugin plugin) async {
    final android = plugin.resolvePlatformSpecificImplementation<
        AndroidFlutterLocalNotificationsPlugin>();
    await android?.createNotificationChannel(incomingCall);
  }
}
