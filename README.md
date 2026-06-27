# Smart Intercom (Android)

A native Android indoor video-intercom station. It pairs with a Tuya-based IP door
station on the **local Wi-Fi network** (no cloud): the door discovers the app over
UDP, opens a TCP call channel when a visitor presses the button, and the app rings
full-screen, shows live video, carries two-way audio, and releases the door latch.

Built to the four specs in this folder:
[`01_PRD.md`](01_PRD.md), [`02_TECHNICAL_ARCHITECTURE.md`](02_TECHNICAL_ARCHITECTURE.md),
[`03_FRONTEND_SPEC.md`](03_FRONTEND_SPEC.md), [`04_FEATURE_TICKETS.md`](04_FEATURE_TICKETS.md).

---

## ⚠️ This project cannot be compiled in the environment it was written in

The machine used to author this code has only **Java 8** and no Android SDK, Gradle,
or Kotlin compiler. The deliverable is a **complete, correct Android Studio project**
intended to be built on a normal development machine. It has **not** been compiled or
run here — treat the first build as the verification step.

### Build prerequisites
- **Android Studio** Koala (2024.1.1) or newer — AGP is `8.5.2`.
- **JDK 17** (Android Studio bundles one; `compileOptions`/`kotlinOptions` target 17).
- **Android SDK Platform 34** installed via the SDK Manager.
- A device or emulator on **Android 9 (API 28)** or newer (`minSdk = 28`).

### Build & run
```bash
# From this directory, after opening it in Android Studio and letting Gradle sync:
./gradlew assembleDebug        # build the APK
./gradlew installDebug         # install on a connected device
./gradlew test                 # run the JVM unit tests (no device needed)
```
Or just open the folder in Android Studio, let it sync, and press Run.

> The phone and the door station **must be on the same Wi-Fi subnet**. Sockets are
> pinned to the Wi-Fi interface (never cellular) per SEC-1.

---

## Architecture (where things live)

| Layer | Package | Notes |
|---|---|---|
| Protocol | `protocol/` | Frame codec, streaming `FrameParser`, control `Commands`, `ALawCodec`, UDP `Discovery` |
| Networking | `net/` | `NetworkBinder` (Wi-Fi pinning + locks), `DiscoveryResponder` (UDP 8089), `CallServer`/`CallConnection` (TCP 8189) |
| Media | `media/` | `VideoPipeline` (MediaCodec → Surface), `AudioPipeline` (AudioRecord/AudioTrack, G.711) |
| Call control | `call/` | `CallController` (single source of truth), `CallUiState`/`CallPhase` |
| Config | `config/` | `DeviceConfig` (DataStore-backed identity) |
| Service | `service/` | `IntercomService` (foreground), `CallNotifications` (full-screen intent), `Ringer` |
| UI | `ui/` | `MainActivity` + `IntercomApp`, Compose screens, theme |

**Wire protocol.** One TCP connection multiplexes three channels via an
`[4-byte magic][4-byte little-endian length][payload]` envelope: `AA`=control (JSON),
`BB`=H.264 Annex-B video, `CC`=G.711 A-law audio (160 bytes / 20 ms). **The app is the
TCP server** (`CallServer` accepts on 8189); the door connects as the client.

**State sharing.** `CallController` is a process-wide singleton in `AppContainer`
(see `IntercomApplication`). The foreground service drives the call; the UI observes
the same `StateFlow<CallUiState>` — so they can never disagree about call state without
binding to the service.

---

## 🔌 Seams to confirm against the real device

These are the points where the public specs don't pin down exact bytes. They are
isolated so you can edit one place after capturing traffic from the reference client.

1. **Answer handshake — `protocol/Commands.kt` → `ANSWER_SEQUENCE`.**
   The architecture doc says "the three `Answer` variants followed by `StartTalk`",
   but the exact JSON of the three variants lives in the door firmware/protocol
   reference. Placeholder payloads are in `ANSWER_SEQUENCE`; replace them with the
   captured handshake. Everything downstream forwards them verbatim.

2. **Discovery reply shape — `protocol/Discovery.kt` → `ScreenInfo` / `buildReply`.**
   We reply to the door's probe with `{"SCREEN_INFO": {...}}`. If a firmware capture
   shows additional required fields, add them to `ScreenInfo` (the only place the reply
   is built).

3. **Discovery probe tokens — `protocol/Discovery.kt` → `REQUEST_TOKENS`.**
   We recognise a probe by substring match. Add tokens if your door's probe differs.

`OpenDoor` (`{"command":"OpenDoor"}`) is confirmed working and gated to a connected
call (SEC-2).

---

## Permissions & device setup

The app requests these at runtime; the **Settings screen** shows live status with
one-tap fixes for each:

- **Microphone** (`RECORD_AUDIO`) — two-way talk. If denied, the call still connects
  with video + incoming audio and shows an in-call "enable mic" banner.
- **Notifications** (`POST_NOTIFICATIONS`, API 33+) — required for the full-screen
  incoming-call UI to ring over the lock screen.
- **Battery optimization exemption** — strongly recommended. Under Doze, a backgrounded
  app can miss incoming calls; the Settings row opens the system exemption dialog.

The activity is `showWhenLocked` + `turnScreenOn`, and the service posts a
high-importance full-screen-intent notification, so a call wakes the screen and is
answerable without unlocking.

---

## Tests

JVM unit tests (run with `./gradlew test`, no device required):

- `FrameParserTest` (TST-F1) — split, merged, and desynced byte streams; resync past
  garbage and implausible lengths; buffer growth.
- `ALawCodecTest` (TST-F2) — the `encode(decode(b)) == b` round-trip for all 256 codes,
  buffer-level round-trip, little-endian PCM layout, and full-scale clamping.

The protocol and codec are pure Kotlin specifically so the trickiest logic is testable
without an emulator.

---

## Intentional deferrals

- **Start-on-boot is not implemented.** Open the app once after install/reboot to start
  the listening service; `START_STICKY` then keeps it alive and restarts it if killed.
  A `BOOT_COMPLETED` receiver is the natural follow-up.
- **No mic-level meter.** The in-call UI shows clear Live/Muted state instead; a live
  level meter is a future enhancement.
- **Adaptive launcher icon only** (`mipmap-anydpi-v26`). Sufficient for `minSdk 28`
  (adaptive icons exist from API 26); no legacy PNG densities are shipped.
# intercome_mockup
