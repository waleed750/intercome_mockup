/* Logging, time and small helpers shared across the daemon. */
#ifndef SYNCN_UTIL_H
#define SYNCN_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYNCN_LOG_ERROR = 0,
    SYNCN_LOG_WARN,
    SYNCN_LOG_INFO,
    SYNCN_LOG_DEBUG,
    SYNCN_LOG_TRACE
} syncn_log_level;

void            syncn_log_set_level(syncn_log_level level);
syncn_log_level syncn_log_level_from_name(const char *name);
void            syncn_log(syncn_log_level level, const char *fmt, ...)
                    __attribute__((format(printf, 2, 3)));

#define LOG_ERR(...)   syncn_log(SYNCN_LOG_ERROR, __VA_ARGS__)
#define LOG_WARN(...)  syncn_log(SYNCN_LOG_WARN,  __VA_ARGS__)
#define LOG_INFO(...)  syncn_log(SYNCN_LOG_INFO,  __VA_ARGS__)
#define LOG_DBG(...)   syncn_log(SYNCN_LOG_DEBUG, __VA_ARGS__)
#define LOG_TRACE(...) syncn_log(SYNCN_LOG_TRACE, __VA_ARGS__)

/* Monotonic milliseconds. Never goes backwards, unlike wall-clock time, which
 * matters because every timeout in this daemon is a deadline. */
uint64_t syncn_now_ms(void);

int syncn_set_nonblock(int fd);

/* Best-effort local IPv4 address for the discovery reply, written as a dotted
 * string. Returns false if none could be determined, in which case the reply
 * omits the ip and localIp fields entirely. */
bool syncn_local_ipv4(char *out, size_t cap);

/* The MAC of the interface carrying that address, as 12 lowercase hex digits
 * with no separators — the form the door's own indoor units advertise as their
 * serial ("fa72a237a18d"). Returns false if no usable interface was found. */
bool syncn_local_mac_hex(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_UTIL_H */
