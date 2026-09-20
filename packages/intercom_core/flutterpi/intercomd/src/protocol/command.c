#include "syncn/command.h"

#include <cjson/cJSON.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- outbound --- */

/*
 * Exact wire bytes. Do not reformat these — no added spaces, no reordered
 * keys. They are what the door firmware was tested against.
 */
static const struct {
    const char *payload;
    const char *name;
} outbound[SYNCN_OUT__COUNT] = {
    [SYNCN_OUT_ANSWER_PLAIN] = { "{\"command\":\"Answer\"}",                     "Answer"      },
    [SYNCN_OUT_ANSWER_INT]   = { "{\"command\":\"Answer\",\"OtherAnswer\":1}",   "Answer(1)"   },
    [SYNCN_OUT_ANSWER_BOOL]  = { "{\"command\":\"Answer\",\"OtherAnswer\":true}","Answer(true)"},
    [SYNCN_OUT_START_TALK]   = { "{\"command\":\"StartTalk\"}",                  "StartTalk"   },
    [SYNCN_OUT_OPEN_DOOR]    = { "{\"command\":\"OpenDoor\"}",                   "OpenDoor"    },
    [SYNCN_OUT_HANGUP]       = { "{\"command\":\"HangUp\",\"OtherAnswer\":0}",   "HangUp"      },
    [SYNCN_OUT_DEVICE_BUSY]  = { "{\"command\":\"deviceBusy\"}",                 "deviceBusy"  },
};

const char *syncn_command_payload(syncn_outbound_cmd cmd, size_t *len)
{
    if (cmd < 0 || cmd >= SYNCN_OUT__COUNT) {
        if (len)
            *len = 0;
        return NULL;
    }
    const char *p = outbound[cmd].payload;
    if (len)
        *len = strlen(p);
    return p;
}

const char *syncn_outbound_cmd_name(syncn_outbound_cmd cmd)
{
    if (cmd < 0 || cmd >= SYNCN_OUT__COUNT)
        return "invalid";
    return outbound[cmd].name;
}

static const syncn_outbound_cmd answer_seq[SYNCN_ANSWER_FRAME_COUNT] = {
    SYNCN_OUT_ANSWER_PLAIN,
    SYNCN_OUT_ANSWER_INT,
    SYNCN_OUT_ANSWER_BOOL,
};

const syncn_outbound_cmd *syncn_answer_sequence(void)
{
    return answer_seq;
}

size_t syncn_command_frame(syncn_outbound_cmd cmd, uint8_t *out, size_t cap)
{
    size_t plen = 0;
    const char *payload = syncn_command_payload(cmd, &plen);
    if (!payload)
        return 0;

    const size_t total = SYNCN_FRAME_HEADER_SIZE + plen;
    if (cap < total)
        return 0;

    syncn_frame_write_header(out, SYNCN_CH_CONTROL, (uint32_t)plen);
    memcpy(out + SYNCN_FRAME_HEADER_SIZE, payload, plen);
    return total;
}

size_t syncn_command_frame_max(void)
{
    size_t largest = 0;
    for (int i = 0; i < SYNCN_OUT__COUNT; i++) {
        const size_t n = strlen(outbound[i].payload);
        if (n > largest)
            largest = n;
    }
    return SYNCN_FRAME_HEADER_SIZE + largest;
}

/* -------------------------------------------------------------- inbound --- */

/* Trim ASCII whitespace from both ends and lowercase in place. */
static void normalise(char *s)
{
    char *start = s;
    while (*start && isspace((unsigned char)*start))
        start++;

    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1]))
        end--;

    const size_t n = (size_t)(end - start);
    if (start != s)
        memmove(s, start, n);
    s[n] = '\0';

    for (char *p = s; *p; p++)
        *p = (char)tolower((unsigned char)*p);
}

syncn_inbound_cmd syncn_command_parse(const uint8_t *payload, size_t len)
{
    if (!payload || len == 0)
        return SYNCN_IN_UNKNOWN;

    cJSON *root = cJSON_ParseWithLength((const char *)payload, len);
    if (!root)
        return SYNCN_IN_UNKNOWN;

    /* Either key is accepted. Doors have been seen using both. */
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (!cJSON_IsString(item))
        item = cJSON_GetObjectItemCaseSensitive(root, "cmd");

    syncn_inbound_cmd result = SYNCN_IN_UNKNOWN;

    if (cJSON_IsString(item) && item->valuestring) {
        char name[64];
        const size_t n = strlen(item->valuestring);
        if (n < sizeof name) {
            memcpy(name, item->valuestring, n + 1);
            normalise(name);

            if (strcmp(name, "call") == 0)
                result = SYNCN_IN_CALL;
            else if (strcmp(name, "getcallinfo") == 0)
                result = SYNCN_IN_GET_CALL_INFO;
            else if (strcmp(name, "hangup") == 0)
                result = SYNCN_IN_HANGUP;
        }
    }

    cJSON_Delete(root);
    return result;
}

const char *syncn_inbound_cmd_name(syncn_inbound_cmd cmd)
{
    switch (cmd) {
    case SYNCN_IN_CALL:          return "call";
    case SYNCN_IN_GET_CALL_INFO: return "getCallInfo";
    case SYNCN_IN_HANGUP:        return "hangUp";
    case SYNCN_IN_UNKNOWN:
    default:                     return "unknown";
    }
}
