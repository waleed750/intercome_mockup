#include "syncn/ipc.h"
#include "syncn/util.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define LINE_MAX_BYTES 4096

typedef struct {
    int  fd;
    char in[LINE_MAX_BYTES];
    size_t in_len;

    /* Asked for video frames. Frames carry a file descriptor, and a client
     * that reads the socket without ancillary data -- the Dart UI does --
     * has the kernel install the descriptor into its process and then never
     * sees it. One leaked descriptor per frame, twenty a second. So frames go
     * only to clients that said they can take them. */
    bool wants_video;
} ipc_client;

struct syncn_ipc {
    int  listen_fd;
    char path[108];
    ipc_client clients[SYNCN_IPC_MAX_CLIENTS];
    unsigned n_clients;
    syncn_ipc_handler handler;
    syncn_ipc_gone_cb on_gone;
    void *user;
};

void syncn_ipc_set_disconnect_handler(syncn_ipc *ipc, syncn_ipc_gone_cb cb)
{
    if (ipc)
        ipc->on_gone = cb;
}

static const struct { const char *name; syncn_ipc_cmd cmd; } COMMANDS[] = {
    { "answer",        SYNCN_IPC_CMD_ANSWER        },
    { "reject",        SYNCN_IPC_CMD_REJECT        },
    { "hangup",        SYNCN_IPC_CMD_HANGUP        },
    { "unlock",        SYNCN_IPC_CMD_UNLOCK        },
    { "call",          SYNCN_IPC_CMD_CALL          },
    { "preview_start", SYNCN_IPC_CMD_PREVIEW_START },
    { "preview_stop",  SYNCN_IPC_CMD_PREVIEW_STOP  },
    { "set_mute",      SYNCN_IPC_CMD_SET_MUTE      },
    { "get_state",     SYNCN_IPC_CMD_GET_STATE     },
    { "frame_done",    SYNCN_IPC_CMD_FRAME_DONE    },
    { "subscribe_video", SYNCN_IPC_CMD_SUBSCRIBE_VIDEO },
};

