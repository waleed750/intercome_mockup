#ifndef INTERCOM_AUDIO_PIPELINE_HANDLER_H_
#define INTERCOM_AUDIO_PIPELINE_HANDLER_H_

#include <flutter_linux/flutter_linux.h>

// Registers the "syncn_intercom/audio" method channel and the
// "syncn_intercom/audio_uplink" event channel. Mirrors the Android
// AudioPipelineHandler.kt contract (8kHz mono, 160-byte/20ms A-law frames):
//   start({captureEnabled: bool}) -> void
//   playDownlink(Uint8List alaw)  -> void
//   setMuted(bool)                -> void
//   stop()                        -> void
// Uplink A-law frames are pushed to the event channel as they're captured.
//
// Uses GStreamer's pulsesrc/pulsesink plus the built-in alawenc/alawdec
// elements rather than talking to PulseAudio or doing A-law math directly,
// so the codec logic isn't duplicated a third time (it already exists once
// in Kotlin and once in pure Dart at lib/src/protocol/alaw_codec.dart).
void intercom_audio_pipeline_handler_register(FlBinaryMessenger* messenger);

#endif  // INTERCOM_AUDIO_PIPELINE_HANDLER_H_
