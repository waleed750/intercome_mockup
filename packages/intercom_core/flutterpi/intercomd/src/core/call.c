#include "syncn/call.h"
#include "syncn/audio.h"
#include "syncn/ipc.h"
#include "syncn/net.h"
#include "syncn/util.h"
#include "syncn/video.h"
#include "syncn/ringer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * The door only begins streaming once it has received both the Answer
 * handshake and a StartTalk. The one-second gap on the call path is deliberate
 * and was established by capture; the preview path sends it immediately,
 * without starting audio, purely as a wire trigger — without it, preview video
 * took four to seven seconds to appear.
 */
#define START_TALK_DELAY_MS 1000
#define STATS_INTERVAL_MS   5000

/* A preview refused straight after a call is retried this often, this many
 * times, before the ten-second cadence for an unreachable door takes over. */
#define PREVIEW_RETRY_MS    300
#define PREVIEW_FAST_RETRIES 6

struct syncn_call {
    syncn_config cfg;
    syncn_screen_info info;
    char alias_buf[SYNCN_STR_MAX];
    char serial_buf[32];
    char local_ip[64];
    char learned_door[64];

    int  listen_fd;
    syncn_responder *responder;
    syncn_ipc *ipc;
    syncn_conn *conn;
    syncn_audio *audio;

    syncn_call_state state;
    bool  running;
    bool  audio_wanted;        /* false for preview   */
    bool  start_talk_sent;
    bool  audio_failed;   /* reported once, not every loop iteration */
    uint64_t next_audio_retry_ms;
    unsigned audio_retry_count;
    uint64_t start_talk_due_ms;
    uint64_t next_stats_ms;

    FILE *video_dump;
    char  video_dump_path[SYNCN_STR_MAX + 32];
    syncn_video *video;

    /* Idle preview: when to next open one. 0 means as soon as we are idle. */
    uint64_t next_preview_ms;
    bool     preview_is_auto;   /* opened by the daemon, not the user */
    unsigned preview_retries;   /* consecutive previews that got no picture */
    bool         video_warned;   /* "cannot display" said once, not per call */
    /* Per-session, reset on teardown. Cumulative counters across calls make it
     * impossible to tell from a stats line whether the current call is healthy. */
    uint64_t video_frames, video_bytes;
};

static const char *door_address(const syncn_call *c);

const char *syncn_call_state_name(syncn_call_state s)
{
    switch (s) {
    case SYNCN_CALL_CONNECTING: return "connecting";
    case SYNCN_CALL_RINGING:    return "ringing";
    case SYNCN_CALL_CONNECTED:  return "connected";
    case SYNCN_CALL_PREVIEW:    return "preview";
    case SYNCN_CALL_IDLE:
    default:                    return "idle";
    }
}

syncn_call_state syncn_call_get_state(const syncn_call *c)
{
    return c ? c->state : SYNCN_CALL_IDLE;
}

/* Push the current state to every attached UI. Called on every transition, and
 * whenever a UI connects or asks — so a UI that starts mid-call shows the call
 * rather than an idle screen. */
static void publish_state(syncn_call *c)
{
    if (!c->ipc)
        return;

    const char *door = door_address(c);
    char line[512];
    snprintf(line, sizeof line,
             "{\"event\":\"state\",\"state\":\"%s\",\"door\":\"%s\","
             "\"muted\":%s,\"audio\":%s,\"video_frames\":%llu,\"preview_auto\":%s,"
             "\"live_view\":%s}",
             syncn_call_state_name(c->state),
             door ? door : "",
             (c->audio && syncn_audio_muted(c->audio)) ? "true" : "false",
             syncn_audio_running(c->audio) ? "true" : "false",
             (unsigned long long)c->video_frames,
             c->preview_is_auto ? "true" : "false",
             c->cfg.video_idle_preview ? "true" : "false");
    syncn_ipc_broadcast(c->ipc, line);
}

static void set_state(syncn_call *c, syncn_call_state s)
{
    if (c->state == s)
        return;
    LOG_INFO("call: %s -> %s", syncn_call_state_name(c->state), syncn_call_state_name(s));
    const bool was_ringing = (c->state == SYNCN_CALL_RINGING);
    c->state = s;
    /*
     * Busy means "do not call me": ringing, connecting, or in a call. A
     * preview is not that. It is the panel looking at the door, and a visitor
     * pressing the button during it must ring here, not at some other unit.
     * With the old rule the door read deviceBusy:1 in our discovery reply
     * during a preview and never even tried to connect.
     */
    c->info.device_busy = (s == SYNCN_CALL_IDLE || s == SYNCN_CALL_PREVIEW) ? 0 : 1;

    /* The ring follows the state. Starting is here; stopping is also done
     * explicitly by answer() and teardown(), before anything else wants the
     * speaker, because this runs after they have already opened it. */
    if (s == SYNCN_CALL_RINGING && c->cfg.ring)
        syncn_ringer_start(c->cfg.playback_device, c->cfg.ring_gain_db);
    else if (was_ringing)
        syncn_ringer_stop();

    publish_state(c);
}

/* --------------------------------------------------------- media plumbing --- */

/* Runs on the audio thread. Only enqueues — no allocation, no logging. */
static void on_uplink_audio(const uint8_t *alaw, size_t len, void *user)
{
    syncn_call *c = user;
    if (c->conn)
        syncn_conn_send(c->conn, SYNCN_CH_AUDIO, alaw, len);
}

