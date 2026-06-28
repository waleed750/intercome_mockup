import 'dart:async';
import 'dart:io';

import '../protocol/discovery.dart';

final class DiscoveryResponder {
  DiscoveryResponder({required this.screenInfoProvider});

  final ScreenInfo Function() screenInfoProvider;
  RawDatagramSocket? _socket;
  StreamSubscription<RawSocketEvent>? _subscription;

  bool get isRunning => _socket != null;

  Future<bool> start() async {
    if (_socket != null) return true;
    try {
      final socket = await RawDatagramSocket.bind(
          InternetAddress.anyIPv4, Discovery.port,
          reuseAddress: true);
      socket.broadcastEnabled = true;
      _socket = socket;
      _subscription = socket.listen((event) {
        if (event != RawSocketEvent.read) return;
        final packet = socket.receive();
        if (packet == null) return;
        if (!Discovery.isDiscoveryRequest(packet.data, packet.data.length)) {
          return;
        }
        final door = Discovery.doorAddrFrom(packet.data, packet.data.length);
        final reply = Discovery.buildReply(screenInfoProvider(), door);
        socket.send(reply, packet.address, Discovery.port);
      });
      return true;
    } catch (_) {
      _socket = null;
      return false;
    }
  }

  Future<void> stop() async {
    await _subscription?.cancel();
    _subscription = null;
    _socket?.close();
    _socket = null;
  }
}
