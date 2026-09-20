/*
 * Control channel commands (frame marker 0xAA).
 *
 * Payloads are UTF-8 JSON. Outbound commands go on the wire as the exact byte
 * strings in docs/10-protocol-reference.md section 4.1 — they are emitted
 * verbatim rather than serialised, because the door's parser has been observed
 * to care about spelling and these strings are what it was tested against.
 *
 * Inbound parsing is deliberately lenient: the name is read from either a
 * "command" or a "cmd" key, then trimmed and lowercased before matching.
 * Anything unrecognised is ignored rather than treated as an error.
 */
#ifndef SYNCN_COMMAND_H
#define SYNCN_COMMAND_H

#include <stddef.h>
#include <stdint.h>

#include "syncn/frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- outbound --- */

typedef enum {
    /*
     * The three spellings of Answer. Different door firmware revisions accept
     * different forms of the OtherAnswer field — absent, integer 1, or boolean
     * true — so all three are sent, in this order, every time. Sending only one
     * works against some units and silently fails against others.
     */
    SYNCN_OUT_ANSWER_PLAIN = 0, /* {"command":"Answer"}                        */
    SYNCN_OUT_ANSWER_INT,       /* {"command":"Answer","OtherAnswer":1}        */
    SYNCN_OUT_ANSWER_BOOL,      /* {"command":"Answer","OtherAnswer":true}     */

    SYNCN_OUT_START_TALK,       /* {"command":"StartTalk"}                     */
    SYNCN_OUT_OPEN_DOOR,        /* {"command":"OpenDoor"}                      */
    SYNCN_OUT_HANGUP,           /* {"command":"HangUp","OtherAnswer":0}        */
    SYNCN_OUT_DEVICE_BUSY,      /* {"command":"deviceBusy"}                    */

    SYNCN_OUT__COUNT
} syncn_outbound_cmd;

/* The exact payload bytes for a command. Never NULL for a valid enum value;
 * *len receives the byte count, excluding any terminator. */
const char *syncn_command_payload(syncn_outbound_cmd cmd, size_t *len);

/* Short name for logs. */
const char *syncn_outbound_cmd_name(syncn_outbound_cmd cmd);

#define SYNCN_ANSWER_FRAME_COUNT 3

/* The three-frame Answer handshake, in the order it must be sent. Re-sent
 * verbatim whenever the door asks getCallInfo during an active call. */
const syncn_outbound_cmd *syncn_answer_sequence(void);

/* Serialise a command as a complete control frame (header + payload) into
 * `out`. Returns the total byte count, or 0 if `cap` is too small. */
size_t syncn_command_frame(syncn_outbound_cmd cmd, uint8_t *out, size_t cap);

/* Largest frame syncn_command_frame can produce, for sizing stack buffers. */
size_t syncn_command_frame_max(void);

/* -------------------------------------------------------------- inbound --- */

typedef enum {
    SYNCN_IN_UNKNOWN = 0,  /* unrecognised — ignore silently                 */
    SYNCN_IN_CALL,         /* incoming call: ring, start video, notify the UI */
    SYNCN_IN_GET_CALL_INFO,/* re-send the full Answer sequence               */
    SYNCN_IN_HANGUP        /* tear down the call                             */
} syncn_inbound_cmd;

/*
 * Parse a control payload. Never fails: malformed JSON, a missing key or an
 * unknown name all yield SYNCN_IN_UNKNOWN, because a door that sends us
 * something unexpected must not be able to break the call.
 *
 * `payload` need not be NUL-terminated.
 */
syncn_inbound_cmd syncn_command_parse(const uint8_t *payload, size_t len);

const char *syncn_inbound_cmd_name(syncn_inbound_cmd cmd);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_COMMAND_H */
