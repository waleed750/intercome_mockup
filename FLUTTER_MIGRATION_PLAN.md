# Flutter Intercom Migration Plan

## Overview

Migrate the native Android Kotlin intercom app to **Flutter** and split into two apps:

1. **Kiosk App** — always-on Android tablet/panel mounted on wall. Full indoor station (video, audio, answer, unlock) + notifies mobile apps when a call arrives.
2. **Mobile App (SyncN)** — Android + iOS. Receives notifications, answers calls remotely or on LAN.

---

## Architecture

```
                          ┌─────────────────────────┐
                          │    Door Station (LAN)    │
                          │   UDP 8089 / TCP 8189    │
                          └───────────┬─────────────┘
                                      │
                          ┌───────────▼─────────────┐
                          │   Kiosk App (Flutter)    │
                          │   Always-on Android      │
                          │                          │
                          │   - Discovery listener   │
                          │   - TCP call server      │
                          │   - Video/Audio display  │
                          │   - Answer/Unlock/End    │
                          │   - Notifies mobile apps │
                          └──┬──────────────────┬────┘
                             │                  │
                   ┌─────────▼──────┐  ┌────────▼────────┐
                   │  Same Network  │  │  Different Net  │
                   │  (LAN notify)  │  │  (FCM via API)  │
                   └─────────┬──────┘  └────────┬────────┘
                             │                  │
                   ┌─────────▼──────────────────▼────────┐
                   │     Mobile App (Flutter)             │
                   │     Android + iOS (SyncN)            │
                   │                                      │
                   │  - FCM receiver (wakes app)          │
                   │  - LAN discovery (if same network)   │
                   │  - Video/Audio display               │
                   │  - Answer/Unlock/End                  │
                   └──────────────────────────────────────┘
```

---

## Notification Strategy

### Priority 1: Same Network (LAN Direct)
- Kiosk sends UDP broadcast on a custom port (e.g., **8090**) when a call arrives
- Mobile app listens for this broadcast (Android: foreground service, iOS: only while app is open)
- Fastest response, no internet needed

### Priority 2: FCM Push (for iOS wake-up and remote access)
- Kiosk sends HTTP request to your backend API: `POST /api/intercom/call`
- Backend sends FCM push to all registered devices
- **Android**: FCM wakes the app via `FirebaseMessagingService` → shows notification
- **iOS**: FCM wakes the app via APNs → shows notification (works even when killed)
- This solves the iOS background problem completely

### Flow when door rings:
```
1. Door sends TCP Call to Kiosk
2. Kiosk rings (shows video, plays ringtone)
3. Simultaneously:
   a. Kiosk broadcasts UDP on port 8090 (LAN notify)
   b. Kiosk sends POST to backend → FCM push to all mobile apps
4. Mobile app receives notification
5. User taps Answer:
   a. If on same LAN → app connects directly to door via TCP 8189
   b. If remote → app connects via kiosk as relay (WebRTC or TCP relay)
6. Kiosk stops ringing (another device answered)
```

---

## Project Structure

```
intercom_flutter/
├── packages/
│   └── intercom_protocol/           # Shared Dart package
│       ├── lib/
│       │   ├── frame.dart            # Frame encode/decode (AA/BB/CC channels)
│       │   ├── frame_parser.dart     # Streaming frame parser
│       │   ├── commands.dart         # Control command JSON encode/decode
│       │   ├── discovery.dart        # UDP discovery (ScreenInfo, probe/reply)
│       │   ├── alaw_codec.dart       # G.711 A-law encode/decode
│       │   └── intercom_protocol.dart # barrel export
│       └── pubspec.yaml
│
├── apps/
│   ├── kiosk/                        # Kiosk app (Android only)
│   │   ├── lib/
│   │   │   ├── main.dart
│   │   │   ├── services/
│   │   │   │   ├── call_controller.dart    # Call state machine
│   │   │   │   ├── call_server.dart        # TCP server on 8189
│   │   │   │   ├── discovery_responder.dart # UDP listener on 8089
│   │   │   │   ├── video_decoder.dart       # H.264 decode (platform channel → MediaCodec)
│   │   │   │   ├── audio_pipeline.dart      # A-law audio (record + playback)
│   │   │   │   └── notifier.dart            # Notify mobile apps (LAN + FCM)
│   │   │   ├── screens/
│   │   │   │   ├── idle_screen.dart
│   │   │   │   ├── incoming_call_screen.dart
│   │   │   │   └── in_call_screen.dart
│   │   │   └── widgets/
│   │   │       ├── video_surface.dart
│   │   │       └── action_buttons.dart
│   │   └── pubspec.yaml
│   │
│   └── mobile/                       # Mobile app (Android + iOS)
│       ├── lib/
│       │   ├── main.dart
│       │   ├── services/
│       │   │   ├── fcm_handler.dart         # Firebase messaging receiver
│       │   │   ├── lan_listener.dart         # UDP broadcast listener (same network)
│       │   │   ├── call_controller.dart      # Call state machine
│       │   │   ├── call_connection.dart      # TCP client to door or kiosk relay
│       │   │   ├── video_decoder.dart
│       │   │   └── audio_pipeline.dart
│       │   ├── screens/
│       │   │   ├── home_screen.dart
│       │   │   ├── incoming_call_screen.dart
│       │   │   └── in_call_screen.dart
│       │   └── widgets/
│       │       ├── video_surface.dart
│       │       └── action_buttons.dart
│       └── pubspec.yaml
│
└── pubspec.yaml                      # Workspace root
```

