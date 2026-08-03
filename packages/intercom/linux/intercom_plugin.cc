#include "include/intercom/intercom_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gst/gst.h>

#include "audio_pipeline_handler.h"
#include "ringer_handler.h"
#include "video_decoder_handler.h"

struct _IntercomPlugin {
  GObject parent_instance;
};

G_DEFINE_TYPE(IntercomPlugin, intercom_plugin, g_object_get_type())

static void intercom_plugin_class_init(IntercomPluginClass* klass) {}

static void intercom_plugin_init(IntercomPlugin* self) {}

void intercom_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  // Safe to call more than once per process; GStreamer no-ops after the
  // first successful init. There's no foreground-service equivalent to
  // register here (see android_foreground_service.dart) - that channel is
  // guarded to Android-only on the Dart side.
  if (!gst_is_initialized()) {
    gst_init(nullptr, nullptr);
  }

  IntercomPlugin* plugin =
      INTERCOM_PLUGIN(g_object_new(intercom_plugin_get_type(), nullptr));

  FlBinaryMessenger* messenger = fl_plugin_registrar_get_messenger(registrar);
  FlTextureRegistrar* textures =
      fl_plugin_registrar_get_texture_registrar(registrar);

  intercom_video_decoder_handler_register(messenger, textures);
  intercom_audio_pipeline_handler_register(messenger);
  intercom_ringer_handler_register(messenger);

  g_object_unref(plugin);
}
