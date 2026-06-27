# Product Requirements Document (PRD)
## Smart Home Video Intercom — Android App

**Status:** Draft v1
**Platform:** Native Android (phone & tablet)
**Owner:** _your name_
**Last updated:** _date_

---

## 1. Summary

A native Android app that turns a phone or tablet into an **indoor video door
station**. When a visitor presses the call button on the IP door station, the
app rings, shows live video of the visitor, and lets the user **answer with
two-way voice**, **unlock the door**, and **end the call** — all over the local
network, with no cloud dependency.

The protocol and behaviour are already proven by a working desktop reference
client. This PRD defines the Android product built on that same protocol.

## 2. Goals & non-goals

**Goals**
- Reliable incoming-call ringing and full-screen answer experience on Android.
- Live door video shown within ~1–2 seconds, including during ringing.
- Clear two-way audio (hands-free, with echo handled by the platform).
- One-tap door unlock.
- Works entirely on the local Wi-Fi network.

**Non-goals (v1)**
- Remote/over-the-internet access (cloud relay, push when off-Wi-Fi).
- Multi-tenant building management.
- Recording / clip storage.
- iOS app (separate future effort).
- Sending the phone's camera to the door (video is one-way, door → phone).

## 3. Target users & context

A resident at home on the same Wi-Fi as their door station. They want to see and
speak to whoever is at the door and let them in, from their phone, the way the
wall-mounted indoor panel works today.

## 4. User stories

- As a resident, when someone calls from the door, **my phone rings and shows who's there** so I can decide whether to answer.
- As a resident, I can **answer and talk** to the visitor hands-free.
- As a resident, I can **unlock the door** with one tap while on the call.
- As a resident, I can **decline or hang up**.
- As a resident, I can **mute my mic** while still hearing the visitor.
- As a resident, I can see video **while it's still ringing**, before I answer.

## 5. Functional requirements

| ID | Requirement | Priority |
|----|-------------|----------|
| FR-1 | App announces itself to the door via UDP discovery so the door can call it. | Must |
| FR-2 | App receives an incoming call (door `Call` command) and rings audibly. | Must |
| FR-3 | App shows live H.264 door video, starting during ring. | Must |
| FR-4 | Answer sends the accept handshake and opens two-way audio. | Must |
| FR-5 | Audio is G.711 A-law @ 8 kHz both directions; platform echo cancellation enabled. | Must |
| FR-6 | One-tap **Unlock** sends `{"command":"OpenDoor"}`. | Must |
| FR-7 | **Mute** silences the mic without dropping the call. | Should |
| FR-8 | **End/Decline** sends hang-up and closes the call. | Must |
| FR-9 | Full-screen incoming-call UI even when the app is backgrounded or screen is locked. | Must |
| FR-10 | App keeps answering discovery while idle so the door always sees it. | Must |
| FR-11 | Graceful handling of a second/overlapping call (busy). | Should |

## 6. Non-functional requirements

- **Latency:** video visible ≤ 2 s after ring; audio round-trip suitable for conversation.
- **Reliability:** call connects on first answer; no stuck-ringing state.
- **Resource use:** must run while backgrounded to receive calls (foreground service).
- **Network:** local LAN only; no ports exposed to the internet.
- **Security:** see §8.
- **Compatibility:** Android 9+ (API 28+) as a baseline target.

## 7. Key user flows

**Incoming call:** door `Call` → app shows full-screen incoming UI + ringtone + live video preview → user taps **Answer** (handshake + StartTalk + audio) or **Decline** (hang-up).

**During call:** user talks hands-free; can tap **Unlock** (OpenDoor), **Mute**, or **End**.

**Missed/ended:** door hang-up or user end → return to idle; stop ring, video, audio.

## 8. Security & privacy

The underlying protocol is unauthenticated, unencrypted LAN traffic. v1 assumes a
trusted home network. Requirements: never expose the call/discovery ports to the
internet; the app must not transmit door video/audio off-device; door unlock must
only be triggerable from an active, on-screen call. A future version should add
device pairing/auth and transport encryption (see roadmap).

## 9. Success metrics

- % of calls that ring successfully on the phone.
- % of answered calls that establish two-way audio on first try.
- Time-to-first-video after ring.
- Unlock success rate.
- Crash-free session rate.

## 10. Assumptions & dependencies

- Door station is the known Tuya-based IP unit using the documented call protocol.
- Phone and door share the same Wi-Fi/subnet.
- Door video is H.264; audio is G.711 A-law @ 8 kHz.
- Unlock command is `{"command":"OpenDoor"}` (confirmed working).

## 11. Open questions / future roadmap

- Remote access when away from home (requires a cloud relay or VPN).
- Multiple indoor devices ringing simultaneously (group call / `group_ip` multicast).
- Snapshot/clip on call; call history.
- Device pairing, authentication, and encryption.
- iOS app.