/* Build a per-session filename from the configured path, so a later call does
 * not destroy an earlier capture. Losing the only recording of a fault to the
 * next test run is a bad way to spend a bench session. */
static void session_dump_path(const syncn_call *c, char *out, size_t cap)
{
    const char *base = c->cfg.video_dump_path;
    const char *dot  = strrchr(base, '.');
    const size_t stem_len = dot ? (size_t)(dot - base) : strlen(base);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);

    snprintf(out, cap, "%.*s-%s%s", (int)stem_len, base, stamp,
             dot ? dot : ".h264");
}

/* ------------------------------------------------------------- video out --- */

/*
 * Hand a converted frame to the interface.
 *
 * The descriptor travels by SCM_RIGHTS, which duplicates it — the pool keeps
 * its own and the interface gets an independent handle to the same memory. The
 * id is how it comes back: the interface imports once per id, caches the
 * framebuffer, and returns the buffer with {"cmd":"frame_done","id":N} when it
 * has been scanned out.
 *
 * Returning false means nobody took it, and the pipeline puts the buffer
 * straight back rather than losing it to a consumer that was never told.
 */
static bool cb_frame(const syncn_frame_buffer *buf, uint64_t pts_us, void *user)
{
    syncn_call *c = user;

    if (!c->ipc || syncn_ipc_video_client_count(c->ipc) == 0)
        return false;

    char line[256];
    snprintf(line, sizeof line,
             "{\"event\":\"frame\",\"id\":%llu,\"width\":%u,\"height\":%u,"
             "\"stride\":%u,\"size\":%zu,\"format\":\"BGRA8888\",\"pts_us\":%llu}\n",
             (unsigned long long)buf->id, buf->width, buf->height,
             buf->stride, buf->size, (unsigned long long)pts_us);

    return syncn_ipc_send_fd(c->ipc, line, buf->fd);
}

/*
 * The geometry changed, so any framebuffer the interface imported for an
 * earlier id describes memory of the wrong size. Ids are never reused, so an
 * interface that ignores this cannot draw the wrong buffer — it only keeps a
 * cache it will never hit again.
 */
static void cb_video_reset(unsigned width, unsigned height, void *user)
{
    syncn_call *c = user;
    if (!c->ipc)
        return;

    char line[128];
    snprintf(line, sizeof line,
             "{\"event\":\"video_reset\",\"width\":%u,\"height\":%u}\n",
             width, height);
    syncn_ipc_broadcast(c->ipc, line);
}

/* The interface is gone and may be holding buffers it will never return. It
 * keeps its own descriptors, so the memory outlives us reclaiming the slots. */
static void cb_ui_gone(unsigned remaining, void *user)
{
    syncn_call *c = user;
    (void)remaining;
    if (c->video)
        syncn_video_consumer_gone(c->video);
}

static void start_video_pipeline(syncn_call *c)
{
    if (c->video)
        return;

    if (!syncn_video_available()) {
        if (!c->video_warned) {
            c->video_warned = true;
            LOG_WARN("video: cannot be displayed — %s. The call proceeds with "
                     "audio; set [video] present=file to keep the stream.",
                     syncn_video_unavailable_reason());
        }
        return;
    }

    const syncn_video_config vc = { .buffers = 0, .fast_output = true };
    c->video = syncn_video_create(&vc, cb_frame, cb_video_reset, c);
    if (!c->video && !c->video_warned) {
        c->video_warned = true;
        LOG_ERR("video: the display pipeline could not be started");
    }
}

static void stop_video_pipeline(syncn_call *c)
{
    if (!c->video)
        return;

    char line[512];
    syncn_video_format_stats(c->video, line, sizeof line);
    LOG_INFO("%s", line);

    syncn_video_destroy(c->video);
    c->video = NULL;

    if (c->ipc)
        syncn_ipc_broadcast(c->ipc, "{\"event\":\"video_stopped\"}\n");
}

static void open_video_dump(syncn_call *c)
{
    if (c->video_dump)
        return;

    if (strcmp(c->cfg.video_present, "file") != 0) {
        /* 'drm' is the display path and is handled by the pipeline, not here.
         * Anything else means the stream is received and thrown away, which is
         * not obvious from the config file alone — so say it out loud. */
        if (strcmp(c->cfg.video_present, "drm") != 0)
            LOG_WARN("video: present=%s displays and saves nothing — video is "
                     "being received and discarded. Use present=drm to show it "
                     "or present=file to keep it.", c->cfg.video_present);
        return;
    }
    session_dump_path(c, c->video_dump_path, sizeof c->video_dump_path);

    c->video_dump = fopen(c->video_dump_path, "wb");
    if (c->video_dump)
        LOG_INFO("video: writing the door's H.264 stream to %s", c->video_dump_path);
    else
        LOG_WARN("video: cannot open %s: %s", c->video_dump_path, strerror(errno));
}

/*
 * Everything that has to happen when the door's stream starts arriving.
 *
 * Called from three places — an incoming call, a dialled call, and preview —
 * because in all three the picture should already be there by the time anyone
 * looks at the screen. Both halves are independent: a display path that cannot
 * start must not cost us the capture, which is how faults get diagnosed after
 * the fact.
 */
