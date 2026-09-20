/*
 * Control socket for the user interface.
 *
 * A Unix domain socket carrying newline-delimited JSON, one object per line.
 * The UI sends commands; the daemon pushes state changes as they happen.
 *
 * Why a socket rather than linking the UI into the daemon: a door entry device
 * has to keep working when the interface crashes. The daemon owns the call, the
 * audio and the network; the UI is a view over it. Restart the UI mid-call and
 * it reconnects, asks for the current state, and carries on.
 *
 * The protocol is deliberately plain text so it can be driven from a shell
 * while debugging:
 *
 *     socat - UNIX-CONNECT:/run/syncn/intercomd.sock
 *     {"cmd":"answer"}
 */
#ifndef SYNCN_IPC_H
#define SYNCN_IPC_H

#include <poll.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>   /* ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

#define SYNCN_IPC_DEFAULT_PATH "/run/syncn/intercomd.sock"
#define SYNCN_IPC_MAX_CLIENTS  4

typedef enum {
    SYNCN_IPC_CMD_UNKNOWN = 0,
    SYNCN_IPC_CMD_ANSWER,
    SYNCN_IPC_CMD_REJECT,
    SYNCN_IPC_CMD_HANGUP,
    SYNCN_IPC_CMD_UNLOCK,
    SYNCN_IPC_CMD_CALL,
    SYNCN_IPC_CMD_PREVIEW_START,
    SYNCN_IPC_CMD_PREVIEW_STOP,
    SYNCN_IPC_CMD_SET_MUTE,
    SYNCN_IPC_CMD_GET_STATE,

    /*
     * The interface has finished with a video buffer and it may be written
     * again: {"cmd":"frame_done","id":1234}.
     *
     * This is the message that keeps the picture moving. Without it the daemon
     * cannot know when a buffer has been scanned out, and writing into one the
     * display is still reading tears. The buffer pool has only a handful, so a
     * UI that stops sending these stops the picture — deliberately, visibly,
     * and with a log line naming it, rather than by wedging the daemon.
     */
    SYNCN_IPC_CMD_FRAME_DONE,

    /* {"cmd":"subscribe_video"}: this connection can receive frames with a
     * descriptor attached. Handled inside the IPC layer; the call never sees
     * it. Frames are only ever sent to connections that sent this. */
    SYNCN_IPC_CMD_SUBSCRIBE_VIDEO
} syncn_ipc_cmd;

typedef struct {
    syncn_ipc_cmd cmd;
    bool          flag;   /* set_mute's value  */
    uint64_t      id;     /* frame_done's buffer id; 0 when absent */
} syncn_ipc_message;

typedef void (*syncn_ipc_handler)(const syncn_ipc_message *msg, void *user);

typedef struct syncn_ipc syncn_ipc;

/*
 * A client went away.
 *
 * The daemon needs this because the interface may be holding video buffers it
 * will now never return. It holds its own descriptors to that memory, so the
 * pages survive as long as it needs them — reclaiming the pool slots the
 * moment the socket drops is safe, and it is what lets a restarted UI get a
 * picture without restarting the daemon.
 */
typedef void (*syncn_ipc_gone_cb)(unsigned remaining, void *user);
void syncn_ipc_set_disconnect_handler(syncn_ipc *ipc, syncn_ipc_gone_cb cb);

/* NULL path uses SYNCN_IPC_DEFAULT_PATH. Returns NULL if the socket cannot be
 * created — which is not fatal for the daemon, it just means no UI. */
syncn_ipc *syncn_ipc_start(const char *path, syncn_ipc_handler handler, void *user);
void       syncn_ipc_stop(syncn_ipc *ipc);

/* Append this module's descriptors to a poll set. Returns how many were added. */
int  syncn_ipc_poll_fds(syncn_ipc *ipc, struct pollfd *out, int max);
void syncn_ipc_handle(syncn_ipc *ipc, const struct pollfd *fds, int count);

/* Push a line to every connected UI. A client that cannot keep up is dropped
 * rather than allowed to block the daemon. */
void syncn_ipc_broadcast(syncn_ipc *ipc, const char *json_line);

unsigned syncn_ipc_client_count(const syncn_ipc *ipc);

/* How many of them asked for video frames. Zero means a frame has nowhere to
 * go and should stay in the pool, however many control clients are attached. */
unsigned syncn_ipc_video_client_count(const syncn_ipc *ipc);

/* --------------------------------------------------- file descriptor passing --- */

/*
 * Send a line with a file descriptor attached.
 *
 * This is how a decoded video frame reaches the UI. The daemon cannot put
 * pixels on the screen — flutter-pi holds DRM master and a device has exactly
 * one — so it passes the converted buffer across as a DMA-BUF descriptor and
 * the UI, being master, creates the framebuffer and commits the plane.
 *
 * A descriptor cannot travel through an ordinary write; it needs SCM_RIGHTS
 * ancillary data on a Unix socket, and the kernel installs a new descriptor in
 * the receiver pointing at the same open file. The sender keeps its own and
 * must still close it.
 */
bool syncn_ipc_send_fd(syncn_ipc *ipc, const char *json_line, int fd);

/*
 * The primitives, exposed so the UI side uses the same code as the daemon and
 * so both can be tested without a daemon at all.
 *
 * syncn_fd_recv sets *fd_out to -1 when the message carried no descriptor.
 * The caller owns any descriptor it receives and must close it.
 */
ssize_t syncn_fd_send(int sock, const void *buf, size_t len, int fd);
ssize_t syncn_fd_recv(int sock, void *buf, size_t len, int *fd_out);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_IPC_H */
