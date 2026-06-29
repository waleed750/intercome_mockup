import '../net/call_server.dart';
import 'connection_provider.dart';

final class LanConnectionProvider implements ConnectionProvider {
  LanConnectionProvider({required this.server});

  final CallServer server;

  @override
  Future<bool> start() => server.start();

  @override
  Future<void> stop() => server.stop();

  @override
  bool get isActive => server.isRunning;
}