static void video_session_start(syncn_call *c)
{
    open_video_dump(c);
    if (strcmp(c->cfg.video_present, "drm") == 0)
        start_video_pipeline(c);
}

static void close_video_dump(syncn_call *c)
{
    if (!c->video_dump)
        return;
    fclose(c->video_dump);
    c->video_dump = NULL;
    LOG_INFO("video: %llu frames, %llu bytes written to %s",
             (unsigned long long)c->video_frames, (unsigned long long)c->video_bytes,
             c->video_dump_path);
}

/* ----------------------------------------------------------- audio start --- */

static void audio_start(syncn_call *c)
{
    if (c->audio || !c->audio_wanted)
        return;

    syncn_audio_config ac;
    syncn_audio_config_defaults(&ac);
    ac.capture_device   = c->cfg.capture_device;
    ac.playback_device  = c->cfg.playback_device;
    ac.aec              = syncn_aec_backend_from_name(c->cfg.aec);
    ac.aec_tail_ms      = c->cfg.aec_tail_ms;
    ac.aec_delay_ms     = c->cfg.aec_delay_ms;
    ac.duplex           = syncn_duplex_from_name(c->cfg.duplex_mode);
    ac.capture_channels = c->cfg.capture_channels;
    ac.capture_channel  = c->cfg.capture_channel;
    ac.highpass_hz      = c->cfg.highpass_hz;
    ac.mic_gain_db      = c->cfg.mic_gain_db;
    ac.speaker_gain_db  = c->cfg.speaker_gain_db;
    ac.gate_threshold_dbfs = c->cfg.gate_threshold_dbfs;
    ac.gate_hangover_ms    = c->cfg.gate_hangover_ms;
    ac.uplink_ceiling_dbfs = c->cfg.uplink_ceiling_dbfs;
    ac.uplink_release_ms   = c->cfg.uplink_release_ms;
    ac.uplink_noise_floor_dbfs   = c->cfg.uplink_noise_floor_dbfs;
    ac.downlink_noise_floor_dbfs = c->cfg.downlink_noise_floor_dbfs;
    ac.uplink_noise_hangover_ms   = c->cfg.uplink_noise_hangover_ms;
    ac.downlink_noise_hangover_ms = c->cfg.downlink_noise_hangover_ms;
    ac.noise_release_ms   = c->cfg.noise_release_ms;
    ac.comfort_noise_dbfs = c->cfg.comfort_noise_dbfs;
    ac.near_priority_db   = c->cfg.near_priority_db;
    ac.denoise            = syncn_denoise_from_name(c->cfg.denoise);
    ac.denoise_downlink   = syncn_denoise_from_name(c->cfg.denoise_downlink);
    ac.denoise_wet        = (float)c->cfg.denoise_wet / 100.0f;
    ac.aec_denoise        = c->cfg.aec_denoise;
    ac.aec_noise_suppress_db = c->cfg.aec_noise_suppress_db;
    ac.return_cancel      = c->cfg.return_cancel;
    ac.return_tail_ms     = c->cfg.return_tail_ms;
    ac.return_noise_suppress_db = c->cfg.return_noise_suppress_db;
    ac.return_echo_db     = c->cfg.return_echo_db;
    ac.hold_floor_dbfs    = c->cfg.hold_floor_dbfs;
    ac.hold_loud_dbfs     = c->cfg.hold_loud_dbfs;
    ac.lowpass_hz         = c->cfg.lowpass_hz;
    ac.downlink_highpass_hz = c->cfg.downlink_highpass_hz;
    ac.presence_hz        = c->cfg.presence_hz;
    ac.presence_db        = c->cfg.presence_db;
    ac.jitter_target_ms = c->cfg.jitter_target_ms;

    c->audio = syncn_audio_start(&ac, on_uplink_audio, c);
    if (!c->audio) {
        LOG_ERR("call: audio failed to start — the call will be video only");
        c->next_audio_retry_ms = syncn_now_ms() + 500;
        c->audio_retry_count++;
    } else {
        c->next_audio_retry_ms = 0;
        c->audio_retry_count = 0;
        c->audio_failed = false;
    }
}

static void audio_stop(syncn_call *c)
{
    if (!c->audio)
        return;
    syncn_audio_stop(c->audio);
    c->audio = NULL;
}

/* -------------------------------------------------------------- handshake --- */

static void send_answer_sequence(syncn_call *c)
{
    const syncn_outbound_cmd *seq = syncn_answer_sequence();
    for (int i = 0; i < SYNCN_ANSWER_FRAME_COUNT; i++)
        syncn_conn_send_command(c->conn, seq[i]);
    LOG_INFO("call: Answer handshake sent (%d frames, all three spellings)",
             SYNCN_ANSWER_FRAME_COUNT);
}

static void schedule_start_talk(syncn_call *c, bool immediate)
{
    c->start_talk_sent    = false;
    c->start_talk_due_ms  = syncn_now_ms() + (immediate ? 0 : START_TALK_DELAY_MS);
}

/* ------------------------------------------------------------- teardown ---- */

