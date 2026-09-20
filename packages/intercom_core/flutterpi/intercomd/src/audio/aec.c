#include "syncn/aec.h"
#include "syncn/util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef SYNCN_HAVE_SPEEXDSP
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#endif

struct syncn_aec {
    syncn_aec_backend backend;
    int    rate;
    int    frame_samples;

    /* Rolling energy ratio between what we played and what survived
     * cancellation, used as a crude residual-echo estimate. */
    double far_energy;
    double residual_energy;

    /* What the linear filter alone removed from the last frame, in dB. */
    float  cancel_db;

    /*
     * The far-end signal, delayed to line up with the microphone.
     *
     * What the mic hears now is what the speaker played a moment ago: the ALSA
     * playback buffer, the codec, and the air between speaker and mic. The
     * canceller has to be given the reference at that same moment, not the
     * frame we are about to queue.
     *
     * speexdsp's asynchronous API does this internally, through a queue whose
     * depth we cannot see or control. On the bench it drifted twice in seventy
     * seconds and reset itself — "the echo canceller started acting funny and
     * got slapped" — with no way to inspect why. This is the same mechanism
     * made explicit: a ring of past frames, a depth in milliseconds we choose
     * and log, and the synchronous cancellation call that takes both signals
     * together so they cannot come apart.
     */
    int16_t *ref_ring;
    size_t   ref_frames;   /* ring capacity, in frames   */
    size_t   ref_head;
    size_t   ref_filled;
    int      delay_frames; /* how far back to read       */

#ifdef SYNCN_HAVE_SPEEXDSP
    SpeexEchoState       *echo;
    SpeexPreprocessState *pre;
#endif
};

const char *syncn_aec_backend_name(syncn_aec_backend b)
{
    switch (b) {
    case SYNCN_AEC_SPEEX:  return "speex";
    case SYNCN_AEC_WEBRTC: return "webrtc";
    case SYNCN_AEC_NONE:
    default:               return "none";
    }
}

syncn_aec_backend syncn_aec_backend_from_name(const char *name)
{
    if (!name)
        return SYNCN_AEC_SPEEX;
    if (strcasecmp(name, "speex") == 0)
        return SYNCN_AEC_SPEEX;
    if (strcasecmp(name, "webrtc") == 0)
        return SYNCN_AEC_WEBRTC;
    if (strcasecmp(name, "none") == 0 || strcasecmp(name, "off") == 0 ||
        strcasecmp(name, "false") == 0)
        return SYNCN_AEC_NONE;
    /* "true"/"on"/anything else: give them the best backend we actually have. */
    return SYNCN_AEC_SPEEX;
}

void syncn_aec_default_tuning(syncn_aec_tuning *out)
{
    if (!out)
        return;

    /*
     * Denoise and AGC off, residual suppression gentle.
     *
     * The previous values — denoise on at -25 dB, AGC on, residual suppression
     * at -40 dB idle — were set without measurement and removed 34 to 37 dB of
     * the talker on this panel, with no echo present to justify any of it. The
     * door streams audio continuously at 50 frames a second, so "far end
     * quiet" never happens and the -40 dB setting was effectively always armed.
     *
     * Denoise is off because the measured noise problem on this panel was 60 Hz
     * mains hum, and the 150 Hz high-pass already takes 16 dB out of it. A
     * broadband suppressor on a microphone delivering -41 dBFS cannot tell
     * quiet speech from noise, and it did not.
     *
     * AGC is off because it pins the output to a fixed level whatever goes in —
     * which silently overrides mic_gain_db and cuts a loud talker. Measured: a
     * -13 dBFS input came out at -37 dBFS, the same as a -41 dBFS one.
     *
     * The linear canceller itself stays on. It subtracts an estimate of the
     * echo and is not what was destroying the speech.
     */
    out->denoise                 = false;
    out->agc                     = false;
    out->noise_suppress_db       = -10;
    out->echo_suppress_db        = -15;
    out->echo_suppress_active_db = -10;
}

syncn_aec *syncn_aec_create(syncn_aec_backend want, int rate, int frame_samples,
                            int tail_ms, int delay_ms)
{
    return syncn_aec_create_tuned(want, rate, frame_samples, tail_ms, delay_ms, NULL);
}

