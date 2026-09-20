#include "syncn/discovery.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const REQUEST_TOKENS[] = {
    "cmd_send_get_call_device",
    "cmd_send_get_device_info",
};

/* memmem is a GNU extension; this keeps the module portable and the datagrams
 * are a few hundred bytes, so a naive scan costs nothing. */
static bool contains(const char *hay, size_t hay_len, const char *needle)
{
    const size_t n = strlen(needle);
    if (n == 0 || hay_len < n)
        return false;
    for (size_t i = 0; i + n <= hay_len; i++) {
        if (memcmp(hay + i, needle, n) == 0)
            return true;
    }
    return false;
}

bool syncn_discovery_parse(const void *datagram, size_t len,
                           syncn_discovery_request *out)
{
    memset(out, 0, sizeof *out);
    out->dst_type = SYNCN_DEFAULT_DST_TYPE;

    if (!datagram || len == 0)
        return false;

    const char *text = datagram;

    bool matched = false;
    for (size_t i = 0; i < sizeof REQUEST_TOKENS / sizeof REQUEST_TOKENS[0]; i++) {
        if (contains(text, len, REQUEST_TOKENS[i])) {
            matched = true;
            break;
        }
    }
    if (!matched)
        return false;

    out->is_request = true;

    /* The address fields are a bonus: if the JSON is malformed we still answer,
     * just with the defaults. Recognising the request matters more than
     * parsing it perfectly. */
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (root) {
        const cJSON *addr = cJSON_GetObjectItemCaseSensitive(root, "localAddr");
        if (cJSON_IsString(addr) && addr->valuestring) {
            snprintf(out->dst_addr, sizeof out->dst_addr, "%s", addr->valuestring);
        }

        const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "localType");
        if (cJSON_IsNumber(type))
            out->dst_type = type->valueint;

        cJSON_Delete(root);
    }

    return true;
}

void syncn_screen_info_init(syncn_screen_info *info)
{
    memset(info, 0, sizeof *info);
    info->appid        = SYNCN_DEFAULT_APPID;
    info->group_ip     = SYNCN_DEFAULT_GROUP_IP;
    info->verify       = 0;
    info->device_busy  = 0;
    info->camera_en    = 0; /* the panel has no outgoing camera */
    info->relay0_delay = 1;
}

size_t syncn_discovery_build_reply(const syncn_screen_info *info,
                                   const syncn_discovery_request *req,
                                   char *out, size_t cap)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return 0;

    const char *appid    = info->appid    ? info->appid    : SYNCN_DEFAULT_APPID;
    const char *group_ip = info->group_ip ? info->group_ip : SYNCN_DEFAULT_GROUP_IP;
    const char *alias    = info->alias    ? info->alias    : "";
    const char *serial   = info->serial   ? info->serial   : "";

    /* Field order mirrors the reference implementation. Order should not matter
     * to a JSON parser, but this protocol was reverse-engineered and matching
     * the observed output costs nothing. */
    cJSON_AddStringToObject(root, "command",  "cmd_reply_get_device_info");
    cJSON_AddStringToObject(root, "appid",    appid);
    cJSON_AddStringToObject(root, "alias",    alias);
    cJSON_AddStringToObject(root, "group_ip", group_ip);
    cJSON_AddStringToObject(root, "serial",   serial);
    cJSON_AddNumberToObject(root, "dstType",  req ? req->dst_type : SYNCN_DEFAULT_DST_TYPE);
    cJSON_AddStringToObject(root, "dstAddr",  (req && req->dst_addr[0]) ? req->dst_addr : "");

    /* Both emitted, or neither. An empty string here is worse than absence —
     * the door treats "" as an address and tries to use it. */
    if (info->local_ip && info->local_ip[0]) {
        cJSON_AddStringToObject(root, "ip",      info->local_ip);
        cJSON_AddStringToObject(root, "localIp", info->local_ip);
    }

    cJSON_AddNumberToObject(root, "verify",       info->verify);
    cJSON_AddNumberToObject(root, "deviceBusy",   info->device_busy);
    cJSON_AddNumberToObject(root, "camera_en",    info->camera_en);
    cJSON_AddNumberToObject(root, "relay0_delay", info->relay0_delay);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
        return 0;

    const size_t n = strlen(json);
    if (n + 1 > cap) {
        free(json);
        return 0;
    }
    memcpy(out, json, n + 1);
    free(json);
    return n;
}
