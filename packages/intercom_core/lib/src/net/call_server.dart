import 'dart:async';
import 'dart:io';

final class CallServer {
  CallServer({
    this.port = defaultPort,
    this.bindAddressProvider,
    required this.onAccepted,
    this.onError,
  });

  static const defaultPort = 8189;

  final int port;
  final FutureOr<InternetAddress?> Function()? bindAddressProvider;
  final void Function(Socket socket) onAccepted;
  final void Function(Object error)? onError;

  ServerSocket? _server;
  bool _started = false;

  bool get isRunning => _server != null && _started;

  Future<bool> start() async {
    if (_started) return true;
    try {
      final bindAddress = await bindAddressProvider?.call();
      _server = await ServerSocket.bind(
          bindAddress ?? InternetAddress.anyIPv4, port,
          shared: true);
      _started = true;
      _server!.listen(onAccepted, onError: (error) {
        onError?.call(error);
        stop();
      });
      return true;
    } catch (error) {
      _started = false;
      onError?.call(error);
      return false;
    }
  }

  Future<void> stop() async {
    _started = false;
    await _server?.close();
    _server = null;
  }
}
