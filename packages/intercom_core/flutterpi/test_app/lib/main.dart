import 'package:flutter/material.dart';
import 'package:intercom_core/intercom_core.dart';
import 'package:provider/provider.dart';

// Minimal harness to exercise intercom_core's native audio/video channels
// on a real flutter-pi build. Not the product app -- just enough UI to
// drive a call end to end (idle -> ringing -> connected) against
// mock_door.py and see whether real frames make it onto screen/speaker.
Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();

  final controller = await IntercomModule.init(
    incomingCallHandler: SilentCallHandler(),
  );

  runApp(
    ChangeNotifierProvider<CallController>.value(
      value: controller,
      child: const TestApp(),
    ),
  );
}

final class TestApp extends StatelessWidget {
  const TestApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark(useMaterial3: true),
      home: const _PhaseSwitcher(),
    );
  }
}

final class _PhaseSwitcher extends StatelessWidget {
  const _PhaseSwitcher();

  @override
  Widget build(BuildContext context) {
    final controller = context.watch<CallController>();
    switch (controller.state.phase) {
      case CallPhase.idle:
        return const IdleScreen();
      case CallPhase.ringing:
        return const IncomingCallScreen();
      case CallPhase.connecting:
      case CallPhase.connected:
        return const InCallScreen();
    }
  }
}
