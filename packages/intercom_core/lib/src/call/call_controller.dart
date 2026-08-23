import 'dart:async';
import 'dart:io';

import 'package:connectivity_plus/connectivity_plus.dart';
import 'package:flutter/foundation.dart';
import 'package:permission_handler/permission_handler.dart';

import '../background/android_foreground_service.dart';
import '../config/device_config.dart';
import '../media/audio_pipeline.dart';
import '../media/video_decoder.dart';
import '../net/call_connection.dart';
import '../net/call_server.dart';
import '../net/discovery_responder.dart';
import '../net/local_ip_address.dart';
import '../protocol/commands.dart';
import '../protocol/discovery.dart';
import '../protocol/frame.dart';
import '../transport/connection_provider.dart';
import '../transport/lan_connection_provider.dart';
import 'call_phase.dart';
import 'call_ui_state.dart';
import 'incoming_call_handler.dart';

final class CallController extends ChangeNotifier {
  /// G.711 A-law silence byte -- 160 of these = one 20ms uplink frame at
  /// 8kHz, exactly matching what a healthy mic produces. Tuya-style door
  /// stations gate their downlink video/audio on seeing *continuous* CC
  /// frames from us (confirmed against the known-good wire capture), so
  /// when mic capture is unavailable we send synthetic silence instead --
  /// listen-only + video still work, and the door starts streaming.
  static final Uint8List _alawSilenceFrame =
      Uint8List(160)..fillRange(0, 160, 0xD5);

  CallController({
    required this.deviceConfig,
    required this.incomingCallHandler,
    ConnectionProvider? connectionProvider,
    VideoDecoder? video,
    AudioPipeline? audio,
    bool startDiscovery = true,
    bool startForegroundService = true,
  })  : _video = video ?? VideoDecoder(),
        _audio = audio ?? AudioPipeline(),
        _startDiscovery = startDiscovery,
        _startForegroundService = startForegroundService {
    final id = deviceConfig.identity;
    _state = _state.copyWith(
        unitName: id.alias, pairedDoor: id.doorName, callerLabel: id.doorName);

    final server = CallServer(
      onAccepted: _onSocketAccepted,
      onError: _onServerError,
    );
    _connectionProvider =
        connectionProvider ?? LanConnectionProvider(server: server);
    _discovery = DiscoveryResponder(screenInfoProvider: _buildScreenInfo);
  }

  final DeviceConfig deviceConfig;
  final IncomingCallHandler incomingCallHandler;
  final VideoDecoder _video;
  final AudioPipeline _audio;
  final bool _startDiscovery;
  final bool _startForegroundService;

  late final ConnectionProvider _connectionProvider;
  late final DiscoveryResponder _discovery;
  CallConnection? _connection;
  StreamSubscription? _audioSub;
  Timer? _transientTimer;
  Timer? _uplinkWatchdogTimer;
  Timer? _silenceTimer;
  Timer? _statsTimer;
  bool _uplinkFrameSeen = false;

  // Per-call frame counters (diagnostics for "panel-initiated call shows no
  // video" -- compare sent vs received on-device to see which side is silent).
  int _framesSent = 0;
  int _controlFramesReceived = 0;
  int _videoFramesReceived = 0;
  int _audioFramesReceived = 0;
  Stopwatch? _callStopwatch;

  // Remembered so _teardownCall can silently reconnect the preview after a
  // call ends, instead of leaving the screen fully idle. Only ever set by
  // startPreview() -- if a preview was never started (e.g. a cold incoming
  // call with no prior tab visit), teardown has nothing to resume to and
  // that's fine, it just goes to idle like before.
  String? _previewHost;
  int _previewPort = CallServer.defaultPort;

  CallUiState _state = const CallUiState();

  CallUiState get state => _state;
  int? get videoTextureId => _video.textureId;

  void refreshIdentity() {
    final id = deviceConfig.identity;
    _setState(_state.copyWith(
      unitName: id.alias,
      pairedDoor: id.doorName,
      callerLabel: id.doorName,
    ));
  }

