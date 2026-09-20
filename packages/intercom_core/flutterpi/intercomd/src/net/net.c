#include "syncn/net.h"
#include "syncn/util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* From the protocol reference section 8. */
#define OUTBOX_MAX_FRAMES  256
#define OUTBOX_MAX_BYTES   (4u * 1024u * 1024u)
#define SEND_STUCK_MS      5000
#define DIAL_TIMEOUT_MS    5000

struct syncn_conn {
    int   fd;
    char  peer[64];

    syncn_parser *parser;
    syncn_conn_callbacks cb;
    void *user;

    uint8_t *out;
    size_t   out_cap, out_len, out_sent;
    unsigned out_frames;

    /* Send-stuck detection: when the outbox last drained anything. */
    uint64_t last_progress_ms;

    bool     dead;
    uint64_t frames_in, frames_out, bytes_in, bytes_out, dropped;
};

/* ------------------------------------------------------------- outbox ------ */

static bool outbox_reserve(syncn_conn *c, size_t extra)
{
    if (c->out_len + extra <= c->out_cap)
        return true;

    /* Reclaim the sent prefix before growing. */
    if (c->out_sent > 0) {
        memmove(c->out, c->out + c->out_sent, c->out_len - c->out_sent);
        c->out_len -= c->out_sent;
        c->out_sent = 0;
        if (c->out_len + extra <= c->out_cap)
            return true;
    }

    size_t want = c->out_cap ? c->out_cap : 16384;
    while (want < c->out_len + extra) {
        if (want > OUTBOX_MAX_BYTES)
            return false;
        want *= 2;
    }
    uint8_t *nb = realloc(c->out, want);
    if (!nb)
        return false;
    c->out     = nb;
    c->out_cap = want;
    return true;
}

bool syncn_conn_send(syncn_conn *c, syncn_channel ch, const void *payload, size_t len)
{
    if (!c || c->dead)
        return false;

    if (c->out_frames >= OUTBOX_MAX_FRAMES) {
        /* The peer is not draining. Dropping is correct: queueing more would
         * only add latency to audio that is already stale by the time it left. */
        c->dropped++;
        return false;
    }

    const size_t total = SYNCN_FRAME_HEADER_SIZE + len;
    if (!outbox_reserve(c, total)) {
        c->dropped++;
        return false;
    }

    syncn_frame_write_header(c->out + c->out_len, ch, (uint32_t)len);
    if (len)
        memcpy(c->out + c->out_len + SYNCN_FRAME_HEADER_SIZE, payload, len);
    c->out_len += total;
    c->out_frames++;
    return true;
}

bool syncn_conn_send_command(syncn_conn *c, syncn_outbound_cmd cmd)
{
    size_t len = 0;
    const char *p = syncn_command_payload(cmd, &len);
    if (!p)
        return false;
    const bool ok = syncn_conn_send(c, SYNCN_CH_CONTROL, p, len);
    if (ok)
        LOG_DBG("-> %s", p);
    return ok;
}

/* ---------------------------------------------------------- frame intake --- */

static int on_frame(syncn_channel ch, const uint8_t *payload, size_t len, void *user)
{
    syncn_conn *c = user;
    c->frames_in++;

    switch (ch) {
    case SYNCN_CH_CONTROL: {
        const syncn_inbound_cmd cmd = syncn_command_parse(payload, len);
        LOG_DBG("<- control (%zu bytes): %.*s", len, (int)(len > 200 ? 200 : len),
                (const char *)payload);
        if (c->cb.on_control)
            c->cb.on_control(cmd, (const char *)payload, len, c->user);
        break;
    }
    case SYNCN_CH_AUDIO:
        if (c->cb.on_audio)
            c->cb.on_audio(payload, len, c->user);
        break;
    case SYNCN_CH_VIDEO:
        if (c->cb.on_video)
            c->cb.on_video(payload, len, c->user);
        break;
    default:
        break;
    }
    return 0;
}

/* --------------------------------------------------------- construction ---- */

static syncn_conn *conn_new(int fd, const char *peer,
                            const syncn_conn_callbacks *cb, void *user)
{
    syncn_conn *c = calloc(1, sizeof *c);
    if (!c) {
        close(fd);
        return NULL;
    }

    c->parser = syncn_parser_new(0);
    if (!c->parser) {
        free(c);
        close(fd);
        return NULL;
    }

    c->fd   = fd;
    c->cb   = *cb;
    c->user = user;
    snprintf(c->peer, sizeof c->peer, "%s", peer ? peer : "?");
    c->last_progress_ms = syncn_now_ms();

    syncn_set_nonblock(fd);
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return c;
}

