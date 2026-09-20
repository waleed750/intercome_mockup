/*
 * Door station simulator.
 *
 * Speaks the door side of the intercom protocol so the panel can be developed
 * and tested without the real hardware — and, just as usefully, so the panel's
 * behaviour can be *checked*. The simulator enforces the two rules that real
 * door firmware enforces and that are easy to get wrong:
 *
 *   1. It streams nothing until it has received BOTH the three-frame Answer
 *      handshake AND a StartTalk. A panel that skips either gets silence, the
 *      same way the real door gives silence.
 *
 *   2. It gates its downlink audio on a continuous uplink. If the panel stops
 *      sending audio frames for longer than the grace period, the simulator
 *      stops sending audio back and says so. This is the behaviour that makes
 *      the panel's silence-frame watchdog load-bearing.
 *
 * See docs/10-protocol-reference.md.
 */
#include "syncn/alaw.h"
#include "syncn/command.h"
#include "syncn/discovery.h"
#include "syncn/frame.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ----------------------------------------------------------------- util --- */

static volatile sig_atomic_t g_stop = 0;
static bool g_verbose = false;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void logf_(const char *level, const char *fmt, ...)
{
    static uint64_t t0 = 0;
    if (!t0)
        t0 = now_ms();

    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%7.3f] %-5s ", (double)(now_ms() - t0) / 1000.0, level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

#define LOG(...)   logf_("info", __VA_ARGS__)
#define WARN(...)  logf_("WARN", __VA_ARGS__)
#define VERB(...)  do { if (g_verbose) logf_("dbg", __VA_ARGS__); } while (0)

