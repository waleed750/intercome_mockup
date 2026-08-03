#include "ringer_handler.h"

#include <flutter_linux/flutter_linux.h>
#include <gst/gst.h>
#include <unistd.h>

#include <climits>

namespace {

constexpr char kChannelName[] = "syncn_intercom/ringer";
// Bundled via packages/intercom/pubspec.yaml's `flutter: assets:` entry;
// Flutter packages' assets land under packages/<package>/<asset path>
// inside the app bundle's data/flutter_assets directory. The clip itself
// bakes in the ring-then-pause cadence (2s tone / 3s silence), so looping
// it whole is enough - no separate on/off timer needed here.
constexpr char kRingtoneRelativePath[] =
    "data/flutter_assets/packages/intercom/assets/sounds/ringtone.wav";

// Flutter Linux apps always lay out their bundle as
// <bundle>/<executable>, <bundle>/data/flutter_assets/..., so resolving our
// own binary's location via /proc/self/exe finds the bundle root without
// needing Dart's help (asset paths aren't otherwise exposed to plugins).
gchar* ResolveRingtonePath() {
  char exe_path[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  if (len <= 0) return nullptr;
  exe_path[len] = '\0';

  g_autofree gchar* bundle_dir = g_path_get_dirname(exe_path);
  return g_build_filename(bundle_dir, kRingtoneRelativePath, nullptr);
}

class RingerHandler {
 public:
  explicit RingerHandler(FlBinaryMessenger* messenger) {
    g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
    channel_ =
        fl_method_channel_new(messenger, kChannelName, FL_METHOD_CODEC(codec));
    fl_method_channel_set_method_call_handler(channel_, MethodCallTrampoline,
                                               this, nullptr);
  }

  ~RingerHandler() {
    StopInternal();
    g_object_unref(channel_);
  }

 private:
  static void MethodCallTrampoline(FlMethodChannel* channel,
                                    FlMethodCall* call, gpointer user_data) {
    reinterpret_cast<RingerHandler*>(user_data)->HandleMethodCall(call);
  }

  void HandleMethodCall(FlMethodCall* call) {
    const gchar* method = fl_method_call_get_name(call);
    g_autoptr(GError) error = nullptr;
    if (g_strcmp0(method, "start") == 0) {
      Start();
      fl_method_call_respond_success(call, nullptr, &error);
    } else if (g_strcmp0(method, "stop") == 0) {
      StopInternal();
      fl_method_call_respond_success(call, nullptr, &error);
    } else {
      fl_method_call_respond_not_implemented(call, &error);
    }
  }

  void Start() {
    if (pipeline_ != nullptr) return;

    g_autofree gchar* path = ResolveRingtonePath();
    if (path == nullptr || !g_file_test(path, G_FILE_TEST_EXISTS)) {
      g_printerr(
          "intercom: ringtone asset not found at %s - did `flutter build` "
          "bundle packages/intercom/assets/sounds/ringtone.wav?\n",
          path != nullptr ? path : "(unresolved)");
      return;
    }

    pipeline_ = gst_element_factory_make("playbin", "ringer");
    if (pipeline_ == nullptr) {
      g_printerr("intercom: failed to create playbin for ringer\n");
      return;
    }

    g_autofree gchar* uri = gst_filename_to_uri(path, nullptr);
    g_object_set(pipeline_, "uri", uri, nullptr);

    bus_ = gst_element_get_bus(pipeline_);
    gst_bus_add_watch(bus_, OnBusMessage, this);

    gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  }

  static gboolean OnBusMessage(GstBus* bus, GstMessage* message,
                                gpointer user_data) {
    auto* self = reinterpret_cast<RingerHandler*>(user_data);
    switch (GST_MESSAGE_TYPE(message)) {
      case GST_MESSAGE_EOS:
        // Loop: the ringtone clip already contains its own ring/pause
        // cadence, so restarting from 0 on end-of-stream repeats it.
        gst_element_seek_simple(self->pipeline_, GST_FORMAT_TIME,
                                 GST_SEEK_FLAG_FLUSH, 0);
        break;
      case GST_MESSAGE_ERROR: {
        g_autoptr(GError) error = nullptr;
        gst_message_parse_error(message, &error, nullptr);
        g_printerr("intercom: ringer pipeline error: %s\n", error->message);
        self->StopInternal();
        break;
      }
      default:
        break;
    }
    return TRUE;
  }

  void StopInternal() {
    if (bus_ != nullptr) {
      gst_bus_remove_watch(bus_);
      gst_object_unref(bus_);
      bus_ = nullptr;
    }
    if (pipeline_ != nullptr) {
      gst_element_set_state(pipeline_, GST_STATE_NULL);
      gst_object_unref(pipeline_);
      pipeline_ = nullptr;
    }
  }

  FlMethodChannel* channel_ = nullptr;
  GstElement* pipeline_ = nullptr;
  GstBus* bus_ = nullptr;
};

}  // namespace

void intercom_ringer_handler_register(FlBinaryMessenger* messenger) {
  // Deliberately never deleted - see the matching comment in
  // video_decoder_handler.cc.
  new RingerHandler(messenger);
}
