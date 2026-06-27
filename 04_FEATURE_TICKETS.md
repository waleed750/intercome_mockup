# Feature Ticket List (Backlog)
## Smart Home Video Intercom — Android App

**Status:** Draft v1
**Legend — Priority:** P0 (must, blocks release) · P1 (should) · P2 (nice-to-have)
**Legend — Type:** FEAT · INFRA · UI · TEST · CHORE

Tickets are grouped into epics. IDs are stable references for your tracker.

---

## EPIC A — Networking & discovery

**INT-A1 (P0, INFRA) — UDP discovery responder**
Bind UDP 8089; reply to `cmd_send_get_call_device`/`cmd_send_get_device_info` with the unit's `SCREEN_INFO` (to sender, port 8089).
_AC:_ door lists this app as an available indoor unit; responder runs continuously while service alive.

**INT-A2 (P0, INFRA) — TCP call socket on 8189**
Accept the door's connection; manage one active connection's lifecycle.
_AC:_ connection establishes on call; clean teardown on end; survives reconnect.

**INT-A3 (P0, INFRA) — Length-prefixed frame parser**
Parse `[magic][LE length][payload]`; dispatch AA/BB/CC; handle split/merged frames and desync recovery.
_AC:_ unit tests pass on captured byte sequences (see TST-F1).

**INT-A4 (P1, INFRA) — Unique device identity / config**
Per-install `alias`, `serial`, `dstAddr` in `SCREEN_INFO`.
_AC:_ two installs don't collide; values editable in Settings.

**INT-A5 (P1, INFRA) — Wi-Fi binding & multicast lock**
Bind sockets to the Wi-Fi network; acquire `MulticastLock` if required for UDP receipt.
_AC:_ discovery received reliably; no traffic over mobile data.

---

## EPIC B — Call control & lifecycle

**INT-B1 (P0, FEAT) — Inbound command handling**
Handle `Call` (ring), `GetCallInfo` (re-confirm), `HangUp` (end).
_AC:_ ring triggers on `Call`; remote hang-up returns to idle.

**INT-B2 (P0, FEAT) — Answer handshake**
On Answer, send the three `Answer` variants, then `StartTalk`.
_AC:_ door stops ringing and opens its speaker; audio flows both ways.

**INT-B3 (P0, FEAT) — Door unlock**
Unlock action sends `{"command":"OpenDoor"}`; only available while connected.
_AC:_ door releases; confirmation shown; not triggerable when idle.

**INT-B4 (P0, FEAT) — End / decline**
Send `HangUp` and close the socket; tear down media.
_AC:_ no stuck-ringing or stuck-call state; both sides return to idle.

**INT-B5 (P1, FEAT) — Busy handling**
Advertise/handle `deviceBusy`; reject overlapping calls gracefully.
_AC:_ a second call doesn't corrupt the active one.

---

## EPIC C — Video pipeline

**INT-C1 (P0, FEAT) — H.264 decode via MediaCodec**
Strip envelope; feed `BB` payload to MediaCodec rendering to a Surface.
_AC:_ live video renders; syncs at next keyframe when joining mid-stream.

**INT-C2 (P0, FEAT) — Video preview during ring**
Start decoding/rendering on `Call`, before Answer.
_AC:_ visitor visible while the phone is still ringing.

**INT-C3 (P1, FEAT) — Video resiliency**
Recover from decoder errors; show "Video unavailable" while keeping audio.
_AC:_ a bad frame doesn't crash or permanently blank the call.

---

## EPIC D — Audio pipeline

**INT-D1 (P0, FEAT) — Downlink A-law playback**
`CC` → A-law→PCM16 → AudioTrack @ 8 kHz mono.
_AC:_ visitor audio is clear.

**INT-D2 (P0, FEAT) — Uplink A-law capture**
AudioRecord @ 8 kHz → PCM16→A-law → `CC` frame → send (after StartTalk).
_AC:_ door plays the user's voice clearly.

