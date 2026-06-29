import 'dart:async';
import 'dart:collection';
import 'dart:io';
import 'dart:typed_data';

import '../protocol/frame.dart';
import '../protocol/frame_parser.dart';

typedef CallFrameHandler = void Function(Channel channel, Uint8List payload);

final class CallConnection {
  CallConnection({
    required this.socket,
    required this.onFrame,
    required this.onClosed,
  }) : _parser = FrameParser(onFrame: onFrame);

  final Socket socket;
  final CallFrameHandler onFrame;
  final void Function() onClosed;
  final FrameParser _parser;
  final Queue<Uint8List> _outbox = Queue<Uint8List>();
  bool _writing = false;
  bool _closed = false;
  StreamSubscription<List<int>>? _subscription;

  InternetAddress get remoteAddress => socket.remoteAddress;

  void start() {
    socket.setOption(SocketOption.tcpNoDelay, true);
    _subscription = socket.listen(
      _parser.offer,
      onDone: _notifyClosed,
      onError: (_) => _notifyClosed(),
      cancelOnError: true,
    );
  }

  bool enqueue(Uint8List frame) {
    if (_closed || _outbox.length >= 256) return false;
    _outbox.add(frame);
    _flush();
    return true;
  }

  Future<void> close() async {
    if (_closed) return;
    _closed = true;
    await _subscription?.cancel();
    socket.destroy();
  }

  Future<void> _flush() async {
    if (_writing) return;
    _writing = true;
    try {
      while (!_closed && _outbox.isNotEmpty) {
        socket.add(_outbox.removeFirst());
        await socket.flush();
      }
    } catch (_) {
      _notifyClosed();
    } finally {
      _writing = false;
    }
  }

  void _notifyClosed() {
    if (_closed) return;
    _closed = true;
    socket.destroy();
    onClosed();
  }
}
