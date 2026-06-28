import 'call_phase.dart';

final class CallUiState {
  const CallUiState({
    this.phase = CallPhase.idle,
    this.callerLabel = 'Front Door',
    this.muted = false,
    this.videoAvailable = true,
    this.hasVideoFrames = false,
    this.micAvailable = true,
    this.onWifi = true,
    this.transientMessage,
    this.unitName = '',
    this.pairedDoor = 'Front Door',
    this.tcpServerListening = false,
    this.discoveryListening = false,
  });

  final CallPhase phase;
  final String callerLabel;
  final bool muted;
  final bool videoAvailable;
  final bool hasVideoFrames;
  final bool micAvailable;
  final bool onWifi;
  final String? transientMessage;
  final String unitName;
  final String pairedDoor;
  final bool tcpServerListening;
  final bool discoveryListening;

  bool get isInCall =>
      phase == CallPhase.connecting || phase == CallPhase.connected;
  bool get isRinging => phase == CallPhase.ringing;

  CallUiState copyWith({
    CallPhase? phase,
    String? callerLabel,
    bool? muted,
    bool? videoAvailable,
    bool? hasVideoFrames,
    bool? micAvailable,
    bool? onWifi,
    Object? transientMessage = _sentinel,
    String? unitName,
    String? pairedDoor,
    bool? tcpServerListening,
    bool? discoveryListening,
  }) {
    return CallUiState(
      phase: phase ?? this.phase,
      callerLabel: callerLabel ?? this.callerLabel,
      muted: muted ?? this.muted,
      videoAvailable: videoAvailable ?? this.videoAvailable,
      hasVideoFrames: hasVideoFrames ?? this.hasVideoFrames,
      micAvailable: micAvailable ?? this.micAvailable,
      onWifi: onWifi ?? this.onWifi,
      transientMessage: identical(transientMessage, _sentinel)
          ? this.transientMessage
          : transientMessage as String?,
      unitName: unitName ?? this.unitName,
      pairedDoor: pairedDoor ?? this.pairedDoor,
      tcpServerListening: tcpServerListening ?? this.tcpServerListening,
      discoveryListening: discoveryListening ?? this.discoveryListening,
    );
  }
}

const _sentinel = Object();