static void teardown(syncn_call *c, const char *why)
{
    if (c->state == SYNCN_CALL_IDLE && !c->conn)
        return;

    LOG_INFO("call: tearing down (%s)", why);
    syncn_ringer_stop();

    /*
     * When to look at the door again.
     *
     * After a call: now. The interface keeps the call's last frame on screen
     * until the preview's first one arrives, so every millisecond here is a
     * frozen picture, not a blank one -- and the door has taken a redial the
     * instant after a hang-up every time we have tried.
     *
     * After a preview that showed nothing: it was refused, most likely by a
     * door still finishing the previous call. Try again in a moment, a few
     * times, before falling back to the slow cadence.
     *
     * After a preview the door dropped: soon; the gap is the only cost.
     * After the user stopping one by hand: a good while, or the button does
     * nothing.
     */
    if (c->cfg.video_idle_preview) {
        const uint64_t now = syncn_now_ms();
        if (c->state != SYNCN_CALL_PREVIEW || c->video_frames > 0)
            c->preview_retries = 0;

        if (c->state != SYNCN_CALL_PREVIEW)
            c->next_preview_ms = now;
        else if (!c->preview_is_auto)
            c->next_preview_ms = now + 30000;
        else if (c->video_frames == 0 && c->preview_retries < PREVIEW_FAST_RETRIES) {
            c->preview_retries++;
            c->next_preview_ms = now + PREVIEW_RETRY_MS;
        } else
            c->next_preview_ms = now + 1500;
    }
    c->preview_is_auto = false;

    audio_stop(c);
    c->audio_wanted = false;
    c->next_audio_retry_ms = 0;
    c->audio_retry_count = 0;
    stop_video_pipeline(c);
    close_video_dump(c);
    c->video_frames = 0;
    c->video_bytes  = 0;

    if (c->conn) {
        syncn_conn_close(c->conn);
        c->conn = NULL;
    }
    c->start_talk_sent = false;
    c->audio_failed    = false;
    set_state(c, SYNCN_CALL_IDLE);
}

/* ---------------------------------------------------- connection callbacks --- */

static void cb_control(syncn_inbound_cmd cmd, const char *raw, size_t len, void *user)
{
    syncn_call *c = user;
    (void)raw; (void)len;

    switch (cmd) {
    case SYNCN_IN_CALL:
        if (c->state == SYNCN_CALL_IDLE || c->state == SYNCN_CALL_CONNECTING) {
            LOG_INFO("call: *** INCOMING CALL from %s ***", syncn_conn_peer(c->conn));
            LOG_INFO("call: press 'a' to answer, 'h' to reject");
            set_state(c, SYNCN_CALL_RINGING);
            if (c->ipc) {
                char line[256];
                snprintf(line, sizeof line,
                         "{\"event\":\"incoming_call\",\"door\":\"%s\"}",
                         syncn_conn_peer(c->conn));
                syncn_ipc_broadcast(c->ipc, line);
            }
            /* Video starts ringing, before the user answers, so the picture is
             * already there the moment they accept. */
            video_session_start(c);
        }
        break;

    case SYNCN_IN_GET_CALL_INFO:
        /* The door re-asks mid-call. Answer with the whole sequence again,
         * verbatim — a partial reply is not accepted by every firmware. */
        if (c->state == SYNCN_CALL_CONNECTED) {
            LOG_INFO("call: door asked getCallInfo — re-sending the Answer sequence");
            send_answer_sequence(c);
        }
        break;

    case SYNCN_IN_HANGUP:
        LOG_INFO("call: the door hung up");
        teardown(c, "door hung up");
        break;

    case SYNCN_IN_UNKNOWN:
    default:
        break; /* ignored, by design */
    }
}

static void cb_audio(const uint8_t *alaw, size_t len, void *user)
{
    syncn_call *c = user;
    if (c->audio)
        syncn_audio_push_downlink(c->audio, alaw, len);
}

static void cb_video(const uint8_t *nal, size_t len, void *user)
{
    syncn_call *c = user;
    c->video_frames++;
    c->video_bytes += len;

    /* Both, independently. A capture is how a fault gets diagnosed after the
     * fact, and the display path must never be the reason there is no
     * recording of one. */
    if (c->video_dump)
        fwrite(nal, 1, len, c->video_dump);

    if (c->video)
        syncn_video_feed(c->video, nal, len);
}

static void cb_closed(const char *reason, void *user)
{
    syncn_call *c = user;
    (void)reason;
    /* Do not tear down from inside the callback — the connection object is
     * still in use further up the stack. The loop notices and cleans up. */
    c->state = (c->state == SYNCN_CALL_IDLE) ? SYNCN_CALL_IDLE : c->state;
}

static const syncn_conn_callbacks CONN_CB = {
    .on_control = cb_control,
    .on_audio   = cb_audio,
    .on_video   = cb_video,
    .on_closed  = cb_closed,
};

/* ----------------------------------------------------------------- actions --- */

static const char *door_address(const syncn_call *c)
{
    if (c->cfg.door_address[0])
        return c->cfg.door_address;
    if (c->learned_door[0])
        return c->learned_door;
    return NULL;
}

bool syncn_call_dial(syncn_call *c)
{
    if (c->state != SYNCN_CALL_IDLE) {
        LOG_WARN("call: already %s", syncn_call_state_name(c->state));
        return false;
    }
    const char *addr = door_address(c);
    if (!addr) {
        LOG_ERR("call: no door address. Set [door] address in the config, or wait "
                "for the door to broadcast so we can learn it.");
        return false;
    }

    c->conn = syncn_conn_dial(addr, c->cfg.door_port, 5000, &CONN_CB, c);
    if (!c->conn)
        return false;

    set_state(c, SYNCN_CALL_CONNECTING);
    c->audio_wanted = true;
    send_answer_sequence(c);
    video_session_start(c);
    schedule_start_talk(c, false);
    audio_start(c);
    set_state(c, SYNCN_CALL_CONNECTED);
    return true;
}

