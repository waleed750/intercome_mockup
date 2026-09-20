import 'package:flutter/widgets.dart';

import 'call/call_controller.dart';
import 'call/incoming_call_handler.dart';
import 'config/device_config.dart';
import 'daemon/daemon_call_controller.dart';
import 'daemon/intercomd_client.dart';
import 'daemon/intercomd_video.dart';
import 'transport/connection_provider.dart';

final class IntercomModule {
  const IntercomModule._();

  /// Entry point for the flutter-pi panel build (2026-09-20 port): backs the
  /// call controller with `syncn-intercomd` instead of this package's own
  /// Dart-side door client/native GStreamer plugins. See
  /// `DaemonCallController`'s doc comment for why this is a separate
  /// entry point rather than a flag on [init].
  static Future<DaemonCallController> initForPanel({
    required IncomingCallHandler incomingCallHandler,
    IntercomdClient? client,
    IntercomdVideo? video,
  }) async {
    final config = await _startupStep('device_config.load', DeviceConfig.load);
    final controller = DaemonCallController(
      deviceConfig: config,
      incomingCallHandler: incomingCallHandler,
      client: client,
      video: video,
    );
    try {
      await _startupStep('daemon_call_controller.start', controller.start);
    } catch (error, stackTrace) {
      debugPrint('Intercom (panel) startup failed: $error');
      debugPrintStack(stackTrace: stackTrace);
      Error.throwWithStackTrace(error, stackTrace);
    }
    return controller;
  }

  static Future<CallController> init({
    required IncomingCallHandler incomingCallHandler,
    ConnectionProvider? connectionProvider,
    bool startDiscovery = true,
    bool startForegroundService = true,
    bool micAvailable = true,
    bool requestNotificationPermission = true,
  }) async {
    final config = await _startupStep('device_config.load', DeviceConfig.load);
    final controller = CallController(
      deviceConfig: config,
      incomingCallHandler: incomingCallHandler,
      connectionProvider: connectionProvider,
      startDiscovery: startDiscovery,
      startForegroundService: startForegroundService,
      micAvailable: micAvailable,
      requestNotificationPermission: requestNotificationPermission,
    );
    try {
      await _startupStep('call_controller.start', controller.start);
    } catch (error, stackTrace) {
      debugPrint('Intercom startup failed: $error');
      debugPrintStack(stackTrace: stackTrace);
      Error.throwWithStackTrace(error, stackTrace);
    }
    return controller;
  }

  static Future<T> _startupStep<T>(
    String name,
    Future<T> Function() action,
  ) async {
    debugPrint('Intercom startup step started: $name');
    final stopwatch = Stopwatch()..start();
    try {
      final result = await action();
      debugPrint(
          'Intercom startup step finished: $name (${stopwatch.elapsedMilliseconds}ms)');
      return result;
    } catch (error, stackTrace) {
      debugPrint('Intercom startup step failed: $name -> $error');
      debugPrintStack(stackTrace: stackTrace);
      Error.throwWithStackTrace(Exception('$name failed: $error'), stackTrace);
    }
  }
}