syncn_ipc *syncn_ipc_start(const char *path, syncn_ipc_handler handler, void *user)
{
    if (!path || !*path)
        path = SYNCN_IPC_DEFAULT_PATH;

    syncn_ipc *ipc = calloc(1, sizeof *ipc);
    if (!ipc)
        return NULL;
    snprintf(ipc->path, sizeof ipc->path, "%s", path);
    ipc->handler = handler;
    ipc->user    = user;
    for (int i = 0; i < SYNCN_IPC_MAX_CLIENTS; i++)
        ipc->clients[i].fd = -1;

    ipc->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ipc->listen_fd < 0) {
        LOG_WARN("ipc: socket: %s", strerror(errno));
        free(ipc);
        return NULL;
    }

    /* A stale socket file from a crash would block bind. Removing it is safe:
     * if another daemon were really listening, the bind below still fails on
     * the port-level check when it tries to accept. */
    unlink(ipc->path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", ipc->path);

    if (bind(ipc->listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        LOG_WARN("ipc: bind %s: %s — the UI will not be able to connect",
                 ipc->path, strerror(errno));
        close(ipc->listen_fd);
        free(ipc);
        return NULL;
    }

    /* Group-writable so the UI can run as a different user in the syncn group
     * without needing the daemon's privileges. */
    chmod(ipc->path, 0660);

    if (listen(ipc->listen_fd, 4) < 0) {
        LOG_WARN("ipc: listen: %s", strerror(errno));
        close(ipc->listen_fd);
        unlink(ipc->path);
        free(ipc);
        return NULL;
    }

    syncn_set_nonblock(ipc->listen_fd);
    LOG_INFO("ipc: listening on %s", ipc->path);
    return ipc;
}

void syncn_ipc_stop(syncn_ipc *ipc)
{
    if (!ipc)
        return;
    for (int i = 0; i < SYNCN_IPC_MAX_CLIENTS; i++)
        if (ipc->clients[i].fd >= 0)
            close(ipc->clients[i].fd);
    close(ipc->listen_fd);
    unlink(ipc->path);
    free(ipc);
}

unsigned syncn_ipc_client_count(const syncn_ipc *ipc)
{
    return ipc ? ipc->n_clients : 0;
}

unsigned syncn_ipc_video_client_count(const syncn_ipc *ipc)
{
    if (!ipc)
        return 0;
    unsigned n = 0;
    for (int i = 0; i < SYNCN_IPC_MAX_CLIENTS; i++)
        if (ipc->clients[i].fd >= 0 && ipc->clients[i].wants_video)
            n++;
    return n;
}

int syncn_ipc_poll_fds(syncn_ipc *ipc, struct pollfd *out, int max)
{
    if (!ipc || max <= 0)
        return 0;

    int n = 0;
    out[n].fd = ipc->listen_fd;
    out[n].events = POLLIN;
    out[n].revents = 0;
    n++;

    for (int i = 0; i < SYNCN_IPC_MAX_CLIENTS && n < max; i++) {
        if (ipc->clients[i].fd < 0)
            continue;
        out[n].fd = ipc->clients[i].fd;
        out[n].events = POLLIN;
        out[n].revents = 0;
        n++;
    }
    return n;
}

static void drop_client(syncn_ipc *ipc, int idx)
{
    if (ipc->clients[idx].fd < 0)
        return;
    const bool had_video = ipc->clients[idx].wants_video;

    close(ipc->clients[idx].fd);
    ipc->clients[idx].fd = -1;
    ipc->clients[idx].in_len = 0;
    ipc->clients[idx].wants_video = false;
    if (ipc->n_clients)
        ipc->n_clients--;
    LOG_INFO("ipc: UI disconnected (%u still connected)", ipc->n_clients);

    /* The last VIDEO subscriber leaving reclaims the buffers, whether or not
     * a control-only client is still attached. Keyed on subscribers rather
     * than on the client count, because the Dart UI and the video plugin are
     * two connections, and the plugin dying while the UI lives on would
     * otherwise strand every buffer it held until the UI also went. */
    if (had_video && ipc->on_gone) {
        bool another = false;
        for (int i = 0; i < SYNCN_IPC_MAX_CLIENTS; i++)
            if (ipc->clients[i].fd >= 0 && ipc->clients[i].wants_video)
                another = true;
        if (!another)
            ipc->on_gone(ipc->n_clients, ipc->user);
    }
}

static void dispatch_line(syncn_ipc *ipc, int client_idx, char *line)
{
    cJSON *root = cJSON_Parse(line);
    if (!root) {
        LOG_DBG("ipc: ignoring unparseable line: %.80s", line);
        return;
    }

    const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    syncn_ipc_message msg = { .cmd = SYNCN_IPC_CMD_UNKNOWN, .flag = false, .id = 0 };

    if (cJSON_IsString(c) && c->valuestring) {
        for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++) {
            if (strcmp(c->valuestring, COMMANDS[i].name) == 0) {
                msg.cmd = COMMANDS[i].cmd;
                break;
            }
        }
        if (msg.cmd == SYNCN_IPC_CMD_UNKNOWN)
            LOG_WARN("ipc: unknown command '%s'", c->valuestring);
    }

    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (cJSON_IsBool(v))
        msg.flag = cJSON_IsTrue(v);

    /*
     * Buffer ids outgrow what a double represents exactly at 2^53, and cJSON
     * stores every number as a double. That is decades of frames at 20 fps, so
     * the ceiling is theoretical — but a silently rounded id would free the
     * wrong buffer, and reading it back from valuedouble costs nothing.
     * Negative and fractional values are simply not ids.
     */
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsNumber(id) && id->valuedouble >= 1.0)
        msg.id = (uint64_t)id->valuedouble;

    cJSON_Delete(root);

    if (msg.cmd == SYNCN_IPC_CMD_SUBSCRIBE_VIDEO) {
        /* Handled here: it is about this connection, not about the call. */
        ipc->clients[client_idx].wants_video = true;
        LOG_INFO("ipc: client %d subscribed to video frames", client_idx);
        return;
    }

    if (msg.cmd != SYNCN_IPC_CMD_UNKNOWN && ipc->handler)
        ipc->handler(&msg, ipc->user);
}

static void read_client(syncn_ipc *ipc, int idx)
{
    ipc_client *cl = &ipc->clients[idx];

    for (;;) {
        const size_t room = sizeof cl->in - cl->in_len - 1;
        if (room == 0) {
            /* A line longer than the buffer is not a command we understand.
             * Reset rather than closing — a UI bug should not lose the socket. */
            LOG_WARN("ipc: over-long line discarded");
            cl->in_len = 0;
            continue;
        }

        const ssize_t n = recv(cl->fd, cl->in + cl->in_len, room, 0);
        if (n == 0) {
            drop_client(ipc, idx);
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            drop_client(ipc, idx);
            return;
        }

        cl->in_len += (size_t)n;
        cl->in[cl->in_len] = '\0';

        /* Consume every complete line in the buffer. */
        char *start = cl->in;
        for (;;) {
            char *nl = memchr(start, '\n', cl->in_len - (size_t)(start - cl->in));
            if (!nl)
                break;
            *nl = '\0';
            if (*start)
                dispatch_line(ipc, idx, start);
            start = nl + 1;
        }

        const size_t consumed = (size_t)(start - cl->in);
        if (consumed) {
            memmove(cl->in, start, cl->in_len - consumed);
            cl->in_len -= consumed;
        }
    }
}

