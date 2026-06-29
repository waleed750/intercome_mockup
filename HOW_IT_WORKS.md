# How the Intercom App Works

A deep-dive into the architecture, protocol, and code of this video intercom system.

---

## Table of Contents

1. [The Big Picture](#1-the-big-picture)
2. [Phase 1: Discovery (UDP)](#2-phase-1-discovery-udp)
3. [Phase 2: Call Connection (TCP)](#3-phase-2-call-connection-tcp)
4. [Phase 3: The Frame Protocol (0xAA, 0xBB, 0xCC)](#4-phase-3-the-frame-protocol-0xaa-0xbb-0xcc)
5. [Phase 4: The Frame Parser](#5-phase-4-the-frame-parser)
6. [Phase 5: Control Commands](#6-phase-5-control-commands)
7. [Phase 6: Call Lifecycle & State Machine](#7-phase-6-call-lifecycle--state-machine)
8. [Phase 7: Audio — G.711 A-Law](#8-phase-7-audio--g711-a-law)
9. [Phase 8: Video — H.264](#9-phase-8-video--h264)
10. [Phase 9: Networking — Wi-Fi & Ethernet](#10-phase-9-networking--wi-fi--ethernet)
11. [Phase 10: Background Service](#11-phase-10-background-service)
12. [Full Call Flow (End to End)](#12-full-call-flow-end-to-end)
13. [File Map](#13-file-map)

---

## 1. The Big Picture

This app turns an Android phone or tablet into a **video intercom indoor station**. It pairs with a **Tuya-based IP door station** (a device with a camera, speaker, microphone, and door-latch relay) over the **local network** (Wi-Fi or Ethernet).

```
                        Local Network (Wi-Fi / Ethernet)
                                    |
    ┌─────────────────┐             |            ┌──────────────────┐
    │  Door Station    │   UDP 8089 (discovery)   │  This Android App│
    │  (Tuya IP cam)   │ ─────────────────────── >│  (Indoor Panel)  │
    │                  │                          │                  │
    │  - Camera        │   TCP 8189 (call data)   │  - Video display │
    │  - Microphone    │ ─────────────────────── >│  - Speaker       │
    │  - Speaker       │   Control + Video + Audio│  - Microphone    │
    │  - Door relay    │                          │  - Unlock button │
    └─────────────────┘                          └──────────────────┘
```

**What happens when someone rings the doorbell:**

1. The door discovers the app on the network via UDP
2. The door connects to the app via TCP
3. The door sends a "Call" command — the app starts ringing
4. The user taps "Answer" — two-way audio begins, live video plays
5. The user can talk, see the visitor, mute their mic, or unlock the door
6. Either side hangs up — everything tears down, back to idle

---

## 2. Phase 1: Discovery (UDP)

**Purpose:** The door needs to find the app on the network. It doesn't know the app's IP address in advance.

**How it works:**

1. The door periodically **broadcasts** a UDP packet on port `8089`
2. The packet contains a JSON probe like:
   ```json
   {
     "command": "cmd_send_get_device_info",
     "localAddr": "door-001",
     "localType": 3
   }
   ```
3. The app listens on UDP port 8089 via `DiscoveryResponder`
4. When it receives a probe, it checks if the payload contains known keywords:
   - `"cmd_send_get_call_device"`
   - `"cmd_send_get_device_info"`
5. If matched, the app replies with a `ScreenInfo` JSON:
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
6. The door learns the app's IP from the **UDP packet's source address** — no IP field needed in the JSON
7. The door now knows: "There's an indoor station at this IP, named 'Indoor 4521'"

**Key files:**
- `protocol/Discovery.kt` — Parses probes, builds replies
- `protocol/ScreenInfo` (in Discovery.kt) — The reply data class
- `net/DiscoveryResponder.kt` — The UDP listener loop

**Why substring matching?** Different door firmware versions send slightly different probe formats. Rather than parsing exact JSON schemas, we just check if the payload *contains* the known command strings. This makes the app compatible with multiple firmware versions.

---

## 3. Phase 2: Call Connection (TCP)

**Purpose:** When a visitor presses the door's call button, the door needs a reliable, ordered channel to send video, audio, and control commands.

**How it works:**

1. The app runs a **TCP server** on port `8189` via `CallServer`
2. The door is the **TCP client** — it connects to the app when a visitor calls
3. The `CallServer` accepts the socket and hands it to `CallController.onSocketAccepted()`
4. A `CallConnection` is created with two coroutines:
   - **Read loop:** Reads raw bytes from the socket → feeds them to `FrameParser`
   - **Write loop:** Drains an outbox queue → writes frames to the socket

```
Door (TCP client) ──connects──> App (TCP server on port 8189)
                                  │
                                  ├── Read coroutine:  socket → FrameParser → onFrame()
                                  └── Write coroutine: outbox queue → socket
```

**Why the app is the server (not the client)?** The door discovers the app's IP via UDP, then connects to it. This means the app just needs to listen — it doesn't need to know the door's IP. Multiple doors can connect to the same app.

**Key files:**
- `net/CallSocket.kt` — Contains both `CallConnection` (one active call) and `CallServer` (TCP listener)

---

## 4. Phase 3: The Frame Protocol (0xAA, 0xBB, 0xCC)

**Purpose:** Video, audio, and control commands all flow over the **same TCP connection**. We need a way to tell them apart. This is called **multiplexing**.

**The frame format:**

Every piece of data (a JSON command, a video frame, an audio chunk) is wrapped in a frame:

```
┌──────────────────────┬──────────────────────┬──────────────────────┐
│   Magic (4 bytes)    │  Length (4 bytes)     │  Payload (N bytes)   │
│   marker × 4         │  little-endian uint32 │  raw data            │
└──────────────────────┴──────────────────────┴──────────────────────┘
```

**The three channels:**

| Marker Byte | Magic on Wire     | Channel     | Content                          |
|-------------|-------------------|-------------|----------------------------------|
| `0xAA`      | `AA AA AA AA`     | **CONTROL** | JSON command strings             |
| `0xBB`      | `BB BB BB BB`     | **VIDEO**   | H.264 video NAL units           |
| `0xCC`      | `CC CC CC CC`     | **AUDIO**   | G.711 A-law encoded audio        |

**Concrete example — a "Call" command:**

The door sends `{"command":"Call"}` (16 bytes of JSON). On the wire it looks like:

```
AA AA AA AA        ← Magic: this is a CONTROL frame
10 00 00 00        ← Length: 16 in little-endian (0x10 = 16)
7B 22 63 6F 6D 6D  ← Payload: {"comm
61 6E 64 22 3A 22  ←          and":"
43 61 6C 6C 22 7D  ←          Call"}
```

**Why repeat the marker 4 times?** It's a **sync marker**. If the byte stream gets misaligned (a dropped byte, a corrupt length field), the parser can scan byte-by-byte until it finds 4 identical known bytes (`AA AA AA AA`, `BB BB BB BB`, or `CC CC CC CC`), then re-lock onto the frame boundary. This is the **desync recovery** mechanism.

**Why 0xAA, 0xBB, 0xCC specifically?** These are arbitrary but well-chosen:
- They're easy to spot in hex dumps when debugging
- They're distinct from each other and from common data patterns
- 0xAA = `10101010` in binary — an alternating bit pattern, unlikely to appear naturally in compressed video/audio

**Key file:** `protocol/Frame.kt`

---

## 5. Phase 4: The Frame Parser

**Purpose:** TCP is a **stream** protocol — it delivers bytes, not messages. A single `read()` call might return half a frame, two frames merged together, or anything in between. The `FrameParser` reassembles complete frames from this byte stream.

**The three hazards it handles:**

### 1. Split Frames
A large video frame might arrive across multiple TCP reads:
```
Read 1: [AA AA AA AA 00 10 00 00 ...first 50 bytes...]
Read 2: [...remaining 4046 bytes of the video frame...]
```
The parser **buffers** partial data until the full frame arrives.

### 2. Merged Frames
Multiple small frames arrive in one read:
```
Read 1: [AA AA AA AA 10 00 00 00 ...control... CC CC CC CC A0 00 00 00 ...audio...]
```
The parser extracts each frame in a loop.

### 3. Desync
The read position lands on garbage (corrupted data, bad length field):
```
[XX XX AA AA AA AA 10 00 00 00 ...]
 ↑ garbage
```
The parser **skips one byte at a time** until it finds 4 matching known marker bytes, then re-locks.

**How `drain()` works step by step:**

```
1. Do we have at least 4 bytes? No → wait for more data
2. Are bytes [0..3] a valid magic (AA×4, BB×4, or CC×4)? No → skip 1 byte, goto 1
3. Do we have at least 8 bytes (magic + length)? No → wait for more data
4. Read the 4-byte little-endian length. Is it plausible (> 0, < 4MB)? No → skip 1 byte, goto 1
5. Do we have magic(4) + length(4) + payload(N) bytes? No → wait for more data
6. Extract the payload, emit it via onFrame(channel, payload)
7. Advance past this frame, goto 1
```

**Key file:** `protocol/FrameParser.kt`

---

## 6. Phase 5: Control Commands

**Purpose:** JSON messages on the CONTROL channel (`0xAA`) drive the call lifecycle — start ringing, answer, hang up, unlock.

### Inbound Commands (Door → App)

| Command        | JSON                         | What It Means                        |
|----------------|------------------------------|--------------------------------------|
| **Call**        | `{"command":"Call"}`         | Visitor pressed the doorbell button  |
| **GetCallInfo** | `{"command":"GetCallInfo"}` | Door re-querying — re-confirm answer |
| **HangUp**      | `{"command":"HangUp"}`      | Door ended the call                  |

The `command` field may also appear as `cmd` in some firmware. Classification is case-insensitive:
```kotlin
fun classify(): InboundCommand = when (name?.trim()?.lowercase()) {
    "call"        -> InboundCommand.CALL
    "getcallinfo" -> InboundCommand.GET_CALL_INFO
    "hangup"      -> InboundCommand.HANG_UP
    else          -> InboundCommand.UNKNOWN
}
```

### Outbound Commands (App → Door)

| Command          | JSON                                              | When Sent                          |
|------------------|---------------------------------------------------|------------------------------------|
| **Answer** (×3)  | See handshake below                               | User taps Answer                   |
| **HangUp**       | `{"command":"HangUp","OtherAnswer":0}`            | User taps End/Decline              |
| **OpenDoor**     | `{"command":"OpenDoor"}`                          | User taps Unlock (connected only)  |
| **DeviceBusy**   | `{"command":"deviceBusy"}`                        | Reject a second incoming call      |

### The Answer Handshake

When the user answers, the app sends **three** commands in rapid sequence:
```json
{"command":"Answer"}
{"command":"Answer","OtherAnswer":1}
{"command":"Answer","OtherAnswer":true}
```

This is reverse-engineered from the Tuya reference client. The door expects all three variants (with different types for `OtherAnswer`: absent, integer `1`, and boolean `true`) to confirm the call is accepted. The reference client spaces them ~80ms apart; this app sends them back-to-back on the ordered write loop.

**Key file:** `protocol/Commands.kt`

---

## 7. Phase 6: Call Lifecycle & State Machine

**Purpose:** The `CallController` is the **single source of truth** for the call. It coordinates discovery, networking, media, and UI state.

### State Machine

```
    ┌──────────────────────────────────────────┐
    │                                          │
    ▼                                          │
  IDLE ──(door sends "Call")──> RINGING        │
    ▲                              │           │
    │                    (user answers)         │
    │                              │           │
    │                              ▼           │
    │                         CONNECTING       │
    │                              │           │
    │                    (handshake done)       │
    │                              │           │
    │                              ▼           │
    │                          CONNECTED       │
    │                              │           │
    │         (HangUp / Decline / connection   │
    │          closed / remote HangUp)         │
    │                              │           │
    └──────────────────────────────┘
```

### What Each Transition Does

**IDLE → RINGING** (door sends `{"command":"Call"}`):
- Set phase to RINGING
- Update UI state (caller label, reset mute/video flags)
- Start the video decoder (so video shows even before answering)

**RINGING → CONNECTING → CONNECTED** (user taps Answer):
- Send the 3-part answer handshake
- Start the audio pipeline (mic capture + speaker playback)
- Transition through CONNECTING to CONNECTED

**Any → IDLE** (call ends):
- Stop audio pipeline
- Stop video decoder
- Close the TCP connection
- Reset all UI state flags
- Show "Call ended" transient message (auto-clears after 2.5s)

### Thread Safety

The `CallController` uses `synchronized(stateLock)` for all state-changing operations. This prevents races between:
- The **socket read thread** (receiving "Call", "HangUp" from the door)
- The **main/UI thread** (user tapping Answer, End, Unlock)

The hot path (video frame submission at ~30fps) reads the volatile `phase` field without locking to avoid bottlenecking.

**Key files:**
- `call/CallController.kt` — The brain
- `call/CallState.kt` — `CallPhase` enum and `CallUiState` data class

---

## 8. Phase 7: Audio — G.711 A-Law

**Purpose:** Two-way voice between the visitor and the person inside.

### What is G.711 A-Law?

G.711 is the **original digital telephone codec** standardized by ITU-T. It compresses 16-bit PCM audio down to 8 bits using **logarithmic companding** (compressing + expanding):

- Quiet sounds get **more precision** (more bits per amplitude step)
- Loud sounds get **less precision** (fewer bits, but humans can't tell)
- This matches how human hearing works — we're more sensitive to quiet sounds

**A-Law** is the European variant (μ-Law is the North American variant). The Tuya door uses A-Law.

### Audio Parameters

| Parameter    | Value                        |
|--------------|------------------------------|
| Codec        | G.711 A-Law (ITU-T)         |
| Sample rate  | 8,000 Hz (telephone quality) |
| Channels     | Mono (1 channel)             |
| Frame size   | 160 A-Law bytes = 20ms       |
| PCM size     | 320 bytes (160 samples × 2 bytes each) |

### Audio Flow

**Downlink (door → app — you hear the visitor):**
```
Door mic → A-Law encode → [CC CC CC CC][len][160 bytes] → TCP →
→ FrameParser → AudioPipeline.playDownlink() → ALawCodec.decode() → PCM → Speaker
```

**Uplink (app → door — visitor hears you):**
```
Mic → PCM capture → ALawCodec.encode() → Frame.encode(AUDIO, alaw) →
→ CallConnection.enqueue() → TCP → Door speaker
```

### How A-Law Encoding Works (Simplified)

```
Input:  16-bit signed PCM sample (-32768 to +32767)
Output: 8-bit A-Law code (0 to 255)

1. Take the absolute value, note the sign
2. Shift right by 3 (divide by 8) — lose the 3 least significant bits
3. Find which "segment" the value falls into (0-7), like a logarithmic scale
4. Take 4 bits of mantissa from within that segment
5. Combine: [sign bit][3 segment bits][4 mantissa bits] = 8 bits
6. XOR with 0x55 (alternating bits) for better transmission properties
```

The decode is the reverse, done via a precomputed 256-entry lookup table for speed.

**Key property:** `encode(decode(b)) == b` for all 256 possible byte values. This is verified by the unit test.

### Native Audio Features (Android)

The `AudioPipeline` uses Android's native audio APIs with:
- **AEC** (Acoustic Echo Cancellation) — prevents feedback loop
- **NS** (Noise Suppression) — filters background noise
- **AGC** (Automatic Gain Control) — normalizes volume
- **Speakerphone mode** — routes audio to the loudspeaker
- **Audio focus** — pauses other apps' audio during a call

**Key files:**
- `protocol/ALawCodec.kt` — Pure Kotlin A-Law encoder/decoder
- `media/AudioPipeline.kt` — Mic capture, speaker playback, native audio features

---

## 9. Phase 8: Video — H.264

**Purpose:** Live video from the door's camera displayed on the app.

### How It Works

1. The door captures video from its camera and encodes it as **H.264** (Annex-B format)
2. Each encoded piece (called a **NAL unit** — Network Abstraction Layer unit) is sent as a VIDEO frame (`0xBB`)
3. The app's `VideoPipeline` feeds these NAL units to Android's **MediaCodec** hardware decoder
4. MediaCodec decodes the compressed data and renders it directly to a `Surface` (SurfaceView in the UI)

### Video Parameters

| Parameter   | Value               |
|-------------|---------------------|
| Codec       | H.264 (AVC)        |
| Format      | Annex-B (start codes) |
| Resolution  | 1920×1080 (default) |
| Frame rate  | ~30 fps             |
| Rendering   | MediaCodec → Surface |

### What is Annex-B?

H.264 has two packaging formats:
- **AVCC** (used in MP4 files) — length-prefixed NAL units
- **Annex-B** (used in streaming) — NAL units separated by start codes (`00 00 00 01` or `00 00 01`)

The door uses Annex-B because it's natural for streaming — each NAL unit is self-delimiting.

### Video Flow

```
Door camera → H.264 encode → [BB BB BB BB][len][NAL unit] → TCP →
→ FrameParser → onFrame(VIDEO, payload) → VideoPipeline.submit() →
→ MediaCodec (hardware decoder) → Surface → Screen
```

**Key file:** `media/VideoPipeline.kt`

---

## 10. Phase 9: Networking — Wi-Fi & Ethernet

**Purpose:** Keep intercom traffic on the local network, support both Wi-Fi devices and Ethernet-connected panels.

### The Problem

The app might be running on:
- A **phone/tablet** connected via Wi-Fi
- An **embedded panel** (like PX30 Rockchip) connected via Ethernet
- A device with **both** Wi-Fi and mobile data (must not leak to cellular)

### How NetworkBinder Solves It

```kotlin
// Prefers Wi-Fi, falls back to Ethernet
fun lanNetwork(): Network? =
    findNetwork(TRANSPORT_WIFI) ?: findNetwork(TRANSPORT_ETHERNET)
```

**On Wi-Fi:**
- Binds sockets to the Wi-Fi network interface (prevents traffic on cellular)
- Acquires a **MulticastLock** (Android blocks multicast by default to save battery — we need it for UDP discovery)
- Acquires a **WifiLock** (keeps Wi-Fi radio awake during calls)

**On Ethernet:**
- Binds sockets to the Ethernet interface
- Skips Wi-Fi locks (unnecessary on wired connections — no multicast blocking, no radio sleep)

**Key file:** `net/NetworkBinder.kt`

---

## 11. Phase 10: Background Service

**Purpose:** The app must receive calls even when it's in the background or the screen is off.

### Why a Foreground Service?

Android aggressively kills background apps to save battery. A **foreground service** tells Android: "This app is doing important work — don't kill it." It must show a persistent notification.

### How IntercomService Works

```
App launches → IntercomService.start()
  → onCreate():
      1. Create notification channels
      2. Show "Listening for calls" foreground notification
      3. Start CallController (discovery + TCP server)
      4. Begin observing call state

Call comes in → CallController emits RINGING state
  → IntercomService.onState():
      1. Start ringtone + vibration (Ringer)
      2. Post full-screen incoming call notification
      3. Notification has Answer/Decline buttons

User taps Answer (from notification or UI)
  → IntercomService.onStartCommand(ACTION_ANSWER)
      → controller.answer()

Call ends
  → Stop ringer, cancel notification, update foreground notification
```

### Notification Actions

The incoming call notification has two buttons that work even when the app isn't in the foreground:
- **Answer** → Sends `ACTION_ANSWER` intent to the service
- **Decline** → Sends `ACTION_DECLINE` intent to the service

On Android 10+, the notification uses a **full-screen intent** that wakes the screen and shows the call UI.

**Key files:**
- `service/IntercomService.kt` — The foreground service
- `service/CallNotifications.kt` — Notification builders
- `service/Ringer.kt` — Ringtone + vibration
- `service/BootReceiver.kt` — Auto-starts the service on device boot

---

## 12. Full Call Flow (End to End)

Here's everything that happens during a complete call, with the actual bytes on the wire:

```
1. DISCOVERY
   Door → broadcasts UDP to port 8089:
     {"command":"cmd_send_get_device_info","localAddr":"door-001","localType":3}

   App → replies UDP:
     {"command":"cmd_reply_get_device_info","appid":"7551000","alias":"Indoor 4521",...}

2. CALL INITIATION
   Door → connects TCP to app:8189
   Door → sends frame:
     [AA AA AA AA] [10 00 00 00] [{"command":"Call"}]

   App: IDLE → RINGING
     - Starts video decoder
     - Starts ringtone + vibration
     - Shows incoming call notification

3. VIDEO STREAMING (starts immediately, even before answer)
   Door → sends frames continuously:
     [BB BB BB BB] [len] [H.264 NAL unit]
     [BB BB BB BB] [len] [H.264 NAL unit]
     ... (~30 per second)

   App: Decodes and displays live video

4. USER ANSWERS
   App → sends 3 answer frames:
     [AA AA AA AA] [len] [{"command":"Answer"}]
     [AA AA AA AA] [len] [{"command":"Answer","OtherAnswer":1}]
     [AA AA AA AA] [len] [{"command":"Answer","OtherAnswer":true}]

   App: RINGING → CONNECTING → CONNECTED
     - Stops ringtone
     - Starts audio pipeline (mic + speaker)

5. TWO-WAY AUDIO
   Door → app (downlink):
     [CC CC CC CC] [A0 00 00 00] [160 bytes of A-law audio]  (every 20ms)

   App → door (uplink):
     [CC CC CC CC] [A0 00 00 00] [160 bytes of A-law audio]  (every 20ms)

6. USER UNLOCKS DOOR
   App → sends:
     [AA AA AA AA] [len] [{"command":"OpenDoor"}]

   Door: Activates relay, latch opens
   App: Shows "Door unlocked" for 2.5 seconds

7. CALL ENDS
   App → sends:
     [AA AA AA AA] [len] [{"command":"HangUp","OtherAnswer":0}]

   App: CONNECTED → IDLE
     - Stops audio pipeline
     - Stops video decoder
     - Closes TCP connection
     - Shows "Call ended" for 2.5 seconds
```

---

## 13. File Map

### Protocol Layer (`protocol/`)
| File | Purpose |
|------|---------|
| `Frame.kt` | Frame format: Channel enum (0xAA/0xBB/0xCC) + Frame.encode() |
| `FrameParser.kt` | Streaming parser: bytes → complete frames, with desync recovery |
| `Commands.kt` | Control commands: parse inbound JSON, build outbound JSON |
| `Discovery.kt` | UDP discovery: parse probes, build ScreenInfo replies |
| `ALawCodec.kt` | G.711 A-Law audio codec: PCM ↔ A-Law |

### Network Layer (`net/`)
| File | Purpose |
|------|---------|
| `CallSocket.kt` | TCP server (port 8189) + CallConnection (read/write loops) |
| `DiscoveryResponder.kt` | UDP listener (port 8089) + reply sender |
| `NetworkBinder.kt` | LAN network resolution (Wi-Fi/Ethernet), socket binding, locks |

### Call Layer (`call/`)
| File | Purpose |
|------|---------|
| `CallController.kt` | Single source of truth: state machine, media coordination |
| `CallState.kt` | CallPhase enum + CallUiState data class |

### Media Layer (`media/`)
| File | Purpose |
|------|---------|
| `AudioPipeline.kt` | Two-way audio: mic capture, speaker playback, AEC/NS/AGC |
| `VideoPipeline.kt` | H.264 decode via MediaCodec → Surface rendering |

### Service Layer (`service/`)
| File | Purpose |
|------|---------|
| `IntercomService.kt` | Foreground service: keeps everything alive in background |
| `CallNotifications.kt` | Notification builders (foreground + incoming call) |
| `Ringer.kt` | Ringtone + vibration during ringing |
| `BootReceiver.kt` | Auto-start on device boot |

### UI Layer (`ui/`)
| File | Purpose |
|------|---------|
| `MainActivity.kt` | Single activity, hosts Compose UI |
| `CallViewModel.kt` | Bridge between Compose and CallController |
| `screens/IdleScreen.kt` | Home screen: status, device info, settings |
| `screens/IncomingCallScreen.kt` | Ringing screen: video preview + Answer/Decline |
| `screens/InCallScreen.kt` | Connected call: video + mute/unlock/end buttons |
| `screens/SettingsScreen.kt` | Device identity editor + permission management |

### Config (`config/`)
| File | Purpose |
|------|---------|
| `DeviceConfig.kt` | Persisted device identity (alias, serial, address, door name) |
