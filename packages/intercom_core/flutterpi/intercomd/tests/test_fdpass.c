#define _GNU_SOURCE   /* memfd_create */
#include "test.h"
#include "syncn/ipc.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * Descriptor passing is how a decoded video frame reaches the UI process, and
 * it is the one part of the video path that cannot be reasoned about from a
 * log: either the receiving process gets a working handle to the same memory,
 * or it gets a number that means nothing. The ancillary-data mechanics are
 * fiddly enough — alignment, truncation, ownership — that they are worth
 * proving here rather than on a panel nobody can see.
 */

static int make_buffer(const char *contents)
{
    const int fd = memfd_create("syncn-test", MFD_CLOEXEC);
    if (fd < 0)
        return -1;
    const size_t n = strlen(contents);
    if (ftruncate(fd, (off_t)n) != 0 || write(fd, contents, n) != (ssize_t)n) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool contents_match(int fd, const char *expected)
{
    char buf[256] = {0};
    const ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
    return n > 0 && strcmp(buf, expected) == 0;
}

void suite_fdpass(void)
{
    SUITE("fdpass: the receiver gets a working handle to the same memory");
    {
        int sv[2];
        CHECK_EQ_INT(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv), 0);

        const char *payload = "decoded frame goes here";
        const int buffer = make_buffer(payload);
        CHECK(buffer >= 0);

        const char *line = "{\"event\":\"video_frame\",\"width\":1280}\n";
        CHECK(syncn_fd_send(sv[0], line, strlen(line), buffer) > 0);

        char got[256] = {0};
        int received = -1;
        const ssize_t n = syncn_fd_recv(sv[1], got, sizeof got - 1, &received);

        CHECK(n > 0);
        CHECK(received >= 0);
        /* A different number in this process, pointing at the same open file —
         * which is the whole point. */
        CHECK(received != buffer);
        CHECK(contents_match(received, payload));

        /* The message travels alongside the descriptor, not instead of it. */
        CHECK(strstr(got, "video_frame") != NULL);

        close(received);
        close(buffer);
        close(sv[0]);
        close(sv[1]);
    }

    SUITE("fdpass: the receiver's handle outlives the sender's");
    {
        /* The daemon closes its descriptor as soon as it has sent the frame.
         * If that invalidated the UI's copy, every frame would arrive dead. */
        int sv[2];
        socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);

        const char *payload = "still readable";
        const int buffer = make_buffer(payload);
        const char *line = "{\"event\":\"video_frame\"}\n";
        syncn_fd_send(sv[0], line, strlen(line), buffer);
        close(buffer);              /* sender lets go immediately */

        char got[128] = {0};
        int received = -1;
        syncn_fd_recv(sv[1], got, sizeof got - 1, &received);

        CHECK(received >= 0);
        CHECK(contents_match(received, payload));

        close(received);
        close(sv[0]);
        close(sv[1]);
    }

    SUITE("fdpass: a message with no descriptor reports none");
    {
        /* Most IPC traffic is state and stats, carrying nothing. Reporting a
         * stale descriptor for those would hand the UI a frame that is not
         * there. */
        int sv[2];
        socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);

        const char *line = "{\"event\":\"state\",\"state\":\"ringing\"}\n";
        CHECK(syncn_fd_send(sv[0], line, strlen(line), -1) > 0);

        char got[128] = {0};
        int received = 12345;   /* must be overwritten */
        const ssize_t n = syncn_fd_recv(sv[1], got, sizeof got - 1, &received);

        CHECK(n > 0);
        CHECK_EQ_INT(received, -1);
        CHECK(strstr(got, "ringing") != NULL);

        close(sv[0]);
        close(sv[1]);
    }

    SUITE("fdpass: descriptors are not leaked when the caller wants none");
    {
        /*
         * A receiver that ignores the ancillary data still has the descriptor
         * installed by the kernel. Fifty frames a second with none closed
         * exhausts the table in about twenty seconds, and the failure looks
         * like something else entirely.
         */
        int sv[2];
        socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);

        int before = 0;
        for (int fd = 3; fd < 512; fd++)
            if (fcntl(fd, F_GETFD) != -1)
                before++;

        for (int i = 0; i < 100; i++) {
            const int buffer = make_buffer("x");
            syncn_fd_send(sv[0], "{}\n", 3, buffer);
            close(buffer);

            char got[16];
            syncn_fd_recv(sv[1], got, sizeof got, NULL);  /* deliberately NULL */
        }

        int after = 0;
        for (int fd = 3; fd < 512; fd++)
            if (fcntl(fd, F_GETFD) != -1)
                after++;

        CHECK_EQ_INT(after, before);

        close(sv[0]);
        close(sv[1]);
    }

    SUITE("fdpass: received descriptors are close-on-exec");
    {
        /* Otherwise every frame leaks into any process the daemon or UI spawns,
         * and a buffer stays alive long after the decoder wants it back. */
        int sv[2];
        socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);

        const int buffer = make_buffer("y");
        syncn_fd_send(sv[0], "{}\n", 3, buffer);

        char got[16];
        int received = -1;
        syncn_fd_recv(sv[1], got, sizeof got, &received);

        CHECK(received >= 0);
        CHECK(fcntl(received, F_GETFD) & FD_CLOEXEC);

        close(received);
        close(buffer);
        close(sv[0]);
        close(sv[1]);
    }
}