syncn_conn *syncn_conn_adopt(int fd, const char *peer,
                             const syncn_conn_callbacks *cb, void *user)
{
    return conn_new(fd, peer, cb, user);
}

syncn_conn *syncn_conn_dial(const char *host, int port, int timeout_ms,
                            const syncn_conn_callbacks *cb, void *user)
{
    if (!host || !*host) {
        LOG_ERR("net: no door address to dial");
        return NULL;
    }
    if (timeout_ms <= 0)
        timeout_ms = DIAL_TIMEOUT_MS;

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERR("net: socket: %s", strerror(errno));
        return NULL;
    }

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        LOG_ERR("net: '%s' is not a valid IPv4 address", host);
        close(fd);
        return NULL;
    }

    syncn_set_nonblock(fd);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0 && errno != EINPROGRESS) {
        LOG_ERR("net: connect to %s:%d: %s", host, port, strerror(errno));
        close(fd);
        return NULL;
    }

    struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
    const int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) {
        LOG_ERR("net: connect to %s:%d timed out after %d ms", host, port, timeout_ms);
        close(fd);
        return NULL;
    }

    int err = 0;
    socklen_t elen = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
    if (err != 0) {
        LOG_ERR("net: connect to %s:%d: %s", host, port, strerror(err));
        close(fd);
        return NULL;
    }

    char peer[64];
    snprintf(peer, sizeof peer, "%s:%d", host, port);
    LOG_INFO("net: connected to %s", peer);
    return conn_new(fd, peer, cb, user);
}

void syncn_conn_close(syncn_conn *c)
{
    if (!c)
        return;
    if (c->fd >= 0)
        close(c->fd);
    syncn_parser_free(c->parser);
    free(c->out);
    free(c);
}

int   syncn_conn_fd(const syncn_conn *c)   { return c ? c->fd : -1; }
const char *syncn_conn_peer(const syncn_conn *c) { return c ? c->peer : "?"; }

short syncn_conn_events(const syncn_conn *c)
{
    if (!c)
        return 0;
    return (short)(POLLIN | (c->out_sent < c->out_len ? POLLOUT : 0));
}

/* --------------------------------------------------------------- driving --- */

static void die(syncn_conn *c, const char *reason)
{
    if (c->dead)
        return;
    c->dead = true;
    LOG_INFO("net: connection to %s closed: %s", c->peer, reason);
    if (c->cb.on_closed)
        c->cb.on_closed(reason, c->user);
}

