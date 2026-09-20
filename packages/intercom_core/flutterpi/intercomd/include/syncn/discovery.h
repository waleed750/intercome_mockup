/*
 * Device discovery (UDP 8089).
 *
 * Plain UTF-8 JSON datagrams with no framing header. The door broadcasts to
 * find panels; the panel replies with its ScreenInfo. Both directions use
 * port 8089.
 *
 * This runs even when the door's address is statically configured. Static
 * config covers panel-to-door; the responder covers door-to-panel, and without
 * it the door may never learn the panel exists, so incoming calls never arrive.
 *
 * See docs/10-protocol-reference.md section 2.
 */
#ifndef SYNCN_DISCOVERY_H
#define SYNCN_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNCN_DISCOVERY_PORT      8089
#define SYNCN_CALL_PORT           8189

/* Vendor app id. The door ignores a reply that does not carry this. */
#define SYNCN_DEFAULT_APPID       "7551000"
#define SYNCN_DEFAULT_GROUP_IP    "239.255.74.199"
#define SYNCN_DEFAULT_DST_TYPE    3

#define SYNCN_ADDR_MAX            64

/* What we understood from an inbound datagram. */
typedef struct {
    bool is_request;                 /* false: not a discovery request at all */
    char dst_addr[SYNCN_ADDR_MAX];   /* echoed back from the door's localAddr  */
    int  dst_type;                   /* echoed back; SYNCN_DEFAULT_DST_TYPE if absent */
} syncn_discovery_request;

/*
 * Classify a datagram. A datagram is a discovery request if its raw text
 * contains "cmd_send_get_call_device" or "cmd_send_get_device_info" — the
 * check is on the bytes, not on parsed JSON, so a datagram we cannot parse is
 * still recognised.
 *
 * Returns true if this is a request. `out` is always initialised.
 */
bool syncn_discovery_parse(const void *datagram, size_t len,
                           syncn_discovery_request *out);

/* What the panel says about itself. Any NULL string takes its default; a NULL
 * or empty local_ip causes the ip and localIp fields to be omitted entirely,
 * which is what the reference implementation does when the address is unknown. */
typedef struct {
    const char *alias;        /* panel display name shown on the door         */
    const char *serial;       /* panel serial number                          */
    const char *local_ip;     /* NULL or "" omits both ip and localIp         */
    const char *appid;        /* NULL -> SYNCN_DEFAULT_APPID                  */
    const char *group_ip;     /* NULL -> SYNCN_DEFAULT_GROUP_IP               */
    int         verify;       /* pairing verification, 0 = off                */
    int         device_busy;  /* 1 while a call is in progress                */
    int         camera_en;    /* 0 — the panel sends no video                 */
    int         relay0_delay; /* door relay hold time                         */
} syncn_screen_info;

/* Sensible defaults: appid, group_ip, verify 0, device_busy 0, camera_en 0,
 * relay0_delay 1, everything else NULL. */
void syncn_screen_info_init(syncn_screen_info *info);

/*
 * Build the reply datagram. Writes a NUL-terminated JSON object into `out` and
 * returns its length excluding the NUL, or 0 if it did not fit.
 */
size_t syncn_discovery_build_reply(const syncn_screen_info *info,
                                   const syncn_discovery_request *req,
                                   char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_DISCOVERY_H */