bool syncn_call_preview(syncn_call *c)
{
    if (c->state != SYNCN_CALL_IDLE) {
        LOG_WARN("call: already %s", syncn_call_state_name(c->state));
        return false;
    }
    const char *addr = door_address(c);
    if (!addr) {
        LOG_ERR("call: no door address for preview");
        return false;
    }

    c->conn = syncn_conn_dial(addr, c->cfg.door_port, 5000, &CONN_CB, c);
    if (!c->conn)
        return false;

    c->audio_wanted = false;
    send_answer_sequence(c);
    video_session_start(c);
    /* Immediately, and without starting audio: it is a wire trigger, not a
     * request to talk. */
    schedule_start_talk(c, true);
    set_state(c, SYNCN_CALL_PREVIEW);
    return true;
}

void syncn_call_answer(syncn_call *c)
{
    if (c->state != SYNCN_CALL_RINGING) {
        LOG_WARN("call: nothing is ringing");
        return;
    }
    /* The ringer holds the speaker. It has to have let go before the audio
     * engine opens the device, and stop() returns only once it has. */
    syncn_ringer_stop();

    c->audio_wanted = true;
    send_answer_sequence(c);
    schedule_start_talk(c, false);
    audio_start(c);
    set_state(c, SYNCN_CALL_CONNECTED);
}

void syncn_call_hangup(syncn_call *c)
{
    if (c->state == SYNCN_CALL_IDLE) {
        LOG_WARN("call: no call in progress");
        return;
    }
    if (c->conn)
        syncn_conn_send_command(c->conn, SYNCN_OUT_HANGUP);
    teardown(c, "local hangup");
}

void syncn_call_unlock(syncn_call *c)
{
    if (c->conn) {
        /* Ride the socket we already have. */
        syncn_conn_send_command(c->conn, SYNCN_OUT_OPEN_DOOR);
        LOG_INFO("call: OpenDoor sent on the active connection");
        return;
    }

    /*
     * Standalone unlock: no call, no open socket. Open a short-lived blocking
     * connection, send the handshake then OpenDoor, and close. It never enters
     * the call state machine — this is a fire-and-forget command.
     */
    const char *addr = door_address(c);
    if (!addr) {
        LOG_ERR("call: no door address to unlock");
        return;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERR("call: socket: %s", strerror(errno));
        return;
    }
    struct sockaddr_in a = { .sin_family = AF_INET,
                             .sin_port   = htons((uint16_t)c->cfg.door_port) };
    if (inet_pton(AF_INET, addr, &a.sin_addr) != 1) {
        LOG_ERR("call: bad door address '%s'", addr);
        close(fd);
        return;
    }

    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        LOG_ERR("call: connect to door for unlock: %s", strerror(errno));
        close(fd);
        return;
    }

    uint8_t buf[512];
    size_t n = 0;
    const syncn_outbound_cmd *seq = syncn_answer_sequence();
    for (int i = 0; i < SYNCN_ANSWER_FRAME_COUNT; i++)
        n += syncn_command_frame(seq[i], buf + n, sizeof buf - n);
    n += syncn_command_frame(SYNCN_OUT_OPEN_DOOR, buf + n, sizeof buf - n);

    if (send(fd, buf, n, MSG_NOSIGNAL) == (ssize_t)n)
        LOG_INFO("call: standalone unlock sent to %s", addr);
    else
        LOG_ERR("call: standalone unlock failed: %s", strerror(errno));

    close(fd);
}

void syncn_call_toggle_mute(syncn_call *c)
{
    if (!c->audio) {
        LOG_WARN("call: audio is not running");
        return;
    }
    syncn_audio_set_mute(c->audio, !syncn_audio_muted(c->audio));
}

static void ipc_command(const syncn_ipc_message *msg, void *user)
{
    syncn_call *c = user;

    switch (msg->cmd) {
    case SYNCN_IPC_CMD_ANSWER:        syncn_call_answer(c);  break;
    case SYNCN_IPC_CMD_CALL:          syncn_call_dial(c);    break;
    case SYNCN_IPC_CMD_PREVIEW_START: syncn_call_preview(c); break;
    case SYNCN_IPC_CMD_UNLOCK:        syncn_call_unlock(c);  break;

    case SYNCN_IPC_CMD_REJECT:
    case SYNCN_IPC_CMD_HANGUP:
    case SYNCN_IPC_CMD_PREVIEW_STOP:
        /* Reject, hang up and stop-preview are the same wire action: tell the
         * door we are done and tear the connection down. */
        syncn_call_hangup(c);
        break;

    case SYNCN_IPC_CMD_SET_MUTE:
        if (c->audio) {
            syncn_audio_set_mute(c->audio, msg->flag);
            publish_state(c);
        }
        break;

    case SYNCN_IPC_CMD_FRAME_DONE:
        /* The interface has scanned this buffer out. Until it says so the
         * buffer cannot be written again without tearing, and the pool has
         * only a few — so a UI that stops sending these stops the picture,
         * visibly and with a log line, rather than wedging the daemon. */
        if (c->video)
            syncn_video_recycle(c->video, msg->id);
        break;

    case SYNCN_IPC_CMD_GET_STATE:
        publish_state(c);
        break;

    case SYNCN_IPC_CMD_UNKNOWN:
    default:
        break;
    }
}

