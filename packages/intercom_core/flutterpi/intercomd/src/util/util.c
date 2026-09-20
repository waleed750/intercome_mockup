#include "syncn/util.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static syncn_log_level g_level = SYNCN_LOG_INFO;

static const char *const LEVEL_NAME[] = { "error", "warn", "info", "debug", "trace" };

void syncn_log_set_level(syncn_log_level level) { g_level = level; }

syncn_log_level syncn_log_level_from_name(const char *name)
{
    if (!name)
        return SYNCN_LOG_INFO;
    for (size_t i = 0; i < sizeof LEVEL_NAME / sizeof LEVEL_NAME[0]; i++) {
        if (strcasecmp(name, LEVEL_NAME[i]) == 0)
            return (syncn_log_level)i;
    }
    return SYNCN_LOG_INFO;
}

void syncn_log(syncn_log_level level, const char *fmt, ...)
{
    if (level > g_level)
        return;

    /* Elapsed rather than wall clock: when reading the log of a failing call,
     * "1.240" is more useful than a timestamp. */
    static uint64_t t0 = 0;
    const uint64_t now = syncn_now_ms();
    if (!t0)
        t0 = now;

    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    fprintf(stderr, "[%8.3f] %-5s %s\n",
            (double)(now - t0) / 1000.0, LEVEL_NAME[level], line);
    fflush(stderr);
}

uint64_t syncn_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

int syncn_set_nonblock(int fd)
{
    const int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Name of the first up, non-loopback interface with an IPv4 address, and
 * optionally that address. One walk serves both the discovery reply's ip field
 * and the serial derived from the same interface's MAC, so the two can never
 * describe different NICs. */
static bool first_ipv4_iface(char *name_out, size_t name_cap,
                             char *addr_out, size_t addr_cap)
{
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0)
        return false;

    bool found = false;
    for (struct ifaddrs *p = ifa; p && !found; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK))
            continue;

        const struct sockaddr_in *sin = (const struct sockaddr_in *)p->ifa_addr;
        char addr[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &sin->sin_addr, addr, sizeof addr))
            continue;

        if (addr_out) {
            if (strlen(addr) + 1 > addr_cap)
                continue;
            memcpy(addr_out, addr, strlen(addr) + 1);
        }
        if (name_out && p->ifa_name)
            snprintf(name_out, name_cap, "%s", p->ifa_name);
        found = true;
    }
    freeifaddrs(ifa);
    return found;
}

bool syncn_local_ipv4(char *out, size_t cap)
{
    return first_ipv4_iface(NULL, 0, out, cap);
}

bool syncn_local_mac_hex(char *out, size_t cap)
{
    /* 12 digits and a terminator. Anything shorter would silently truncate a
     * serial, which is worse than reporting that we have none. */
    if (cap < 13)
        return false;

    char want[IF_NAMESIZE] = "";
    const bool have_name = first_ipv4_iface(want, sizeof want, NULL, 0);

    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0)
        return false;

    bool found = false;
    for (struct ifaddrs *p = ifa; p && !found; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_PACKET)
            continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK))
            continue;
        if (have_name && want[0] && p->ifa_name && strcmp(p->ifa_name, want) != 0)
            continue;

        const struct sockaddr_ll *ll = (const struct sockaddr_ll *)p->ifa_addr;
        if (ll->sll_halen != 6)
            continue;

        /* An all-zero MAC is what an interface that has not come up yet
         * reports. Sending it as a serial would hand every panel on the
         * network the same identity. */
        bool all_zero = true;
        for (int i = 0; i < 6; i++) {
            if (ll->sll_addr[i] != 0)
                all_zero = false;
        }
        if (all_zero)
            continue;

        snprintf(out, cap, "%02x%02x%02x%02x%02x%02x",
                 ll->sll_addr[0], ll->sll_addr[1], ll->sll_addr[2],
                 ll->sll_addr[3], ll->sll_addr[4], ll->sll_addr[5]);
        found = true;
    }
    freeifaddrs(ifa);
    return found;
}
