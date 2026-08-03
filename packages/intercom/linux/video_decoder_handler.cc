#include "video_decoder_handler.h"

#include <flutter_linux/flutter_linux.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <cstring>

namespace {

constexpr char kChannelName[] = "syncn_intercom/video";
// Matches VideoDecoderHandler.kt's DEFAULT_WIDTH/DEFAULT_HEIGHT: the pixel
// buffer is pre-sized for this and only grows if the door ever sends a
// larger frame, so steady-state playback never reallocates.
constexpr guint32 kDefaultWidth = 1920;
constexpr guint32 kDefaultHeight = 1080;

}  // namespace

// --- IntercomVideoTexture: FlPixelBufferTexture backed by a single,
// mutex-guarded, monotonically-growing buffer. The buffer's address never
// changes once grown to a given size, so a copy_pixels() call racing a
// concurrent frame update can read stale-but-valid bytes (a torn frame at
// worst) rather than freed memory - there is no reallocation while a read
// could be in flight, only in-place overwrite under the lock.
G_DECLARE_FINAL_TYPE(IntercomVideoTexture, intercom_video_texture, INTERCOM,
                      VIDEO_TEXTURE, FlPixelBufferTexture)

struct _IntercomVideoTexture {
  FlPixelBufferTexture parent_instance;
  GMutex mutex;
  guint8* pixels;
  gsize capacity;
  gsize size;
  guint32 width;
  guint32 height;
};

G_DEFINE_TYPE(IntercomVideoTexture, intercom_video_texture,
              fl_pixel_buffer_texture_get_type())

static gboolean intercom_video_texture_copy_pixels(
    FlPixelBufferTexture* texture, const uint8_t** out_buffer,
    uint32_t* width, uint32_t* height, GError** error) {
  auto* self = INTERCOM_VIDEO_TEXTURE(texture);
  g_mutex_lock(&self->mutex);
  if (self->pixels == nullptr || self->size == 0) {
    g_mutex_unlock(&self->mutex);
    g_set_error(error, g_quark_from_static_string("intercom"), 0,
                "no video frame decoded yet");
    return FALSE;
  }
  *out_buffer = self->pixels;
  *width = self->width;
  *height = self->height;
  g_mutex_unlock(&self->mutex);
  return TRUE;
}

static void intercom_video_texture_dispose(GObject* object) {
  auto* self = INTERCOM_VIDEO_TEXTURE(object);
  g_mutex_clear(&self->mutex);
  g_free(self->pixels);
  self->pixels = nullptr;
  G_OBJECT_CLASS(intercom_video_texture_parent_class)->dispose(object);
}

static void intercom_video_texture_class_init(
    IntercomVideoTextureClass* klass) {
  FL_PIXEL_BUFFER_TEXTURE_CLASS(klass)->copy_pixels =
      intercom_video_texture_copy_pixels;
  G_OBJECT_CLASS(klass)->dispose = intercom_video_texture_dispose;
}

static void intercom_video_texture_init(IntercomVideoTexture* self) {
  g_mutex_init(&self->mutex);
  self->pixels = nullptr;
  self->capacity = 0;
  self->size = 0;
  self->width = 0;
  self->height = 0;
}

// Overwrites the texture's pixel buffer in place, growing (never shrinking)
// the backing allocation as needed. Safe to call from the GStreamer
// streaming thread while copy_pixels() runs on the render thread - see the
// comment above IntercomVideoTexture.
static void intercom_video_texture_update(IntercomVideoTexture* self,
                                           const guint8* data, gsize size,
                                           guint32 width, guint32 height) {
  g_mutex_lock(&self->mutex);
  if (self->capacity < size) {
    g_free(self->pixels);
    self->pixels = static_cast<guint8*>(g_malloc(size));
    self->capacity = size;
  }
  std::memcpy(self->pixels, data, size);
  self->size = size;
  self->width = width;
  self->height = height;
  g_mutex_unlock(&self->mutex);
}

static void intercom_video_texture_init_blank(IntercomVideoTexture* self) {
  // Give the texture valid, correctly-sized black pixels before the first
  // real frame decodes, in case the engine polls copy_pixels() early.
  gsize size = static_cast<gsize>(kDefaultWidth) * kDefaultHeight * 4;
  guint8* blank = static_cast<guint8*>(g_malloc0(size));
  intercom_video_texture_update(self, blank, size, kDefaultWidth,
                                 kDefaultHeight);
  g_free(blank);
}

// --- VideoDecoderHandler: owns the method channel and the GStreamer decode
// pipeline (appsrc -> h264parse -> avdec_h264 -> videoconvert -> appsink).
namespace {

class VideoDecoderHandler {
 public:
  VideoDecoderHandler(FlBinaryMessenger* messenger,
                       FlTextureRegistrar* textures)
      : textures_(textures) {
    g_object_ref(textures_);
    g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
    channel_ = fl_method_channel_new(messenger, kChannelName,
                                      FL_METHOD_CODEC(codec));
    fl_method_channel_set_method_call_handler(channel_, MethodCallTrampoline,
                                               this, nullptr);
  }

  ~VideoDecoderHandler() {
    StopInternal();
    g_object_unref(textures_);
    g_object_unref(channel_);
  }

