import 'call_phase.dart';

final class CallUiState {
  const CallUiState({
    this.phase = CallPhase.idle,
    this.callerLabel = 'Front Door',
    this.muted = false,
    this.headsetMode = false,
    this.videoAvailable = true,
    this.hasVideoFrames = false,
    this.micAvailable = true,
    this.onWifi = true,
    this.transientMessage,
    this.unitName = '',
    this.pairedDoor = 'Front Door',
    this.localIpAddress,
    this.tcpServerListening = false,
    this.discoveryListening = false,
    this.isIncoming = false,
  });

  final CallPhase phase;
  final String callerLabel;
  final bool muted;
  final bool headsetMode;
  final bool videoAvailable;
  final bool hasVideoFrames;
  final bool micAvailable;
  final bool onWifi;
  final String? transientMessage;
  final String unitName;
  final String pairedDoor;
  final String? localIpAddress;
  final bool tcpServerListening;
  final bool discoveryListening;
  // True when the current/last ringing-or-connected call originated from
  // the door calling in, false when it was dialed out from the panel
  // (connectToDoor/startPreview+upgradeToCall) -- lets the UI show a
  // fullscreen takeover only for incoming calls, since an outbound call
  // upgraded from the already-visible preview doesn't need one.
  final bool isIncoming;

  bool get isInCall =>
      phase == CallPhase.connecting || phase == CallPhase.connected;
  bool get isRinging => phase == CallPhase.ringing;

  CallUiState copyWith({
    CallPhase? phase,
    String? callerLabel,
    bool? muted,
    bool? headsetMode,
    bool? videoAvailable,
    bool? hasVideoFrames,
    bool? micAvailable,
    bool? onWifi,
    Object? transientMessage = _sentinel,
    String? unitName,
    String? pairedDoor,
    Object? localIpAddress = _sentinel,
    bool? tcpServerListening,
    bool? discoveryListening,
    bool? isIncoming,
  }) {
    return CallUiState(
      phase: phase ?? this.phase,
      callerLabel: callerLabel ?? this.callerLabel,
      muted: muted ?? this.muted,
      headsetMode: headsetMode ?? this.headsetMode,
      videoAvailable: videoAvailable ?? this.videoAvailable,
      hasVideoFrames: hasVideoFrames ?? this.hasVideoFrames,
      micAvailable: micAvailable ?? this.micAvailable,
      onWifi: onWifi ?? this.onWifi,
      transientMessage: identical(transientMessage, _sentinel)
          ? this.transientMessage
          : transientMessage as String?,
      unitName: unitName ?? this.unitName,
      pairedDoor: pairedDoor ?? this.pairedDoor,
      localIpAddress: identical(localIpAddress, _sentinel)
          ? this.localIpAddress
          : localIpAddress as String?,
      tcpServerListening: tcpServerListening ?? this.tcpServerListening,
      discoveryListening: discoveryListening ?? this.discoveryListening,
      isIncoming: isIncoming ?? this.isIncoming,
    );
  }
}

const _sentinel = Object();