  Future<void> start() async {
    await _startupStep(
        'incomingCallHandler.initialize', incomingCallHandler.initialize);
    // permission_handler has no Linux platform implementation at all --
    // every call on this channel throws MissingPluginException there, not
    // just an unsupported/denied result. Bare-DRM kiosk targets have no
    // notification daemon to grant permission to in the first place (see
    // FullScreenCallHandler's Linux handling), so this step is meaningless
    // there and skipped entirely rather than treated as a failure.
    if (!Platform.isLinux) {
      await _startupStep('permission.notification.request',
          () => Permission.notification.request());
    }
    await _startupStep('connectivity.refresh', refreshConnectivity);

    if (_startDiscovery) {
      final discoveryStarted =
          await _startupStep('discovery.start', _discovery.start);
      _setState(_state.copyWith(discoveryListening: discoveryStarted));
    }

    final serverStarted =
        await _startupStep('connection_provider.start', _connectionProvider.start);
    _setState(_state.copyWith(tcpServerListening: serverStarted));

    if (_startForegroundService && Platform.isAndroid) {
      await _startupStep('foreground.startPanelService',
          AndroidForegroundService.startPanelService);
      await _startupStep(
          'foreground.takePendingIncomingCall', checkPendingBackgroundCall);
    }
  }

  Future<T> _startupStep<T>(
    String name,
    Future<T> Function() action,
  ) async {
    const timeout = Duration(seconds: 4);
    debugPrint('Intercom startup step started: $name');
    final stopwatch = Stopwatch()..start();
    try {
      final result = await action().timeout(timeout);
      debugPrint(
          'Intercom startup step finished: $name (${stopwatch.elapsedMilliseconds}ms)');
      return result;
    } on TimeoutException catch (error, stackTrace) {
      final message =
          'Intercom startup step timed out: $name after ${timeout.inSeconds}s';
      debugPrint(message);
      debugPrintStack(stackTrace: stackTrace);
      throw TimeoutException(message, error.duration);
    } catch (error, stackTrace) {
      debugPrint('Intercom startup step failed: $name -> $error');
      debugPrintStack(stackTrace: stackTrace);
      Error.throwWithStackTrace(Exception('$name failed: $error'), stackTrace);
    }
  }

  Future<void> checkPendingBackgroundCall() async {
    if (_state.phase != CallPhase.idle) return;
    if (!Platform.isAndroid) return;
    final hasPendingCall =
        await AndroidForegroundService.takePendingIncomingCall();
    if (hasPendingCall) {
      final id = deviceConfig.identity;
      _setState(_state.copyWith(
        phase: CallPhase.ringing,
        callerLabel: id.doorName,
        videoAvailable: true,
        hasVideoFrames: false,
        muted: false,
        transientMessage: null,
        isIncoming: true,
      ));
      await incomingCallHandler.onIncomingCall(doorName: id.doorName);
    }
  }

  Future<void> shutdown() async {
    await _teardownCall(
      showEnded: false,
      resumePreview: false,
      stopVideo: true,
    );
    await _discovery.stop();
    await _connectionProvider.stop();
    if (_startForegroundService && Platform.isAndroid) {
      await AndroidForegroundService.stopPanelService();
    }
    _setState(_state.copyWith(
      discoveryListening: false,
      tcpServerListening: false,
    ));
  }

  Future<void> refreshConnectivity() async {
    final result = await Connectivity().checkConnectivity();
    final localIpAddress = await resolveLocalIpv4Address();
    _setState(_state.copyWith(
      onWifi: result.contains(ConnectivityResult.wifi) ||
          result.contains(ConnectivityResult.ethernet),
      localIpAddress: localIpAddress,
    ));
  }