/* ------------------------------------------------------------- lifecycle --- */

syncn_call *syncn_call_create(const syncn_config *cfg)
{
    syncn_call *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->cfg = *cfg;

    syncn_screen_info_init(&c->info);
    c->info.appid    = c->cfg.appid;
    c->info.group_ip = c->cfg.group_ip;
    if (c->cfg.serial[0]) {
        c->info.serial = c->cfg.serial;
    } else if (syncn_local_mac_hex(c->serial_buf, sizeof c->serial_buf)) {
        /* The door's own indoor units identify themselves by MAC — the unit at
         * .160 advertises "fa72a237a18d". A blank serial is not a neutral
         * default: it is an indoor unit with no identity, which is the leading
         * suspicion for why this door accepts our call and then plays nothing.
         * Derive one rather than ship a blank. */
        c->info.serial = c->serial_buf;
        LOG_INFO("call: serial derived from MAC: %s", c->serial_buf);
    } else {
        c->info.serial = c->cfg.serial;
        LOG_WARN("call: no serial configured and no MAC found — advertising a blank serial");
    }

    if (c->cfg.alias[0]) {
        c->info.alias = c->cfg.alias;
    } else {
        /* Blank alias means "use the hostname" — one less thing to commission. */
        if (gethostname(c->alias_buf, sizeof c->alias_buf) != 0)
            snprintf(c->alias_buf, sizeof c->alias_buf, "SyncN Panel");
        c->info.alias = c->alias_buf;
    }

    if (syncn_local_ipv4(c->local_ip, sizeof c->local_ip)) {
        c->info.local_ip = c->local_ip;
        LOG_INFO("call: panel address %s", c->local_ip);
    } else {
        LOG_WARN("call: no local IPv4 found — the discovery reply will omit ip/localIp");
    }

    c->listen_fd = syncn_listen_tcp(c->cfg.listen_port ? c->cfg.listen_port : c->cfg.door_port);
    if (c->listen_fd < 0) {
        free(c);
        return NULL;
    }

    c->responder = syncn_responder_start(&c->info);
    if (!c->responder)
        LOG_WARN("call: discovery responder unavailable — the door may not find us");

    /* Not fatal: with no control socket the daemon still takes calls, it just
     * has no user interface attached. */
    c->ipc = syncn_ipc_start(NULL, ipc_command, c);
    if (!c->ipc)
        LOG_WARN("call: no control socket — the UI will not be able to connect");
    else
        syncn_ipc_set_disconnect_handler(c->ipc, cb_ui_gone);

    return c;
}

void syncn_call_destroy(syncn_call *c)
{
    if (!c)
        return;
    teardown(c, "shutting down");
    if (c->listen_fd >= 0)
        close(c->listen_fd);
    syncn_ipc_stop(c->ipc);
    syncn_responder_stop(c->responder);
    free(c);
}

void syncn_call_stop(syncn_call *c) { if (c) c->running = false; }

static void accept_pending(syncn_call *c)
{
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        const int fd = accept(c->listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0)
            return;

        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);

        if (c->conn && c->state == SYNCN_CALL_PREVIEW) {
            /*
             * The door is calling while we are watching it. A call beats a
             * look: drop the preview session and take the call on this new
             * connection, which then rings exactly as it would from idle.
             */
            LOG_INFO("call: door connected from %s during a preview — ending the preview to take the call", ip);
            teardown(c, "call arriving during preview");
            c->next_preview_ms = 0;
        }

        if (c->conn) {
            /*
             * A second door while we are busy. Tell it so and drop the socket
             * immediately — leaving it open would have it waiting for media
             * that is never coming.
             */
            LOG_INFO("call: second connection from %s while busy — sending deviceBusy", ip);
            uint8_t buf[128];
            const size_t n = syncn_command_frame(SYNCN_OUT_DEVICE_BUSY, buf, sizeof buf);
            send(fd, buf, n, MSG_NOSIGNAL);
            close(fd);
            continue;
        }

        LOG_INFO("call: door connected from %s", ip);
        if (!c->learned_door[0])
            snprintf(c->learned_door, sizeof c->learned_door, "%s", ip);

        char peer_name[64];
        snprintf(peer_name, sizeof peer_name, "%s:%u", ip, (unsigned)ntohs(peer.sin_port));
        c->conn = syncn_conn_adopt(fd, peer_name, &CONN_CB, c);
    }
}

