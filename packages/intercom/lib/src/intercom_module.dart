import 'package:flutter/widgets.dart';
import 'package:provider/provider.dart';

import 'call/call_controller.dart';
import 'config/device_config.dart';
import 'intercom_mode.dart';

final class IntercomModule {
  const IntercomModule._();

  static Future<CallController> init({required IntercomMode mode}) async {
    final config = await _startupStep('device_config.load', DeviceConfig.load);
    final controller = CallController(mode: mode, deviceConfig: config);
    try {
      await _startupStep('call_controller.start', controller.start);
    } catch (error, stackTrace) {
      debugPrint('Intercom startup failed: $error');
      debugPrintStack(stackTrace: stackTrace);
      Error.throwWithStackTrace(error, stackTrace);
    }
    return controller;
  }

  static Future<Widget> provider({
    required IntercomMode mode,
    required Widget child,
  }) async {
    final controller = await init(mode: mode);
    return ChangeNotifierProvider.value(value: controller, child: child);
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
