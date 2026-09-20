import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';

/// Talks to `syncn-intercomd` over its Unix domain control socket.
///
/// Ported from the reference implementation's `intercom_client.dart`
/// (github.com/RayanH19/syncn-intercom, `ui/syncn_intercom/lib/src/
/// intercom_client.dart`) — newline-delimited JSON, one object per line. Dart
/// speaks Unix sockets natively, so this needs no native plugin for call
/// control; only video needs one (see `../../../flutterpi/src/syncn_video.c`
/// and the `syncn/video` platform channel), because a file descriptor cannot
/// cross a Dart `Socket`.
///
/// This class is intentionally NOT a drop-in port of `CallController` (this
/// package's other, older call-control class): `CallController` speaks the
/// door's wire protocol itself (see `call_connection.dart`/`commands.dart`)
/// and drives its own `VideoDecoder`/`AudioPipeline` MethodChannels.
/// `syncn-intercomd` now owns all of that natively (ALSA, the door TCP/UDP
/// protocol, H.264 decode) in one C process, so this client only needs to
/// mirror the daemon's state machine and forward button presses -- see
/// `DaemonCallController` in this same directory for the class that actually
/// implements `CallController`'s public shape on top of this client.
///
/// Reconnects on its own. The daemon is the thing that must never stop, and
/// this client is a view over it -- a UI restart mid-call reconnects, asks
/// for the current state, and carries on showing the call.
final class IntercomdClient extends ChangeNotifier {
  IntercomdClient({this.socketPath = defaultSocketPath});

  /// Matches `SYNCN_IPC_DEFAULT_PATH` in the daemon's `include/syncn/ipc.h`
  /// and the path `syncn_video.c` connects to by default (overridable there
  /// via `SYNCN_IPC_PATH`, not plumbed through here since both this client
  /// and that plugin only ever run on the same panel talking to the same
  /// local daemon).
  static const String defaultSocketPath = '/run/syncn/intercomd.sock';
  static const Duration _retryDelay = Duration(seconds: 2);

  final String socketPath;

  Socket? _socket;
  StreamSubscription<String>? _lines;
  Timer? _retry;
  bool _disposed = false;

  IntercomdState _state = const IntercomdState();
  String? _lastError;

  IntercomdState get state => _state;
  String? get lastError => _lastError;
  bool get connected => _socket != null;

  /// Fires once per incoming call so the UI can ring / wake the screen,
  /// distinct from the general state-change notification.
  final StreamController<String> _incoming =
      StreamController<String>.broadcast();
  Stream<String> get incomingCalls => _incoming.stream;

  Future<void> start() async {
    if (_disposed) return;
    _retry?.cancel();

    try {
      final socket = await Socket.connect(
        InternetAddress(socketPath, type: InternetAddressType.unix),
        0,
        timeout: const Duration(seconds: 3),
      );
      if (_disposed) {
        socket.destroy();
        return;
      }

      _socket = socket;
      _lastError = null;

      _lines = socket
          .cast<List<int>>()
          .transform(utf8.decoder)
          .transform(const LineSplitter())
          .listen(
            _onLine,
            onError: (Object e) => _dropped('$e'),
            onDone: () => _dropped('daemon closed the connection'),
            cancelOnError: true,
          );

      // We may be attaching mid-call (e.g. a hot restart of the Flutter UI
      // while the daemon kept a call running underneath it). Ask rather than
      // assume idle.
      send('get_state');
      notifyListeners();
    } on Object catch (e) {
      _dropped('$e');
    }
  }

  void _dropped(String reason) {
    if (_disposed) return;

    _lines?.cancel();
    _lines = null;
    _socket?.destroy();
    _socket = null;

    _lastError = reason;
    // Offline is distinct from idle: idle means the daemon is up and quiet,
    // offline means this client cannot see it at all -- worth surfacing
    // differently on a door entry device (see CallPhase-mapping callers).
    _state = _state.copyWith(offline: true);
    notifyListeners();

    _retry = Timer(_retryDelay, start);
  }

  void _onLine(String line) {
    if (line.trim().isEmpty) return;

    final Map<String, dynamic> json;
    try {
      json = jsonDecode(line) as Map<String, dynamic>;
    } on Object {
      debugPrint('intercomd: unparseable line: $line');
      return;
    }

    switch (json['event'] as String?) {
      case 'state':
        _state = IntercomdState.fromJson(json);
        notifyListeners();
      case 'incoming_call':
        _incoming.add((json['door'] as String?) ?? '');
      case 'error':
        _lastError = (json['message'] as String?) ?? 'unknown error';
        notifyListeners();
      default:
        break;
    }
  }

  /// Send a command. Silently does nothing when disconnected -- the caller is
  /// almost always a button press, and throwing at the UI layer helps nobody
  /// (matches this package's existing CallController posture of logging and
  /// swallowing native-call failures rather than propagating them to taps).
  void send(String cmd, {bool? value}) {
    final socket = _socket;
    if (socket == null) return;

    final payload = <String, dynamic>{'cmd': cmd};
    if (value != null) payload['value'] = value;
    try {
      socket.write('${jsonEncode(payload)}\n');
    } on Object catch (e) {
      _dropped('$e');
    }
  }

  void answer() => send('answer');
  void reject() => send('reject');
  void hangUp() => send('hangup');
  void unlock() => send('unlock');
  void callDoor() => send('call');
  void startPreview() => send('preview_start');
  void stopPreview() => send('preview_stop');
  void setMuted(bool muted) => send('set_mute', value: muted);

  @override
  void dispose() {
    _disposed = true;
    _retry?.cancel();
    _lines?.cancel();
    _socket?.destroy();
    _incoming.close();
    super.dispose();
  }
}

/// Mirrors the daemon's own call-state JSON exactly (see
/// `docs/10-protocol-reference.md` and `include/syncn/ipc.h` in the reference
/// repo, and `syncn_ipc_broadcast` callers in `src/core/call.c`) -- this
/// client never derives a phase of its own, so it cannot disagree with the
/// daemon about what is happening.
enum IntercomdPhase {
  idle,
  connecting,
  ringing,
  connected,
  preview,

  /// The daemon is not reachable over the control socket at all. Distinct
  /// from [idle] the same way `CallUiState`'s old discovery/TCP-listening
  /// flags were distinct from a working call -- "quiet" and "broken" must
  /// never look the same on a door entry device.
  offline;

  static IntercomdPhase parse(String? s) => switch (s) {
        'connecting' => IntercomdPhase.connecting,
        'ringing' => IntercomdPhase.ringing,
        'connected' => IntercomdPhase.connected,
        'preview' => IntercomdPhase.preview,
        'idle' => IntercomdPhase.idle,
        _ => IntercomdPhase.offline,
      };
}

@immutable
class IntercomdState {
  const IntercomdState({
    this.phase = IntercomdPhase.offline,
    this.door = '',
    this.muted = false,
  });

  final IntercomdPhase phase;
  final String door;
  final bool muted;

  factory IntercomdState.fromJson(Map<String, dynamic> json) => IntercomdState(
        phase: IntercomdPhase.parse(json['state'] as String?),
        door: (json['door'] as String?) ?? '',
        muted: (json['muted'] as bool?) ?? false,
      );

  IntercomdState copyWith({
    IntercomdPhase? phase,
    String? door,
    bool? muted,
    bool offline = false,
  }) =>
      IntercomdState(
        phase: offline ? IntercomdPhase.offline : (phase ?? this.phase),
        door: door ?? this.door,
        muted: muted ?? this.muted,
      );
}
