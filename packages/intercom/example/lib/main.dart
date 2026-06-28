import 'package:flutter/material.dart';
import 'package:intercom/intercom.dart';
import 'package:provider/provider.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const IntercomExampleApp());
}

final class IntercomExampleApp extends StatefulWidget {
  const IntercomExampleApp({super.key});

  @override
  State<IntercomExampleApp> createState() => _IntercomExampleAppState();
}

final class _IntercomExampleAppState extends State<IntercomExampleApp>
    with WidgetsBindingObserver {
  late final Future<CallController> _controller = _loadController();
  CallController? _activeController;

  Future<CallController> _loadController() async {
    final controller = await IntercomModule.init(mode: IntercomMode.panel)
        .timeout(const Duration(seconds: 8));
    _activeController = controller;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      controller.checkPendingBackgroundCall();
    });
    return controller;
  }

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _activeController?.dispose();
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) {
      _activeController?.checkPendingBackgroundCall();
    }
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: IntercomTheme.dark(),
      home: FutureBuilder<CallController>(
        future: _controller,
        builder: (context, snapshot) {
          final controller = snapshot.data;
          if (controller != null) {
            return ChangeNotifierProvider.value(
              value: controller,
              child: const _IntercomHome(),
            );
          }
          if (snapshot.hasError) {
            return _StartupError(error: snapshot.error);
          }
          return const _StartupLoading();
        },
      ),
    );
  }
}

final class _StartupLoading extends StatelessWidget {
  const _StartupLoading();

  @override
  Widget build(BuildContext context) {
    return const Scaffold(
      body: Center(child: CircularProgressIndicator()),
    );
  }
}

final class _StartupError extends StatelessWidget {
  const _StartupError({required this.error});

  final Object? error;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: SafeArea(
        child: Center(
          child: Padding(
            padding: const EdgeInsets.all(24),
            child: Text(
              'Intercom startup failed\n$error',
              textAlign: TextAlign.center,
            ),
          ),
        ),
      ),
    );
  }
}

final class _IntercomHome extends StatelessWidget {
  const _IntercomHome();

  @override
  Widget build(BuildContext context) {
    final phase =
        context.select((CallController controller) => controller.state.phase);
    return switch (phase) {
      CallPhase.ringing => const IncomingCallScreen(),
      CallPhase.connecting || CallPhase.connected => const InCallScreen(),
      CallPhase.idle => const IdleScreen(),
    };
  }
}