void syncn_ipc_handle(syncn_ipc *ipc, const struct pollfd *fds, int count)
{
    if (!ipc)
        return;

    for (int i = 0; i < count; i++) {
        if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;

        if (fds[i].fd == ipc->listen_fd) {
            for (;;) {
                const int fd = accept(ipc->listen_fd, NULL, NULL);
                if (fd < 0)
                    break;

                int slot = -1;
                for (int k = 0; k < SYNCN_IPC_MAX_CLIENTS; k++) {
                    if (ipc->clients[k].fd < 0) {
                        slot = k;
                        break;
                    }
                }
                if (slot < 0) {
                    LOG_WARN("ipc: refusing a UI connection, %d already attached",
                             SYNCN_IPC_MAX_CLIENTS);
                    close(fd);
                    continue;
                }

                syncn_set_nonblock(fd);
                ipc->clients[slot].fd = fd;
                ipc->clients[slot].in_len = 0;
                ipc->clients[slot].wants_video = false;
                ipc->n_clients++;
                LOG_INFO("ipc: UI connected (%u total)", ipc->n_clients);

                /* The UI has no idea what is happening yet. Tell it, so a UI
                 * that starts mid-call shows the call rather than an idle
                 * screen. */
                if (ipc->handler) {
                    const syncn_ipc_message hello = { .cmd = SYNCN_IPC_CMD_GET_STATE,
                                                      .flag = false };
                    ipc->handler(&hello, ipc->user);
                }
            }
            continue;
        }

        for (int k = 0; k < SYNCN_IPC_MAX_CLIENTS; k++) {
            if (ipc->clients[k].fd == fds[i].fd) {
                read_client(ipc, k);
                break;
            }
        }
    }
}

/* --------------------------------------------------- descriptor passing --- */

ssize_t syncn_fd_send(int sock, const void *buf, size_t len, int fd)
{
    struct iovec iov = { .iov_base = (void *)buf, .iov_len = len };

    /* The control buffer must be aligned for struct cmsghdr, which is what the
     * union achieves; a bare char array is not guaranteed to be. */
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } control;
    memset(&control, 0, sizeof control);

    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;

    if (fd >= 0) {
        msg.msg_control    = control.buf;
        msg.msg_controllen = sizeof control.buf;

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fd, sizeof fd);
    }

    ssize_t n;
    do {
        n = sendmsg(sock, &msg, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    return n;
}

ssize_t syncn_fd_recv(int sock, void *buf, size_t len, int *fd_out)
{
    if (fd_out)
        *fd_out = -1;

    struct iovec iov = { .iov_base = buf, .iov_len = len };

    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } control;
    memset(&control, 0, sizeof control);

    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = control.buf;
    msg.msg_controllen = sizeof control.buf;

    ssize_t n;
    do {
        n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);

    if (n <= 0)
        return n;

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            int fd;
            memcpy(&fd, CMSG_DATA(cmsg), sizeof fd);
            if (fd_out)
                *fd_out = fd;
            else
                close(fd); /* nobody wants it; leaking it would exhaust the table */
            break;
        }
    }

    /* A descriptor that arrived in a truncated message is unusable and must not
     * be left open. */
    if ((msg.msg_flags & MSG_CTRUNC) && fd_out && *fd_out >= 0) {
        close(*fd_out);
        *fd_out = -1;
    }
    return n;
}

bool syncn_ipc_send_fd(syncn_ipc *ipc, const char *json_line, int fd)
{
    if (!ipc || !json_line)
        return false;

    char buf[LINE_MAX_BYTES];
    const int len = snprintf(buf, sizeof buf, "%s\n", json_line);
    if (len <= 0)
        return false;

    bool any = false;
    for (int i = 0; i < SYNCN_IPC_MAX_CLIENTS; i++) {
        if (ipc->clients[i].fd < 0 || !ipc->clients[i].wants_video)
            continue;
        if (syncn_fd_send(ipc->clients[i].fd, buf, (size_t)len, fd) > 0)
            any = true;
        else
            drop_client(ipc, i);
    }
    return any;
}

void syncn_ipc_broadcast(syncn_ipc *ipc, const char *json_line)
{
    if (!ipc || !json_line)
        return;

    char buf[LINE_MAX_BYTES];
    const int len = snprintf(buf, sizeof buf, "%s\n", json_line);
    if (len <= 0)
        return;

    for (int i = 0; i < SYNCN_IPC_MAX_CLIENTS; i++) {
        if (ipc->clients[i].fd < 0)
            continue;
        /*
         * MSG_DONTWAIT: a UI that has stopped reading must never stall the
         * daemon. This runs on the same thread that services the call, and a
         * blocked write here would drop audio frames.
         */
        const ssize_t n = send(ipc->clients[i].fd, buf, (size_t)len,
                               MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            drop_client(ipc, i);
        } else if (n >= 0 && n < len) {
            LOG_DBG("ipc: short write to client %d, dropping it", i);
            drop_client(ipc, i);
        }
    }
}
