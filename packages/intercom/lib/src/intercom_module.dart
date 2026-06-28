import 'package:flutter/widgets.dart';
import 'package:provider/provider.dart';

import 'call/call_controller.dart';
import 'config/device_config.dart';
import 'intercom_mode.dart';

final class IntercomModule {
  const IntercomModule._();

  static Future<CallController> init({required IntercomMode mode}) async {
    final config = await DeviceConfig.load();
    final controller = CallController(mode: mode, deviceConfig: config);
    try {
      await controller.start();
    } catch (error, stackTrace) {
      debugPrint('Intercom startup skipped: $error');
      debugPrintStack(stackTrace: stackTrace);
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
}
