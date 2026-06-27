# Frontend / UI Specification
## Smart Home Video Intercom — Android App

**Status:** Draft v1
**Platform:** Native Android (phone-first, tablet-friendly)
**Recommended toolkit:** Jetpack Compose

---

## 1. Scope

Defines the screens, states, components, and interactions for the Android indoor
intercom. Visual design (exact colors, type scale) is left to the design system;
this spec defines structure and behaviour.

## 2. Screen map

```
Idle/Home  ──(incoming Call)──►  Incoming Call (full screen)
                                      │ Answer ──► In-Call
                                      │ Decline ─► Idle/Home
In-Call  ──(End / remote HangUp)──►  Idle/Home
Settings (from Home)
```

## 3. Screens

### 3.1 Idle / Home
Shown when no call is active.
- **Status line:** connection state — "Ready — listening for the door" or
  "Not connected to door network."
- **Device info (secondary):** this unit's name (`alias`) and the door it's paired with.
- **Entry to Settings.**
- Optional: a **"View door camera"** (monitor/peek) button if live-view-on-demand is added later (not v1).

### 3.2 Incoming Call (full screen, highest priority)
Appears over the lock screen when the door calls.
- **Live video preview** fills the screen (starts during ring).
- **Caller label:** "Front Door" (from `dstAddr`/config).
- **Status:** "Incoming call…" with the ringing animation.
- **Primary actions:** large **Answer** (green) and **Decline** (red) buttons.
- Ringtone plays and the device vibrates until answered/declined.

### 3.3 In-Call
After Answer.
- **Live video** (full or large region).
- **Talk indicator / mic level.**
- **Action bar:**
  - **Unlock** (primary, prominent) — one tap sends `OpenDoor`; shows a brief "Door unlocked" confirmation.
  - **Mute** — toggles mic; clearly shows muted vs live.
  - **End** (red) — hangs up.
- **Connection/quality indicator** (optional): subtle, e.g. "Connected."

### 3.4 Settings
- Unit name (`alias`) and address (`dstAddr`).
- Paired door / network info.
- Permissions status (mic, notifications, battery-optimization exemption) with quick fixes.
- About / version.

## 4. Component states

| Component | States |
|-----------|--------|
| Answer button | enabled (ringing) → hidden (after answer) |
| Decline/End button | enabled during ring/call; disabled when idle |
| Unlock button | disabled until call connected; pressed → momentary "Unlocked" confirm |
| Mute button | Live ⇄ Muted (icon + label change) |
| Video surface | placeholder ("Connecting to door…") → live frames → cleared on end |
| Status line | listening / ringing / connected / ended / error |
| Mic level meter | animates with input; flat when muted |

## 5. Interaction rules

- **Unlock** is only tappable during a connected call (security + clarity).
- **Mute** never drops the call; it only silences the uplink.
- Pressing **End** or **Decline** always returns to Idle and stops ring/video/audio.
- If the door hangs up, auto-return to Idle with a brief "Call ended" message.
- Ringtone/vibration stop the instant the user answers or declines.
- The incoming-call screen must show even when the app is backgrounded or the phone is locked.

## 6. Accessibility

- Answer/Decline/Unlock/End are large touch targets (≥ 48 dp), labeled for screen readers.
- Don't rely on color alone (Mute/active states also change icon + text).
- Captions/labels for all icon-only buttons.
- Support landscape (and tablet layouts) without hiding primary actions.

## 7. Empty / error states

| Situation | UI |
|-----------|-----|
| Not on the door's Wi-Fi | "Connect to your home Wi-Fi to receive door calls." |
| Mic permission denied | In-call banner: "Allow microphone to talk" + grant action. |
| Notifications disabled | Settings warning: calls may not ring full-screen. |
| Battery optimization on | Settings warning + one-tap exemption request. |
| Video fails to decode | Keep audio; show "Video unavailable" over the surface. |

## 8. Copy (suggested microcopy)

- Idle: "Ready — listening for the door"
- Ringing: "Front Door is calling…"
- Connected: "Connected"
- Unlock confirm: "Door unlocked"
- Muted: "You're muted"
- Ended: "Call ended"

## 9. Notes for design handoff

- Two hero screens to design first: **Incoming Call** and **In-Call** — they carry the product.
- Unlock is the most important in-call action; make it unmistakable and hard to mis-tap.
- Mirror the reliability of a hardware indoor panel: instant ring, instant video, one-tap unlock.