static int set_nonblock(int fd)
{
    const int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* -------------------------------------------------------------- outbox --- */

/* A plain growable send buffer. The simulator is a test tool, so clarity beats
 * the zero-copy gymnastics the daemon itself needs. */
typedef struct {
    uint8_t *buf;
    size_t   cap, len, sent;
    uint64_t dropped_bytes;
} outbox;

static bool outbox_push(outbox *o, const void *data, size_t n)
{
    /* Cap it so a stalled peer cannot grow this without bound. */
    if (o->len - o->sent > 4u * 1024u * 1024u) {
        o->dropped_bytes += n;
        return false;
    }
    if (o->len + n > o->cap) {
        size_t want = o->cap ? o->cap : 8192;
        while (want < o->len + n)
            want *= 2;
        uint8_t *nb = realloc(o->buf, want);
        if (!nb)
            return false;
        o->buf = nb;
        o->cap = want;
    }
    memcpy(o->buf + o->len, data, n);
    o->len += n;
    return true;
}

static bool outbox_pending(const outbox *o) { return o->sent < o->len; }

/* Returns false if the socket died. */
static bool outbox_flush(outbox *o, int fd)
{
    while (o->sent < o->len) {
        const ssize_t n = send(fd, o->buf + o->sent, o->len - o->sent, MSG_NOSIGNAL);
        if (n > 0) {
            o->sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return true; /* try again on the next POLLOUT */
        return false;
    }
    o->sent = o->len = 0;
    return true;
}

static void outbox_free(outbox *o) { free(o->buf); memset(o, 0, sizeof *o); }

/* --------------------------------------------------------- media sources --- */

/* A tone, so you can actually hear whether downlink audio is arriving and
 * whether it is clean. Silence would tell you much less. */
typedef struct {
    double phase;
    double step;
    double amplitude;
} tone_gen;

static void tone_init(tone_gen *t, double hz, double amplitude)
{
    t->phase     = 0.0;
    t->step      = 2.0 * M_PI * hz / (double)SYNCN_AUDIO_RATE;
    t->amplitude = amplitude;
}

static void tone_fill_alaw(tone_gen *t, uint8_t *out, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        const double s = sin(t->phase) * t->amplitude * 32767.0;
        t->phase += t->step;
        if (t->phase > 2.0 * M_PI)
            t->phase -= 2.0 * M_PI;
        out[i] = syncn_alaw_encode_sample((int16_t)s);
    }
}

/* An Annex-B H.264 file, split into individual NAL units. The protocol carries
 * exactly one NAL per video frame, so the split has to happen here rather than
 * at the socket. */
typedef struct {
    uint8_t *data;
    size_t   size;
    struct { size_t off, len; } *nals;
    size_t   count, next;
} nal_source;

static bool is_start_code(const uint8_t *p, size_t remaining, size_t *sc_len)
{
    if (remaining >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
        *sc_len = 4;
        return true;
    }
    if (remaining >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) {
        *sc_len = 3;
        return true;
    }
    return false;
}

static bool nal_source_load(nal_source *ns, const char *path)
{
    memset(ns, 0, sizeof *ns);

    FILE *f = fopen(path, "rb");
    if (!f) {
        WARN("cannot open %s: %s", path, strerror(errno));
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        WARN("%s is empty", path);
        return false;
    }

    ns->data = malloc((size_t)size);
    if (!ns->data || fread(ns->data, 1, (size_t)size, f) != (size_t)size) {
        free(ns->data);
        fclose(f);
        WARN("cannot read %s", path);
        return false;
    }
    fclose(f);
    ns->size = (size_t)size;

    /* Index every NAL: from just after one start code to just before the next.
     * The start code itself is included, because the panel feeds Annex-B
     * straight into the decoder. */
    size_t cap = 256;
    ns->nals = malloc(cap * sizeof *ns->nals);
    if (!ns->nals) {
        free(ns->data);
        return false;
    }

    size_t i = 0, sc = 0;
    while (i < ns->size && !is_start_code(ns->data + i, ns->size - i, &sc))
        i++;

    while (i < ns->size) {
        const size_t start = i;
        i += sc;
        size_t next_sc = 0;
        while (i < ns->size && !is_start_code(ns->data + i, ns->size - i, &next_sc))
            i++;

        if (ns->count == cap) {
            cap *= 2;
            void *nb = realloc(ns->nals, cap * sizeof *ns->nals);
            if (!nb)
                break;
            ns->nals = nb;
        }
        ns->nals[ns->count].off = start;
        ns->nals[ns->count].len = i - start;
        ns->count++;
        sc = next_sc;
    }

    LOG("loaded %s: %zu bytes, %zu NAL units", path, ns->size, ns->count);
    return ns->count > 0;
}

static const uint8_t *nal_source_next(nal_source *ns, size_t *len)
{
    if (ns->count == 0)
        return NULL;
    if (ns->next >= ns->count)
        ns->next = 0; /* loop the clip */
    const size_t idx = ns->next++;
    *len = ns->nals[idx].len;
    return ns->data + ns->nals[idx].off;
}

static void nal_source_free(nal_source *ns)
{
    free(ns->data);
    free(ns->nals);
    memset(ns, 0, sizeof *ns);
}

/* ------------------------------------------------------------ call state --- */

typedef struct {
    int    answers_seen;      /* frames of the Answer handshake received      */
    bool   start_talk_seen;
    bool   streaming;         /* both preconditions met                       */
    bool   peer_closed;

    uint64_t last_uplink_audio_ms;
    bool     downlink_gated;  /* we stopped sending because uplink went quiet */

    uint64_t audio_rx, audio_tx, video_tx, control_rx;
    uint64_t opendoor_count;

    outbox      out;
    tone_gen    tone;
    nal_source *video;
    bool        send_audio;
    int         fps;
} call_state;

/* How long the panel may go without sending an audio frame before the door
 * stops sending audio back. The real door does this; reproducing it here is
 * what makes the panel's watchdog testable. */
#define UPLINK_GRACE_MS 400

static int on_frame(syncn_channel ch, const uint8_t *payload, size_t len, void *user)
{
    call_state *cs = user;

    switch (ch) {
    case SYNCN_CH_CONTROL: {
        cs->control_rx++;
        char text[256];
        const size_t n = len < sizeof text - 1 ? len : sizeof text - 1;
        memcpy(text, payload, n);
        text[n] = '\0';
        LOG("<- control: %s", text);

        /* The simulator is the door, so it receives the panel's outbound
         * commands. Recognise them by their literal payloads. */
        if (strstr(text, "\"Answer\"")) {
            cs->answers_seen++;
            if (cs->answers_seen == SYNCN_ANSWER_FRAME_COUNT)
                LOG("   Answer handshake complete (%d frames)", cs->answers_seen);
        } else if (strstr(text, "\"StartTalk\"")) {
            cs->start_talk_seen = true;
        } else if (strstr(text, "\"OpenDoor\"")) {
            cs->opendoor_count++;
            LOG("   *** DOOR UNLOCKED *** (relay would fire now)");
        } else if (strstr(text, "\"HangUp\"")) {
            LOG("   panel hung up");
            cs->peer_closed = true;
            return 1;
        } else if (strstr(text, "\"deviceBusy\"")) {
            LOG("   panel says it is busy");
            cs->peer_closed = true;
            return 1;
        }

        /* Real door firmware waits for both before it streams anything. A panel
         * that sends only the handshake, or only StartTalk, must get silence. */
        if (!cs->streaming &&
            cs->answers_seen >= SYNCN_ANSWER_FRAME_COUNT && cs->start_talk_seen) {
            cs->streaming = true;
            cs->last_uplink_audio_ms = now_ms();
            LOG("   preconditions met -> streaming media");
        }
        break;
    }

    case SYNCN_CH_AUDIO:
        cs->audio_rx++;
        cs->last_uplink_audio_ms = now_ms();
        if (cs->downlink_gated) {
            cs->downlink_gated = false;
            LOG("   uplink resumed -> downlink audio ungated");
        }
        if (len != SYNCN_AUDIO_FRAME_BYTES)
            WARN("uplink audio frame is %zu bytes, expected %u", len, SYNCN_AUDIO_FRAME_BYTES);
        VERB("<- audio %zu bytes (total %llu)", len, (unsigned long long)cs->audio_rx);
        break;

    case SYNCN_CH_VIDEO:
        /* camera_en is 0 — the panel should never send video. */
        WARN("panel sent a video frame (%zu bytes); it declares camera_en=0", len);
        break;

    default:
        WARN("frame on unknown channel 0x%02X", (unsigned)ch);
        break;
    }
    return 0;
}

static void queue_frame(call_state *cs, syncn_channel ch, const void *payload, size_t len)
{
    uint8_t hdr[SYNCN_FRAME_HEADER_SIZE];
    syncn_frame_write_header(hdr, ch, (uint32_t)len);
    outbox_push(&cs->out, hdr, sizeof hdr);
    if (len)
        outbox_push(&cs->out, payload, len);
}

static void queue_command(call_state *cs, const char *json)
{
    queue_frame(cs, SYNCN_CH_CONTROL, json, strlen(json));
    LOG("-> control: %s", json);
}

/* ------------------------------------------------------------- the loop --- */

typedef struct {
    int         port;         /* call port; default SYNCN_CALL_PORT */
    const char *h264_path;
    double      tone_hz;
    int         fps;
    bool        send_audio;
    int         hangup_after_s;
    int         getcallinfo_after_s;
    bool        announce_call;   /* send {"command":"call"} on connect */
} sim_options;

static int run_session(int fd, const sim_options *opt, nal_source *video)
{
    set_nonblock(fd);
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    call_state cs;
    memset(&cs, 0, sizeof cs);
    cs.video      = video;
    cs.send_audio = opt->send_audio;
    cs.fps        = opt->fps;
    tone_init(&cs.tone, opt->tone_hz, 0.30);

    syncn_parser *parser = syncn_parser_new(0);
    if (!parser)
        return -1;

    if (opt->announce_call)
        queue_command(&cs, "{\"command\":\"call\"}");

    const uint64_t started   = now_ms();
    uint64_t next_audio_ms   = started;
    uint64_t next_video_ms   = started;
    uint64_t next_report_ms  = started + 5000;
    bool     getcallinfo_done = false;
    int      rc = 0;

    uint8_t rx[8192];
    uint8_t audio_frame[SYNCN_AUDIO_FRAME_BYTES];

    while (!g_stop && !cs.peer_closed) {
        struct pollfd pfd = {
            .fd      = fd,
            .events  = POLLIN | (outbox_pending(&cs.out) ? POLLOUT : 0),
            .revents = 0,
        };

        /* Wake for whichever tick is due first, but never sleep so long that a
         * 20 ms audio deadline slips. */
        int timeout = 5;
        if (poll(&pfd, 1, timeout) < 0) {
            if (errno == EINTR)
                continue;
            WARN("poll: %s", strerror(errno));
            rc = -1;
            break;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            LOG("connection closed by peer");
            break;
        }

        if (pfd.revents & POLLIN) {
            const ssize_t n = recv(fd, rx, sizeof rx, 0);
            if (n == 0) {
                LOG("panel closed the connection");
                break;
            }
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    WARN("recv: %s", strerror(errno));
                    break;
                }
            } else if (syncn_parser_feed(parser, rx, (size_t)n, on_frame, &cs) != 0) {
                break; /* callback asked us to stop, or allocation failed */
            }
        }

        const uint64_t t = now_ms();

        /* Reproduce the door's own gating: no uplink, no downlink audio. */
        if (cs.streaming && !cs.downlink_gated &&
            t - cs.last_uplink_audio_ms > UPLINK_GRACE_MS) {
            cs.downlink_gated = true;
            WARN("no uplink audio for %d ms -> gating downlink audio, as the real door does",
                 UPLINK_GRACE_MS);
        }

        if (cs.streaming && cs.send_audio && !cs.downlink_gated) {
            while (t >= next_audio_ms) {
                tone_fill_alaw(&cs.tone, audio_frame, sizeof audio_frame);
                queue_frame(&cs, SYNCN_CH_AUDIO, audio_frame, sizeof audio_frame);
                cs.audio_tx++;
                next_audio_ms += SYNCN_AUDIO_FRAME_MS;
                /* If we fell badly behind, resynchronise rather than burst. */
                if (t > next_audio_ms + 200)
                    next_audio_ms = t;
            }
        }

        if (cs.streaming && cs.video && cs.video->count) {
            const uint64_t period = (uint64_t)(1000 / (cs.fps > 0 ? cs.fps : 15));
            while (t >= next_video_ms) {
                size_t len = 0;
                const uint8_t *nal = nal_source_next(cs.video, &len);
                if (nal) {
                    queue_frame(&cs, SYNCN_CH_VIDEO, nal, len);
                    cs.video_tx++;
                }
                next_video_ms += period;
                if (t > next_video_ms + 500)
                    next_video_ms = t;
            }
        }

        /* Optional: ask for call info mid-call. A correct panel answers by
         * re-sending the whole three-frame handshake. */
        if (opt->getcallinfo_after_s > 0 && !getcallinfo_done &&
            t - started > (uint64_t)opt->getcallinfo_after_s * 1000) {
            getcallinfo_done = true;
            const int before = cs.answers_seen;
            queue_command(&cs, "{\"command\":\"getCallInfo\"}");
            LOG("   expecting %d more Answer frames (have %d)",
                SYNCN_ANSWER_FRAME_COUNT, before);
        }

        if (opt->hangup_after_s > 0 &&
            t - started > (uint64_t)opt->hangup_after_s * 1000) {
            queue_command(&cs, "{\"command\":\"hangUp\"}");
            outbox_flush(&cs.out, fd);
            LOG("hanging up after %d s", opt->hangup_after_s);
            break;
        }

        if (outbox_pending(&cs.out) && !outbox_flush(&cs.out, fd)) {
            WARN("send failed: %s", strerror(errno));
            break;
        }

        if (t >= next_report_ms) {
            next_report_ms = t + 5000;
            syncn_parser_stats st;
            syncn_parser_stats_get(parser, &st);
            LOG("stats: audio rx=%llu tx=%llu | video tx=%llu | control rx=%llu | "
                "frames=%llu resync=%llu buffered=%zu",
                (unsigned long long)cs.audio_rx, (unsigned long long)cs.audio_tx,
                (unsigned long long)cs.video_tx, (unsigned long long)cs.control_rx,
                (unsigned long long)st.frames_parsed,
                (unsigned long long)st.resync_events, st.buffered);
        }
    }

    LOG("session over: audio rx=%llu tx=%llu, video tx=%llu, unlocks=%llu",
        (unsigned long long)cs.audio_rx, (unsigned long long)cs.audio_tx,
        (unsigned long long)cs.video_tx, (unsigned long long)cs.opendoor_count);

    if (!cs.streaming)
        WARN("never started streaming: Answer frames=%d/%d, StartTalk=%s",
             cs.answers_seen, SYNCN_ANSWER_FRAME_COUNT,
             cs.start_talk_seen ? "yes" : "no");

    syncn_parser_free(parser);
    outbox_free(&cs.out);
    close(fd);
    return rc;
}

