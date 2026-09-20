#include "test.h"
#include "syncn/discovery.h"

#include <cjson/cJSON.h>
#include <string.h>

/* Pull a string field out of the reply so assertions read clearly. */
static int reply_str_is(const char *json, const char *key, const char *want)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return 0;
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    const int ok = cJSON_IsString(it) && it->valuestring && strcmp(it->valuestring, want) == 0;
    cJSON_Delete(root);
    return ok;
}

static int reply_num_is(const char *json, const char *key, int want)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return 0;
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    const int ok = cJSON_IsNumber(it) && it->valueint == want;
    cJSON_Delete(root);
    return ok;
}

static int reply_has(const char *json, const char *key)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return 0;
    const int ok = cJSON_HasObjectItem(root, key);
    cJSON_Delete(root);
    return ok;
}

void suite_discovery(void)
{
    SUITE("discovery: request recognition");
    {
        syncn_discovery_request req;

        const char *a = "{\"command\":\"cmd_send_get_call_device\",\"localAddr\":\"01\"}";
        CHECK(syncn_discovery_parse(a, strlen(a), &req));
        CHECK(req.is_request);
        CHECK(strcmp(req.dst_addr, "01") == 0);
        CHECK_EQ_INT(req.dst_type, SYNCN_DEFAULT_DST_TYPE);

        const char *b = "{\"command\":\"cmd_send_get_device_info\",\"localAddr\":\"door-1\",\"localType\":7}";
        CHECK(syncn_discovery_parse(b, strlen(b), &req));
        CHECK(strcmp(req.dst_addr, "door-1") == 0);
        CHECK_EQ_INT(req.dst_type, 7);
    }

    SUITE("discovery: the token check is on raw text, not parsed JSON");
    {
        /* A datagram we cannot parse must still be recognised — answering a
         * malformed request beats ignoring a door that is trying to find us. */
        syncn_discovery_request req;
        const char *broken = "{\"command\":\"cmd_send_get_device_info\", this is not json";
        CHECK(syncn_discovery_parse(broken, strlen(broken), &req));
        CHECK(req.is_request);
        CHECK_EQ_INT(req.dst_addr[0], 0);          /* no address recovered */
        CHECK_EQ_INT(req.dst_type, SYNCN_DEFAULT_DST_TYPE);

        /* Token anywhere in the text counts. */
        const char *padded = "\x01\x02 cmd_send_get_call_device trailing";
        CHECK(syncn_discovery_parse(padded, strlen(padded), &req));
    }

    SUITE("discovery: non-requests are rejected");
    {
        syncn_discovery_request req;
        const char *other = "{\"command\":\"cmd_reply_get_device_info\"}";
        CHECK(!syncn_discovery_parse(other, strlen(other), &req));
        CHECK(!req.is_request);

        CHECK(!syncn_discovery_parse("", 0, &req));
        CHECK(!syncn_discovery_parse(NULL, 0, &req));
        CHECK(!syncn_discovery_parse("hello", 5, &req));

        /* A near miss must not match. */
        const char *nearly = "cmd_send_get_device_inf";
        CHECK(!syncn_discovery_parse(nearly, strlen(nearly), &req));
    }

    SUITE("discovery: reply contents");
    {
        syncn_discovery_request req;
        const char *r = "{\"command\":\"cmd_send_get_device_info\",\"localAddr\":\"door-9\",\"localType\":5}";
        CHECK(syncn_discovery_parse(r, strlen(r), &req));

        syncn_screen_info info;
        syncn_screen_info_init(&info);
        info.alias    = "Villa Entrance";
        info.serial   = "SN-0001";
        info.local_ip = "192.168.100.210";

        char out[1024];
        const size_t n = syncn_discovery_build_reply(&info, &req, out, sizeof out);
        CHECK(n > 0);
        CHECK_EQ_INT(strlen(out), n);

        CHECK(reply_str_is(out, "command",  "cmd_reply_get_device_info"));
        CHECK(reply_str_is(out, "appid",    SYNCN_DEFAULT_APPID));
        CHECK(reply_str_is(out, "alias",    "Villa Entrance"));
        CHECK(reply_str_is(out, "group_ip", SYNCN_DEFAULT_GROUP_IP));
        CHECK(reply_str_is(out, "serial",   "SN-0001"));
        CHECK(reply_str_is(out, "ip",       "192.168.100.210"));
        CHECK(reply_str_is(out, "localIp",  "192.168.100.210"));

        /* dstType and dstAddr are echoed straight back from the request. */
        CHECK(reply_num_is(out, "dstType", 5));
        CHECK(reply_str_is(out, "dstAddr", "door-9"));

        CHECK(reply_num_is(out, "verify",       0));
        CHECK(reply_num_is(out, "deviceBusy",   0));
        CHECK(reply_num_is(out, "camera_en",    0));
        CHECK(reply_num_is(out, "relay0_delay", 1));
    }

    SUITE("discovery: an unknown local IP omits ip and localIp entirely");
    {
        /* Emitting "" would be worse than absence — the door treats an empty
         * string as an address and tries to connect to it. */
        syncn_screen_info info;
        syncn_screen_info_init(&info);
        info.alias = "Panel";

        char out[1024];
        CHECK(syncn_discovery_build_reply(&info, NULL, out, sizeof out) > 0);
        CHECK(!reply_has(out, "ip"));
        CHECK(!reply_has(out, "localIp"));

        info.local_ip = "";
        CHECK(syncn_discovery_build_reply(&info, NULL, out, sizeof out) > 0);
        CHECK(!reply_has(out, "ip"));
        CHECK(!reply_has(out, "localIp"));

        info.local_ip = "10.0.0.5";
        CHECK(syncn_discovery_build_reply(&info, NULL, out, sizeof out) > 0);
        CHECK(reply_has(out, "ip"));
        CHECK(reply_has(out, "localIp"));
    }

    SUITE("discovery: busy flag and defaults");
    {
        syncn_screen_info info;
        syncn_screen_info_init(&info);
        CHECK(strcmp(info.appid, SYNCN_DEFAULT_APPID) == 0);
        CHECK(strcmp(info.group_ip, SYNCN_DEFAULT_GROUP_IP) == 0);
        CHECK_EQ_INT(info.camera_en, 0);
        CHECK_EQ_INT(info.relay0_delay, 1);

        info.device_busy = 1;
        char out[1024];
        CHECK(syncn_discovery_build_reply(&info, NULL, out, sizeof out) > 0);
        CHECK(reply_num_is(out, "deviceBusy", 1));

        /* A NULL request still produces a valid reply with defaults. */
        CHECK(reply_num_is(out, "dstType", SYNCN_DEFAULT_DST_TYPE));
        CHECK(reply_str_is(out, "dstAddr", ""));
    }

    SUITE("discovery: a buffer that is too small writes nothing");
    {
        syncn_screen_info info;
        syncn_screen_info_init(&info);
        char tiny[16] = {0};
        CHECK_EQ_INT(syncn_discovery_build_reply(&info, NULL, tiny, sizeof tiny), 0);
        CHECK_EQ_INT(tiny[0], 0);
    }

    SUITE("discovery: an overlong localAddr is truncated, not overflowed");
    {
        char req_json[512];
        char addr[256];
        memset(addr, 'A', sizeof addr - 1);
        addr[sizeof addr - 1] = '\0';
        snprintf(req_json, sizeof req_json,
                 "{\"command\":\"cmd_send_get_device_info\",\"localAddr\":\"%s\"}", addr);

        syncn_discovery_request req;
        CHECK(syncn_discovery_parse(req_json, strlen(req_json), &req));
        CHECK_EQ_INT(strlen(req.dst_addr), SYNCN_ADDR_MAX - 1);
    }

    SUITE("discovery: ports match the protocol");
    {
        CHECK_EQ_INT(SYNCN_DISCOVERY_PORT, 8089);
        CHECK_EQ_INT(SYNCN_CALL_PORT, 8189);
    }
}
