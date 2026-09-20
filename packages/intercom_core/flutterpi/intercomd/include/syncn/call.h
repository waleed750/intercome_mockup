/*
 * Call orchestration: the state machine, and the poll loop that drives it.
 *
 * Owns the connection, the audio engine, the discovery responder and the
 * listening socket. Every timing quirk the door firmware demands lives here and
 * is commented where it appears, because each one was established by capture
 * against real hardware rather than chosen.
 */
#ifndef SYNCN_CALL_H
#define SYNCN_CALL_H

#include <stdbool.h>

#include "syncn/config.h"
#include "syncn/discovery.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYNCN_CALL_IDLE = 0,
    SYNCN_CALL_CONNECTING, /* dialled out, handshake in flight   */
    SYNCN_CALL_RINGING,    /* door called us, waiting for answer  */
    SYNCN_CALL_CONNECTED,  /* media flowing                       */
    SYNCN_CALL_PREVIEW     /* video only, no microphone           */
} syncn_call_state;

const char *syncn_call_state_name(syncn_call_state s);

typedef struct syncn_call syncn_call;

syncn_call *syncn_call_create(const syncn_config *cfg);
void        syncn_call_destroy(syncn_call *c);

/* Runs until syncn_call_stop(). `interactive` also watches stdin for keys. */
void syncn_call_run(syncn_call *c, bool interactive);
void syncn_call_stop(syncn_call *c);

bool syncn_call_dial(syncn_call *c);     /* panel-initiated call   */
bool syncn_call_preview(syncn_call *c);  /* video peek, no audio   */
void syncn_call_answer(syncn_call *c);   /* accept a ringing call  */
void syncn_call_hangup(syncn_call *c);
void syncn_call_unlock(syncn_call *c);   /* works with or without a call */
void syncn_call_toggle_mute(syncn_call *c);

syncn_call_state syncn_call_get_state(const syncn_call *c);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_CALL_H */
