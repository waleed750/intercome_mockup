#include "audio_pipeline_handler.h"

#include <flutter_linux/flutter_linux.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <algorithm>
#include <atomic>
#include <vector>

namespace {

constexpr char kMethodChannelName[] = "syncn_intercom/audio";
constexpr char kEventChannelName[] = "syncn_intercom/audio_uplink";
// Matches AudioPipelineHandler.kt: 8kHz mono, 160-byte A-law frames = 20ms
// (the uplink pipeline's `audiobuffersplit` stage guarantees this size).
constexpr guint8 kSilenceByte = 0xD5;

class AudioPipelineHandler {
 public:
  explicit AudioPipelineHandler(FlBinaryMessenger* messenger) {
    g_autoptr(FlStandardMethodCodec) method_codec =
        fl_standard_method_codec_new();
    method_channel_ = fl_method_channel_new(messenger, kMethodChannelName,
                                             FL_METHOD_CODEC(method_codec));
    fl_method_channel_set_method_call_handler(
        method_channel_, MethodCallTrampoline, this, nullptr);

    g_autoptr(FlStandardMethodCodec) event_codec =
        fl_standard_method_codec_new();
    event_channel_ = fl_event_channel_new(messenger, kEventChannelName,
                                           FL_METHOD_CODEC(event_codec));
    fl_event_channel_set_stream_handlers(event_channel_, OnListenTrampoline,
                                          OnCancelTrampoline, this, nullptr);
  }

  ~AudioPipelineHandler() {
    StopInternal();
    g_object_unref(method_channel_);
    g_object_unref(event_channel_);
  }

 private:
  static void MethodCallTrampoline(FlMethodChannel* channel,
                                    FlMethodCall* call, gpointer user_data) {
    reinterpret_cast<AudioPipelineHandler*>(user_data)->HandleMethodCall(
        call);
  }

  static FlMethodErrorResponse* OnListenTrampoline(FlEventChannel* channel,
                                                     FlValue* args,
                                                     gpointer user_data) {
    reinterpret_cast<AudioPipelineHandler*>(user_data)->has_listener_ = true;
    return nullptr;
  }

  static FlMethodErrorResponse* OnCancelTrampoline(FlEventChannel* channel,
                                                     FlValue* args,
                                                     gpointer user_data) {
    reinterpret_cast<AudioPipelineHandler*>(user_data)->has_listener_ = false;
    return nullptr;
  }

  void HandleMethodCall(FlMethodCall* call) {
    const gchar* method = fl_method_call_get_name(call);
    g_autoptr(GError) error = nullptr;
    if (g_strcmp0(method, "start") == 0) {
      FlValue* args = fl_method_call_get_args(call);
      bool capture_enabled = false;
      if (args != nullptr && fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
        FlValue* value = fl_value_lookup_string(args, "captureEnabled");
        if (value != nullptr && fl_value_get_type(value) == FL_VALUE_TYPE_BOOL) {
          capture_enabled = fl_value_get_bool(value);
        }
      }
      Start(capture_enabled);
      fl_method_call_respond_success(call, nullptr, &error);
    } else if (g_strcmp0(method, "playDownlink") == 0) {
      FlValue* args = fl_method_call_get_args(call);
      if (args != nullptr &&
          fl_value_get_type(args) == FL_VALUE_TYPE_UINT8_LIST) {
        PlayDownlink(fl_value_get_uint8_list(args), fl_value_get_length(args));
      }
      fl_method_call_respond_success(call, nullptr, &error);
    } else if (g_strcmp0(method, "setMuted") == 0) {
      FlValue* args = fl_method_call_get_args(call);
      bool muted = args != nullptr && fl_value_get_type(args) == FL_VALUE_TYPE_BOOL
                       ? fl_value_get_bool(args)
                       : false;
      muted_.store(muted);
      fl_method_call_respond_success(call, nullptr, &error);
    } else if (g_strcmp0(method, "stop") == 0) {
      StopInternal();
      fl_method_call_respond_success(call, nullptr, &error);
    } else {
      fl_method_call_respond_not_implemented(call, &error);
    }
  }

  void Start(bool capture_enabled) {
    if (running_) return;
    running_ = true;
    muted_.store(false);
    StartDownlink();
    if (capture_enabled) StartUplink();
  }

  void StartDownlink() {
    g_autoptr(GError) error = nullptr;
    downlink_pipeline_ = gst_parse_launch(
        "appsrc name=src is-live=true format=time do-timestamp=true "
        "caps=audio/x-alaw,rate=8000,channels=1 ! alawdec ! audioconvert ! "
        "audioresample ! pulsesink name=sink buffer-time=40000",
        &error);
    if (downlink_pipeline_ == nullptr) {
      g_printerr("intercom: failed to build downlink audio pipeline: %s\n",
                 error != nullptr ? error->message : "unknown error");
      return;
    }
    downlink_src_ = gst_bin_get_by_name(GST_BIN(downlink_pipeline_), "src");
    gst_element_set_state(downlink_pipeline_, GST_STATE_PLAYING);
  }

