# flutter-pi native backend for `syncn_intercom/audio` and `syncn_intercom/video`

This directory is the Linux/flutter-pi counterpart to `android/` and `ios/` in
this package. It implements the same two platform channels those native
implementations provide (`syncn_intercom/audio` +
`syncn_intercom/audio_uplink`, `syncn_intercom/video`), so
`lib/src/media/audio_pipeline.dart` and `lib/src/media/video_decoder.dart`
work unmodified on the Debian/flutter-pi panel.

## Why this isn't a normal Flutter Linux plugin

The panel runs `flutter-pi` (github.com/ardera/flutter-pi), a standalone C
engine embedder — not the standard GTK-based Flutter Linux desktop runner.
flutter-pi does not load third-party plugins from a `linux/` CMake folder the
way the desktop embedder does; its plugins are C sources compiled directly
into the `flutter-pi` binary itself and registered via its
`FLUTTERPI_PLUGIN(...)` macro (see `flutter-pi`'s own `src/plugins/` for
precedent — `gstreamer_video_player`, `audioplayers`, `test_plugin`, etc.).

So instead of Dart-plugin platform code, this directory ships:

- `src/syncn_intercom_video.c` / `.h` — a flutter-pi plugin implementing
  `syncn_intercom/video`. Decodes H.264 via a GStreamer pipeline
  (`appsrc ! h264parse ! avdec_h264 ! videoconvert`) and uploads each decoded
  frame into a plain `GL_TEXTURE_2D` on a dedicated EGL context shared with
  flutter-pi's main GL context, then pushes it through flutter-pi's
  `texture_registry`.
- `src/syncn_intercom_audio.c` / `.h` — a flutter-pi plugin implementing
  `syncn_intercom/audio` (downlink playback: `appsrc ! alawdec ! audioconvert
  ! audioresample ! alsasink`) and `syncn_intercom/audio_uplink`
  (capture: `alsasrc ! audioconvert ! audioresample ! volume ! alawenc !
  appsink`, forwarded to Dart as `EventChannel` events via
  `platch_send_success_event_std`). On the RK809 panel codec it also sets
  `Playback Path=SPK_HP` and `Capture MIC Path=Main Mic` before pipeline start,
  logging and ignoring failures on other hardware.
- `patches/0001-add-syncn-intercom-audio-video-plugins.patch` — a patch
  against `ardera/flutter-pi` that drops the two `.c`/`.h` pairs above into
  `src/plugins/` and wires them into `CMakeLists.txt` behind a new
  `BUILD_SYNCN_INTERCOM_PLUGIN` option (mirroring the existing
  `BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN` / `BUILD_GSTREAMER_AUDIO_PLAYER_PLUGIN`
  options).

**Verified:** this patch applies cleanly with `git apply` against
`ardera/flutter-pi` commit `f0b333052f4e71e51f9049e5016082357175057a` — the
same commit the app repo's `flutterpi-real-app-poc.yml` CI workflow currently
pins and already applies `patches/flutter-pi-gem-close-on-exit.patch`
against. Both patches can be applied in the same step (order doesn't matter,
they touch disjoint regions of `CMakeLists.txt`/`src/modesetting.c`).

## Consuming this from the app repo (`syncn_smarthome_panel`)

That repo's CI (`.github/workflows/flutterpi-real-app-poc.yml`) already
clones `ardera/flutter-pi`, applies `patches/flutter-pi-gem-close-on-exit.patch`,
and builds with CMake. To pick this up, that workflow's "Clone and patch
flutter-pi source" step needs one more `git apply` line for
`packages/intercom_core/flutterpi/patches/0001-add-syncn-intercom-audio-video-plugins.patch`
(from wherever this package is checked out in that build — it's a git
dependency there), and its "Install flutter-pi native build dependencies"
step needs GStreamer *development* headers added:
`libgstreamer1.0-dev`, `libgstreamer-plugins-base1.0-dev`,
`libgstreamer-plugins-good1.0-dev` (for `alawenc`/`alawdec`, part of the
"law" element in gst-plugins-good), `gstreamer1.0-libav` (for `avdec_h264`).
The runtime packages already added to `packaging/debian/control.in` /
`scripts/install_debian_panel_remote.sh` in that repo cover the on-device
runtime side of this; they are not sufficient for *compiling* the plugin,
only for running it. **That workflow/packaging change is out of scope for
this package repo** and belongs in `syncn_smarthome_panel` once this patch
is available at a pinned ref here.