---

## Shared Protocol Package (`intercom_protocol`)

Pure Dart, no Flutter dependency. Used by both apps.

### frame.dart
```dart
// Channel markers
const int channelControl = 0xAA;
const int channelVideo = 0xBB;
const int channelAudio = 0xCC;

// Encode: [marker×4][length LE 4][payload]
Uint8List encodeFrame(int channel, Uint8List payload) { ... }
```

### frame_parser.dart
```dart
// Streaming parser — feed bytes, get callbacks
class FrameParser {
  void Function(int channel, Uint8List payload) onFrame;
  void feed(Uint8List data) { ... }  // handles split/merged/desync
}
```

### commands.dart
```dart
// Outbound
Uint8List callCommand() => _control('{"command":"Call"}');
Uint8List answerFrames() => [...];  // 3 Answer variants
Uint8List hangUpCommand() => ...;
Uint8List openDoorCommand() => ...;

// Inbound parsing
enum InboundCommand { call, answer, hangUp, openDoor, getCallInfo, deviceBusy, unknown }
InboundCommand classify(Uint8List payload) { ... }
```

### discovery.dart
```dart
// UDP discovery responder
class DiscoveryResponder {
  void start();  // bind UDP 8089, reply to probes
  void stop();
}
```

### alaw_codec.dart
```dart
// G.711 A-law encode/decode (pure Dart)
Uint8List pcmToAlaw(Uint8List pcm16le) { ... }
Uint8List alawToPcm(Uint8List alaw) { ... }
```

---

## Kiosk App — Key Implementation Details

### Call Server (TCP 8189)
```dart
// Uses dart:io ServerSocket
// Binds to WiFi interface
// Accepts connections from door station
// Feeds frames to FrameParser
// Routes: CONTROL → CallController, VIDEO → decoder, AUDIO → speaker
```

### Video Decoding
Flutter can't decode H.264 directly. Options:

| Approach | Pros | Cons |
|----------|------|------|
| **Platform channel → MediaCodec** | Hardware accel, low latency | Android only, native code needed |
| **ffmpeg via FFI (ffi_plugin)** | Cross-platform | Complex setup, CPU decode |
| **flutter_vlc_player** | Easy | Needs a stream URL, not raw bytes |
| **Texture + platform channel** | Best control | Most code to write |

**Recommended:** Platform channel to Android `MediaCodec` (same as current Kotlin code). The kiosk is Android-only so this is fine. For the mobile app (iOS too), use the same approach with platform-specific decoders.

### Audio Pipeline
```dart
// Use flutter_sound or record + just_audio packages
// Capture: mic → PCM 16-bit 8kHz → A-law encode → send as CC frame
// Playback: receive CC frame → A-law decode → PCM → speaker
// Alternative: platform channel to native AudioRecord/AudioTrack (lower latency)
```

### Notifier (LAN + FCM)
```dart
class CallNotifier {
  // When door rings:
  void notifyMobileApps(String doorName, String doorIp) {
    // 1. LAN broadcast
    _sendUdpBroadcast(port: 8090, payload: {
      "type": "incoming_call",
      "door": doorName,
      "door_ip": doorIp,
      "kiosk_ip": myIp,
      "timestamp": DateTime.now().toIso8601String(),
    });

    // 2. FCM via backend
    _sendToBackend(
      endpoint: "POST /api/intercom/call",
      body: {
        "door": doorName,
        "door_ip": doorIp,
        "kiosk_ip": myIp,
      },
    );
    // Backend sends FCM to all registered device tokens
  }
}
```

---

## Mobile App — Key Implementation Details

### FCM Handler (Android + iOS)
```dart
// firebase_messaging package
// Background handler (runs even when app is killed):
@pragma('vm:entry-point')
Future<void> _firebaseBackgroundHandler(RemoteMessage message) async {
  if (message.data['type'] == 'incoming_call') {
    // Show local notification with answer/decline actions
    // On Android: full-screen intent notification
    // On iOS: notification with actions
  }
}
```

### LAN Listener (same network)
```dart
// Android: runs in foreground service (flutter_background_service)
// iOS: only active while app is in foreground
// Listens for UDP broadcast on port 8090 from kiosk
class LanCallListener {
  RawDatagramSocket? _socket;
  void start() async {
    _socket = await RawDatagramSocket.bind('0.0.0.0', 8090);
    _socket!.listen((event) {
      if (event == RawSocketEvent.read) {
        final datagram = _socket!.receive();
        // Parse incoming call notification
        // Show call UI
      }
    });
  }
}
```

### Call Connection
When user answers on mobile:
```dart
// If on same LAN → connect directly to door
Socket.connect(doorIp, 8189).then((socket) {
  // Send Answer frames
  // Start video/audio
});

// If remote → connect via kiosk relay (future: WebRTC)
```