static void report_stats(syncn_call *c)
{
    syncn_conn_stats ns;
    memset(&ns, 0, sizeof ns);
    if (c->conn)
        syncn_conn_get_stats(c->conn, &ns);

    syncn_audio_stats as;
    syncn_audio_get_stats(c->audio, &as);

    LOG_INFO("stats: %s | net in=%llu out=%llu drop=%llu resync=%llu outbox=%u",
             syncn_call_state_name(c->state),
             (unsigned long long)ns.frames_in, (unsigned long long)ns.frames_out,
             (unsigned long long)ns.dropped_frames, (unsigned long long)ns.resyncs,
             ns.outbox_depth);

    if (c->video) {
        /* On its own line and in full. A frozen picture is diagnosed by which
         * counter is moving: au climbing with shown flat and buffer= rising
         * means the interface stopped returning buffers, which is a different
         * fault from decoded staying at zero. */
        char vline[512];
        syncn_video_format_stats(c->video, vline, sizeof vline);
        LOG_INFO("stats: %s", vline);
    }

    if (c->audio)
        LOG_INFO("stats: audio ticks=%llu rx=%llu under=%llu drop=%llu "
                 "xrun c/p=%llu/%llu late=%llu cpu=%.0f/%.0f%% jitter=%u(%llu trimmed) gated=%llu near_won=%llu spk_muted=%llu echo=%llu ret=%.0f/%.0fdB(%llu) held=%llu "
                 "aec=%s out/far=%.3f duplex=%s mic=%.1fdBFS(%s) near=%.1fdBFS uplink=%.1fdBFS far=%.1fdBFS(raw %.1f) clip=%llu sat=%llu lim=%llu ng_up=%llu ng_dn=%llu",
                 (unsigned long long)as.ticks,
                 (unsigned long long)as.downlink_received,
                 (unsigned long long)as.downlink_underruns,
                 (unsigned long long)as.downlink_dropped,
                 (unsigned long long)as.xruns_capture,
                 (unsigned long long)as.xruns_playback,
                 (unsigned long long)as.late_resyncs,
                 (double)as.cpu_avg_pct, (double)as.cpu_max_pct,
                 as.jitter_depth, (unsigned long long)as.downlink_trimmed, (unsigned long long)as.gated_ticks,
                 (unsigned long long)as.near_won_ticks,
                 (unsigned long long)as.speaker_muted_ticks,
                 (unsigned long long)as.echo_muted_ticks,
                 (double)as.return_total_db, (double)as.return_linear_db,
                 (unsigned long long)as.return_judged,
                 (unsigned long long)as.held_ticks,
                 syncn_aec_backend_name(as.aec_backend), (double)as.aec_residual,
                 syncn_duplex_name(as.duplex),
                 (double)as.mic_dbfs, syncn_channel_mode_name(as.capture_channel),
                 (double)as.near_dbfs, (double)as.uplink_dbfs, (double)as.far_dbfs,
                 (double)as.far_raw_dbfs,
                 (unsigned long long)as.clipped_samples,
                 (unsigned long long)as.saturated_samples,
                 (unsigned long long)as.limited_samples,
                 (unsigned long long)as.uplink_ng_frames,
                 (unsigned long long)as.downlink_ng_frames);

    if (c->video_frames)
        LOG_INFO("stats: video %llu NALs, %llu bytes",
                 (unsigned long long)c->video_frames, (unsigned long long)c->video_bytes);

    if (c->ipc) {
        char line[512];
        snprintf(line, sizeof line,
                 "{\"event\":\"stats\",\"state\":\"%s\",\"audio_rx\":%llu,"
                 "\"audio_ticks\":%llu,\"underruns\":%llu,\"xruns\":%llu,"
                 "\"aec\":\"%s\",\"residual\":%.3f,\"duplex\":\"%s\","
                 "\"video_frames\":%llu,\"net_drop\":%llu,\"mic_dbfs\":%.1f}",
                 syncn_call_state_name(c->state),
                 (unsigned long long)as.downlink_received,
                 (unsigned long long)as.ticks,
                 (unsigned long long)as.downlink_underruns,
                 (unsigned long long)(as.xruns_capture + as.xruns_playback),
                 syncn_aec_backend_name(as.aec_backend), (double)as.aec_residual,
                 syncn_duplex_name(as.duplex),
                 (unsigned long long)c->video_frames,
                 (unsigned long long)ns.dropped_frames,
                 (double)as.mic_dbfs);
        syncn_ipc_broadcast(c->ipc, line);
    }
}

static void handle_key(syncn_call *c, int ch)
{
    switch (ch) {
    case 'c': syncn_call_dial(c);        break;
    case 'p': syncn_call_preview(c);     break;
    case 'a': syncn_call_answer(c);      break;
    case 'h': syncn_call_hangup(c);      break;
    case 'u': syncn_call_unlock(c);      break;
    case 'm': syncn_call_toggle_mute(c); break;
    case 's': report_stats(c);           break;
    case 'q': syncn_call_stop(c);        break;
    case '?':
        LOG_INFO("keys: c=call  p=preview  a=answer  h=hangup  u=unlock  "
                 "m=mute  s=stats  q=quit");
        break;
    default:
        break;
    }
}