  Future<void> connectToDoor(String host,
      {int port = CallServer.defaultPort}) async {
    if (_state.phase != CallPhase.idle) return;
    _resetCallDiagnostics();

    final id = deviceConfig.identity;
    _setState(_state.copyWith(
      phase: CallPhase.connecting,
      callerLabel: id.doorName,
      videoAvailable: true,
      hasVideoFrames: false,
      muted: false,
      transientMessage: null,
      isIncoming: false,
    ));

    debugPrint('Intercom: connecting to door $host:$port');
    final socket =
        await Socket.connect(host, port, timeout: const Duration(seconds: 5));
    _onSocketAccepted(socket);
    final conn = _connection;
    if (conn == null) return;

    var handshakeSent = 0;
    for (final frame in Commands.answerFrames()) {
      if (_send(frame)) handshakeSent++;
    }
    debugPrint('Intercom: Answer handshake sent ($handshakeSent frames)');
    await _video.start();
    debugPrint(
        'Intercom: video pipeline started (textureId=${_video.textureId})');
    try {
      await _startAudio();
    } catch (e) {
      debugPrint('Failed to start audio: $e');
    }
    _setState(_state.copyWith(phase: CallPhase.connected));
    _startStatsLogging();

    // Door units in this protocol family only start streaming once they've
    // both received the Answer handshake above AND seen a StartTalk shortly
    // after -- confirmed via wire capture against the real hardware.
    Timer(const Duration(seconds: 1), () {
      if (_state.phase == CallPhase.connected && _send(Commands.startTalk())) {
        debugPrint('Intercom: StartTalk sent');
      }
    });
  }

  /// Lightweight video-only preview: connects to the door and starts the
  /// video pipeline, but does NOT start audio or send StartTalk. The door
  /// should start streaming video frames once it sees the Answer handshake,
  /// giving the user a live camera peek without committing to a full call.
  Future<void> startPreview(String host,
      {int port = CallServer.defaultPort}) async {
    if (_state.phase != CallPhase.idle) return;
    _resetCallDiagnostics();
    _previewHost = host;
    _previewPort = port;

    final id = deviceConfig.identity;
    _setState(_state.copyWith(
      phase: CallPhase.previewing,
      callerLabel: id.doorName,
      videoAvailable: true,
      hasVideoFrames: false,
      muted: false,
      transientMessage: null,
      isIncoming: false,
    ));

    debugPrint('Intercom: starting preview for $host:$port');
    final socket =
        await Socket.connect(host, port, timeout: const Duration(seconds: 5));
    _onSocketAccepted(socket);
    final conn = _connection;
    if (conn == null) return;

    for (final frame in Commands.answerFrames()) {
      _send(frame);
    }
    await _video.start();
    debugPrint(
        'Intercom: preview video started (textureId=${_video.textureId})');
    _startStatsLogging();
  }

  /// Stops the preview without showing "Call ended". Called when navigating
  /// away from the intercom tab or when transitioning to a full call.
  Future<void> stopPreview() async {
    if (_state.phase != CallPhase.previewing) return;
    debugPrint('Intercom: stopping preview');
    await _teardownCall(showEnded: false, resumePreview: false);
  }

  /// Upgrades a running preview to a full call: starts audio and sends
  /// StartTalk. If no preview is active, does nothing — the caller should
  /// use connectToDoor() for a fresh call.
  Future<void> upgradeToCall() async {
    final conn = _connection;
    if (_state.phase != CallPhase.previewing || conn == null) {
      debugPrint('Intercom: upgradeToCall called but no preview active');
      return;
    }
    debugPrint('Intercom: upgrading preview to full call');
    _resetCallDiagnostics();
    try {
      await _startAudio();
    } catch (e) {
      debugPrint('Failed to start audio: $e');
    }
    _setState(_state.copyWith(phase: CallPhase.connected));
    _startStatsLogging();
    Timer(const Duration(seconds: 1), () {
      if (_state.phase == CallPhase.connected && _send(Commands.startTalk())) {
        debugPrint('Intercom: StartTalk sent');
      }
    });
  }

  Future<void> simulateIncomingCall() async {
    if (_state.phase != CallPhase.idle) return;
    final id = deviceConfig.identity;
    _setState(_state.copyWith(
      phase: CallPhase.ringing,
      callerLabel: id.doorName,
      videoAvailable: true,
      hasVideoFrames: false,
      muted: false,
      transientMessage: null,
      isIncoming: true,
    ));
    await incomingCallHandler.onIncomingCall(doorName: id.doorName);
    await _video.start();
  }

