/*
 * The ring.
 *
 * A two-tone chime played out of the panel's speaker while a call is ringing.
 * The audio engine does not open the device until the call is answered, so
 * the speaker is free during ringing and the ringer opens it directly; it
 * must have let go again before the engine starts, which is why stopping it
 * is synchronous and why the call answers by stopping the ring first.
 *
 * The chime is synthesised, not a file: nothing to install on the panel,
 * nothing to go missing, and the level is known exactly.
 */
#ifndef SYNCN_RINGER_H
#define SYNCN_RINGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNCN_RING_RATE 16000

/* Start ringing on `device`, repeating until stopped. `gain_db` trims the
 * built-in level, which peaks at about -6 dBFS at 0. Idempotent: calling it
 * while ringing does nothing. Returns false if the device could not be
 * opened, in which case the call still rings silently. */
bool syncn_ringer_start(const char *device, float gain_db);

/* Stop, and return only once the device is closed. Idempotent. */
void syncn_ringer_stop(void);

bool syncn_ringer_active(void);

/*
 * Render one cycle of the chime -- ding, dong, then silence -- into `out`.
 * Returns the number of samples written, which is always `cap` or fewer.
 * Exposed so the sound can be tested without a speaker.
 */
size_t syncn_ringer_synth(int16_t *out, size_t cap, float gain_db);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_RINGER_H */
