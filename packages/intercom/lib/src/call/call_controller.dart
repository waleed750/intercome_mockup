import 'dart:async';
import 'dart:io';

import 'package:connectivity_plus/connectivity_plus.dart';
import 'package:flutter/foundation.dart';
import 'package:permission_handler/permission_handler.dart';

import '../background/android_foreground_service.dart';
import '../config/device_config.dart';
import '../intercom_mode.dart';
import '../media/audio_pipeline.dart';
import '../media/video_decoder.dart';
import '../net/call_connection.dart';
import '../net/call_server.dart';
import '../net/discovery_responder.dart';
import '../net/local_ip_address.dart';
import '../notifications/intercom_notification_handler.dart';
import '../notifications/notification_actions.dart';
import '../protocol/commands.dart';
import '../protocol/discovery.dart';
import '../protocol/frame.dart';
import '../ringer/ringer.dart';
import 'call_phase.dart';
import 'call_ui_state.dart';

final class CallController extends ChangeNotifier {
  CallController({
    required this.mode,
    required this.deviceConfig,
    Ringer? ringer,
    VideoDecoder? video,
    AudioPipeline? audio,
  })  : _ringer = ringer ?? Ringer(),
        _video = video ?? VideoDecoder(),
        _audio = audio ?? AudioPipeline() {
    final id = deviceConfig.identity;
    _state = _state.copyWith(
        unitName: id.alias, pairedDoor: id.doorName, callerLabel: id.doorName);
    _server = CallServer(
      onAccepted: _onSocketAccepted,
      onError: _onServerError,
    );
    _discovery = DiscoveryResponder(screenInfoProvider: _buildScreenInfo);
    _notifications = IntercomNotificationHandler(onTap: _onNotificationTap);
  }

  final IntercomMode mode;
  final DeviceConfig deviceConfig;
  final Ringer _ringer;
  final VideoDecoder _video;
  final AudioPipeline _audio;
  late final IntercomNotificationHandler _notifications;
  late final CallServer _server;
  late final DiscoveryResponder _discovery;
  CallConnection? _connection;
  StreamSubscription? _audioSub;
  Timer? _transientTimer;
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
    await _startupStep('notifications.initialize', _notifications.initialize);
    await _startupStep('permission.notification.request',
        () => Permission.notification.request());
    await _startupStep('connectivity.refresh', refreshConnectivity);
    if (mode == IntercomMode.panel) {
      final discoveryStarted =
          await _startupStep('discovery.start', _discovery.start);
      final serverStarted =
          await _startupStep('call_server.start', _server.start);
      await _startupStep('foreground.startPanelService',
          AndroidForegroundService.startPanelService);
      _setState(_state.copyWith(
        discoveryListening: discoveryStarted,
        tcpServerListening: serverStarted,
      ));
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
    if (mode != IntercomMode.panel || _state.phase != CallPhase.idle) return;
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
      ));
    }
  }

  Future<void> shutdown() async {
    await _teardownCall(showEnded: false);
    await _discovery.stop();
    await _server.stop();
    if (mode == IntercomMode.panel) {
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
      onWifi: result.contains(ConnectivityResult.wifi),
      localIpAddress: localIpAddress,
    ));
  }

  Future<void> connectToDoor(String host,
      {int port = CallServer.defaultPort}) async {
    final socket =
        await Socket.connect(host, port, timeout: const Duration(seconds: 5));
    _onSocketAccepted(socket);
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
    ));
    await _notifications.showIncomingCall(doorName: id.doorName);
    await _video.start();
    await _ringer.start();
  }

  Future<void> answer() async {
    if (_state.phase != CallPhase.ringing) return;
    final conn = _connection;
    if (conn == null) {
      await _teardownCall(showEnded: true);
      return;
    }
    await _notifications.dismissIncomingCall();
    await _ringer.stop();
    _setState(_state.copyWith(phase: CallPhase.connecting));
    for (final frame in Commands.answerFrames()) {
      conn.enqueue(frame);
    }
    await _startAudio();
    _setState(_state.copyWith(phase: CallPhase.connected));
  }

  Future<void> decline() async {
    if (_state.phase == CallPhase.idle) return;
    await _notifications.dismissIncomingCall();
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

  void _onSocketAccepted(Socket socket) {
    if (_connection != null &&
        _state.phase != CallPhase.idle &&
        _state.phase != CallPhase.ringing) {
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
        await _handleControl(payload);
      case Channel.video:
        if (_state.phase != CallPhase.idle) {
          await _video.submit(payload);
          if (!_state.hasVideoFrames)
            _setState(_state.copyWith(hasVideoFrames: true));
        }
      case Channel.audio:
        if (_state.phase == CallPhase.connected)
          await _audio.playDownlink(payload);
    }
  }

  Future<void> _handleControl(Uint8List payload) async {
    final message = Commands.parse(payload);
    switch (message?.classify() ?? InboundCommand.unknown) {
      case InboundCommand.call:
        if (_state.phase != CallPhase.idle) return;
        final id = deviceConfig.identity;
        _setState(_state.copyWith(
          phase: CallPhase.ringing,
          callerLabel: id.doorName,
          videoAvailable: true,
          hasVideoFrames: false,
          muted: false,
          transientMessage: null,
        ));
        await _notifications.showIncomingCall(doorName: id.doorName);
        await _video.start();
        await _ringer.start();
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
    var micStatus = await Permission.microphone.status;
    if (!micStatus.isGranted) {
      micStatus = await Permission.microphone.request();
    }
    final micGranted = micStatus.isGranted;
    await _audio.start(captureEnabled: micGranted);
    _audioSub?.cancel();
    _audioSub = _audio.uplink.listen((alaw) {
      _connection?.enqueue(Frame.encode(Channel.audio, alaw));
    });
    _setState(_state.copyWith(micAvailable: micGranted));
  }

  Future<void> _teardownCall({required bool showEnded}) async {
    await _notifications.dismissIncomingCall();
    await _ringer.stop();
    await _audioSub?.cancel();
    _audioSub = null;
    await _audio.stop().catchError((_) {});
    await _video.stop().catchError((_) {});
    final conn = _connection;
    _connection = null;
    await conn?.close();
    _setState(_state.copyWith(
      phase: CallPhase.idle,
      muted: false,
      hasVideoFrames: false,
      videoAvailable: true,
      micAvailable: true,
    ));
    if (showEnded) _showTransient('Call ended');
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
      if (_state.transientMessage == message)
        _setState(_state.copyWith(transientMessage: null));
    });
  }

  void _setState(CallUiState state) {
    _state = state;
    notifyListeners();
  }

  Future<void> _onNotificationTap(
    IntercomNotificationAction action,
    Map<String, dynamic> data,
  ) async {
    switch (action) {
      case IntercomNotificationAction.answer:
        await answer();
      case IntercomNotificationAction.reject:
        await decline();
    }
  }

  @override
  void dispose() {
    _transientTimer?.cancel();
    unawaited(shutdown());
    super.dispose();
  }
}
