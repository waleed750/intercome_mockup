/*
 * A neural denoiser on 8 kHz frames.
 *
 * Why not the speex preprocessor: it works band by band on the ratio of
 * signal to its noise estimate, and this panel's microphone delivers speech
 * only about 9 dB above its own floor. At that ratio it cannot take the
 * hiss without taking the quiet consonants -- every setting tried on the
 * bench was either "noise" or "muffled". RNNoise (xiph, 2017; vendored in
 * third_party/rnnoise) was trained on speech at exactly that kind of ratio
 * and keeps the consonants a band-by-band suppressor discards.
 *
 * It runs at 48 kHz on 10 ms frames. This wrapper resamples each 20 ms
 * 8 kHz frame up, runs two of its frames, and resamples back, so the rest
 * of the engine is unchanged. One frame of latency (20 ms) is added.
 */
#ifndef SYNCN_DENOISE_H
#define SYNCN_DENOISE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct syncn_denoise syncn_denoise;

/* wet: 0..1, how much of the denoised signal replaces the original. 1 is
 * the full effect; below that the two are mixed, which softens both the
 * suppression and any artefact of it. NULL when not built in. */
syncn_denoise *syncn_denoise_create(int rate, float wet);
void           syncn_denoise_destroy(syncn_denoise *d);

/* In place, exactly `count` samples in and out. */
void syncn_denoise_run(syncn_denoise *d, int16_t *pcm, size_t count);

/* The network's own speech probability for the last frame, 0..1. */
float syncn_denoise_voice(const syncn_denoise *d);

/* Whether this build carries the denoiser at all. */
int syncn_denoise_available(void);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_DENOISE_H */