/* ---------------------------------------------------------------- modes --- */

static int mode_listen(const sim_options *opt, nal_source *video)
{
    const int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        WARN("socket: %s", strerror(errno));
        return 1;
    }
    const int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)opt->port),
        .sin_addr   = { .s_addr = INADDR_ANY },
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) {
        WARN("bind :%d: %s", opt->port, strerror(errno));
        close(srv);
        return 1;
    }
    listen(srv, 4);
    LOG("door simulator listening on :%d — waiting for the panel to dial", opt->port);

    while (!g_stop) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        const int fd = accept(srv, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            WARN("accept: %s", strerror(errno));
            break;
        }
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        LOG("panel connected from %s:%u", ip, (unsigned)ntohs(peer.sin_port));
        run_session(fd, opt, video);
        LOG("waiting for the next connection");
    }
    close(srv);
    return 0;
}

static int mode_call(const char *panel_ip, const sim_options *opt, nal_source *video)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        WARN("socket: %s", strerror(errno));
        return 1;
    }

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)opt->port) };
    if (inet_pton(AF_INET, panel_ip, &addr.sin_addr) != 1) {
        WARN("bad panel address: %s", panel_ip);
        close(fd);
        return 1;
    }

    LOG("dialling panel at %s:%d", panel_ip, opt->port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        WARN("connect: %s", strerror(errno));
        close(fd);
        return 1;
    }
    LOG("connected — announcing an incoming call");
    return run_session(fd, opt, video);
}