 private:
  static void MethodCallTrampoline(FlMethodChannel* channel,
                                    FlMethodCall* call, gpointer user_data) {
    reinterpret_cast<VideoDecoderHandler*>(user_data)->HandleMethodCall(call);
  }

  void HandleMethodCall(FlMethodCall* call) {
    const gchar* method = fl_method_call_get_name(call);
    g_autoptr(GError) error = nullptr;
    if (g_strcmp0(method, "start") == 0) {
      int64_t texture_id = Start();
      g_autoptr(FlValue) result = fl_value_new_int(texture_id);
      fl_method_call_respond_success(call, result, &error);
    } else if (g_strcmp0(method, "submit") == 0) {
      FlValue* args = fl_method_call_get_args(call);
      if (args != nullptr &&
          fl_value_get_type(args) == FL_VALUE_TYPE_UINT8_LIST) {
        Submit(fl_value_get_uint8_list(args), fl_value_get_length(args));
      }
      fl_method_call_respond_success(call, nullptr, &error);
    } else if (g_strcmp0(method, "stop") == 0) {
      StopInternal();
      fl_method_call_respond_success(call, nullptr, &error);
    } else {
      fl_method_call_respond_not_implemented(call, &error);
    }
  }

  int64_t Start() {
    if (pipeline_ != nullptr) {
      return texture_ != nullptr ? fl_texture_get_id(FL_TEXTURE(texture_))
                                  : -1;
    }

    texture_ = INTERCOM_VIDEO_TEXTURE(
        g_object_new(intercom_video_texture_get_type(), nullptr));
    intercom_video_texture_init_blank(texture_);
    if (!fl_texture_registrar_register_texture(textures_,
                                                FL_TEXTURE(texture_))) {
      g_object_unref(texture_);
      texture_ = nullptr;
      return -1;
    }

    g_autoptr(GError) error = nullptr;
    pipeline_ = gst_parse_launch(
        "appsrc name=src is-live=true format=time do-timestamp=true "
        "block=false max-bytes=4194304 ! h264parse ! avdec_h264 ! "
        "videoconvert ! video/x-raw,format=RGBA ! "
        "appsink name=sink emit-signals=true sync=false max-buffers=1 "
        "drop=true",
        &error);
    if (pipeline_ == nullptr) {
      g_printerr("intercom: failed to build video pipeline: %s\n",
                 error != nullptr ? error->message : "unknown error");
      StopInternal();
      return -1;
    }

    appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
    GstElement* appsink = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
    g_signal_connect(appsink, "new-sample", G_CALLBACK(OnNewSample), this);
    g_object_unref(appsink);

    bus_ = gst_element_get_bus(pipeline_);
    gst_bus_add_watch(bus_, OnBusMessage, this);

    gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    return fl_texture_get_id(FL_TEXTURE(texture_));
  }

  void Submit(const guint8* data, gsize size) {
    if (appsrc_ == nullptr || size == 0) return;
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    gst_buffer_fill(buffer, 0, data, size);
    gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
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
    appsrc_ = nullptr;
    if (texture_ != nullptr) {
      fl_texture_registrar_unregister_texture(textures_,
                                               FL_TEXTURE(texture_));
      g_object_unref(texture_);
      texture_ = nullptr;
    }
  }

  static GstFlowReturn OnNewSample(GstElement* sink, gpointer user_data) {
    auto* self = reinterpret_cast<VideoDecoderHandler*>(user_data);
    GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (sample == nullptr) return GST_FLOW_OK;

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstCaps* caps = gst_sample_get_caps(sample);
    GstMapInfo map;
    if (buffer != nullptr && caps != nullptr &&
        gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      GstStructure* structure = gst_caps_get_structure(caps, 0);
      gint width = kDefaultWidth;
      gint height = kDefaultHeight;
      gst_structure_get_int(structure, "width", &width);
      gst_structure_get_int(structure, "height", &height);
      intercom_video_texture_update(self->texture_, map.data, map.size,
                                     static_cast<guint32>(width),
                                     static_cast<guint32>(height));
      gst_buffer_unmap(buffer, &map);
      fl_texture_registrar_mark_texture_frame_available(
          self->textures_, FL_TEXTURE(self->texture_));
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  static gboolean OnBusMessage(GstBus* bus, GstMessage* message,
                                gpointer user_data) {
    auto* self = reinterpret_cast<VideoDecoderHandler*>(user_data);
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      g_autoptr(GError) error = nullptr;
      gst_message_parse_error(message, &error, nullptr);
      g_printerr("intercom: video pipeline error: %s\n", error->message);
      self->StopInternal();
    }
    return TRUE;
  }

  FlTextureRegistrar* textures_;
  FlMethodChannel* channel_ = nullptr;
  IntercomVideoTexture* texture_ = nullptr;
  GstElement* pipeline_ = nullptr;
  GstElement* appsrc_ = nullptr;
  GstBus* bus_ = nullptr;
};

}  // namespace

void intercom_video_decoder_handler_register(FlBinaryMessenger* messenger,
                                              FlTextureRegistrar* textures) {
  // Deliberately never deleted: this mirrors the Android/iOS side, where
  // the handler is a singleton (`object VideoDecoderHandler`) that lives
  // for the whole process. The plugin registrar has no per-plugin teardown
  // hook on Linux desktop outside of process exit.
  new VideoDecoderHandler(messenger, textures);
}
