/*
 * G.711 A-law codec — the door station's audio format.
 *
 * 8000 Hz, mono, 160 samples (20 ms) per frame. Both directions use the
 * identical format. See docs/10-protocol-reference.md section 5.
 *
 * A-law is narrowband by definition. Nothing here can widen it, and the door
 * side is fixed, so "muffled" is a property of the link rather than a defect
 * to chase.
 */
#ifndef SYNCN_ALAW_H
#define SYNCN_ALAW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNCN_AUDIO_RATE           8000u
#define SYNCN_AUDIO_CHANNELS       1u
#define SYNCN_AUDIO_FRAME_MS       20u
#define SYNCN_AUDIO_FRAME_SAMPLES  160u   /* 20 ms at 8 kHz */
#define SYNCN_AUDIO_FRAME_BYTES    160u   /* one byte per sample in A-law */

/*
 * The encoding of PCM zero, and therefore the byte a silence frame is filled
 * with. A-law has no exact zero; 0xD5 decodes to +8, the closest representable
 * value.
 *
 * This constant is load-bearing. The door gates its own downlink on seeing a
 * continuous uplink stream, so when there is nothing to send we still send
 * frames — filled with this — at the same size and cadence. Stop sending and
 * the door goes quiet.
 */
#define SYNCN_ALAW_SILENCE         0xD5u

uint8_t syncn_alaw_encode_sample(int16_t pcm);
int16_t syncn_alaw_decode_sample(uint8_t alaw);

/* Bulk helpers. `out` must hold `count` elements. In-place is not supported. */
void syncn_alaw_encode(const int16_t *pcm, size_t count, uint8_t *out);
void syncn_alaw_decode(const uint8_t *alaw, size_t count, int16_t *out);

/* Fill a buffer with encoded silence. */
void syncn_alaw_fill_silence(uint8_t *out, size_t count);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_ALAW_H */
