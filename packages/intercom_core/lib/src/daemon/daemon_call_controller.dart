import 'dart:async';

import 'package:flutter/foundation.dart';

import '../call/call_phase.dart';
import '../call/call_ui_state.dart';
import '../call/incoming_call_handler.dart';
import '../config/device_config.dart';
import 'intercomd_client.dart';
import 'intercomd_video.dart';

/// `CallController`-shaped facade over `syncn-intercomd`.
///
/// WHY THIS CLASS EXISTS, RATHER THAN CHANGING `CallController` ITSELF
/// (2026-09-20): `CallController` implements this package's OWN door wire
/// protocol end to end (`CallConnection`, `Commands`, `Frame` -- see
/// `call_controller.dart`) plus Android/iOS specifics (foreground service,
/// permission_handler, connectivity_plus) that have nothing to do with the
/// flutter-pi panel and must keep working unmodified on those platforms.
/// `syncn-intercomd` is a Linux/Rockchip-only daemon that now speaks that
/// SAME door protocol itself (confirmed: UDP discovery on 8089, TCP session
/// on 8189, `appid=7551000`, mandatory StartTalk -- identical to
/// `docs/10-protocol-reference.md` in the reference repo this was ported
/// from) and owns audio/video entirely in its own process. Rather than
/// carving platform branches into one already-large, already-fragile 900-
/// line class shared across every platform this package targets, this is a
/// separate class exposing the same shape `IntercomCubit`/`intercom_page.dart`
/// already call through (`state`, `videoTextureId`, `answer`/`decline`/
/// `endCall`/`unlock`/`setMuted`/`connectToDoor`/`startPreview`/`stopPreview`/
/// `upgradeToCall`/`simulateIncomingCall`/`shutdown`/`deviceConfig`/
/// `refreshIdentity`), so swapping which one `IntercomModule.init` builds is
/// the entire integration point on the app side -- see
/// `IntercomModule.initForPanel` in `intercom_module.dart`.
///
/// Not a subtype of `CallController` (that class is `final`, deliberately --
/// it is not designed to be extended) -- Dart does not require a shared
/// supertype for `IntercomCubit`'s `CallController?` field to work here; that
/// field's static type would need loosening to a common interface if strict
/// type-checking against `CallController` specifically is ever added back.
/// Flagged in the port's handback report as a follow-up worth doing properly
/// (extract an `IntercomController` interface) rather than as a side effect
/// of this port.
final class DaemonCallController extends ChangeNotifier {
  DaemonCallController({
    required this.deviceConfig,
    required this.incomingCallHandler,
    IntercomdClient? client,
    IntercomdVideo? video,
  })  : _client = client ?? IntercomdClient(),
        _video = video ?? IntercomdVideo() {
    _client.addListener(_onDaemonStateChanged);
    _client.incomingCalls.listen(_onIncomingCall);
  }

  final DeviceConfig deviceConfig;
  final IncomingCallHandler incomingCallHandler;
  final IntercomdClient _client;
  final IntercomdVideo _video;

  CallUiState _state = const CallUiState();
  CallUiState get state => _state;
  int? get videoTextureId => _video.textureId;

  bool _wasRinging = false;
  bool _videoActiveForPhase = false;
  CallPhase? _videoActivePhase;
  bool _shutdown = false;
  Future<void> _videoLifecycle = Future<void>.value();
  bool _videoWanted = false;
  CallPhase? _videoWantedPhase;
  bool _videoRestartWanted = false;

  void refreshIdentity() {
    final id = deviceConfig.identity;
    _setState(_state.copyWith(unitName: id.alias, pairedDoor: id.doorName));
  }

  Future<void> start() async {
    await incomingCallHandler.initialize();
    refreshIdentity();
    await _client.start();
  }

  Future<void> shutdown() async {
    _shutdown = true;
    _client.removeListener(_onDaemonStateChanged);
    await _stopVideoIfActive();
    _client.dispose();
  }

  Future<void> checkPendingBackgroundCall() async {
    // No Android-style backgrounded-call handoff on this daemon-backed path
    // (flutter-pi has no notion of a background service at all) -- present
    // for API-shape parity with CallController, whose Dart-side callers
    // (IntercomCubit) call it unconditionally at startup.
  }

  void _onIncomingCall(String door) {
    unawaited(incomingCallHandler.onIncomingCall(doorName: door));
  }