  Future<void> answer() async {
    if (_state.phase != CallPhase.ringing) return;
    final conn = _connection;
    if (conn == null) {
      await _teardownCall(showEnded: true);
      return;
    }
    debugPrint(
        'Intercom: answering call (t+${_callStopwatch?.elapsed.inMilliseconds ?? 0}ms)');
    await incomingCallHandler.onCallDismissed();
    _setState(_state.copyWith(phase: CallPhase.connecting));
    var handshakeSent = 0;
    for (final frame in Commands.answerFrames()) {
      if (_send(frame)) handshakeSent++;
    }
    debugPrint('Intercom: Answer handshake sent ($handshakeSent frames)');
    try {
      await _startAudio();
    } catch (e) {
      debugPrint('Failed to start audio: $e');
    }
    _setState(_state.copyWith(phase: CallPhase.connected));
    _startStatsLogging();
    Timer(const Duration(seconds: 1), () {
      if (_state.phase == CallPhase.connected && _send(Commands.startTalk())) {
        debugPrint('Intercom: StartTalk sent');
      }
    });
  }

  Future<void> decline() async {
    if (_state.phase == CallPhase.idle) return;
    await incomingCallHandler.onCallDismissed();
    _connection?.enqueue(Commands.hangUp());
    await _teardownCall(showEnded: false);
  }

  Future<void> endCall() => decline();

  void unlock() {
    if (_state.phase != CallPhase.connected) return;
    _connection?.enqueue(Commands.openDoor());
    _showTransient('Door unlocked');
  }

  Future<void> setMuted(bool muted) async {
    await _audio.setMuted(muted);
    _setState(_state.copyWith(muted: muted));
  }

  Future<void> setHeadsetMode(bool headsetMode) async {
    await _audio.setHeadsetMode(headsetMode);
    _setState(_state.copyWith(headsetMode: headsetMode));
  }

  void _onSocketAccepted(Socket socket) {
    if (_connection != null &&
        _state.phase != CallPhase.idle &&
        _state.phase != CallPhase.ringing &&
        _state.phase != CallPhase.previewing) {
      socket.add(Commands.deviceBusy());
      socket.destroy();
      return;
    }
    _connection?.close();
    final conn = CallConnection(
      socket: socket,
      onFrame: _onFrame,
      onClosed: () => _teardownCall(showEnded: true),
    );
    _connection = conn;
    conn.start();
  }

  void _onServerError(Object error) {
    if (error is SocketException && error.osError?.errorCode == 98) {
      _showTransient('Port 8189 is already in use');
      return;
    }
    _showTransient('Intercom server unavailable');
  }

  Future<void> _onFrame(Channel channel, Uint8List payload) async {
    switch (channel) {
      case Channel.control:
        _controlFramesReceived++;
        await _handleControl(payload);
      case Channel.video:
        _videoFramesReceived++;
        if (_state.phase != CallPhase.idle) {
          await _video.submit(payload);
          if (!_state.hasVideoFrames) {
            debugPrint(
                'Intercom: first video frame received '
                '(t+${_callStopwatch?.elapsed.inMilliseconds ?? 0}ms)');
            _setState(_state.copyWith(hasVideoFrames: true));
          }
        }
      case Channel.audio:
        _audioFramesReceived++;
        if (_state.phase == CallPhase.connected) {
          await _audio.playDownlink(payload);
        }
    }
  }