static int mode_discover(const char *broadcast)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        WARN("socket: %s", strerror(errno));
        return 1;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port   = htons(SYNCN_DISCOVERY_PORT),
        .sin_addr   = { .s_addr = INADDR_ANY },
    };
    if (bind(fd, (struct sockaddr *)&local, sizeof local) < 0)
        WARN("bind :%d: %s (replies may not arrive)", SYNCN_DISCOVERY_PORT, strerror(errno));

    struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons(SYNCN_DISCOVERY_PORT) };
    if (inet_pton(AF_INET, broadcast, &dst.sin_addr) != 1) {
        WARN("bad broadcast address: %s", broadcast);
        close(fd);
        return 1;
    }

    const char *req =
        "{\"command\":\"cmd_send_get_device_info\",\"localAddr\":\"door-sim\",\"localType\":3}";
    LOG("broadcasting discovery to %s:%d", broadcast, SYNCN_DISCOVERY_PORT);
    LOG("-> %s", req);
    sendto(fd, req, strlen(req), 0, (struct sockaddr *)&dst, sizeof dst);

    const uint64_t deadline = now_ms() + 3000;
    int replies = 0;
    while (!g_stop && now_ms() < deadline) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        if (poll(&pfd, 1, 200) <= 0)
            continue;

        char buf[2048];
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        const ssize_t n = recvfrom(fd, buf, sizeof buf - 1, 0,
                                   (struct sockaddr *)&from, &flen);
        if (n <= 0)
            continue;
        buf[n] = '\0';

        /* Ignore our own broadcast coming back to us. */
        syncn_discovery_request echo;
        if (syncn_discovery_parse(buf, (size_t)n, &echo))
            continue;

        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        LOG("<- reply from %s: %s", ip, buf);
        replies++;
    }

    LOG("%d repl%s", replies, replies == 1 ? "y" : "ies");
    if (replies == 0)
        WARN("no panel answered — is the responder running, and does broadcast reach it?");
    close(fd);
    return replies > 0 ? 0 : 1;
}

