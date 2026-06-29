import 'package:flutter/widgets.dart';

import 'call/call_controller.dart';
import 'call/incoming_call_handler.dart';
import 'config/device_config.dart';
import 'transport/connection_provider.dart';

final class IntercomModule {
  const IntercomModule._();

  static Future<CallController> init({
    required IncomingCallHandler incomingCallHandler,
    ConnectionProvider? connectionProvider,
    bool startDiscovery = true,
    bool startForegroundService = true,
  }) async {
    final config = await _startupStep('device_config.load', DeviceConfig.load);
    final controller = CallController(
      deviceConfig: config,
      incomingCallHandler: incomingCallHandler,
      connectionProvider: connectionProvider,
      startDiscovery: startDiscovery,
      startForegroundService: startForegroundService,
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
