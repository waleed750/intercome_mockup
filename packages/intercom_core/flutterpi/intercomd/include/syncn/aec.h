/*
 * Acoustic echo cancellation.
 *
 * This is the piece that makes real two-way audio possible. The panel has two
 * 1 W speakers centimetres from its microphone inside a sealed in-wall
 * enclosure, so the far end's voice comes straight back into the mic and gets
 * transmitted to them as echo.
 *
 * The workaround that was in use before — replacing the uplink with silence
 * whenever the far end is talking — removes the echo by making the call
 * half-duplex. Nobody can interrupt, and every turn-change clips. Cancellation
 * is what lets that gate be switched off.
 *
 * Two backends behind one interface:
 *
 *   SPEEX   speexdsp's canceller. Packaged everywhere, plain C, works today.
 *           Handles moderate coupling well. This is the default.
 *
 *   WEBRTC  WebRTC's AEC3. Materially better on this geometry — loud speaker,
 *           near mic, small enclosure — but needs webrtc-audio-processing >= 1.0
 *           built from source, since Debian ships 0.3.x with the older AECM.
 *           scripts/provision.sh builds it. NOT YET IMPLEMENTED: it will be
 *           added and tuned on the panel, where its delay estimate can be
 *           measured against real acoustics rather than guessed at.
 *
 * Call order matters. syncn_aec_play() must be given the far-end signal before
 * it is handed to the speaker, and syncn_aec_capture() must then be given the
 * microphone frame. Getting this pair the wrong way round, or feeding the wrong
 * signal as the reference, is the most common reason echo cancellation appears
 * to "not work".
 */
#ifndef SYNCN_AEC_H
#define SYNCN_AEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYNCN_AEC_NONE = 0, /* pass-through; the half-duplex gate carries the load */
    SYNCN_AEC_SPEEX,
    SYNCN_AEC_WEBRTC
} syncn_aec_backend;

typedef struct syncn_aec syncn_aec;

const char *syncn_aec_backend_name(syncn_aec_backend b);
syncn_aec_backend syncn_aec_backend_from_name(const char *name);

/*
 * `tail_ms` is how far back the canceller looks for the echo — it must exceed
 * the real speaker-to-mic round trip including ALSA buffering, or cancellation
 * degrades badly. 200 ms is a safe starting point for a sealed panel; the true
 * figure is a bench measurement.
 *
 * `delay_ms` is how far the far-end reference is held back before the canceller
 * sees it, so that it lines up with the echo actually arriving at the
 * microphone: the ALSA playback buffer plus the codec plus the air. Roughly the
 * playback latency, 80 ms with our four 20 ms periods. Too small and the
 * canceller is shown the echo before it happens; too large and it is shown it
 * after. Either way it fails to converge, and the symptom is echo that comes
 * and goes rather than echo that is simply present.
 *
 * Falls back to SYNCN_AEC_NONE rather than failing if the requested backend is
 * unavailable, so a missing library degrades the call instead of killing it.
 * Check syncn_aec_active() to see what you actually got.
 */
/*
 * What the preprocessor is allowed to do to the microphone after the linear
 * canceller has run.
 *
 * These were guesses, and they cost the panel most of its uplink. The linear
 * canceller subtracts an estimate of the echo and is well-behaved; the
 * preprocessor's residual suppressor, denoiser and AGC are each capable of
 * removing speech outright, and on a panel whose microphone delivers -41 dBFS
 * they did. Measured, with no echo present at all: 34 to 37 dB of the talker
 * gone. They are settings, not constants, because the right values depend on
 * the microphone and the room.
 *
 * All the dB figures are negative: they are maximum attenuations.
 */
typedef struct {
    bool denoise;               /* default off  */
    bool agc;                   /* default off  */
    int  noise_suppress_db;     /* only with denoise           */
    int  echo_suppress_db;      /* residual, far end quiet     */
    int  echo_suppress_active_db;  /* residual, far end active */
} syncn_aec_tuning;

/* The defaults, filled in for you. Start here and change one thing at a time. */
void syncn_aec_default_tuning(syncn_aec_tuning *out);

syncn_aec *syncn_aec_create(syncn_aec_backend want, int rate, int frame_samples,
                            int tail_ms, int delay_ms);

/* As above, with the preprocessor spelled out. NULL tuning means the defaults,
 * so this and syncn_aec_create() agree. */
syncn_aec *syncn_aec_create_tuned(syncn_aec_backend want, int rate,
                                  int frame_samples, int tail_ms, int delay_ms,
                                  const syncn_aec_tuning *tuning);
void       syncn_aec_destroy(syncn_aec *aec);

syncn_aec_backend syncn_aec_active(const syncn_aec *aec);

/* The far-end frame, before it reaches the speaker. */
void syncn_aec_play(syncn_aec *aec, const int16_t *far, size_t samples);

/* The microphone frame, cleaned in place. */
void syncn_aec_capture(syncn_aec *aec, int16_t *near_end, size_t samples);

/* Rough estimate of how much echo remains, 0.0 (clean) to 1.0. Used to decide
 * whether the half-duplex gate needs to come back on in `auto` duplex mode. */
float syncn_aec_residual(const syncn_aec *aec);

/*
 * How much the LINEAR canceller took out of the last capture frame, in dB:
 * the frame's level before speex_echo_cancellation() over its level after,
 * before the preprocessor touches it. Positive means the frame correlated
 * with the reference, which only our own signal coming back does; a talker
 * the reference does not contain leaves this near zero, whatever the
 * preprocessor then removes as noise. That is the difference between "this
 * is echo" and "this is quiet", and the reason the preprocessor is left out.
 * 0 with no backend, or on a frame too quiet to judge.
 */
float syncn_aec_last_cancel_db(const syncn_aec *aec);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_AEC_H */