void syncn_call_run(syncn_call *c, bool interactive)
{
    c->running = true;
    c->next_stats_ms = syncn_now_ms() + STATS_INTERVAL_MS;

    if (interactive)
        LOG_INFO("keys: c=call  p=preview  a=answer  h=hangup  u=unlock  "
                 "m=mute  s=stats  q=quit");

    while (c->running) {
        struct pollfd pfd[8 + SYNCN_IPC_MAX_CLIENTS];
        int n = 0;

        const int idx_listen = n;
        pfd[n].fd = c->listen_fd; pfd[n].events = POLLIN; pfd[n].revents = 0; n++;

        int idx_resp = -1;
        if (c->responder) {
            idx_resp = n;
            pfd[n].fd = syncn_responder_fd(c->responder);
            pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
        }

        int idx_conn = -1;
        if (c->conn) {
            idx_conn = n;
            pfd[n].fd = syncn_conn_fd(c->conn);
            pfd[n].events = syncn_conn_events(c->conn);
            pfd[n].revents = 0; n++;
        }

        int idx_stdin = -1;
        if (interactive) {
            idx_stdin = n;
            pfd[n].fd = 0; pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
        }

        const int idx_ipc = n;
        const int n_ipc = syncn_ipc_poll_fds(c->ipc, pfd + n,
                                             (int)(sizeof pfd / sizeof pfd[0]) - n);
        n += n_ipc;

        /* Short timeout: the StartTalk deadline is a one-second target and the
         * outbox needs draining promptly. */
        const int pr = poll(pfd, (nfds_t)n, 20);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            LOG_ERR("call: poll: %s", strerror(errno));
            break;
        }

        if (pfd[idx_listen].revents & POLLIN)
            accept_pending(c);

        if (idx_resp >= 0 && (pfd[idx_resp].revents & POLLIN))
            syncn_responder_handle(c->responder, c->learned_door, sizeof c->learned_door);

        if (idx_conn >= 0 && pfd[idx_conn].revents) {
            if (!syncn_conn_handle(c->conn, pfd[idx_conn].revents))
                teardown(c, "connection lost");
        }

        if (n_ipc > 0)
            syncn_ipc_handle(c->ipc, pfd + idx_ipc, n_ipc);

        if (idx_stdin >= 0 && (pfd[idx_stdin].revents & POLLIN)) {
            char buf[64];
            const ssize_t rn = read(0, buf, sizeof buf);
            for (ssize_t i = 0; i < rn; i++)
                handle_key(c, buf[i]);
        }

        /*
         * Convert and publish whatever the decoder has ready.
         *
         * Every pass, unconditionally, and it never blocks: with nothing ready
         * it returns immediately, and with the interface holding every buffer
         * it drops the frame rather than waiting. Putting it here rather than
         * on its own thread is deliberate — video has no hard deadline the way
         * audio does, and a path with no locks has no locks to get wrong.
         */
        if (c->video)
            syncn_video_pump(c->video);

        /* The send-stuck detector. */
        if (c->conn && !syncn_conn_tick(c->conn))
            teardown(c, "send stalled");

        /* StartTalk, once the delay has elapsed. */
        if (c->conn && !c->start_talk_sent && c->start_talk_due_ms &&
            syncn_now_ms() >= c->start_talk_due_ms &&
            (c->state == SYNCN_CALL_CONNECTED || c->state == SYNCN_CALL_PREVIEW)) {
            syncn_conn_send_command(c->conn, SYNCN_OUT_START_TALK);
            c->start_talk_sent = true;
            LOG_INFO("call: StartTalk sent — the door should begin streaming now");
        }

        /* An engine that has died mid-call must reach the UI, or the user
         * talks to nobody and blames the door. */
        if (c->audio && !syncn_audio_running(c->audio) && !c->audio_failed) {
            c->audio_failed = true;
            LOG_ERR("call: the audio engine has stopped — the call continues "
                    "with video only");
            publish_state(c);
            /* Release the failed ALSA handles before the retry. Without this,
             * audio_start() sees a non-NULL engine and silently refuses to
             * recreate it for the remainder of the call. */
            syncn_audio_stop(c->audio);
            c->audio = NULL;
            c->next_audio_retry_ms = syncn_now_ms() + 500;
            c->audio_retry_count++;
        }

        /* ALSA can still be busy for a short interval after the previous
         * session closes. Retry a failed audio open while the call remains
         * connected instead of permanently accepting a video-only call. */
        if (c->audio_wanted &&
            c->state == SYNCN_CALL_CONNECTED && !c->audio &&
            syncn_now_ms() >= c->next_audio_retry_ms &&
            c->audio_retry_count < 6) {
            LOG_WARN("call: retrying audio start (%u/6)", c->audio_retry_count + 1);
            audio_start(c);
            if (c->audio)
                publish_state(c);
        }

        if (syncn_now_ms() >= c->next_stats_ms) {
            c->next_stats_ms = syncn_now_ms() + STATS_INTERVAL_MS;
            if (c->state != SYNCN_CALL_IDLE)
                report_stats(c);
        }

        /* Idle preview: the door's picture on screen whenever nothing else is
         * happening. A failed dial right after a call is retried in a moment
         * (the door may still be closing the last session); one with no
         * address yet, or a door that stays unreachable, backs off ten
         * seconds rather than hammering. */
        if (c->cfg.video_idle_preview && c->state == SYNCN_CALL_IDLE && !c->conn &&
            syncn_now_ms() >= c->next_preview_ms) {
            if (syncn_call_preview(c)) {
                c->preview_is_auto = true;
                publish_state(c);
            } else if (c->preview_retries < PREVIEW_FAST_RETRIES) {
                c->preview_retries++;
                c->next_preview_ms = syncn_now_ms() + PREVIEW_RETRY_MS;
            } else {
                c->next_preview_ms = syncn_now_ms() + 10000;
            }
        }
    }

    teardown(c, "stopping");
}