---

## Flutter Dependencies

### Shared
```yaml
# intercom_protocol - no dependencies (pure Dart)
```

### Kiosk App
```yaml
dependencies:
  intercom_protocol:
    path: ../../packages/intercom_protocol
  provider: ^6.0.0          # State management
  wakelock_plus: ^1.0.0     # Keep screen on
  # Platform channels for MediaCodec video + AudioRecord/AudioTrack
```

### Mobile App
```yaml
dependencies:
  intercom_protocol:
    path: ../../packages/intercom_protocol
  firebase_messaging: ^14.0.0    # FCM push notifications
  firebase_core: ^2.0.0
  flutter_local_notifications: ^17.0.0  # Show call notifications
  flutter_background_service: ^5.0.0    # Android background service
  provider: ^6.0.0
  # Platform channels for video/audio
```

---

## Migration Steps

### Phase 1: Protocol Package (Week 1)
- [ ] Create `intercom_protocol` Dart package
- [ ] Port Frame encode/decode from Kotlin
- [ ] Port FrameParser from Kotlin
- [ ] Port Commands (classify, encode) from Kotlin
- [ ] Port ALawCodec from Kotlin
- [ ] Port Discovery (ScreenInfo, probe/reply) from Kotlin
- [ ] Write unit tests (port existing Kotlin tests)

### Phase 2: Kiosk App — Core (Week 2)
- [ ] Flutter project setup (Android only)
- [ ] TCP CallServer using `dart:io ServerSocket`
- [ ] UDP DiscoveryResponder using `dart:io RawDatagramSocket`
- [ ] CallController state machine (IDLE → RINGING → CONNECTED)
- [ ] Platform channel for H.264 decoding (reuse existing MediaCodec Kotlin code)
- [ ] Platform channel for audio (capture + playback with A-law)
- [ ] Basic UI: Idle, IncomingCall, InCall screens

### Phase 3: Kiosk App — Notifications (Week 2-3)
- [ ] LAN broadcast notifier (UDP 8090)
- [ ] Backend API call for FCM push
- [ ] Kiosk mode setup (lock to app, auto-start on boot)

### Phase 4: Mobile App — Core (Week 3)
- [ ] Flutter project setup (Android + iOS)
- [ ] FCM integration (firebase_messaging)
- [ ] Background message handler
- [ ] Local notification with answer/decline actions
- [ ] Call connection (TCP client to door)
- [ ] Video/Audio (platform channels)
- [ ] UI: Home, IncomingCall, InCall screens

### Phase 5: Mobile App — Background (Week 4)
- [ ] Android foreground service for LAN listener
- [ ] iOS push notification handling
- [ ] Answer from notification (opens app + auto-answers)
- [ ] Testing on both platforms

### Phase 6: Integration & Polish (Week 4-5)
- [ ] Multi-device coordination (one answers → others stop ringing)
- [ ] Call relay through kiosk (for remote access)
- [ ] Settings screen (device identity, door name)
- [ ] Boot receiver (Android)
- [ ] Battery optimization handling (Android)

---

## Key Technical Decisions

### 1. Video Decoding in Flutter
**Decision:** Use platform channels to native decoders (MediaCodec on Android, VideoToolbox on iOS). Flutter's Texture widget displays the decoded frames.

**Why not ffmpeg/VLC:** Too much latency for real-time intercom. Native hardware decoders are essential.

### 2. Audio in Flutter
**Decision:** Platform channels to native audio APIs for low-latency capture/playback at 8kHz.

**Why not flutter_sound:** It adds overhead and may not support raw 8kHz A-law efficiently. Direct platform channel gives control over buffer sizes and latency.

### 3. State Management
**Decision:** Provider + ChangeNotifier for simplicity (same pattern as the Kotlin StateFlow approach).

### 4. iOS Background Wake
**Decision:** FCM push notification (via backend). The kiosk device solves the "no internet on door" problem by acting as the bridge.

---

## What Gets Reused from Current Kotlin Code

| Component | Reuse Strategy |
|-----------|---------------|
| Frame/FrameParser | Port to Dart (pure logic, straightforward) |
| ALawCodec | Port to Dart (pure math, straightforward) |
| Commands | Port to Dart (JSON encode/decode) |
| Discovery | Port to Dart (UDP socket logic) |
| VideoPipeline (MediaCodec) | Keep as Kotlin, wrap in platform channel |
| AudioPipeline | Keep as Kotlin, wrap in platform channel |
| CallController state machine | Port to Dart (business logic) |
| CallNotifications | Replace with flutter_local_notifications |
| IntercomService | Replace with flutter_background_service (Android) + FCM (iOS) |

---

## Notes

- The kiosk app is **Android only** — no need for iOS platform channels there
- The mobile app needs **both Android and iOS** platform channels for video/audio
- The protocol package is **pure Dart** — testable, no platform dependencies
- FCM handles iOS background wake-up — no need for NEAppPushProvider
- The mock_door Python service can still be used for testing during development