syncn_aec *syncn_aec_create_tuned(syncn_aec_backend want, int rate,
                                  int frame_samples, int tail_ms, int delay_ms,
                                  const syncn_aec_tuning *tuning)
{
    syncn_aec_tuning tune;
    syncn_aec_default_tuning(&tune);
    if (tuning)
        tune = *tuning;

    syncn_aec *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;

    a->rate          = rate;
    a->frame_samples = frame_samples;
    a->backend       = SYNCN_AEC_NONE;

    /* One frame is 20 ms. The ring holds a generous margin beyond the delay so
     * the figure can be retuned at runtime without reallocating. */
    const int frame_ms = (frame_samples * 1000) / rate;
    a->delay_frames    = delay_ms > 0 ? delay_ms / (frame_ms ? frame_ms : 20) : 0;
    a->ref_frames      = (size_t)(a->delay_frames + 8);

    a->ref_ring = calloc(a->ref_frames * (size_t)frame_samples, sizeof *a->ref_ring);
    if (!a->ref_ring) {
        free(a);
        return NULL;
    }

    if (want == SYNCN_AEC_WEBRTC) {
        /* Deliberately not silently downgraded without saying so — the whole
         * point of choosing webrtc is that speex was not good enough. */
        LOG_WARN("aec: webrtc backend is not implemented yet; falling back to speex");
        want = SYNCN_AEC_SPEEX;
    }

#ifdef SYNCN_HAVE_SPEEXDSP
    if (want == SYNCN_AEC_SPEEX) {
        /* The filter has to be longer than the real echo tail or cancellation
         * falls apart. Longer costs CPU, which on four A55 cores at 8 kHz is
         * not a concern. */
        const int filter_len = (rate * tail_ms) / 1000;

        a->echo = speex_echo_state_init(frame_samples, filter_len);
        if (a->echo) {
            int r = rate;
            speex_echo_ctl(a->echo, SPEEX_ECHO_SET_SAMPLING_RATE, &r);

            a->pre = speex_preprocess_state_init(frame_samples, rate);
            if (a->pre) {
                /* The preprocessor does the residual suppression that the
                 * canceller alone leaves behind, plus noise reduction — useful
                 * in a hallway, which is where these panels live. */
                speex_preprocess_ctl(a->pre, SPEEX_PREPROCESS_SET_ECHO_STATE, a->echo);

                int denoise = tune.denoise ? 1 : 0;
                int agc     = tune.agc ? 1 : 0;
                int off     = 0;
                speex_preprocess_ctl(a->pre, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
                speex_preprocess_ctl(a->pre, SPEEX_PREPROCESS_SET_AGC, &agc);
                speex_preprocess_ctl(a->pre, SPEEX_PREPROCESS_SET_VAD, &off);

                int noise_suppress       = tune.noise_suppress_db;
                int echo_suppress        = tune.echo_suppress_db;
                int echo_suppress_active = tune.echo_suppress_active_db;
                speex_preprocess_ctl(a->pre, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS,
                                     &noise_suppress);
                speex_preprocess_ctl(a->pre, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS,
                                     &echo_suppress);
                speex_preprocess_ctl(a->pre, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE,
                                     &echo_suppress_active);

                LOG_INFO("aec: preprocessor denoise=%s agc=%s "
                         "echo_suppress=%d/%d dB noise_suppress=%d dB",
                         tune.denoise ? "on" : "off", tune.agc ? "on" : "off",
                         echo_suppress, echo_suppress_active, noise_suppress);
            }

            a->backend = SYNCN_AEC_SPEEX;
            LOG_INFO("aec: speex, %d Hz, %d-sample frames, %d ms tail (%d taps), "
                     "%d ms reference delay (%d frames)",
                     rate, frame_samples, tail_ms, filter_len,
                     delay_ms, a->delay_frames);
        } else {
            LOG_WARN("aec: speex_echo_state_init failed; running without cancellation");
        }
    }
#else
    (void)tail_ms;
    if (want == SYNCN_AEC_SPEEX)
        LOG_WARN("aec: built without speexdsp; running without cancellation");
#endif

    if (a->backend == SYNCN_AEC_NONE)
        LOG_WARN("aec: disabled — the half-duplex gate is the only thing holding "
                 "echo back, so interruption will not work");

    return a;
}

void syncn_aec_destroy(syncn_aec *a)
{
    if (!a)
        return;
#ifdef SYNCN_HAVE_SPEEXDSP
    if (a->pre)
        speex_preprocess_state_destroy(a->pre);
    if (a->echo)
        speex_echo_state_destroy(a->echo);
#endif
    free(a->ref_ring);
    free(a);
}

syncn_aec_backend syncn_aec_active(const syncn_aec *a)
{
    return a ? a->backend : SYNCN_AEC_NONE;
}

static double frame_energy(const int16_t *s, size_t n)
{
    double e = 0.0;
    for (size_t i = 0; i < n; i++)
        e += (double)s[i] * (double)s[i];
    return n ? e / (double)n : 0.0;
}

void syncn_aec_play(syncn_aec *a, const int16_t *far, size_t samples)
{
    if (!a || !far)
        return;

    /* Track far-end energy regardless of backend: the residual estimate and
     * the half-duplex gate both need it. */
    const double e = frame_energy(far, samples);
    a->far_energy = a->far_energy * 0.9 + e * 0.1;

    /* Keep the frame; it is handed to the canceller once the microphone has
     * had time to hear it. */
    if (a->ref_ring && (size_t)a->frame_samples <= a->ref_frames * (size_t)a->frame_samples) {
        memcpy(a->ref_ring + a->ref_head * (size_t)a->frame_samples, far,
               (size_t)a->frame_samples * sizeof *far);
        a->ref_head = (a->ref_head + 1) % a->ref_frames;
        if (a->ref_filled < a->ref_frames)
            a->ref_filled++;
    }
}

/* The reference frame from `delay_frames` ago, or NULL until enough history
 * exists. Before that the canceller is fed nothing and simply passes audio
 * through — briefly, at the very start of a call. */
static const int16_t *delayed_reference(const syncn_aec *a)
{
    if (!a->ref_ring || a->ref_filled <= (size_t)a->delay_frames)
        return NULL;

    const size_t back = (size_t)a->delay_frames + 1;
    const size_t idx  = (a->ref_head + a->ref_frames - back) % a->ref_frames;
    return a->ref_ring + idx * (size_t)a->frame_samples;
}

void syncn_aec_capture(syncn_aec *a, int16_t *near_end, size_t samples)
{
    if (!a || !near_end)
        return;

#ifdef SYNCN_HAVE_SPEEXDSP
    if (a->backend == SYNCN_AEC_SPEEX && a->echo) {
        const int16_t *reference = delayed_reference(a);

        /* The synchronous call takes both signals together, so there is no
         * internal queue to fall out of step. Cancellation writes to a separate
         * buffer, so one scratch copy is unavoidable — 160 samples is 320
         * bytes. */
        int16_t clean[1024];
        if (reference && samples <= sizeof clean / sizeof clean[0]) {
            const double before = frame_energy(near_end, samples);
            speex_echo_cancellation(a->echo, near_end, reference, clean);
            const double after  = frame_energy(clean, samples);
            /* Below about -60 dBFS there is nothing to judge: the ratio of
             * two floors is noise, and a positive one there would mute a
             * speaker that is playing nothing anyway. */
            const double floor  = 32768.0 * 32768.0 * 1e-6;
            a->cancel_db = before > floor
                         ? (float)(10.0 * log10(before / (after + 1e-9)))
                         : 0.0f;
            if (a->pre)
                speex_preprocess_run(a->pre, clean);
            memcpy(near_end, clean, samples * sizeof *clean);
        }
    }
#endif

    const double e = frame_energy(near_end, samples);
    a->residual_energy = a->residual_energy * 0.9 + e * 0.1;
}

float syncn_aec_last_cancel_db(const syncn_aec *a)
{
    return a ? a->cancel_db : 0.0f;
}

float syncn_aec_residual(const syncn_aec *a)
{
    if (!a || a->far_energy < 1.0)
        return 0.0f;

    /*
     * Read this carefully before drawing a conclusion from it: it is the
     * canceller's OUTPUT energy over the far end's energy. It is not echo
     * return loss and it is not a measure of how much echo remains.
     *
     * A quiet output makes this small, whatever the reason. Real cancellation
     * does that, and so does a dead microphone, an over-aggressive suppressor,
     * or a muted talker. So a falling figure alongside a falling uplink level
     * is one observation reported twice, not two pieces of evidence — and
     * treating it as two is how a tautology gets mistaken for a diagnosis.
     *
     * It exists for one job: auto duplex mode watches it to decide whether to
     * fall back to half duplex. For that, "is anything getting through
     * relative to what we played" is the right question, and its blind spot
     * (a silent uplink scores perfectly) is the reason auto mode is not the
     * default.
     */
    const double ratio = a->residual_energy / a->far_energy;
    return ratio > 1.0 ? 1.0f : (float)ratio;
}
