# flutter-pi backend for the panel's intercom (2026-09-20 rewrite)

This directory used to hold two flutter-pi plugins (`syncn_intercom_audio.c`,
`syncn_intercom_video.c`) that ran GStreamer pipelines directly inside the
`flutter-pi` process and talked to Dart over `syncn_intercom/audio` +
`syncn_intercom/audio_uplink` (MethodChannel/EventChannel) and
`syncn_intercom/video` (MethodChannel + texture). **That audio path never
worked reliably on real hardware** (see this repo's own git history and
`syncn_smarthome_panel`'s `docs/panel-intercom-*.md`), and video showed a
black preview on incoming calls, root cause never confirmed.

This has been replaced wholesale with a standalone daemon,
**`syncn-intercomd`**, ported as-is from `github.com/RayanH19/syncn-intercom`
(a proven-working reference implementation for the same door hardware and
wire protocol — confirmed identical: UDP discovery on 8089, TCP call session
on 8189, `appid=7551000`, mandatory StartTalk handshake; see that project's
`docs/10-protocol-reference.md`).

## What lives here now

- `intercomd/` — the daemon's full source tree, copied verbatim from the
  reference repo (`src/`, `include/syncn/`, `third_party/rnnoise/`,
  `systemd/`, its own `CMakeLists.txt`). It is a **standalone C project**,
  built and run independently of flutter-pi — it owns ALSA, the door's TCP/UDP
  protocol, H.264 decode (Rockchip MPP) and RGA colour conversion directly, in
  one process (`syncn-intercomd`). It does not link against flutter-pi at all.
  See `intercomd/README.md`-equivalent docs shipped inside that tree (its own
  `docs/*.md`, not duplicated here) for the daemon's internals.
- `src/syncn_video.c` / `src/syncn_video_proto.h` — the ONE remaining
  flutter-pi plugin. It is a thin DMA-BUF-to-Flutter-texture bridge: it
  connects to the daemon's Unix socket (`/run/syncn/intercomd.sock`),
  subscribes to video frames, imports each decoded frame the daemon hands it
  as an EGL image (zero-copy — the daemon already decoded and colour-converted
  it via hardware), and pushes it to Flutter as an external texture on channel
  `syncn/video`. This is a materially different, and simpler, design than the
  old `syncn_intercom_video.c`: no GStreamer, no CPU-side `glTexImage2D` copy,
  no H.264 decode in this process at all.

## What is GONE

- `syncn_intercom_audio.c/.h`, `syncn_intercom_aec3.cpp/.h`,
  `syncn_intercom_video.c/.h`, `syncn_intercom_debug.h`, `syncn_intercom_dsp.h`,
  `syncn_intercom_gst_util.h` — deleted outright (2026-09-20), not left as dead
  code. The daemon now owns 100% of audio I/O (ALSA, speexdsp-based echo
  cancellation, the "hold-the-floor" half-duplex gate, the independent return
  canceller) and H.264 decode/colour-conversion. There is no flutter-pi
  MethodChannel/EventChannel for audio any more — audio never crosses into
  Dart or into the flutter-pi process at all.
- `patches/0001-add-syncn-intercom-audio-video-plugins.patch` — deleted. It
  wired the two deleted plugins (and a `webrtc-audio-processing`/abseil
  dependency chain that was awkward to build — see its own historical
  comments in git blame) into a patched `ardera/flutter-pi` CMakeLists.txt.
  Superseded by `syncn_smarthome_panel`'s own
  `patches/0003-add-syncn-video-plugin.patch`, which adds ONLY
  `syncn_video.c` (no GStreamer, no webrtc-audio-processing/abseil at all —
  just EGL/GLES, already required for everything else flutter-pi renders).

## Consuming this from the app repo (`syncn_smarthome_panel`)

Three independent things need to happen there (see that repo's own
`patches/0003-add-syncn-video-plugin.patch`, `.github/workflows/release-panel-deb.yml`,
and `packaging/debian/`):

1. **flutter-pi** gets `patches/0003-add-syncn-video-plugin.patch` applied
   (replacing the old `0001` intercom_core patch), building `syncn_video.c`
   into the `flutter-pi` binary under `BUILD_SYNCN_VIDEO_PLUGIN` (default ON,
   no GStreamer dependency).
2. **`syncn-intercomd`** is built as a second, independent binary (see
   `intercomd/CMakeLists.txt`) and packaged alongside `flutter-pi` in the
   `.deb`, with its own systemd unit (`intercomd/systemd/syncn-intercomd.service`,
   adapted for this repo's install paths and started before `syncnhome.service`).
3. **Dart call control** (`lib/src/call/call_controller.dart` and friends) is
   rewired to speak the daemon's JSON-over-Unix-socket protocol instead of
   running its own TCP door client — see
   `lib/src/daemon/` (new in this port) and the reference's
   `ui/syncn_intercom/lib/src/intercom_client.dart`, which
   `lib/src/daemon/intercomd_client.dart` here is ported from.

## What's NOT verified

Same caveat as before this rewrite: no on-device or full build/link test was
possible in the environment this port was written in (no panel hardware
reachable). The reference daemon is described by its own authors as
hardware-verified against a real door station; this port's *packaging and
Dart wiring* around it is new and unverified. See the handback report for the
specific list.