**INT-D3 (P0, FEAT) — Voice-comm mode + echo cancellation**
Configure session for `MODE_IN_COMMUNICATION` / `VOICE_COMMUNICATION` to enable platform AEC/NS.
_AC:_ hands-free with no significant echo.

**INT-D4 (P1, FEAT) — Mute**
Toggle uplink silence without dropping the call.
_AC:_ muted = door hears nothing; user still hears visitor; clear UI state.

**INT-D5 (P1, CHORE) — A-law codec utility**
Table-based A-law⇄PCM16 (no `audioop`).
_AC:_ round-trip unit tests pass.

---

## EPIC E — Android platform integration

**INT-E1 (P0, INFRA) — Foreground service**
Own the call lifecycle so calls arrive when backgrounded.
_AC:_ ring works with app in background.

**INT-E2 (P0, UI) — Full-screen incoming call over lock screen**
High-importance notification channel + full-screen intent.
_AC:_ incoming-call UI shows over the lock screen and rings.

**INT-E3 (P0, CHORE) — Permissions & runtime requests**
`RECORD_AUDIO`, `POST_NOTIFICATIONS`, foreground-service types, etc.
_AC:_ permissions requested with clear rationale; graceful denial handling.

**INT-E4 (P1, CHORE) — Doze / battery exemption**
Survive Doze to receive calls; guide user to battery-optimization exemption.
_AC:_ call received after device idle.

**INT-E5 (P1, FEAT) — Ringtone & vibration**
Loop ringtone + vibrate on incoming; stop on answer/decline.
_AC:_ audible/haptic ring; stops instantly on action.

---

## EPIC F — UI screens

**INT-F1 (P0, UI) — Incoming Call screen** — live preview, Answer/Decline, caller label. _AC:_ matches frontend spec §3.2.
**INT-F2 (P0, UI) — In-Call screen** — video, Unlock, Mute, End, mic meter. _AC:_ matches §3.3.
**INT-F3 (P1, UI) — Idle/Home screen** — connection status, device info, Settings entry. _AC:_ matches §3.1.
**INT-F4 (P1, UI) — Settings screen** — name/address, permissions status, about. _AC:_ matches §3.4.
**INT-F5 (P1, UI) — Error/empty states** — wrong Wi-Fi, mic denied, notifications off. _AC:_ matches §7.

---

## EPIC G — Quality & testing

**TST-F1 (P0, TEST) — Frame parser unit tests** — split/merged/desync byte streams.
**TST-F2 (P0, TEST) — A-law codec round-trip tests.**
**TST-F3 (P1, TEST) — Software door emulator** for CI (sends Call/video/audio, accepts commands).
**TST-F4 (P1, TEST) — Lifecycle tests** — background, locked, post-Doze call receipt.
**TST-F5 (P1, TEST) — End-to-end against real hardware** — ring→video→answer→audio→unlock→hangup.

---

## EPIC H — Security (foundation now, hardening later)

**SEC-1 (P0, CHORE) — Lock down exposure** — never bind ports to public interfaces; no off-device media relay.
**SEC-2 (P1, FEAT) — Gate unlock to active call** — `OpenDoor` only from a connected, on-screen call.
**SEC-3 (P2, FEAT, roadmap) — Device pairing & auth.**
**SEC-4 (P2, FEAT, roadmap) — Transport encryption.**

---

## Suggested release slices

- **MVP (P0 only):** A1–A3, B1–B4, C1–C2, D1–D3, E1–E3, F1–F2, TST-F1/F2, SEC-1 → a phone that rings, shows the visitor, talks both ways, and unlocks.
- **v1.0 (add P1):** identity/config, busy handling, mute, ringtone, idle/settings UI, Doze handling, emulator + lifecycle tests.
- **v1.x (P2 / roadmap):** pairing, auth, encryption, remote access, recording, iOS.
