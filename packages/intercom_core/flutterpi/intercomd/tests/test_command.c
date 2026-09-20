#include "test.h"
#include "syncn/command.h"

#include <string.h>

static int parse(const char *json)
{
    return (int)syncn_command_parse((const uint8_t *)json, strlen(json));
}

void suite_command(void)
{
    SUITE("command: outbound payloads are byte-exact");
    {
        /* These strings are the contract with the door. If a refactor
         * reformats them — an added space, a reordered key — the door may stop
         * responding, and it will look like a network fault. */
        size_t len = 0;
        const char *p;

        p = syncn_command_payload(SYNCN_OUT_ANSWER_PLAIN, &len);
        CHECK(strcmp(p, "{\"command\":\"Answer\"}") == 0);
        CHECK_EQ_INT(len, strlen(p));

        p = syncn_command_payload(SYNCN_OUT_ANSWER_INT, &len);
        CHECK(strcmp(p, "{\"command\":\"Answer\",\"OtherAnswer\":1}") == 0);

        p = syncn_command_payload(SYNCN_OUT_ANSWER_BOOL, &len);
        CHECK(strcmp(p, "{\"command\":\"Answer\",\"OtherAnswer\":true}") == 0);

        p = syncn_command_payload(SYNCN_OUT_START_TALK, &len);
        CHECK(strcmp(p, "{\"command\":\"StartTalk\"}") == 0);

        p = syncn_command_payload(SYNCN_OUT_OPEN_DOOR, &len);
        CHECK(strcmp(p, "{\"command\":\"OpenDoor\"}") == 0);

        p = syncn_command_payload(SYNCN_OUT_HANGUP, &len);
        CHECK(strcmp(p, "{\"command\":\"HangUp\",\"OtherAnswer\":0}") == 0);

        p = syncn_command_payload(SYNCN_OUT_DEVICE_BUSY, &len);
        CHECK(strcmp(p, "{\"command\":\"deviceBusy\"}") == 0);

        /* Out-of-range must not walk off the table. */
        CHECK(syncn_command_payload((syncn_outbound_cmd)-1, &len) == NULL);
        CHECK_EQ_INT(len, 0);
        CHECK(syncn_command_payload(SYNCN_OUT__COUNT, &len) == NULL);
    }

    SUITE("command: the Answer handshake is all three spellings, in order");
    {
        const syncn_outbound_cmd *seq = syncn_answer_sequence();
        CHECK_EQ_INT(seq[0], SYNCN_OUT_ANSWER_PLAIN);
        CHECK_EQ_INT(seq[1], SYNCN_OUT_ANSWER_INT);
        CHECK_EQ_INT(seq[2], SYNCN_OUT_ANSWER_BOOL);
        CHECK_EQ_INT(SYNCN_ANSWER_FRAME_COUNT, 3);
    }

    SUITE("command: framing");
    {
        uint8_t buf[256];
        const size_t n = syncn_command_frame(SYNCN_OUT_START_TALK, buf, sizeof buf);
        const char *expect = "{\"command\":\"StartTalk\"}";

        CHECK_EQ_INT(n, SYNCN_FRAME_HEADER_SIZE + strlen(expect));
        CHECK_EQ_INT(buf[0], 0xAA);
        CHECK_EQ_INT(buf[1], 0xAA);
        CHECK_EQ_INT(buf[2], 0xAA);
        CHECK_EQ_INT(buf[3], 0xAA);
        CHECK_EQ_INT(buf[4], strlen(expect));
        CHECK_EQ_INT(buf[5], 0);
        CHECK_EQ_MEM(buf + SYNCN_FRAME_HEADER_SIZE, expect, strlen(expect));

        /* Too small a buffer writes nothing rather than overflowing. */
        uint8_t tiny[4] = {0, 0, 0, 0};
        CHECK_EQ_INT(syncn_command_frame(SYNCN_OUT_START_TALK, tiny, sizeof tiny), 0);
        CHECK_EQ_INT(tiny[0], 0);

        CHECK(syncn_command_frame_max() >= n);
    }

    SUITE("command: inbound recognition");
    {
        CHECK_EQ_INT(parse("{\"command\":\"call\"}"),        SYNCN_IN_CALL);
        CHECK_EQ_INT(parse("{\"command\":\"getCallInfo\"}"), SYNCN_IN_GET_CALL_INFO);
        CHECK_EQ_INT(parse("{\"command\":\"hangUp\"}"),      SYNCN_IN_HANGUP);

        /* Either key is accepted — doors have been seen using both. */
        CHECK_EQ_INT(parse("{\"cmd\":\"call\"}"),            SYNCN_IN_CALL);
        CHECK_EQ_INT(parse("{\"cmd\":\"hangUp\"}"),          SYNCN_IN_HANGUP);

        /* "command" wins when both are present. */
        CHECK_EQ_INT(parse("{\"command\":\"call\",\"cmd\":\"hangUp\"}"), SYNCN_IN_CALL);
    }

    SUITE("command: inbound parsing is lenient about case and whitespace");
    {
        CHECK_EQ_INT(parse("{\"command\":\"CALL\"}"),          SYNCN_IN_CALL);
        CHECK_EQ_INT(parse("{\"command\":\"Call\"}"),          SYNCN_IN_CALL);
        CHECK_EQ_INT(parse("{\"command\":\"  call  \"}"),      SYNCN_IN_CALL);
        CHECK_EQ_INT(parse("{\"command\":\"\\tHangUp\\n\"}"),  SYNCN_IN_HANGUP);
        CHECK_EQ_INT(parse("{\"command\":\"GETCALLINFO\"}"),   SYNCN_IN_GET_CALL_INFO);
        CHECK_EQ_INT(parse("{ \"command\" : \"call\" }"),      SYNCN_IN_CALL);
    }

    SUITE("command: anything unrecognised is ignored, never fatal");
    {
        /* A door sending us nonsense must not be able to break a call. */
        CHECK_EQ_INT(parse("{\"command\":\"Answer\"}"),  SYNCN_IN_UNKNOWN);
        CHECK_EQ_INT(parse("{\"command\":\"nonsense\"}"),SYNCN_IN_UNKNOWN);
        CHECK_EQ_INT(parse("{\"command\":123}"),         SYNCN_IN_UNKNOWN);
        CHECK_EQ_INT(parse("{\"other\":\"call\"}"),      SYNCN_IN_UNKNOWN);
        CHECK_EQ_INT(parse("{\"command\":null}"),        SYNCN_IN_UNKNOWN);
        CHECK_EQ_INT(parse("not json at all"),           SYNCN_IN_UNKNOWN);
        CHECK_EQ_INT(parse("{broken"),                   SYNCN_IN_UNKNOWN);
        CHECK_EQ_INT(parse("[]"),                        SYNCN_IN_UNKNOWN);
        CHECK_EQ_INT(parse("{}"),                        SYNCN_IN_UNKNOWN);
        CHECK_EQ_INT(parse(""),                          SYNCN_IN_UNKNOWN);
        CHECK_EQ_INT(syncn_command_parse(NULL, 0),       SYNCN_IN_UNKNOWN);

        /* An absurdly long name must not overflow the scratch buffer. */
        char big[512];
        memset(big, 'x', sizeof big);
        memcpy(big, "{\"command\":\"", 12);
        memcpy(big + sizeof big - 4, "\"}", 3);
        CHECK_EQ_INT(syncn_command_parse((const uint8_t *)big, strlen(big)), SYNCN_IN_UNKNOWN);
    }

    SUITE("command: payload need not be NUL-terminated");
    {
        /* Payloads come straight out of the frame parser, pointing into the
         * receive buffer with more bytes after them. */
        const char raw[] = "{\"command\":\"call\"}GARBAGE AFTER";
        CHECK_EQ_INT(syncn_command_parse((const uint8_t *)raw, 18), SYNCN_IN_CALL);
    }

    SUITE("command: names for logging");
    {
        CHECK(strcmp(syncn_inbound_cmd_name(SYNCN_IN_CALL), "call") == 0);
        CHECK(strcmp(syncn_inbound_cmd_name(SYNCN_IN_UNKNOWN), "unknown") == 0);
        CHECK(strcmp(syncn_outbound_cmd_name(SYNCN_OUT_OPEN_DOOR), "OpenDoor") == 0);
        CHECK(strcmp(syncn_outbound_cmd_name((syncn_outbound_cmd)99), "invalid") == 0);
    }
}
