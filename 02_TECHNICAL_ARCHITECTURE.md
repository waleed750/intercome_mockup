# Technical Architecture Document
## Smart Home Video Intercom — Android App

**Status:** Draft v1
**Platform:** Native Android
**Companion:** see the protocol reference (reverse-engineered intercom protocol)

---

## 1. Purpose

Describes how to build the Android indoor-station app on top of the existing,
proven door-station call protocol. The protocol is fixed (it's defined by the
hardware); this document covers the app's internal structure, threading, media
pipelines, and Android-specific concerns.

## 2. Protocol recap (the contract with the door)

Two sockets on the LAN:

- **UDP 8089 — discovery.** Door sends a request containing
  `cmd_send_get_call_device` / `cmd_send_get_device_info`; the app replies (to the
  sender, port 8089) with a JSON identity record (`SCREEN_INFO`: `alias`, `serial`,
  `dstAddr`, etc.).
- **TCP 8189 — the call.** One connection carrying length-prefixed frames:

  ```
  [4-byte magic][4-byte little-endian length][payload]
  ```

  | Magic | Channel | Payload |
  |-------|---------|---------|
  | `AA AA AA AA` | Control | UTF-8 JSON command |
  | `BB BB BB BB` | Video | H.264 Annex-B |
  | `CC CC CC CC` | Audio | G.711 A-law, 160 bytes / 20 ms |

Commands: in — `Call`, `GetCallInfo`, `HangUp`; out — `Answer` (×3 variants),
`StartTalk` (opens door speaker), `OpenDoor` (unlock), `HangUp`. Full details in
the protocol reference doc.

## 3. High-level architecture

A single-activity app with a foreground service that owns the network/call
lifecycle, plus a media layer and a UI layer. Recommended: **MVVM** with Kotlin
coroutines.

```
┌──────────────────────────────────────────────────────────┐
│ UI layer (Jetpack Compose / Views)                        │
│  • IncomingCallScreen  • InCallScreen  • Idle/Status       │
└──────────────▲───────────────────────────┬───────────────┘
               │ StateFlow (call state)     │ user intents
┌──────────────┴───────────────────────────▼───────────────┐
│ CallViewModel  (maps protocol events ↔ UI)                │
└──────────────▲───────────────────────────┬───────────────┘
               │ events                     │ commands
┌──────────────┴───────────────────────────▼───────────────┐
│ IntercomService (foreground service)                      │
│  ┌───────────────┐  ┌────────────────┐  ┌──────────────┐  │
│  │ Discovery (UDP)│  │ CallSocket(TCP)│  │ Frame parser │  │
│  └───────────────┘  └────────────────┘  └──────┬───────┘  │
│        ┌──────────────────────────┬────────────┴──────┐   │
│        ▼                          ▼                    ▼   │
│  Control handler           VideoPipeline         AudioPipeline
│  (Answer/StartTalk/         (H.264 → Surface)    (A-law ⇄ mic/spk)
│   OpenDoor/HangUp)                                         │
└──────────────────────────────────────────────────────────┘
```

## 4. Components

### 4.1 IntercomService (foreground service)
Owns the whole call lifecycle so calls arrive even when the app is backgrounded or
the screen is off. Holds the discovery socket and the TCP call socket, runs the
frame parser, and exposes call events as a `StateFlow`. Posts a full-screen
incoming-call notification (high-importance channel + full-screen intent) so the
ring UI appears over the lock screen.

### 4.2 Discovery (UDP 8089)
A coroutine that binds 8089, listens for discovery requests, and replies with the
device's `SCREEN_INFO`. Each installed app instance must use a unique `dstAddr`.
Runs continuously while the service is alive.

### 4.3 CallSocket + Frame parser (TCP 8189)
Listens/accepts on 8189. The parser implements the length-prefixed framing: buffer
incoming bytes, read magic + little-endian length, wait for a whole frame, dispatch
by channel. Recover from desync by scanning to the next known magic. This is a
direct port of the reference client's `_parse`.