  void _onDaemonStateChanged() {
    final daemon = _client.state;
    final phase = switch (daemon.phase) {
      IntercomdPhase.idle => CallPhase.idle,
      IntercomdPhase.connecting => CallPhase.connecting,
      IntercomdPhase.ringing => CallPhase.ringing,
      IntercomdPhase.connected => CallPhase.connected,
      IntercomdPhase.preview => CallPhase.previewing,
      // No CallPhase.offline exists (CallController never modeled it as a
      // call phase, only as separate discoveryListening/tcpServerListening
      // flags) -- idle is the closest honest mapping so the UI doesn't show
      // a stale ringing/connected screen when the daemon actually vanished.
      // lastError below is where "offline" actually surfaces.
      IntercomdPhase.offline => CallPhase.idle,
    };

    final isRingingNow = phase == CallPhase.ringing;
    if (isRingingNow && !_wasRinging) {
      unawaited(incomingCallHandler.onIncomingCall(doorName: daemon.door));
    } else if (!isRingingNow && _wasRinging) {
      unawaited(incomingCallHandler.onCallDismissed());
    }
    _wasRinging = isRingingNow;

    // Includes CallPhase.ringing (2026-09-20, fixed after on-device testing
    // found the incoming-call screen was showing black): the daemon starts
    // its own video pipeline as soon as it enters SYNCN_CALL_RINGING (see
    // video_session_start()'s call site in the door-connected/incoming-call
    // handler in src/core/call.c) and confirmed on-device to already be
    // streaming real frames ("stats: ringing | ... video 91 NALs") well
    // before the call is ever answered. intercom_page.dart's own ringing
    // screen (_CallView) already renders VideoSurface(textureId:
    // controller.videoTextureId) for exactly this reason ("Show the live
    // door camera feed during ringing too, not just after answering") --
    // this controller just never told the video plugin to subscribe during
    // that phase, leaving videoTextureId null the whole time the door was
    // ringing regardless of what the daemon was already sending.
    final shouldHaveVideo = phase == CallPhase.ringing ||
        phase == CallPhase.connected ||
        phase == CallPhase.previewing;
    // A preview -> ringing/connected transition (a real call arriving while
    // previewing) needs a full stop+restart of the video pipeline, not just
    // "leave it active": the daemon's preview and the incoming call are two
    // separate IPC connections server-side (confirmed via panel journal --
    // "ipc: UI disconnected" fires the moment the real call's socket
    // replaces the preview's), so the preview's subscribe_video was already
    // forgotten by the daemon. shouldHaveVideo staying true across that
    // transition previously left _videoActiveForPhase untouched, so
    // _video.start()/stop() were never called again and the native
    // syncn_video.c plugin kept its stale "connected" socket instead of
    // reconnecting and resubscribing -- every decoded frame during the call
    // was then silently dropped (dropped_sink) with audio unaffected.
    // Confirmed on-device 2026-09-22 (panel-v1.4.2): decoder healthy
    // (errors=0, starved=0) but shown=0 for the whole call.
    final wasPreviewing = _videoActivePhase == CallPhase.previewing;
    final enteringCallFromPreview = wasPreviewing &&
        (phase == CallPhase.ringing || phase == CallPhase.connected);
    _videoWanted = shouldHaveVideo;
    _videoWantedPhase = shouldHaveVideo ? phase : null;
    _videoRestartWanted = _videoRestartWanted || enteringCallFromPreview;
    _queueVideoLifecycle();

    _setState(_state.copyWith(
      phase: phase,
      callerLabel: daemon.door.isEmpty ? _state.callerLabel : daemon.door,
      muted: daemon.muted,
      transientMessage:
          daemon.phase == IntercomdPhase.offline ? 'Intercom offline' : null,
      isIncoming: isRingingNow || _state.isIncoming,
    ));
  }

  Future<void> _stopVideoIfActive() async {
    _videoWanted = false;
    _videoWantedPhase = null;
    _videoRestartWanted = false;
    await (_videoLifecycle = _videoLifecycle.then((_) async {
      if (_videoActiveForPhase) {
        await _video.stop();
        _videoActiveForPhase = false;
        _videoActivePhase = null;
      }
    }));
  }

  /// Platform-channel stop/start calls must never overlap. The old code
  /// launched them with `unawaited`, so a quick idle -> ringing -> connected
  /// sequence could execute `start` before the previous `stop` had released
  /// the native IPC socket. That left the daemon decoding frames for a dead
  /// subscriber after the second or third call.
  void _queueVideoLifecycle() {
    _videoLifecycle = _videoLifecycle.then((_) async {
      final wanted = _videoWanted;
      final phase = _videoWantedPhase;
      final restart = _videoRestartWanted;
      _videoRestartWanted = false;

      if (!wanted) {
        if (_videoActiveForPhase) {
          await _video.stop();
          _videoActiveForPhase = false;
          _videoActivePhase = null;
        }
        return;
      }

      if (restart && _videoActiveForPhase) {
        await _video.stop();
        _videoActiveForPhase = false;
        _videoActivePhase = null;
      }
      if (!_videoActiveForPhase) {
        await _video.start();
        _videoActiveForPhase = true;
      }
      _videoActivePhase = phase;
      if (!_shutdown) {
        _setState(_state.copyWith(hasVideoFrames: _video.textureId != null));
      }
    }).catchError((Object error, StackTrace stack) {
      debugPrint('intercom video lifecycle failed: $error\n$stack');
      _videoActiveForPhase = false;
      _videoActivePhase = null;
    });
  }

  Future<void> connectToDoor(String host, {int port = 0}) async {
    // host/port are accepted for call-site parity with CallController, but
    // ignored here: the daemon dials whatever door it is configured for
    // (`[door] address` in /etc/syncn/intercom.conf, or the last one it
    // learned from a discovery broadcast -- see syncn_config.h), not a host
    // the Dart layer chooses per call. See the port's handback report: the
    // in-app "door IP" setting (IntercomCubit.callDoorDirectly) has no effect
    // on this backend until the daemon's config is made to accept it too.
    _client.callDoor();
  }

  Future<void> startPreview(String host, {int port = 0}) async {
    _client.startPreview();
  }

  Future<void> stopPreview() async {
    _client.stopPreview();
  }

  Future<void> upgradeToCall() async {
    // The daemon has no separate "upgrade preview to call" command distinct
    // from answer/call -- a preview it opened itself already carries audio
    // per its own duplex_mode config. Calling answer() here would be wrong
    // when nothing is ringing; this is a deliberate no-op until the daemon
    // gains an equivalent command. Flagged in the handback report.
  }

  Future<void> simulateIncomingCall() async {
    // No daemon equivalent (it only reports real calls); mock/dev builds on
    // this backend cannot simulate one. See simulatePreview below.
  }

  Future<void> answer() async => _client.answer();
  Future<void> decline() async => _client.reject();
  Future<void> endCall() async => _client.hangUp();
  void unlock() => _client.unlock();
  Future<void> setMuted(bool muted) async => _client.setMuted(muted);

  void _setState(CallUiState next) {
    _state = next;
    notifyListeners();
  }
}
