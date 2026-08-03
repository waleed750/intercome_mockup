#ifndef INTERCOM_RINGER_HANDLER_H_
#define INTERCOM_RINGER_HANDLER_H_

#include <flutter_linux/flutter_linux.h>

// Registers the "syncn_intercom/ringer" method channel: start() / stop().
//
// No platform implements this channel today (see lib/src/ringer/ringer.dart
// - the Android/iOS invokeMethod calls are wrapped in catchError and are
// currently no-ops; ringing is done via the `vibration` package only). A
// desktop has no vibration motor, so on Linux this channel is the actual
// ring signal: it loops the ringtone bundled at
// assets/sounds/ringtone.wav (declared in pubspec.yaml) via GStreamer's
// playbin, located at runtime from the app bundle next to this binary.
void intercom_ringer_handler_register(FlBinaryMessenger* messenger);

#endif  // INTERCOM_RINGER_HANDLER_H_