### 4.4 Control handler
Maps inbound commands to call state (`Call` → ringing, `HangUp` → ended) and sends
outbound commands: on answer, the three `Answer` variants followed by `StartTalk`;
on unlock, `OpenDoor`; on end, `HangUp` then socket close. Responds to
`GetCallInfo` by re-confirming the answer.

### 4.5 VideoPipeline (H.264 → screen)
Strip each `BB` frame's 8-byte envelope and feed the H.264 payload to
**MediaCodec** configured for `video/avc`, rendering directly to a `Surface`
(SurfaceView/TextureView). MediaCodec handles SPS/PPS/IDR; the decoder syncs at the
next keyframe when joining mid-stream. (This replaces the reference client's ffmpeg
subprocess — Android has no ffmpeg, and MediaCodec is hardware-accelerated.)

### 4.6 AudioPipeline (G.711 A-law two-way)
- **Downlink:** `CC` payload → A-law→PCM16 → `AudioTrack` (8 kHz mono).
- **Uplink:** `AudioRecord` (8 kHz mono) → PCM16→A-law → wrap in `CC` frame → send,
  but only after `StartTalk`.
- **Echo:** configure the audio session for voice communication
  (`MODE_IN_COMMUNICATION`, `VOICE_COMMUNICATION` source) so Android's built-in AEC
  and noise suppression run — this is the proper replacement for the reference
  client's manual gate/gain and removes the echo problem hands-free.
- G.711 A-law conversion: use a small lookup-table codec (no `audioop` on Android).

## 5. Threading & concurrency

Use Kotlin coroutines with dedicated dispatchers: one for discovery, one for the
TCP read loop, one for audio I/O. Media decode runs on MediaCodec's own threads.
UI state is exposed via `StateFlow` and collected on the main thread. Never touch
UI from socket/codec threads directly.

## 6. Call state machine

```
IDLE → (Call) → RINGING → (Answer) → CONNECTING → CONNECTED
  RINGING/CONNECTED → (HangUp / user End) → IDLE
```

Side effects: RINGING starts ring + video preview; Answer sends handshake +
StartTalk and starts audio; any terminal transition stops ring/video/audio and
closes the socket.

## 7. Android-specific concerns

- **Foreground service + full-screen intent** for calls while backgrounded/locked.
- **Permissions:** `RECORD_AUDIO`, `INTERNET`, `FOREGROUND_SERVICE`,
  `POST_NOTIFICATIONS` (Android 13+), and `FOREGROUND_SERVICE_MICROPHONE` /
  appropriate service type on newer Android.
- **Wi-Fi multicast/UDP:** acquire a `WifiManager.MulticastLock` if needed so UDP
  discovery is received reliably; keep a wake/Wi-Fi lock during calls.
- **Doze/battery:** the service must survive Doze to receive calls; document the
  battery-optimization exemption.
- **Network:** bind to the Wi-Fi network explicitly so traffic doesn't escape to
  mobile data.

## 8. Security

LAN-only, unauthenticated protocol. Mitigations: never expose ports externally; do
not relay media off-device; gate `OpenDoor` behind an active on-screen call.
Roadmap: device pairing/auth and transport encryption.

## 9. Testing strategy

- **Frame parser unit tests** with captured byte sequences (split/merged frames,
  desync recovery).
- **Codec round-trip tests** for A-law encode/decode.
- **Integration test** against the real door (ring → video → answer → audio →
  unlock → hang-up) and against a software door emulator for CI.
- **Lifecycle tests:** call while backgrounded, locked, after Doze.

## 10. Reuse from the reference client

The desktop reference's `CallServer` (framing, commands, sequence) is the
authoritative behaviour to port. The audio/video *pipelines* are replaced with
Android-native APIs (MediaCodec, AudioTrack/AudioRecord) but consume/produce the
exact same payload bytes.
