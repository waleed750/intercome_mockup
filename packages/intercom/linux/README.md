# Linux platform implementation

Native counterpart to `android/` and `ios/` for the three method/event
channels defined in `lib/src/media/video_decoder.dart`,
`lib/src/media/audio_pipeline.dart`, and `lib/src/ringer/ringer.dart`. Video
decode and audio capture/playback are implemented with GStreamer; video
frames are surfaced to Flutter via a `FlPixelBufferTexture`.

Builds clean with `flutter build linux` (clang/cmake/ninja against libgtk-3
and GStreamer) and has been run end-to-end against `mock_door/` on a real
Linux machine: `libintercom_plugin.so` loads, links correctly against
`libgstreamer-1.0.so.0`/`libgtk-3.so.0`, and the app reaches the idle screen
with discovery/call-server/notifications/ringer all functioning.

## One-time setup on your Linux dev machine

```bash
flutter config --enable-linux-desktop

sudo apt install clang cmake ninja-build pkg-config libgtk-3-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libgstreamer-plugins-bad1.0-dev \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-pulseaudio \
  libpulse-dev

# Generates example/linux/ (the GTK runner scaffold + the ephemeral
# flutter/ build glue) - this repo intentionally doesn't hand-author those
# generated files since their exact contents are Flutter-SDK-version
# specific; `flutter create` fills them in correctly for whatever SDK
# you have installed. Safe to re-run any time; it only adds the linux/
# platform folder and won't touch existing files elsewhere in example/.
cd packages/intercom/example
flutter create --platforms=linux .

flutter pub get
flutter run -d linux
```

`gstreamer1.0-libav` provides `avdec_h264` (software H.264 decode - the
`start()`/`submit()`/`stop()` contract in `video_decoder_handler.cc` doesn't
depend on any particular decoder element, so swapping in a hardware one
later, e.g. `vaapih264dec`, is a one-line pipeline-string change).
`libgstreamer-plugins-bad1.0-dev` is required for the `audiobuffersplit`
element the uplink audio pipeline uses to guarantee exact 160-byte/20ms
A-law frames (see `audio_pipeline_handler.cc`).

## Known gaps / follow-ups

- **No AEC/NS/AGC on the uplink** yet. Android enables
  `AcousticEchoCanceler`/`NoiseSuppressor`/`AutomaticGainControl` on capture
  (`AudioPipelineHandler.kt`'s `enableEffects`); the closest GStreamer
  equivalent is the `webrtcdsp` element (gst-plugins-bad, needs
  `libwebrtc-audio-processing`). Left out intentionally rather than guessing
  at its exact property names without being able to build and test it —
  see the `TODO(linux-aec)` in `audio_pipeline_handler.cc`.
- **Software H.264 decode only.** Fine for development/testing; a hardware
  decoder (VA-API/NVDEC) is a drop-in pipeline-element swap if CPU usage
  matters for a real kiosk deployment.
- **Ringer** plays `assets/sounds/ringtone.wav` (bundled via the package's
  `pubspec.yaml`) on loop through `playbin`, since the `vibration` package
  (the phone's ring signal) has no Linux backend and there's no vibration
  motor to substitute for anyway - see `ringer_handler.cc`. The clip is a
  synthesized dual-tone (440/480Hz) telephone-style ring, generated rather
  than sourced, to avoid any licensing ambiguity around bundled audio.