  Future<void> _handleControl(Uint8List payload) async {
    final message = Commands.parse(payload);
    switch (message?.classify() ?? InboundCommand.unknown) {
      case InboundCommand.call:
        if (_state.phase != CallPhase.idle &&
            _state.phase != CallPhase.previewing) return;
        // If previewing, tear it down silently before accepting the call --
        // resumePreview: false here since we're about to set up the
        // incoming call's own state/connection right below; auto-resuming
        // the preview at this exact moment would race with that and could
        // stomp _connection with a stray reconnect (startPreview's own
        // phase==idle check would briefly pass in the gap between this
        // teardown finishing and the ringing state being set just below).
        if (_state.phase == CallPhase.previewing) {
          debugPrint('Intercom: incoming call during preview -- stopping preview');
          await _teardownCall(showEnded: false, resumePreview: false);
        }
        debugPrint('Intercom: incoming call from door');
        _resetCallDiagnostics();
        _callStopwatch = Stopwatch()..start();
        final id = deviceConfig.identity;
        _setState(_state.copyWith(
          phase: CallPhase.ringing,
          callerLabel: id.doorName,
          videoAvailable: true,
          hasVideoFrames: false,
          muted: false,
          transientMessage: null,
          isIncoming: true,
        ));
        // _video.start() runs first, before the (potentially slower) UI/
        // notification handler: CallFrameHandler is a plain `void Function`
        // (see CallConnection/FrameParser), so this async function's `await`s
        // don't block the frame parser from dispatching the next buffered
        // frame -- a door unit that starts streaming video immediately on
        // connect can have several video frames already parsed and
        // dispatched to _onFrame before this function's first await even
        // suspends. Starting the video pipeline first (confirmed on-device:
        // real video frames arriving with the native pipeline not yet
        // started, all silently dropped) minimizes that window; it doesn't
        // eliminate the race (the frame parser still isn't backpressured),
        // but every frame from here on has the best chance of being decoded.
        await _video.start();
        await incomingCallHandler.onIncomingCall(doorName: id.doorName);
      case InboundCommand.getCallInfo:
        if (_state.phase == CallPhase.connected) {
          for (final frame in Commands.answerFrames()) {
            _connection?.enqueue(frame);
          }
        }
      case InboundCommand.hangUp:
        await _teardownCall(showEnded: true);
      case InboundCommand.unknown:
        return;
    }
  }

  Future<void> _startAudio() async {
    // permission_handler has no Linux platform implementation -- there's no
    // OS-level permission prompt to check/request there, so treat mic access
    // as always granted (same as the app itself being allowed to open the
    // audio device, which is a system/user configuration concern, not
    // something this plugin can gate on Linux).
    bool micGranted = true;
    if (!Platform.isLinux) {
      var micStatus = await Permission.microphone.status;
      if (!micStatus.isGranted) {
        micStatus = await Permission.microphone.request();
      }
      micGranted = micStatus.isGranted;
    }
    await _audio.start(
      captureEnabled: micGranted,
      headsetMode: _state.headsetMode,
    );
    _uplinkFrameSeen = false;
    _stopUplinkFallbacks();
    _audioSub?.cancel();
    _audioSub = _audio.uplink.listen((alaw) {
      _uplinkFrameSeen = true;
      if (_silenceTimer != null) {
        debugPrint('Intercom: real mic frames resumed -- silence fallback off');
        _silenceTimer?.cancel();
        _silenceTimer = null;
      }
      _send(Frame.encode(Channel.audio, alaw));
    });
    _armUplinkWatchdog();
    _setState(_state.copyWith(micAvailable: micGranted));
  }

  bool _send(Uint8List frame) {
    final conn = _connection;
    if (conn == null) return false;
    final accepted = conn.enqueue(frame);
    if (accepted) _framesSent++;
    return accepted;
  }

  // The native audio plugin does NOT fail `start` when its capture pipeline
  // can't come up (it logs "mic will be unavailable" and continues with
  // playback only), so absence of uplink CC frames is how we detect a dead
  // mic from Dart. Door stations gate their downlink on seeing continuous
  // uplink audio, so fall back to synthetic A-law silence frames -- exactly
  // what the known-good wire capture shows flowing right after the handshake.
  void _armUplinkWatchdog() {
    _uplinkWatchdogTimer?.cancel();
    _uplinkWatchdogTimer = Timer(const Duration(seconds: 2), () {
      if (_uplinkFrameSeen || _state.phase == CallPhase.idle) return;
      debugPrint(
          'Intercom: no uplink mic frames within 2s -- sending silence '
          'frames so the door starts streaming');
      _silenceTimer?.cancel();
      _silenceTimer = Timer.periodic(const Duration(milliseconds: 20), (_) {
        if (!_send(Frame.encode(Channel.audio, _alawSilenceFrame))) {
          _silenceTimer?.cancel();
          _silenceTimer = null;
        }
      });
    });
  }