bool syncn_conn_handle(syncn_conn *c, short revents)
{
    if (!c || c->dead)
        return false;

    if (revents & (POLLERR | POLLNVAL)) {
        die(c, "socket error");
        return false;
    }

    if (revents & POLLOUT) {
        while (c->out_sent < c->out_len) {
            const ssize_t n = send(c->fd, c->out + c->out_sent,
                                   c->out_len - c->out_sent, MSG_NOSIGNAL);
            if (n > 0) {
                c->out_sent += (size_t)n;
                c->bytes_out += (uint64_t)n;
                c->last_progress_ms = syncn_now_ms();
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            die(c, "send failed");
            return false;
        }
        if (c->out_sent >= c->out_len) {
            c->frames_out += c->out_frames;
            c->out_frames = 0;
            c->out_sent = c->out_len = 0;
        }
    }

    if (revents & POLLIN) {
        uint8_t buf[16384];
        for (;;) {
            const ssize_t n = recv(c->fd, buf, sizeof buf, 0);
            if (n > 0) {
                c->bytes_in += (uint64_t)n;
                if (syncn_parser_feed(c->parser, buf, (size_t)n, on_frame, c) != 0) {
                    die(c, "parser error");
                    return false;
                }
                if ((size_t)n < sizeof buf)
                    break;
                continue;
            }
            if (n == 0) {
                die(c, "peer closed");
                return false;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            die(c, strerror(errno));
            return false;
        }
    }

    /* POLLHUP with data still buffered is normal — read it out first, which
     * the POLLIN branch above already did. */
    if ((revents & POLLHUP) && !(revents & POLLIN)) {
        die(c, "peer hung up");
        return false;
    }

    return !c->dead;
}

bool syncn_conn_tick(syncn_conn *c)
{
    if (!c || c->dead)
        return false;

    if (c->out_sent < c->out_len) {
        const uint64_t stalled = syncn_now_ms() - c->last_progress_ms;
        if (stalled > SEND_STUCK_MS) {
            /* Frames are being accepted but nothing is reaching the door, and
             * TCP has not errored. Without this the call would hang forever
             * looking healthy. */
            LOG_ERR("net: no send progress for %llu ms with %zu bytes queued — "
                    "declaring the connection dead",
                    (unsigned long long)stalled, c->out_len - c->out_sent);
            die(c, "send stalled");
            return false;
        }
    } else {
        c->last_progress_ms = syncn_now_ms();
    }
    return true;
}

void syncn_conn_get_stats(const syncn_conn *c, syncn_conn_stats *out)
{
    memset(out, 0, sizeof *out);
    if (!c)
        return;
    syncn_parser_stats ps;
    syncn_parser_stats_get(c->parser, &ps);

    out->frames_in      = c->frames_in;
    out->frames_out     = c->frames_out;
    out->bytes_in       = c->bytes_in;
    out->bytes_out      = c->bytes_out;
    out->dropped_frames = c->dropped;
    out->resyncs        = ps.resync_events;
    out->outbox_depth   = c->out_frames;
}

/* ---------------------------------------------------------------- server --- */

int syncn_listen_tcp(int port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERR("net: socket: %s", strerror(errno));
        return -1;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)port),
        .sin_addr   = { .s_addr = INADDR_ANY },
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        if (errno == EADDRINUSE)
            LOG_ERR("net: port %d is already in use — another intercom process?", port);
        else
            LOG_ERR("net: bind :%d: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 4) < 0) {
        LOG_ERR("net: listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    syncn_set_nonblock(fd);
    LOG_INFO("net: listening for door connections on :%d", port);
    return fd;
}

/* ---------------------------------------------------- discovery responder --- */

struct syncn_responder {
    int fd;
    const syncn_screen_info *info;
    uint64_t requests, replies;
};

syncn_responder *syncn_responder_start(const syncn_screen_info *info)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        LOG_ERR("net: discovery socket: %s", strerror(errno));
        return NULL;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(SYNCN_DISCOVERY_PORT),
        .sin_addr   = { .s_addr = INADDR_ANY },
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        LOG_ERR("net: bind :%d for discovery: %s", SYNCN_DISCOVERY_PORT, strerror(errno));
        close(fd);
        return NULL;
    }
    syncn_set_nonblock(fd);

    syncn_responder *r = calloc(1, sizeof *r);
    if (!r) {
        close(fd);
        return NULL;
    }
    r->fd   = fd;
    r->info = info;
    LOG_INFO("net: discovery responder on :%d", SYNCN_DISCOVERY_PORT);
    return r;
}

void syncn_responder_stop(syncn_responder *r)
{
    if (!r)
        return;
    close(r->fd);
    free(r);
}

int syncn_responder_fd(const syncn_responder *r) { return r ? r->fd : -1; }

void syncn_responder_handle(syncn_responder *r, char *learned_addr, size_t cap)
{
    for (;;) {
        char buf[2048];
        struct sockaddr_in from;
        socklen_t flen = sizeof from;

        const ssize_t n = recvfrom(r->fd, buf, sizeof buf - 1, 0,
                                   (struct sockaddr *)&from, &flen);
        if (n <= 0)
            return;
        buf[n] = '\0';

        syncn_discovery_request req;
        if (!syncn_discovery_parse(buf, (size_t)n, &req))
            continue; /* not a request — most likely our own reply echoing back */

        r->requests++;

        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);

        /* This is what makes zero-configuration commissioning work: whoever
         * broadcast at us is a door, and now we know where it lives. */
        if (learned_addr && cap)
            snprintf(learned_addr, cap, "%s", ip);

        char reply[1024];
        const size_t rn = syncn_discovery_build_reply(r->info, &req, reply, sizeof reply);
        if (rn == 0) {
            LOG_WARN("net: could not build the discovery reply");
            continue;
        }

        /* Reply to the sender's address, but always on the discovery port —
         * the door listens there, not on its ephemeral source port. */
        struct sockaddr_in to = from;
        to.sin_port = htons(SYNCN_DISCOVERY_PORT);

        if (sendto(r->fd, reply, rn, 0, (struct sockaddr *)&to, sizeof to) < 0)
            LOG_WARN("net: discovery reply to %s failed: %s", ip, strerror(errno));
        else {
            r->replies++;
            LOG_INFO("net: discovery request from %s — replied", ip);
            LOG_DBG("   %s", reply);
        }
    }
}