## What's verified vs. not

Every flutter-pi API used here (`plugin_registry_set_receiver_v2_locked`,
`texture_new`/`texture_push_frame`, `gl_renderer_create_context`,
`gl_renderer_get_egl_display`, `platch_respond_success_std`/`platch_send_success_event_std`, the
`FLUTTERPI_PLUGIN` registration macro, `struct std_value`/`struct platch_obj`
field layouts) was checked directly against `ardera/flutter-pi`'s actual
source at the pinned commit above, not guessed from memory — see comments in
the `.c` files for what precedent each pattern follows (`testplugin.c` for
the platform-channel wiring, `gstreamer_video_player`/`audioplayers` for the
GStreamer + texture/audio patterns).

**Not verified — no on-device or full build/link test was possible in the
environment this was written in** (no root to install `libsystemd-dev` /
`libinput-dev`, and `flutter_embedder.h` is fetched by flutter-pi's own CMake
configure step, which those missing deps blocked). Before shipping, verify:

1. **The patch actually compiles and links** as part of a full flutter-pi
   build (the CI job described above is the natural place to find out).
2. **H.264 NAL framing/alignment — confirmed against `mock_door`, not yet
   against a real door unit.** The appsrc caps use
   `stream-format=byte-stream,alignment=nal` (Annex-B, one NAL unit per
   `submit()` call). This matches `mock_door/video_stream.py`'s
   `_read_ffmpeg_stdout`, which splits ffmpeg's Annex-B output on start
   codes and sends each NAL individually — h264parse reassembles NALs into
   access units on the receiving end. `mock_door` is a development stand-in
   (ffmpeg libx264, baseline profile) though, not the real door unit's
   hardware encoder — re-confirm against real traffic if frames don't
   decode on real hardware.
3. **Audio device selection.** `autoaudiosrc`/`autoaudiosink` pick whatever
   GStreamer autodetects as the default ALSA/Pulse device on the panel —
   confirm that's actually the panel's mic/speaker and not some other ALSA
   card, and that the `SupplementaryGroups=audio ...` the app repo's service
   unit grants is sufficient for device access.
4. **Video upload performance.** Frames are uploaded via `glTexImage2D`/
   `glTexSubImage2D` (a CPU→GPU copy each frame), not a zero-copy DMA-BUF
   path. This is the safe, portable choice (works regardless of whether
   `avdec_h264`'s output is DMA-BUF-backed), but if latency/CPU usage is a
   problem on real hardware, a hardware-decoder + DMA-BUF path (reusing
   `gstreamer_video_player`'s own `frame_interface`/`frame_new` bridge
   instead of manual GL upload) is the next thing to try.
5. **No AEC/NS/AGC yet.** The Android path enables
   `AcousticEchoCanceler`/`NoiseSuppressor`/`AutomaticGainControl`; this
   implementation doesn't have an equivalent yet (GStreamer's `webrtcdsp`,
   from gst-plugins-bad, is the natural fit if echo turns out to be a
   problem on real hardware). Not blocking for the "no sound at all" bug
   this exists to fix, but a known gap versus Android/iOS.
6. **`setMuted` semantics.** Matches Android: mutes the outgoing mic only
   (via the `volume` element's `mute` property ahead of the encoder, so
   frames keep flowing but silent), not the speaker.