  void _stopUplinkFallbacks() {
    _uplinkWatchdogTimer?.cancel();
    _uplinkWatchdogTimer = null;
    _silenceTimer?.cancel();
    _silenceTimer = null;
  }

  void _startStatsLogging() {
    _statsTimer?.cancel();
    _callStopwatch = Stopwatch()..start();
    _statsTimer = Timer.periodic(const Duration(seconds: 5), (_) {
      if (_state.phase == CallPhase.idle) return;
      debugPrint(
          'Intercom stats t+${_callStopwatch?.elapsed.inSeconds ?? 0}s: '
          'sent=$_framesSent rxControl=$_controlFramesReceived '
          'rxVideo=$_videoFramesReceived rxAudio=$_audioFramesReceived'
          '${_silenceTimer != null ? ' [SILENCE FALLBACK ACTIVE]' : ''}');
    });
  }

  void _resetCallDiagnostics() {
    _stopUplinkFallbacks();
    _statsTimer?.cancel();
    _statsTimer = null;
    _framesSent = 0;
    _controlFramesReceived = 0;
    _videoFramesReceived = 0;
    _audioFramesReceived = 0;
    _uplinkFrameSeen = false;
  }

  /// Ends whatever call/preview is active. Video is left running by default
  /// (`resumePreview: true`) -- only the audio pipeline and the socket
  /// connection actually get torn down. Once idle, if a preview host is
  /// known, the preview is silently reconnected so the live camera feed
  /// keeps showing instead of going blank -- since `_video` was never
  /// stopped, that reconnect is just a socket handshake, not a pipeline
  /// rebuild (native handle_start no-ops if already running).
  /// Pass `resumePreview: false` for a real full stop (app shutdown) --
  /// that's the only path that actually stops video.
  Future<void> _teardownCall({
    required bool showEnded,
    bool resumePreview = true,
    bool stopVideo = false,
  }) async {
    await incomingCallHandler.onCallDismissed();
    _stopUplinkFallbacks();
    _statsTimer?.cancel();
    _statsTimer = null;
    _callStopwatch = null;
    await _audioSub?.cancel();
    _audioSub = null;
    await _audio.stop().catchError((_) {});
    if (stopVideo) {
      await _video.stop().catchError((_) {});
    }
    final conn = _connection;
    _connection = null;
    await conn?.close();
    _setState(_state.copyWith(
      phase: CallPhase.idle,
      muted: false,
      hasVideoFrames: false,
      videoAvailable: true,
      micAvailable: true,
      isIncoming: false,
    ));
    if (showEnded) _showTransient('Call ended');

    if (resumePreview && _previewHost != null) {
      // Fire-and-forget: callers like decline()/hangup shouldn't block on
      // the reconnect. startPreview() itself guards on phase == idle,
      // which we just set above.
      unawaited(startPreview(_previewHost!, port: _previewPort));
    }
  }

  ScreenInfo _buildScreenInfo() {
    final id = deviceConfig.identity;
    return ScreenInfo(
      alias: id.alias,
      serial: id.serial,
      dstAddr: id.dstAddr,
      localIp: _state.localIpAddress,
    );
  }

  void _showTransient(String message) {
    _setState(_state.copyWith(transientMessage: message));
    _transientTimer?.cancel();
    _transientTimer = Timer(const Duration(milliseconds: 2500), () {
      if (_state.transientMessage == message) {
        _setState(_state.copyWith(transientMessage: null));
      }
    });
  }

  void _setState(CallUiState state) {
    _state = state;
    notifyListeners();
  }

  @override
  void dispose() {
    _transientTimer?.cancel();
    unawaited(shutdown());
    unawaited(incomingCallHandler.dispose());
    super.dispose();
  }
}
