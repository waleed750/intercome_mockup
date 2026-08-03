#ifndef INTERCOM_VIDEO_DECODER_HANDLER_H_
#define INTERCOM_VIDEO_DECODER_HANDLER_H_

#include <flutter_linux/flutter_linux.h>

// Registers the "syncn_intercom/video" method channel and owns everything
// needed to serve it: a GStreamer decode pipeline and a registered Flutter
// pixel-buffer texture. Mirrors the Android VideoDecoderHandler.kt contract
// exactly so the Dart-side VideoDecoder (lib/src/media/video_decoder.dart)
// needs no platform-specific branches:
//   start()  -> int textureId
//   submit(Uint8List annexBFrame) -> void
//   stop()   -> void
//
// The registered handler is kept alive for the lifetime of the Flutter
// engine (owned by the method channel via its destroy-notify), matching how
// the Android/iOS singletons live for the process lifetime.
void intercom_video_decoder_handler_register(FlBinaryMessenger* messenger,
                                              FlTextureRegistrar* textures);

#endif  // INTERCOM_VIDEO_DECODER_HANDLER_H_
