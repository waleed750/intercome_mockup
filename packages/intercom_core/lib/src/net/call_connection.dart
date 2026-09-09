import 'dart:async';
import 'dart:collection';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/foundation.dart';

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

  int _framesEnqueued = 0;
  int _framesWritten = 0;
  DateTime? _lastFlushStartedAt;

  bool enqueue(Uint8List frame) {
    if (_closed || _outbox.length >= 256) {
      debugPrint(
          'CallConnection.enqueue: rejected (closed=$_closed, outbox=${_outbox.length})');
      return false;
    }
    _framesEnqueued++;
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
    if (_writing) {
      // _flush() is already running -- if it's been "running" for an
      // unreasonable amount of time, the loop below is actually stuck
      // (most likely `await socket.flush()` never resolving, e.g. the
      // peer stopped reading but the TCP connection itself didn't error
      // out) rather than genuinely busy. Confirmed on-device 2026-09-09:
      // enqueue() kept returning true (frames accepted into the outbox)
      // for both unlock's OpenDoor command and mic-uplink audio frames,
      // yet neither ever reached the door and the call never tore down
      // -- consistent with a hang here rather than a thrown exception
      // (which _would have hit the catch below and called onClosed()).
      final stuckFor = _lastFlushStartedAt == null
          ? null
          : DateTime.now().difference(_lastFlushStartedAt!);
      if (stuckFor != null && stuckFor > const Duration(seconds: 5)) {
        debugPrint(
            'CallConnection._flush: still writing after ${stuckFor.inSeconds}s -- '
            'socket.flush() appears hung, forcing connection closed '
            '(enqueued=$_framesEnqueued written=$_framesWritten outbox=${_outbox.length})');
        _notifyClosed();
      }
      return;
    }
    _writing = true;
    _lastFlushStartedAt = DateTime.now();
    try {
      while (!_closed && _outbox.isNotEmpty) {
        socket.add(_outbox.removeFirst());
        await socket.flush();
        _framesWritten++;
      }
    } catch (e) {
      debugPrint(
          'CallConnection._flush: write failed after $_framesWritten/$_framesEnqueued frames: $e');
      _notifyClosed();
    } finally {
      _writing = false;
      _lastFlushStartedAt = null;
    }
  }

  void _notifyClosed() {
    if (_closed) return;
    _closed = true;
    socket.destroy();
    onClosed();
  }
}