  void StartUplink() {
    g_autoptr(GError) error = nullptr;
    // `audiobuffersplit` re-chunks the resampled stream into exact 20ms
    // buffers regardless of PulseAudio's own buffering, so alawenc always
    // emits exactly kAlawFrameBytes (160) per buffer - matching the
    // fixed-size reads AudioPipelineHandler.kt does from AudioRecord, which
    // the door's protocol (CC channel, 160 bytes/20ms) expects.
    //
    // TODO(linux-aec): Android enables AcousticEchoCanceler/NoiseSuppressor/
    // AutomaticGainControl on the capture session (see enableEffects() in
    // AudioPipelineHandler.kt). The closest GStreamer equivalent is the
    // `webrtcdsp` element from gst-plugins-bad (requires
    // libwebrtc-audio-processing). It's intentionally left out here per the
    // Linux support plan - add `webrtcdsp` between audioresample and
    // audiobuffersplit once its availability on the target distro is
    // confirmed, rather than guessing at its property names.
    uplink_pipeline_ = gst_parse_launch(
        "pulsesrc name=src ! audioconvert ! audioresample ! "
        "audio/x-raw,format=S16LE,rate=8000,channels=1 ! "
        "audiobuffersplit output-buffer-duration-fraction=1/50 ! alawenc ! "
        "appsink name=sink emit-signals=true sync=false max-buffers=8 "
        "drop=false",
        &error);
    if (uplink_pipeline_ == nullptr) {
      g_printerr("intercom: failed to build uplink audio pipeline: %s\n",
                 error != nullptr ? error->message : "unknown error");
      return;
    }
    GstElement* appsink = gst_bin_get_by_name(GST_BIN(uplink_pipeline_), "sink");
    g_signal_connect(appsink, "new-sample", G_CALLBACK(OnNewSample), this);
    g_object_unref(appsink);
    gst_element_set_state(uplink_pipeline_, GST_STATE_PLAYING);
  }

  void PlayDownlink(const guint8* data, gsize size) {
    if (downlink_src_ == nullptr || size == 0) return;
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    gst_buffer_fill(buffer, 0, data, size);
    gst_app_src_push_buffer(GST_APP_SRC(downlink_src_), buffer);
  }

  void StopInternal() {
    if (downlink_pipeline_ != nullptr) {
      gst_element_set_state(downlink_pipeline_, GST_STATE_NULL);
      gst_object_unref(downlink_pipeline_);
      downlink_pipeline_ = nullptr;
    }
    downlink_src_ = nullptr;
    if (uplink_pipeline_ != nullptr) {
      gst_element_set_state(uplink_pipeline_, GST_STATE_NULL);
      gst_object_unref(uplink_pipeline_);
      uplink_pipeline_ = nullptr;
    }
    running_ = false;
  }

  static GstFlowReturn OnNewSample(GstElement* sink, gpointer user_data) {
    auto* self = reinterpret_cast<AudioPipelineHandler*>(user_data);
    GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (sample == nullptr) return GST_FLOW_OK;

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buffer != nullptr && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      self->EmitUplinkFrame(map.data, map.size);
      gst_buffer_unmap(buffer, &map);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  // OnNewSample runs on the GStreamer streaming thread, but the event
  // channel must only be touched from the GLib main context the Flutter
  // engine runs on (Android's equivalent AudioPipelineHandler.kt does the
  // same hop via `mainHandler.post {}` before calling `eventSink`). Marshal
  // a heap-owned copy of the frame over via g_idle_add_full rather than
  // calling fl_event_channel_send directly from this thread.
  struct UplinkPayload {
    AudioPipelineHandler* handler;
    std::vector<guint8> bytes;
  };

  void EmitUplinkFrame(const guint8* data, gsize size) {
    if (!has_listener_ || size == 0) return;
    auto* payload = new UplinkPayload{this, std::vector<guint8>(data, data + size)};
    if (muted_.load()) {
      std::fill(payload->bytes.begin(), payload->bytes.end(), kSilenceByte);
    }
    g_idle_add_full(
        G_PRIORITY_DEFAULT, &AudioPipelineHandler::SendUplinkPayload, payload,
        [](gpointer p) { delete reinterpret_cast<UplinkPayload*>(p); });
  }

  static gboolean SendUplinkPayload(gpointer data) {
    auto* payload = reinterpret_cast<UplinkPayload*>(data);
    if (payload->handler->has_listener_) {
      g_autoptr(FlValue) event =
          fl_value_new_uint8_list(payload->bytes.data(), payload->bytes.size());
      g_autoptr(GError) error = nullptr;
      fl_event_channel_send(payload->handler->event_channel_, event, nullptr,
                             &error);
    }
    return G_SOURCE_REMOVE;
  }

  FlMethodChannel* method_channel_ = nullptr;
  FlEventChannel* event_channel_ = nullptr;
  GstElement* downlink_pipeline_ = nullptr;
  GstElement* downlink_src_ = nullptr;
  GstElement* uplink_pipeline_ = nullptr;
  bool running_ = false;
  bool has_listener_ = false;
  std::atomic<bool> muted_{false};
};

}  // namespace

void intercom_audio_pipeline_handler_register(FlBinaryMessenger* messenger) {
  // Deliberately never deleted - see the matching comment in
  // video_decoder_handler.cc.
  new AudioPipelineHandler(messenger);
}