/* ----------------------------------------------------------------- main --- */

static void usage(const char *argv0)
{
    printf(
"Door station simulator for the SyncN intercom.\n"
"\n"
"Usage:\n"
"  %s --listen                 wait for the panel to dial us (outgoing call,\n"
"                              preview, standalone unlock)\n"
"  %s --call <panel-ip>        dial the panel and announce an incoming call\n"
"  %s --discover <broadcast>   broadcast a discovery request and print replies\n"
"                              e.g. --discover 192.168.100.255\n"
"\n"
"Options:\n"
"  --h264 <file>        stream this Annex-B H.264 file, one NAL per frame,\n"
"                       looping. Without it, no video is sent.\n"
"  --port <n>           call port (default 8189)\n"
"  --fps <n>            video frame rate (default 15)\n"
"  --tone <hz>          downlink test tone (default 440; 0 for silence)\n"
"  --no-audio           send no downlink audio at all\n"
"  --hangup-after <s>   hang up after this many seconds\n"
"  --getcallinfo <s>    send getCallInfo after this many seconds; a correct\n"
"                       panel replies with the full three-frame handshake\n"
"  -v, --verbose        log every frame\n"
"\n"
"The simulator enforces what real door firmware enforces: it streams nothing\n"
"until it has received both the three-frame Answer handshake and a StartTalk,\n"
"and it stops sending audio if the panel's uplink goes quiet for %d ms.\n",
    argv0, argv0, argv0, UPLINK_GRACE_MS);
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    sim_options opt = {
        .port                = SYNCN_CALL_PORT,
        .h264_path           = NULL,
        .tone_hz             = 440.0,
        .fps                 = 15,
        .send_audio          = true,
        .hangup_after_s      = 0,
        .getcallinfo_after_s = 0,
        .announce_call       = false,
    };

    const char *mode = NULL;
    const char *target = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--listen")) {
            mode = "listen";
        } else if (!strcmp(a, "--call") && i + 1 < argc) {
            mode = "call";
            target = argv[++i];
            opt.announce_call = true;
        } else if (!strcmp(a, "--discover") && i + 1 < argc) {
            mode = "discover";
            target = argv[++i];
        } else if (!strcmp(a, "--h264") && i + 1 < argc) {
            opt.h264_path = argv[++i];
        } else if (!strcmp(a, "--port") && i + 1 < argc) {
            opt.port = atoi(argv[++i]);
        } else if (!strcmp(a, "--fps") && i + 1 < argc) {
            opt.fps = atoi(argv[++i]);
        } else if (!strcmp(a, "--tone") && i + 1 < argc) {
            opt.tone_hz = atof(argv[++i]);
        } else if (!strcmp(a, "--no-audio")) {
            opt.send_audio = false;
        } else if (!strcmp(a, "--hangup-after") && i + 1 < argc) {
            opt.hangup_after_s = atoi(argv[++i]);
        } else if (!strcmp(a, "--getcallinfo") && i + 1 < argc) {
            opt.getcallinfo_after_s = atoi(argv[++i]);
        } else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) {
            g_verbose = true;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (!mode) {
        usage(argv[0]);
        return 2;
    }

    nal_source video;
    memset(&video, 0, sizeof video);
    if (opt.h264_path && !nal_source_load(&video, opt.h264_path))
        WARN("continuing without video");

    int rc;
    if (!strcmp(mode, "listen"))
        rc = mode_listen(&opt, &video);
    else if (!strcmp(mode, "call"))
        rc = mode_call(target, &opt, &video);
    else
        rc = mode_discover(target);

    nal_source_free(&video);
    return rc;
}
