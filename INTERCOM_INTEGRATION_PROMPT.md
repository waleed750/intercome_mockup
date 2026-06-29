# Intercom Feature Integration Prompt

> **Purpose**: This document is a complete, self-contained prompt for integrating the `intercom` Flutter plugin package into any Flutter mobile app using **BLoC + Clean Architecture**. The intercom package is delivered as-is and must NOT be modified. Your job is to wire it into the consuming app's architecture.

---

## Table of Contents

1. [Package Overview](#1-package-overview)
2. [Protocol Specification](#2-protocol-specification)
3. [Configuration Values](#3-configuration-values)
4. [Android Platform Setup](#4-android-platform-setup)
5. [iOS Platform Setup](#5-ios-platform-setup)
6. [Approach A: Quick Integration](#6-approach-a-quick-integration)
7. [Approach B: Full Clean Architecture](#7-approach-b-full-clean-architecture)
8. [Public API Reference](#8-public-api-reference)
9. [Testing & Verification](#9-testing--verification)

---

## 1. Package Overview

### What It Is

The `intercom` package is a Flutter plugin that turns a mobile device into a **video intercom indoor station**. It communicates with a Tuya-based IP door station over the local Wi-Fi network. The door has a camera, speaker, microphone, and a door latch relay. The app receives video calls from the door when a visitor presses the call button, and can answer, talk, view live video, and unlock the door.

### How It Works (High Level)

```
                          Wi-Fi LAN
                             |
    ┌────────────────┐       |       ┌──────────────────┐
    │  Door Station   │ UDP 8089     │  Your Flutter App │
    │  (Tuya IP cam)  │────────────> │  (Indoor Panel)   │
    │                 │  discovery    │                   │
    │                 │              │  DiscoveryResponder│
    │                 │ TCP 8189     │  CallServer        │
    │                 │────────────> │  CallConnection    │
    │                 │  call data   │  CallController    │
    │  Camera+Mic+    │  (control,   │  AudioPipeline     │
    │  Speaker+Relay  │   video,     │  VideoDecoder      │
    └────────────────┘   audio)     └──────────────────┘
```

1. **Discovery**: Door broadcasts a UDP probe on port `8089`. The app replies with its identity (ScreenInfo JSON). The door now knows the app exists and its IP.
2. **Call**: When a visitor presses the door's call button, the door connects to the app via TCP on port `8189`.
3. **Ringing**: The door sends a `{"command":"Call"}` control message. The app shows the incoming call screen, plays ringtone, vibrates.
4. **Answer**: User taps Answer. The app sends a 3-part answer handshake. Two-way audio starts. Live video from the door's camera is displayed.
5. **In-Call**: User can mute mic, unlock the door (sends `{"command":"OpenDoor"}`), or end the call.
6. **End**: Either side sends `{"command":"HangUp"}` or the TCP connection closes. Everything tears down, returns to idle.

### Architecture Inside the Package

```
CallController (ChangeNotifier — single source of truth)
  ├── DiscoveryResponder    ← UDP 8089 listener, replies with ScreenInfo
  ├── CallServer            ← TCP 8189 server, accepts door connections
  ├── CallConnection        ← read/write loop + FrameParser for active call
  ├── VideoDecoder          ← H.264 → Flutter Texture (native MethodChannel)
  ├── AudioPipeline         ← G.711 A-law mic/speaker (native MethodChannel)
  ├── Ringer                ← vibration + native ringtone
  ├── NotificationHandler   ← incoming call notifications with actions
  └── AndroidForegroundService ← keeps TCP listener alive in background
```

### Platform Support

| Platform | Status |
|----------|--------|
| **Android** | Fully implemented — audio, video, foreground service, notifications |
| **iOS** | **Stub only** — plugin compiles but audio/video handlers are no-ops. Needs native implementation. |

### Package Dependencies

```yaml
dependencies:
  flutter: sdk
  provider: ^6.1.0
  flutter_local_notifications: ^18.0.0
  shared_preferences: ^2.3.0
  vibration: ^2.0.0
  permission_handler: ^11.0.0
  connectivity_plus: ^6.0.0
```

---

## 2. Protocol Specification

### 2.1 UDP Discovery (Port 8089)

The door periodically broadcasts a UDP discovery probe. The app listens and replies.

**Request recognition** — the app matches on these substrings:
- `"cmd_send_get_call_device"`
- `"cmd_send_get_device_info"`

**Door probe example** (varies by firmware):
```json
{
  "command": "cmd_send_get_device_info",
  "localAddr": "door-001",
  "localType": 3
}
```

**App reply** — flat JSON `ScreenInfo`:
```json
{
  "command": "cmd_reply_get_device_info",
  "appid": "7551000",
  "alias": "Indoor 4521",
  "group_ip": "239.255.74.199",
  "serial": "a3f2b9c01e74",
  "dstType": 3,
  "dstAddr": "android-4521",
  "verify": 0,
  "deviceBusy": 0,
  "camera_en": 0,
  "relay0_delay": 1
}
```

When the door probe contains `localAddr`/`localType`, those values are echoed back into `dstAddr`/`dstType` in the reply.

### 2.2 TCP Call Connection (Port 8189)

The app runs a TCP server on port `8189`. The door is the TCP client — it connects when a visitor calls. All call data flows over this single TCP connection, multiplexed by a frame protocol.

### 2.3 Frame Format

All data (control, video, audio) is wrapped in frames:

```
┌───────────────┬──────────────────┬─────────────────┐
│ Magic (4 B)   │ Length (4 B, LE) │ Payload (N B)   │
│ marker×4      │ little-endian    │                 │
└───────────────┴──────────────────┴─────────────────┘
```

**Channel markers:**

| Channel | Marker byte | Magic (4 bytes) | Content |
|---------|-------------|-----------------|---------|
| CONTROL | `0xAA` | `AA AA AA AA` | JSON command strings |
| VIDEO   | `0xBB` | `BB BB BB BB` | H.264 Annex-B NAL units |
| AUDIO   | `0xCC` | `CC CC CC CC` | G.711 A-law encoded audio |

**Example**: A control frame containing `{"command":"Call"}` (16 bytes):
```
AA AA AA AA    ← magic (CONTROL channel)
10 00 00 00    ← length = 16 (little-endian)
7B 22 63 6F 6D 6D 61 6E 64 22 3A 22 43 61 6C 6C 22 7D  ← JSON payload
```

### 2.4 Control Commands

**Inbound (door → app):**

| Command | JSON | Meaning |
|---------|------|---------|
| Call | `{"command":"Call"}` | Visitor pressed call button → start ringing |
| GetCallInfo | `{"command":"GetCallInfo"}` | Door re-querying call state → re-confirm answer |
| HangUp | `{"command":"HangUp"}` | Door ended the call |

The `command` field may also appear as `cmd` in some firmware. Classification is case-insensitive.

**Outbound (app → door):**

| Command | JSON | When sent |
|---------|------|-----------|
| Answer (3-part handshake) | `{"command":"Answer"}` then `{"command":"Answer","OtherAnswer":1}` then `{"command":"Answer","OtherAnswer":true}` | User taps Answer — all 3 sent in order |
| HangUp | `{"command":"HangUp","OtherAnswer":0}` | User declines or ends call |
| OpenDoor | `{"command":"OpenDoor"}` | User taps Unlock (only during connected phase) |
| DeviceBusy | `{"command":"deviceBusy"}` | Reject a second incoming connection |

### 2.5 Audio

- **Codec**: G.711 A-law (ITU-T)
- **Sample rate**: 8 kHz, mono
- **Frame size**: 160 A-law bytes = 320 PCM bytes = 20 ms of audio
- **Downlink** (door → app): A-law frames arrive on the AUDIO channel → decoded to PCM → played on speaker
- **Uplink** (app → door): PCM captured from mic → encoded to A-law → sent on AUDIO channel
- **Native features** (Android): Acoustic Echo Cancellation (AEC), Noise Suppression (NS), Automatic Gain Control (AGC), speakerphone mode, audio focus

### 2.6 Video

- **Codec**: H.264 (Annex-B format with start codes)
- **Resolution**: 1920×1080 (default)
- **Frame rate**: ~30 fps
- **Rendering**: NAL units arrive on VIDEO channel → decoded by native `MediaCodec` → rendered to Flutter `Texture` widget
- **iOS**: Stub — `start()` returns `-1`, no decoding

---

## 3. Configuration Values

### 3.1 Device Identity

Persisted in `SharedPreferences`. Auto-generated on first run, editable by user.

| Field | SharedPreferences Key | Default | Purpose |
|-------|----------------------|---------|---------|
| `alias` | `intercom.alias` | `"Indoor XXXX"` (random 4-digit) | Display name shown in discovery reply |
| `serial` | `intercom.serial` | Random 12-char hex | Unique device identifier |
| `dstAddr` | `intercom.dst_addr` | `"android-XXXX"` | Logical address in discovery protocol |
| `doorName` | `intercom.door_name` | `"Front Door"` | Label for the paired door station |

**None of these are IP addresses.** The door learns the app's IP from the UDP packet source automatically.

### 3.2 Hardcoded Protocol Values

These are baked into the package and must NOT be changed:

| Value | Constant | Where |
|-------|----------|-------|
| UDP discovery port | `8089` | `Discovery.port` |
| TCP call server port | `8189` | `CallServer.defaultPort` |
| App ID | `"7551000"` | `ScreenInfo.appid` |
| Discovery reply command | `"cmd_reply_get_device_info"` | `ScreenInfo.command` |
| Multicast group IP | `"239.255.74.199"` | `ScreenInfo.groupIp` |
| Device type | `3` | `ScreenInfo.dstType` |

### 3.3 Intercom Modes

```dart
enum IntercomMode { panel, mobile }
```

| Mode | Behavior |
|------|----------|
| `panel` | Indoor station — starts discovery responder (UDP), call server (TCP), foreground service. Passively waits for the door to call. |
| `mobile` | Guard/remote mode — does NOT start discovery or server. Can actively connect to a door via `connectToDoor(host)`. |

---

## 4. Android Platform Setup

### 4.1 Required Permissions

Add ALL of these to your app's `android/app/src/main/AndroidManifest.xml`:

```xml
<manifest xmlns:android="http://schemas.android.com/apk/res/android">
    <!-- Network -->
    <uses-permission android:name="android.permission.INTERNET"/>
    <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE"/>
    <uses-permission android:name="android.permission.ACCESS_WIFI_STATE"/>
    <uses-permission android:name="android.permission.CHANGE_WIFI_MULTICAST_STATE"/>

    <!-- Media -->
    <uses-permission android:name="android.permission.RECORD_AUDIO"/>

    <!-- Notifications -->
    <uses-permission android:name="android.permission.POST_NOTIFICATIONS"/>
    <uses-permission android:name="android.permission.USE_FULL_SCREEN_INTENT"/>
    <uses-permission android:name="android.permission.VIBRATE"/>

    <!-- Background service -->
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE"/>
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE_CONNECTED_DEVICE"/>
    <uses-permission android:name="android.permission.WAKE_LOCK"/>

    <application ...>
        <!-- ... your activities ... -->

        <!-- Intercom foreground service (REQUIRED for panel mode) -->
        <service
            android:name="com.syncn.intercom.IntercomForegroundService"
            android:exported="false"
            android:foregroundServiceType="connectedDevice" />
    </application>
</manifest>
```

### 4.2 Native Platform Channels

The package registers these channels automatically via `IntercomPlugin`:

| Channel | Type | Purpose |
|---------|------|---------|
| `syncn_intercom/foreground_service` | MethodChannel | Start/stop foreground service, check pending calls |
| `syncn_intercom/audio` | MethodChannel | Start/stop audio, play downlink, set muted |
| `syncn_intercom/audio_uplink` | EventChannel | Stream of captured A-law audio frames |
| `syncn_intercom/video` | MethodChannel | Start/stop H.264 decoder, submit frames, get texture ID |
| `syncn_intercom/ringer` | MethodChannel | Start/stop native ringtone (stub — Dart uses `vibration` plugin) |

### 4.3 Build Requirements

- **Compile SDK**: 35 (Android 15)
- **Min SDK**: 23 (Android 6.0)
- **Kotlin**: 1.9.0+
- **JVM target**: 11

---

## 5. iOS Platform Setup

### 5.1 Current Status

iOS native code is **stub-only**. The plugin compiles and the Dart code runs, but:
- `VideoDecoderHandler.start()` returns `-1` (no texture)
- `AudioPipelineHandler` methods are no-ops
- No foreground service equivalent

### 5.2 What Needs Implementing

To make iOS work, you'd need to implement in `packages/intercom/ios/Classes/`:

1. **`VideoDecoderHandler.swift`**: Use `VideoToolbox` (`VTDecompressionSession`) to decode H.264 NAL units and render to a Flutter texture via `FlutterTextureRegistry`
2. **`AudioPipelineHandler.swift`**: Use `AVAudioEngine` or `AudioUnit` for 8kHz G.711 A-law capture/playback with echo cancellation
3. **Background modes**: Enable `voip` and `audio` background modes in `Info.plist`

### 5.3 Podspec

- **Min iOS**: 12.0
- **Swift**: 5.0
- **No external dependencies** beyond Flutter

---

## 6. Approach A: Quick Integration

Use the package's built-in screens directly. Wrap `CallController` in a thin Cubit for BLoC compatibility.

### 6.1 Add the Package Dependency

In your app's `pubspec.yaml`:

```yaml
dependencies:
  intercom:
    path: packages/intercom  # or git URL
  provider: ^6.1.0  # required by the package's built-in screens
```

### 6.2 Create a Thin IntercomCubit

This bridges the package's `ChangeNotifier` to BLoC:

```dart
// lib/features/intercom/presentation/cubit/intercom_state.dart

import 'package:equatable/equatable.dart';
import 'package:intercom/intercom.dart';

enum IntercomStatus { loading, ready, error }

class IntercomState extends Equatable {
  const IntercomState({
    this.status = IntercomStatus.loading,
    this.callState = const CallUiState(),
    this.textureId,
    this.errorMessage,
  });

  final IntercomStatus status;
  final CallUiState callState;
  final int? textureId;
  final String? errorMessage;

  CallPhase get phase => callState.phase;
  bool get isRinging => callState.isRinging;
  bool get isInCall => callState.isInCall;

  IntercomState copyWith({
    IntercomStatus? status,
    CallUiState? callState,
    int? textureId,
    String? errorMessage,
  }) {
    return IntercomState(
      status: status ?? this.status,
      callState: callState ?? this.callState,
      textureId: textureId ?? this.textureId,
      errorMessage: errorMessage ?? this.errorMessage,
    );
  }

  @override
  List<Object?> get props => [status, callState, textureId, errorMessage];
}
```

```dart
// lib/features/intercom/presentation/cubit/intercom_cubit.dart

import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:intercom/intercom.dart';

import 'intercom_state.dart';

class IntercomCubit extends Cubit<IntercomState> {
  IntercomCubit({required this.mode}) : super(const IntercomState());

  final IntercomMode mode;
  CallController? _controller;

  CallController? get controller => _controller;

  Future<void> initialize() async {
    try {
      final controller = await IntercomModule.init(mode: mode);
      _controller = controller;
      controller.addListener(_onControllerChanged);
      _onControllerChanged();
      emit(state.copyWith(status: IntercomStatus.ready));
    } catch (e) {
      emit(state.copyWith(
        status: IntercomStatus.error,
        errorMessage: e.toString(),
      ));
    }
  }

  void _onControllerChanged() {
    final controller = _controller;
    if (controller == null) return;
    emit(state.copyWith(
      callState: controller.state,
      textureId: controller.videoTextureId,
    ));
  }

  Future<void> answer() async => _controller?.answer();
  Future<void> decline() async => _controller?.decline();
  Future<void> endCall() async => _controller?.endCall();
  void unlock() => _controller?.unlock();
  Future<void> setMuted(bool muted) async => _controller?.setMuted(muted);
  Future<void> checkPendingCall() async =>
      _controller?.checkPendingBackgroundCall();

  Future<void> saveIdentity(DeviceIdentity identity) async {
    await _controller?.deviceConfig.save(identity);
    _controller?.refreshIdentity();
  }

  @override
  Future<void> close() async {
    _controller?.removeListener(_onControllerChanged);
    _controller?.dispose();
    return super.close();
  }
}
```

### 6.3 Wire It Up in Your App

```dart
// In your app's routing or feature module:

import 'package:flutter/material.dart';
import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:intercom/intercom.dart';
import 'package:provider/provider.dart';

import 'features/intercom/presentation/cubit/intercom_cubit.dart';
import 'features/intercom/presentation/cubit/intercom_state.dart';

class IntercomFeaturePage extends StatelessWidget {
  const IntercomFeaturePage({super.key});

  @override
  Widget build(BuildContext context) {
    return BlocProvider(
      create: (_) => IntercomCubit(mode: IntercomMode.panel)..initialize(),
      child: const _IntercomView(),
    );
  }
}

class _IntercomView extends StatelessWidget {
  const _IntercomView();

  @override
  Widget build(BuildContext context) {
    return BlocBuilder<IntercomCubit, IntercomState>(
      builder: (context, state) {
        if (state.status == IntercomStatus.loading) {
          return const Scaffold(
            body: Center(child: CircularProgressIndicator()),
          );
        }
        if (state.status == IntercomStatus.error) {
          return Scaffold(
            body: Center(child: Text('Error: ${state.errorMessage}')),
          );
        }
        // The built-in screens need CallController in Provider.
        // Bridge it from the Cubit:
        final controller = context.read<IntercomCubit>().controller;
        if (controller == null) {
          return const Scaffold(body: Center(child: Text('Not initialized')));
        }
        return ChangeNotifierProvider.value(
          value: controller,
          child: _PhaseRouter(phase: state.phase),
        );
      },
    );
  }
}

class _PhaseRouter extends StatelessWidget {
  const _PhaseRouter({required this.phase});

  final CallPhase phase;

  @override
  Widget build(BuildContext context) {
    return switch (phase) {
      CallPhase.ringing => const IncomingCallScreen(),
      CallPhase.connecting || CallPhase.connected => const InCallScreen(),
      CallPhase.idle => const IdleScreen(),
    };
  }
}
```

### 6.4 Handle App Lifecycle

```dart
// In your app's root widget or lifecycle observer:

@override
void didChangeAppLifecycleState(AppLifecycleState state) {
  if (state == AppLifecycleState.resumed) {
    context.read<IntercomCubit>().checkPendingCall();
  }
}
```

### 6.5 Theme

The package provides `IntercomTheme.dark()` — a Material 3 dark theme with teal accent (`0xFF1DAF8A`). You can either:
- Use it as the theme for intercom screens: wrap the intercom page in a `Theme(data: IntercomTheme.dark(), child: ...)`
- Use your app's own theme (the built-in screens use standard Material widgets)

---

## 7. Approach B: Full Clean Architecture

Full domain/data/presentation separation. The package becomes a data-layer dependency hidden behind a repository interface.

### 7.1 Folder Structure

```
lib/
  features/
    intercom/
      domain/
        entities/
          intercom_call_entity.dart
          intercom_device_entity.dart
        repositories/
          intercom_repository.dart
        usecases/
          start_intercom_usecase.dart
          answer_call_usecase.dart
          decline_call_usecase.dart
          end_call_usecase.dart
          unlock_door_usecase.dart
          set_muted_usecase.dart
          save_identity_usecase.dart
          connect_to_door_usecase.dart
      data/
        repositories/
          intercom_repository_impl.dart
        mappers/
          call_state_mapper.dart
      presentation/
        cubit/
          intercom_cubit.dart
          intercom_state.dart
        pages/
          intercom_page.dart
        widgets/
          (custom widgets or reuse package widgets)
```

### 7.2 Domain Layer

#### Entities

```dart
// lib/features/intercom/domain/entities/intercom_call_entity.dart

enum IntercomCallPhase { idle, ringing, connecting, connected }

class IntercomCallEntity {
  const IntercomCallEntity({
    this.phase = IntercomCallPhase.idle,
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
    this.videoTextureId,
  });

  final IntercomCallPhase phase;
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
  final int? videoTextureId;

  bool get isInCall =>
      phase == IntercomCallPhase.connecting ||
      phase == IntercomCallPhase.connected;
  bool get isRinging => phase == IntercomCallPhase.ringing;
}
```

```dart
// lib/features/intercom/domain/entities/intercom_device_entity.dart

class IntercomDeviceEntity {
  const IntercomDeviceEntity({
    required this.alias,
    required this.serial,
    required this.dstAddr,
    required this.doorName,
  });

  final String alias;
  final String serial;
  final String dstAddr;
  final String doorName;
}
```

#### Repository (Abstract)

```dart
// lib/features/intercom/domain/repositories/intercom_repository.dart

import '../entities/intercom_call_entity.dart';
import '../entities/intercom_device_entity.dart';

abstract class IntercomRepository {
  Stream<IntercomCallEntity> get callStateStream;
  IntercomCallEntity get currentCallState;
  IntercomDeviceEntity get currentIdentity;

  Future<void> start();
  Future<void> shutdown();
  Future<void> answer();
  Future<void> decline();
  Future<void> endCall();
  void unlock();
  Future<void> setMuted(bool muted);
  Future<void> connectToDoor(String host, {int port});
  Future<void> refreshConnectivity();
  Future<void> checkPendingBackgroundCall();
  Future<void> saveIdentity(IntercomDeviceEntity identity);
  void dispose();
}
```

#### Use Cases

```dart
// lib/features/intercom/domain/usecases/answer_call_usecase.dart

import '../repositories/intercom_repository.dart';

class AnswerCallUseCase {
  const AnswerCallUseCase(this._repository);

  final IntercomRepository _repository;

  Future<void> call() => _repository.answer();
}
```

```dart
// lib/features/intercom/domain/usecases/decline_call_usecase.dart

import '../repositories/intercom_repository.dart';

class DeclineCallUseCase {
  const DeclineCallUseCase(this._repository);

  final IntercomRepository _repository;

  Future<void> call() => _repository.decline();
}
```

```dart
// lib/features/intercom/domain/usecases/end_call_usecase.dart

import '../repositories/intercom_repository.dart';

class EndCallUseCase {
  const EndCallUseCase(this._repository);

  final IntercomRepository _repository;

  Future<void> call() => _repository.endCall();
}
```

```dart
// lib/features/intercom/domain/usecases/unlock_door_usecase.dart

import '../repositories/intercom_repository.dart';

class UnlockDoorUseCase {
  const UnlockDoorUseCase(this._repository);

  final IntercomRepository _repository;

  void call() => _repository.unlock();
}
```

```dart
// lib/features/intercom/domain/usecases/set_muted_usecase.dart

import '../repositories/intercom_repository.dart';

class SetMutedUseCase {
  const SetMutedUseCase(this._repository);

  final IntercomRepository _repository;

  Future<void> call(bool muted) => _repository.setMuted(muted);
}
```

```dart
// lib/features/intercom/domain/usecases/start_intercom_usecase.dart

import '../repositories/intercom_repository.dart';

class StartIntercomUseCase {
  const StartIntercomUseCase(this._repository);

  final IntercomRepository _repository;

  Future<void> call() => _repository.start();
}
```

```dart
// lib/features/intercom/domain/usecases/save_identity_usecase.dart

import '../entities/intercom_device_entity.dart';
import '../repositories/intercom_repository.dart';

class SaveIdentityUseCase {
  const SaveIdentityUseCase(this._repository);

  final IntercomRepository _repository;

  Future<void> call(IntercomDeviceEntity identity) =>
      _repository.saveIdentity(identity);
}
```

```dart
// lib/features/intercom/domain/usecases/connect_to_door_usecase.dart

import '../repositories/intercom_repository.dart';

class ConnectToDoorUseCase {
  const ConnectToDoorUseCase(this._repository);

  final IntercomRepository _repository;

  Future<void> call(String host, {int port = 8189}) =>
      _repository.connectToDoor(host, port: port);
}
```

### 7.3 Data Layer

#### Mapper

```dart
// lib/features/intercom/data/mappers/call_state_mapper.dart

import 'package:intercom/intercom.dart';

import '../../domain/entities/intercom_call_entity.dart';
import '../../domain/entities/intercom_device_entity.dart';

class CallStateMapper {
  static IntercomCallPhase mapPhase(CallPhase phase) {
    return switch (phase) {
      CallPhase.idle => IntercomCallPhase.idle,
      CallPhase.ringing => IntercomCallPhase.ringing,
      CallPhase.connecting => IntercomCallPhase.connecting,
      CallPhase.connected => IntercomCallPhase.connected,
    };
  }

  static IntercomCallEntity fromCallUiState(
    CallUiState state, {
    int? textureId,
  }) {
    return IntercomCallEntity(
      phase: mapPhase(state.phase),
      callerLabel: state.callerLabel,
      muted: state.muted,
      videoAvailable: state.videoAvailable,
      hasVideoFrames: state.hasVideoFrames,
      micAvailable: state.micAvailable,
      onWifi: state.onWifi,
      transientMessage: state.transientMessage,
      unitName: state.unitName,
      pairedDoor: state.pairedDoor,
      tcpServerListening: state.tcpServerListening,
      discoveryListening: state.discoveryListening,
      videoTextureId: textureId,
    );
  }

  static IntercomDeviceEntity fromDeviceIdentity(DeviceIdentity identity) {
    return IntercomDeviceEntity(
      alias: identity.alias,
      serial: identity.serial,
      dstAddr: identity.dstAddr,
      doorName: identity.doorName,
    );
  }

  static DeviceIdentity toDeviceIdentity(IntercomDeviceEntity entity) {
    return DeviceIdentity(
      alias: entity.alias,
      serial: entity.serial,
      dstAddr: entity.dstAddr,
      doorName: entity.doorName,
    );
  }
}
```

#### Repository Implementation

```dart
// lib/features/intercom/data/repositories/intercom_repository_impl.dart

import 'dart:async';

import 'package:intercom/intercom.dart';

import '../../domain/entities/intercom_call_entity.dart';
import '../../domain/entities/intercom_device_entity.dart';
import '../../domain/repositories/intercom_repository.dart';
import '../mappers/call_state_mapper.dart';

class IntercomRepositoryImpl implements IntercomRepository {
  IntercomRepositoryImpl({required this.mode});

  final IntercomMode mode;
  CallController? _controller;
  final _streamController = StreamController<IntercomCallEntity>.broadcast();

  @override
  Stream<IntercomCallEntity> get callStateStream => _streamController.stream;

  @override
  IntercomCallEntity get currentCallState {
    final controller = _controller;
    if (controller == null) return const IntercomCallEntity();
    return CallStateMapper.fromCallUiState(
      controller.state,
      textureId: controller.videoTextureId,
    );
  }

  @override
  IntercomDeviceEntity get currentIdentity {
    final controller = _controller;
    if (controller == null) {
      return const IntercomDeviceEntity(
        alias: '',
        serial: '',
        dstAddr: '',
        doorName: 'Front Door',
      );
    }
    return CallStateMapper.fromDeviceIdentity(controller.deviceConfig.identity);
  }

  @override
  Future<void> start() async {
    final controller = await IntercomModule.init(mode: mode);
    _controller = controller;
    controller.addListener(_emitState);
    _emitState();
  }

  void _emitState() {
    final controller = _controller;
    if (controller == null) return;
    _streamController.add(CallStateMapper.fromCallUiState(
      controller.state,
      textureId: controller.videoTextureId,
    ));
  }

  @override
  Future<void> shutdown() async => _controller?.shutdown();

  @override
  Future<void> answer() async => _controller?.answer();

  @override
  Future<void> decline() async => _controller?.decline();

  @override
  Future<void> endCall() async => _controller?.endCall();

  @override
  void unlock() => _controller?.unlock();

  @override
  Future<void> setMuted(bool muted) async => _controller?.setMuted(muted);

  @override
  Future<void> connectToDoor(String host, {int port = 8189}) async =>
      _controller?.connectToDoor(host, port: port);

  @override
  Future<void> refreshConnectivity() async =>
      _controller?.refreshConnectivity();

  @override
  Future<void> checkPendingBackgroundCall() async =>
      _controller?.checkPendingBackgroundCall();

  @override
  Future<void> saveIdentity(IntercomDeviceEntity identity) async {
    await _controller?.deviceConfig
        .save(CallStateMapper.toDeviceIdentity(identity));
    _controller?.refreshIdentity();
  }

  @override
  void dispose() {
    _controller?.removeListener(_emitState);
    _controller?.dispose();
    _streamController.close();
  }
}
```

### 7.4 Presentation Layer

#### Cubit State

```dart
// lib/features/intercom/presentation/cubit/intercom_state.dart

import 'package:equatable/equatable.dart';

import '../../domain/entities/intercom_call_entity.dart';

enum IntercomStatus { initial, loading, ready, error }

class IntercomState extends Equatable {
  const IntercomState({
    this.status = IntercomStatus.initial,
    this.callEntity = const IntercomCallEntity(),
    this.errorMessage,
  });

  final IntercomStatus status;
  final IntercomCallEntity callEntity;
  final String? errorMessage;

  IntercomCallPhase get phase => callEntity.phase;
  bool get isRinging => callEntity.isRinging;
  bool get isInCall => callEntity.isInCall;

  IntercomState copyWith({
    IntercomStatus? status,
    IntercomCallEntity? callEntity,
    String? errorMessage,
  }) {
    return IntercomState(
      status: status ?? this.status,
      callEntity: callEntity ?? this.callEntity,
      errorMessage: errorMessage ?? this.errorMessage,
    );
  }

  @override
  List<Object?> get props => [status, callEntity, errorMessage];
}
```

#### Cubit

```dart
// lib/features/intercom/presentation/cubit/intercom_cubit.dart

import 'dart:async';

import 'package:flutter_bloc/flutter_bloc.dart';

import '../../domain/entities/intercom_device_entity.dart';
import '../../domain/repositories/intercom_repository.dart';
import '../../domain/usecases/answer_call_usecase.dart';
import '../../domain/usecases/decline_call_usecase.dart';
import '../../domain/usecases/end_call_usecase.dart';
import '../../domain/usecases/save_identity_usecase.dart';
import '../../domain/usecases/set_muted_usecase.dart';
import '../../domain/usecases/start_intercom_usecase.dart';
import '../../domain/usecases/unlock_door_usecase.dart';
import 'intercom_state.dart';

class IntercomCubit extends Cubit<IntercomState> {
  IntercomCubit({
    required IntercomRepository repository,
    required StartIntercomUseCase startIntercom,
    required AnswerCallUseCase answerCall,
    required DeclineCallUseCase declineCall,
    required EndCallUseCase endCall,
    required UnlockDoorUseCase unlockDoor,
    required SetMutedUseCase setMuted,
    required SaveIdentityUseCase saveIdentity,
  })  : _repository = repository,
        _startIntercom = startIntercom,
        _answerCall = answerCall,
        _declineCall = declineCall,
        _endCall = endCall,
        _unlockDoor = unlockDoor,
        _setMuted = setMuted,
        _saveIdentity = saveIdentity,
        super(const IntercomState());

  final IntercomRepository _repository;
  final StartIntercomUseCase _startIntercom;
  final AnswerCallUseCase _answerCall;
  final DeclineCallUseCase _declineCall;
  final EndCallUseCase _endCall;
  final UnlockDoorUseCase _unlockDoor;
  final SetMutedUseCase _setMuted;
  final SaveIdentityUseCase _saveIdentity;
  StreamSubscription? _stateSub;

  Future<void> initialize() async {
    emit(state.copyWith(status: IntercomStatus.loading));
    try {
      await _startIntercom();
      _stateSub = _repository.callStateStream.listen((callEntity) {
        emit(state.copyWith(
          status: IntercomStatus.ready,
          callEntity: callEntity,
        ));
      });
      emit(state.copyWith(
        status: IntercomStatus.ready,
        callEntity: _repository.currentCallState,
      ));
    } catch (e) {
      emit(state.copyWith(
        status: IntercomStatus.error,
        errorMessage: e.toString(),
      ));
    }
  }

  Future<void> answer() async => _answerCall();
  Future<void> decline() async => _declineCall();
  Future<void> endCall() async => _endCall();
  void unlock() => _unlockDoor();
  Future<void> setMuted(bool muted) async => _setMuted(muted);
  Future<void> saveIdentity(IntercomDeviceEntity identity) async =>
      _saveIdentity(identity);

  Future<void> checkPendingCall() async =>
      _repository.checkPendingBackgroundCall();

  @override
  Future<void> close() async {
    await _stateSub?.cancel();
    _repository.dispose();
    return super.close();
  }
}
```

### 7.5 Dependency Injection

Using `get_it` or your DI framework:

```dart
// lib/features/intercom/di/intercom_injection.dart

import 'package:get_it/get_it.dart';
import 'package:intercom/intercom.dart';

import '../data/repositories/intercom_repository_impl.dart';
import '../domain/repositories/intercom_repository.dart';
import '../domain/usecases/answer_call_usecase.dart';
import '../domain/usecases/connect_to_door_usecase.dart';
import '../domain/usecases/decline_call_usecase.dart';
import '../domain/usecases/end_call_usecase.dart';
import '../domain/usecases/save_identity_usecase.dart';
import '../domain/usecases/set_muted_usecase.dart';
import '../domain/usecases/start_intercom_usecase.dart';
import '../domain/usecases/unlock_door_usecase.dart';
import '../presentation/cubit/intercom_cubit.dart';

void registerIntercomFeature(GetIt sl) {
  // Repository
  sl.registerLazySingleton<IntercomRepository>(
    () => IntercomRepositoryImpl(mode: IntercomMode.panel),
  );

  // Use cases
  sl.registerFactory(() => StartIntercomUseCase(sl()));
  sl.registerFactory(() => AnswerCallUseCase(sl()));
  sl.registerFactory(() => DeclineCallUseCase(sl()));
  sl.registerFactory(() => EndCallUseCase(sl()));
  sl.registerFactory(() => UnlockDoorUseCase(sl()));
  sl.registerFactory(() => SetMutedUseCase(sl()));
  sl.registerFactory(() => SaveIdentityUseCase(sl()));
  sl.registerFactory(() => ConnectToDoorUseCase(sl()));

  // Cubit
  sl.registerFactory(() => IntercomCubit(
        repository: sl(),
        startIntercom: sl(),
        answerCall: sl(),
        declineCall: sl(),
        endCall: sl(),
        unlockDoor: sl(),
        setMuted: sl(),
        saveIdentity: sl(),
      ));
}
```

### 7.6 Page (Custom UI)

```dart
// lib/features/intercom/presentation/pages/intercom_page.dart

import 'package:flutter/material.dart';
import 'package:flutter_bloc/flutter_bloc.dart';
import 'package:get_it/get_it.dart';

import '../../domain/entities/intercom_call_entity.dart';
import '../cubit/intercom_cubit.dart';
import '../cubit/intercom_state.dart';

class IntercomPage extends StatelessWidget {
  const IntercomPage({super.key});

  @override
  Widget build(BuildContext context) {
    return BlocProvider(
      create: (_) => GetIt.I<IntercomCubit>()..initialize(),
      child: BlocBuilder<IntercomCubit, IntercomState>(
        builder: (context, state) {
          if (state.status == IntercomStatus.loading ||
              state.status == IntercomStatus.initial) {
            return const Scaffold(
              body: Center(child: CircularProgressIndicator()),
            );
          }
          if (state.status == IntercomStatus.error) {
            return Scaffold(
              body: Center(
                child: Text('Intercom error: ${state.errorMessage}'),
              ),
            );
          }
          return switch (state.phase) {
            IntercomCallPhase.idle => _IdleView(state: state),
            IntercomCallPhase.ringing => _RingingView(state: state),
            IntercomCallPhase.connecting ||
            IntercomCallPhase.connected =>
              _InCallView(state: state),
          };
        },
      ),
    );
  }
}

class _IdleView extends StatelessWidget {
  const _IdleView({required this.state});

  final IntercomState state;

  @override
  Widget build(BuildContext context) {
    final entity = state.callEntity;
    return Scaffold(
      appBar: AppBar(
        title: Text(entity.unitName.isEmpty ? 'Indoor Station' : entity.unitName),
      ),
      body: Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Icon(
              entity.onWifi ? Icons.wifi : Icons.wifi_off,
              size: 48,
              color: entity.onWifi ? Colors.green : Colors.red,
            ),
            const SizedBox(height: 16),
            Text('Paired: ${entity.pairedDoor}',
                style: Theme.of(context).textTheme.headlineSmall),
            const SizedBox(height: 8),
            Text(
              'Discovery: ${entity.discoveryListening ? "ON" : "OFF"} | '
              'Server: ${entity.tcpServerListening ? "ON" : "OFF"}',
            ),
            const SizedBox(height: 24),
            // For testing:
            OutlinedButton(
              onPressed: () {
                // Use simulateIncomingCall if needed for testing
              },
              child: const Text('Waiting for calls...'),
            ),
          ],
        ),
      ),
    );
  }
}

class _RingingView extends StatelessWidget {
  const _RingingView({required this.state});

  final IntercomState state;

  @override
  Widget build(BuildContext context) {
    final cubit = context.read<IntercomCubit>();
    final entity = state.callEntity;
    return Scaffold(
      body: SafeArea(
        child: Column(
          children: [
            // Video preview area
            Expanded(
              child: entity.videoTextureId != null && entity.videoTextureId! >= 0
                  ? Texture(textureId: entity.videoTextureId!)
                  : const ColoredBox(
                      color: Colors.black,
                      child: Center(child: Text('Calling...')),
                    ),
            ),
            // Caller info + buttons
            Padding(
              padding: const EdgeInsets.all(24),
              child: Column(
                children: [
                  Text(entity.callerLabel,
                      style: Theme.of(context).textTheme.headlineMedium),
                  const SizedBox(height: 24),
                  Row(
                    mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                    children: [
                      FloatingActionButton(
                        heroTag: 'decline',
                        backgroundColor: Colors.red,
                        onPressed: cubit.decline,
                        child: const Icon(Icons.call_end),
                      ),
                      FloatingActionButton(
                        heroTag: 'answer',
                        backgroundColor: Colors.green,
                        onPressed: cubit.answer,
                        child: const Icon(Icons.call),
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _InCallView extends StatelessWidget {
  const _InCallView({required this.state});

  final IntercomState state;

  @override
  Widget build(BuildContext context) {
    final cubit = context.read<IntercomCubit>();
    final entity = state.callEntity;
    return Scaffold(
      body: Stack(
        fit: StackFit.expand,
        children: [
          // Video
          if (entity.videoTextureId != null && entity.videoTextureId! >= 0)
            Texture(textureId: entity.videoTextureId!)
          else
            const ColoredBox(color: Colors.black),
          // Controls overlay
          SafeArea(
            child: Padding(
              padding: const EdgeInsets.all(24),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(entity.callerLabel,
                      style: Theme.of(context)
                          .textTheme
                          .headlineMedium
                          ?.copyWith(color: Colors.white)),
                  if (!entity.micAvailable)
                    const Text('Microphone unavailable',
                        style: TextStyle(color: Colors.red)),
                  const Spacer(),
                  if (entity.transientMessage != null)
                    Center(
                      child: Text(entity.transientMessage!,
                          style: const TextStyle(
                              color: Colors.white, fontSize: 18)),
                    ),
                  const SizedBox(height: 16),
                  Row(
                    mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                    children: [
                      FloatingActionButton(
                        heroTag: 'mute',
                        onPressed: () => cubit.setMuted(!entity.muted),
                        child:
                            Icon(entity.muted ? Icons.mic_off : Icons.mic),
                      ),
                      FloatingActionButton(
                        heroTag: 'unlock',
                        backgroundColor: Colors.green,
                        onPressed: cubit.unlock,
                        child: const Icon(Icons.lock_open),
                      ),
                      FloatingActionButton(
                        heroTag: 'end',
                        backgroundColor: Colors.red,
                        onPressed: cubit.endCall,
                        child: const Icon(Icons.call_end),
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }
}
```

---

## 8. Public API Reference

### 8.1 Exports from `package:intercom/intercom.dart`

| Class | Type | Purpose |
|-------|------|---------|
| `CallController` | `ChangeNotifier` | Main controller — call lifecycle, media, discovery, server |
| `CallPhase` | `enum` | `idle`, `ringing`, `connecting`, `connected` |
| `CallUiState` | Immutable data class | Full UI state snapshot |
| `DeviceConfig` | Config manager | Load/save identity via SharedPreferences |
| `DeviceIdentity` | Immutable data class | alias, serial, dstAddr, doorName |
| `IntercomMode` | `enum` | `panel`, `mobile` |
| `IntercomModule` | Static factory | `init(mode)` → returns `CallController` |
| `AndroidForegroundService` | Static methods | Start/stop foreground service, check pending calls |
| `IntercomNotificationHandler` | Notification manager | Show/dismiss incoming call notifications |
| `Commands` | Static protocol builder | Parse inbound, build outbound control frames |
| `Discovery` | Static protocol builder | Parse probes, build replies |
| `ScreenInfo` | Immutable data class | Discovery reply payload |
| `Frame` | Static encoder | Encode channel + payload into wire frames |
| `FrameParser` | Streaming parser | Parse incoming TCP bytes into frames |
| `ALawCodec` | Static codec | G.711 A-law encode/decode |
| `IdleScreen` | Widget | Built-in idle/home screen |
| `IncomingCallScreen` | Widget | Built-in ringing screen |
| `InCallScreen` | Widget | Built-in connected call screen |
| `AddIntercomScreen` | Widget | Built-in settings/identity editor |
| `IntercomTheme` | Theme factory | `dark()` → Material 3 dark theme |

### 8.2 CallController Methods

```dart
// Lifecycle
Future<void> start()                    // Start discovery + server + service
Future<void> shutdown()                 // Stop everything
void refreshIdentity()                  // Re-read identity from DeviceConfig

// Call actions
Future<void> answer()                   // Accept ringing call
Future<void> decline()                  // Decline ringing call
Future<void> endCall()                  // End connected call
void unlock()                           // Send OpenDoor (connected only)
Future<void> setMuted(bool muted)       // Toggle mic mute

// Connectivity
Future<void> refreshConnectivity()      // Re-check Wi-Fi status
Future<void> connectToDoor(String host, {int port = 8189})  // Outbound call (mobile mode)

// Background
Future<void> checkPendingBackgroundCall()  // Check if foreground service caught a call
Future<void> simulateIncomingCall()        // Test: fake incoming call

// State
CallUiState get state                   // Current state snapshot
int? get videoTextureId                 // Flutter texture ID for video
DeviceConfig get deviceConfig           // Access to identity config
```

### 8.3 CallUiState Fields

```dart
CallPhase phase                 // idle, ringing, connecting, connected
String callerLabel              // "Front Door" (or custom door name)
bool muted                      // Mic muted?
bool videoAvailable             // false after decode failure
bool hasVideoFrames             // true after first frame rendered
bool micAvailable               // false if mic permission denied
bool onWifi                     // Wi-Fi connectivity
String? transientMessage        // "Door unlocked", "Call ended" (auto-clears after 2.5s)
String unitName                 // This device's alias
String pairedDoor               // Paired door label
bool tcpServerListening         // TCP 8189 server active?
bool discoveryListening         // UDP 8089 responder active?

// Computed
bool get isInCall               // connecting || connected
bool get isRinging              // ringing
```

---

## 9. Testing & Verification

### 9.1 Without Hardware (Simulated Call)

The package includes `simulateIncomingCall()` which triggers the ringing state without a real door connection:

```dart
// From your Cubit or directly:
controller.simulateIncomingCall();
```

This will:
- Set phase to `ringing`
- Show the incoming call notification
- Start the video decoder (will show placeholder since no real video)
- Start vibration

### 9.2 With Mock Door (Python Tool)

The repository includes a `mock_door/` Python tool that simulates a real door station:

```bash
cd mock_door
pip install -r requirements.txt
python mock_door.py          # CLI mode
python web_dashboard.py      # Web dashboard at http://localhost:5000
```

The mock door:
1. Sends UDP discovery probes to find your app
2. Connects via TCP on port 8189
3. Sends `Call` command
4. Streams webcam video (H.264) and microphone audio (G.711)
5. Receives your audio back

### 9.3 Verification Checklist

Run through this checklist to confirm the integration works:

- [ ] **App starts** — no crash, intercom initializes
- [ ] **Idle screen** — shows unit name, paired door name
- [ ] **Status indicators** — "UDP 8089" and "TCP 8189" show as active (green)
- [ ] **Wi-Fi detection** — Wi-Fi status pill shows correctly
- [ ] **Simulated call** — `simulateIncomingCall()` transitions to ringing screen
- [ ] **Ringing** — vibration plays, notification appears with Answer/Reject buttons
- [ ] **Notification actions** — tapping Answer/Reject in notification works
- [ ] **Answer** — transitions to in-call screen
- [ ] **Video** — live video from door displays (with real door or mock)
- [ ] **Audio downlink** — door's audio plays through speaker
- [ ] **Audio uplink** — mic captures and sends to door
- [ ] **Mute toggle** — mute/unmute works, icon updates
- [ ] **Unlock** — "Door unlocked" transient message appears
- [ ] **End call** — returns to idle, "Call ended" message shows briefly
- [ ] **Decline** — from ringing, returns to idle without "Call ended"
- [ ] **Busy rejection** — second call while in-call gets `deviceBusy` response
- [ ] **Background call** — foreground service catches call when app is in background
- [ ] **App resume** — pending background call is picked up on resume
- [ ] **Settings** — identity can be edited and saved, discovery reply updates
- [ ] **App lifecycle** — dispose cleans up, no socket leaks

### 9.4 Common Issues

| Issue | Cause | Fix |
|-------|-------|-----|
| UDP 8089 shows OFF | Port already in use, or no Wi-Fi | Kill other intercom instances, check Wi-Fi |
| TCP 8189 shows OFF | Port conflict | Another app or instance is using 8189 |
| No video | iOS (stub), or MediaCodec init failed | Check Android logs for `VideoDecoderHandler` |
| No audio | Mic permission denied | Grant RECORD_AUDIO, check `micAvailable` state |
| Door can't find app | Different Wi-Fi network, or multicast blocked | Same SSID, check `CHANGE_WIFI_MULTICAST_STATE` permission |
| Notification not showing | Missing POST_NOTIFICATIONS permission (Android 13+) | Request at runtime |
| Foreground service crash | Missing service declaration in manifest | Add the `<service>` tag |

---

## Summary: Minimum Steps to Get Running

1. Add `intercom` package dependency to `pubspec.yaml`
2. Add ALL Android permissions + service declaration to `AndroidManifest.xml`
3. Create `IntercomCubit` (thin wrapper or full clean arch)
4. Call `IntercomModule.init(mode: IntercomMode.panel)` on startup
5. Route UI based on `CallPhase`: idle → ringing → in-call
6. Handle app lifecycle: call `checkPendingBackgroundCall()` on resume
7. Test with `simulateIncomingCall()` or the mock door tool

The intercom package handles everything else internally: discovery, TCP server, frame parsing, audio/video codecs, notifications, and foreground service.
