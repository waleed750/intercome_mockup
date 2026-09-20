/*
 * Sockets: the framed call connection, the call server, and the discovery
 * responder.
 *
 * Everything here is non-blocking and driven from one poll loop on the main
 * thread. The audio thread never touches a socket; it hands frames across
 * through the call layer.
 */
#ifndef SYNCN_NET_H
#define SYNCN_NET_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "syncn/command.h"
#include "syncn/discovery.h"
#include "syncn/frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ connection --- */

typedef struct syncn_conn syncn_conn;

typedef struct {
    void (*on_control)(syncn_inbound_cmd cmd, const char *raw, size_t len, void *user);
    void (*on_audio)(const uint8_t *alaw, size_t len, void *user);
    void (*on_video)(const uint8_t *nal, size_t len, void *user);
    void (*on_closed)(const char *reason, void *user);
} syncn_conn_callbacks;

/* Take ownership of an accepted socket. */
syncn_conn *syncn_conn_adopt(int fd, const char *peer,
                             const syncn_conn_callbacks *cb, void *user);

/* Connect out. Blocks up to timeout_ms; returns NULL on failure. */
syncn_conn *syncn_conn_dial(const char *host, int port, int timeout_ms,
                            const syncn_conn_callbacks *cb, void *user);

void syncn_conn_close(syncn_conn *c);

int   syncn_conn_fd(const syncn_conn *c);
short syncn_conn_events(const syncn_conn *c);
const char *syncn_conn_peer(const syncn_conn *c);

/* Queue a frame. Returns false if the outbox is full, which is a drop, not an
 * error — the caller decides whether that matters for this frame type. */
bool syncn_conn_send(syncn_conn *c, syncn_channel ch, const void *payload, size_t len);
bool syncn_conn_send_command(syncn_conn *c, syncn_outbound_cmd cmd);

/* Drive I/O. Returns false once the connection is dead and should be closed. */
bool syncn_conn_handle(syncn_conn *c, short revents);

/*
 * Called every loop iteration. Enforces the send-stuck timeout.
 *
 * This exists because of a confirmed on-device failure: frames kept being
 * accepted for transmission — including OpenDoor and live microphone audio —
 * while nothing actually reached the door, and the call never tore down,
 * because TCP had not errored. Nothing else would have noticed. If the outbox
 * has made no progress for the timeout, the connection is declared dead.
 */
bool syncn_conn_tick(syncn_conn *c);

typedef struct {
    uint64_t frames_in, frames_out, bytes_in, bytes_out;
    uint64_t dropped_frames;   /* outbox was full */
    uint64_t resyncs;
    unsigned outbox_depth;
} syncn_conn_stats;

void syncn_conn_get_stats(const syncn_conn *c, syncn_conn_stats *out);

/* ---------------------------------------------------------------- server --- */

/* Listening socket for door-initiated calls. Returns the fd, or -1. */
int syncn_listen_tcp(int port);

/* ---------------------------------------------------- discovery responder --- */

typedef struct syncn_responder syncn_responder;

/* Binds 0.0.0.0:8089 with broadcast enabled. `info` is borrowed and read on
 * every reply, so the caller can update deviceBusy live. */
syncn_responder *syncn_responder_start(const syncn_screen_info *info);
void             syncn_responder_stop(syncn_responder *r);
int              syncn_responder_fd(const syncn_responder *r);

/*
 * Service a readable responder socket. If the datagram was a discovery request
 * we reply, and the sender's address is written to `learned_addr` — that is how
 * the panel finds the door with no configuration at all.
 */
void syncn_responder_handle(syncn_responder *r, char *learned_addr, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_NET_H */
