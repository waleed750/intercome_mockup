#include "syncn/audio.h"
#include "syncn/alaw.h"
#include "syncn/dsp.h"
#include "syncn/jitter.h"
#include "syncn/util.h"

#include <alsa/asoundlib.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FRAME_SAMPLES  ((int)SYNCN_AUDIO_FRAME_SAMPLES)   /* 160, i.e. 20 ms  */
/* The talker holds the floor after this much continuous voice, and keeps it
 * this long after the last voiced frame (the return keeps arriving for up to
 * the canceller's tail). */
#define HOLD_AFTER_TICKS ((int)(300 / SYNCN_AUDIO_FRAME_MS))
#define HOLD_FOR_TICKS   ((int)(600 / SYNCN_AUDIO_FRAME_MS))
#define RATE           ((int)SYNCN_AUDIO_RATE)            /* 8000             */
#define PRIME_PERIODS  3

/* Fallback for the half-duplex gate when the config gives nothing: -34 dBFS,
 * which ordinary speech trips and room noise does not. The live value comes
 * from cfg.gate_threshold_dbfs, because on this panel the door arrives
 * quietly enough that a fixed figure was never going to be right for every
 * site. */
#define GATE_DEFAULT_DBFS  -30.0f

static double dbfs_to_energy(float dbfs)
{
    const double amp = pow(10.0, dbfs / 20.0) * 32768.0;
    return amp * amp;
}

/* In auto mode, how bad the residual has to get, for how long, before we give
 * up on full duplex. Deliberately slow: flapping between modes mid-sentence is
 * worse than either mode. */
#define AUTO_RESIDUAL_LIMIT    0.25f
#define AUTO_BAD_TICKS         150   /* 3 s */

/* ----------------------------------------------------------------- engine --- */

struct syncn_audio {
    syncn_audio_config cfg;
    syncn_audio_uplink_cb cb;
    void *cb_user;

    /* Capture is opened in stereo wherever the device allows it, so that the
     * live channel can be chosen rather than averaged with a dead one. */
    unsigned        capture_channels;
    unsigned        playback_channels;
    syncn_chanpick *chanpick;
    syncn_biquad    highpass;
    syncn_biquad    lowpass[2];      /* microphone, telephony band top     */
    syncn_biquad    dl_highpass;     /* the door's stream, both edges      */
    syncn_biquad    dl_lowpass[2];
    bool            lowpass_on, dl_highpass_on;
    syncn_biquad    presence, dl_presence;
    bool            presence_on;
    syncn_limiter   limiter;
    bool            limiter_on;
    syncn_gate      gate;
    syncn_gate      uplink_ng;      /* noise gate on the microphone      */
    syncn_gate      downlink_ng;    /* noise gate on the door's audio    */
    bool            uplink_ng_on, downlink_ng_on;
    uint64_t        uplink_ng_frames, downlink_ng_frames;
    bool            highpass_on;
    float           mic_dbfs;
    float           uplink_dbfs;
    float           near_dbfs;      /* after canceller and noise gate: the gate's near side */
    bool            near_priority_on;
    uint64_t        near_won;
    /* The other half of half duplex: while the panel has the floor, the
     * speaker is muted, so the door's return of the panel's own voice is
     * neither heard nor picked up again by the microphone. */
    syncn_gate      speaker_gate;
    bool            near_has_floor;  /* last tick's decision, applied to this tick's playback */
    uint64_t        speaker_muted;

    /* Cleans the door's stream of the door's echo of our uplink. */
    syncn_aec      *ret_aec;
    float           far_raw_dbfs;
    /* And mutes what it recognised but could not fully remove. */
    syncn_gate      echo_gate;
    bool            echo_gate_on;
    uint64_t        echo_muted;
    int             ref_active_left;   /* ticks since we last sent voice, counted down from the tail */
    /* The talker holds the floor: consecutive voiced uplink frames, and
     * how long the hold outlasts the last one. */
    int             voiced_run;
    int             hold_left;
    double          hold_atten;        /* what the downlink gate's energy is scaled by while held */
    bool            hold_on;
    uint64_t        held;
    /* What the canceller is measuring on the frames it can judge: the
     * whole removal and the linear filter's share, summed for the log. */
    double          ret_total_sum, ret_linear_sum;
    uint64_t        ret_judged;

    /* The neural denoiser, one per direction, when configured. */
    syncn_denoise  *dn_up, *dn_down;

    /* Samples arriving at or beyond the converter's rails.
     *
     * Worth counting because of what couples into this panel's microphone: the
     * speaker's path into the mic shows +15.3 dB of gain at 700 Hz, so the door
     * arriving at -20 dBFS RMS puts its peaks past full scale. A clipped sample
     * is unrecoverable -- no canceller reconstructs what the ADC never
     * captured -- so this distinguishes "the processing removed the talker"
     * from "the talker was never in the recording". */
    uint64_t        clipped_samples, clipped_frames;

    /* Samples the software gain pushed past the rails. Distinct from the ADC
     * count above: that one says the converter was overdriven, this one says
     * mic_gain_db is. The CLI pointed out that clip= could never catch a gain
     * set too high, and it was right. */
    uint64_t        saturated_samples;

    /* What the door is sending, after speaker_gain_db, so the half-duplex
     * gate's decision can be read off the log rather than guessed. */
    float           far_dbfs;
    double          gate_energy;

    snd_pcm_t *capture;
    snd_pcm_t *playback;
    syncn_aec *aec;

    syncn_jitter *jitter;

    pthread_t thread;
    atomic_bool running;
    atomic_bool muted;
    /* Set when the thread stops on its own. A call whose audio engine has died
     * looks identical to a working one from the outside, so it has to be
     * reported rather than discovered by talking to nobody. */
    atomic_bool failed;

    float mic_gain;
    float speaker_gain;

    syncn_duplex_mode duplex_active;
    int  auto_bad_ticks;

    /* Stats are written only by the audio thread and read elsewhere; they are
     * counters for diagnosis, so a torn read is harmless. */
    uint64_t ticks, captured, played, xruns_cap, xruns_play, gated, muted_ticks;
    uint64_t late_resyncs;   /* times the tick fell far enough behind to reset */
    /* Processing time per tick, from the capture read returning to the frame
     * going out, against the 20 ms budget. The neural denoiser made this
     * worth watching: two of them plus resamplers missed the deadline. */
    double   busy_ns_total;
    long     busy_ns_max;
    double   far_energy;
};

const char *syncn_denoise_name(syncn_denoise_mode m)
{
    switch (m) {
    case SYNCN_DENOISE_SPEEX:   return "speex";
    case SYNCN_DENOISE_RNNOISE: return "rnnoise";
    default:                    return "off";
    }
}

syncn_denoise_mode syncn_denoise_from_name(const char *name)
{
    if (!name)
        return SYNCN_DENOISE_RNNOISE;
    if (strcasecmp(name, "rnnoise") == 0 || strcasecmp(name, "rnn") == 0)
        return SYNCN_DENOISE_RNNOISE;
    if (strcasecmp(name, "speex") == 0)
        return SYNCN_DENOISE_SPEEX;
    if (strcasecmp(name, "off") == 0 || strcasecmp(name, "none") == 0 ||
        strcasecmp(name, "false") == 0)
        return SYNCN_DENOISE_OFF;
    return SYNCN_DENOISE_RNNOISE;
}

const char *syncn_duplex_name(syncn_duplex_mode m)
{
    switch (m) {
    case SYNCN_DUPLEX_HALF: return "half";
    case SYNCN_DUPLEX_AUTO: return "auto";
    case SYNCN_DUPLEX_FULL:
    default:                return "full";
    }
}

syncn_duplex_mode syncn_duplex_from_name(const char *name)
{
    if (name && strcasecmp(name, "half") == 0)
        return SYNCN_DUPLEX_HALF;
    if (name && strcasecmp(name, "auto") == 0)
        return SYNCN_DUPLEX_AUTO;
    return SYNCN_DUPLEX_FULL;
}

void syncn_audio_config_defaults(syncn_audio_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->capture_device   = "default";
    cfg->playback_device  = "default";
    cfg->aec              = SYNCN_AEC_SPEEX;
    cfg->aec_tail_ms      = 200;
    cfg->aec_delay_ms     = 80;   /* four 20 ms periods of ALSA buffering */
    cfg->capture_channels = 1;
    cfg->capture_channel  = "auto";
    cfg->highpass_hz      = 250;  /* below the telephony band G.711 carries */
    cfg->lowpass_hz       = 3400; /* and above it */
    cfg->downlink_highpass_hz = 250;
    cfg->presence_hz      = 2000;
    cfg->presence_db      = 0.0f;   /* +3 was heard as noise; off */
    cfg->duplex           = SYNCN_DUPLEX_FULL;
    cfg->jitter_target_ms = 60;
    cfg->near_priority_db = 0.0f;   /* off: see audio.h */
    cfg->denoise          = SYNCN_DENOISE_SPEEX;
    cfg->denoise_downlink = SYNCN_DENOISE_SPEEX;
    cfg->denoise_wet      = 1.0f;
    cfg->aec_denoise      = true;
    cfg->aec_noise_suppress_db = -18;
    cfg->return_cancel    = true;
    cfg->return_tail_ms   = 600;
    cfg->return_noise_suppress_db = -15;
    cfg->return_echo_db   = 3.0f;
    cfg->hold_floor_dbfs  = -1.0f;
    cfg->hold_loud_dbfs   = -18.0f;
}

/*
 * The return canceller outlives the call. What it learns is the door's
 * speaker-to-microphone path plus the round trip, and neither changes
 * between calls; thrown away with the call, it spent the first second of
 * every call letting the return through while it learned the same path
 * again. Kept here between syncn_audio_stop() and the next start, and
 * reused when the next call asks for the same canceller. One call at a
 * time, on the call thread: no lock.
 */
static syncn_aec       *s_kept_ret;
static int              s_kept_ret_tail;
static syncn_aec_tuning s_kept_ret_tune;

static void return_tuning(const syncn_audio_config *cfg, syncn_aec_tuning *tune)
{
    memset(tune, 0, sizeof *tune);   /* memcmp'd: no padding left to chance */
    syncn_aec_default_tuning(tune);
    tune->denoise                 = cfg->denoise_downlink == SYNCN_DENOISE_SPEEX;
    tune->noise_suppress_db       = cfg->return_noise_suppress_db < 0
                                  ? cfg->return_noise_suppress_db : -15;
    tune->echo_suppress_db        = -25;
    tune->echo_suppress_active_db = -15;
}

static syncn_aec *take_kept_return(int tail, const syncn_aec_tuning *tune)
{
    syncn_aec *r = s_kept_ret;
    if (!r)
        return NULL;
    s_kept_ret = NULL;
    if (tail == s_kept_ret_tail && memcmp(tune, &s_kept_ret_tune, sizeof *tune) == 0)
        return r;
    syncn_aec_destroy(r);
    return NULL;
}

static void keep_return(syncn_aec *r, int tail, const syncn_aec_tuning *tune)
{
    syncn_aec_destroy(s_kept_ret);
    s_kept_ret      = r;
    s_kept_ret_tail = tail;
    s_kept_ret_tune = *tune;
}

static float db_to_linear(int db) { return powf(10.0f, (float)db / 20.0f); }

static unsigned apply_gain(int16_t *pcm, size_t n, float gain)
{
    if (gain > 0.999f && gain < 1.001f)
        return 0;
    unsigned saturated = 0;
    for (size_t i = 0; i < n; i++) {
        float v = (float)pcm[i] * gain;
        if (v > 32767.0f)  { v = 32767.0f;  saturated++; }
        if (v < -32768.0f) { v = -32768.0f; saturated++; }
        pcm[i] = (int16_t)v;
    }
    return saturated;
}

static double energy_of(const int16_t *s, size_t n)
{
    double e = 0.0;
    for (size_t i = 0; i < n; i++)
        e += (double)s[i] * (double)s[i];
    return n ? e / (double)n : 0.0;
}

/* Open one named device and configure it. Returns NULL without logging an
 * error, so the caller can try the next candidate quietly. */
static snd_pcm_t *try_pcm(const char *device, snd_pcm_stream_t dir, unsigned channels)
{
    snd_pcm_t *pcm = NULL;
    if (snd_pcm_open(&pcm, device, dir, 0) < 0)
        return NULL;

    /*
     * soft_resample = 1 lets ALSA convert if the codec cannot do 8 kHz
     * natively — most cannot, they run at 48 kHz. Doing it here rather than
     * hand-rolling a resampler keeps one less thing to get wrong, and the
     * quality ceiling is set by G.711 anyway.
     *
     * The latency figure sets ALSA's buffer. Four periods of 20 ms is enough to
     * absorb scheduling jitter without adding delay the echo canceller then has
     * to model.
     */
    const int err = snd_pcm_set_params(pcm,
                             SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             channels,
                             RATE,
                             1,                       /* allow resampling    */
                             4 * SYNCN_AUDIO_FRAME_MS * 1000); /* 80 ms      */
    if (err < 0) {
        snd_pcm_close(pcm);
        return NULL;
    }
    return pcm;
}

/*
 * Open a device, falling back to the real hardware if the named one fails.
 *
 * "default" is what the config ships with and it is rarely right on a Rockchip
 * panel — there is often no default PCM defined at all, and a daemon that dies
 * because of it looks like a broken intercom rather than a missing asound.conf.
 * So when the configured device will not open, walk the actual cards and try
 * plughw:N,0. plughw rather than hw because the codec almost certainly cannot
 * do 8 kHz mono natively and plug converts for us.
 */
static snd_pcm_t *open_pcm(const char *device, snd_pcm_stream_t dir, const char *what,
                           unsigned channels)
{
    snd_pcm_t *pcm = try_pcm(device, dir, channels);
    if (pcm) {
        LOG_INFO("audio: %s device '%s' open at %d Hz, %u channel%s",
                 what, device, RATE, channels, channels == 1 ? "" : "s");
        return pcm;
    }

    LOG_WARN("audio: %s device '%s' did not open — looking for a working card",
             what, device);

    for (int card = -1; snd_card_next(&card) == 0 && card >= 0; ) {
        char name[32];
        snprintf(name, sizeof name, "plughw:%d,0", card);

        pcm = try_pcm(name, dir, channels);
        if (pcm) {
            char *card_name = NULL;
            snd_card_get_name(card, &card_name);
            LOG_INFO("audio: %s falling back to '%s' (%s) at %d Hz, %u channel%s",
                     what, name, card_name ? card_name : "unknown", RATE,
                     channels, channels == 1 ? "" : "s");
            LOG_INFO("audio: set [audio] %s_device = %s in the config to make "
                     "this the permanent choice",
                     dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback", name);
            free(card_name);
            return pcm;
        }
    }

    LOG_ERR("audio: no usable %s device. Check 'aplay -l' and 'arecord -l' on "
            "the panel, then set [audio] capture_device and playback_device.",
            what);
    return NULL;
}

/* Recover from an underrun or overrun. Returns false if the device is gone. */
static bool recover(snd_pcm_t *pcm, int err, const char *what, uint64_t *counter)
{
    if (err == -EPIPE) {
        (*counter)++;
        /* Do not log every xrun — on a loaded panel that would flood the
         * journal and make the real problem harder to find. The counter is in
         * the stats line. */
        snd_pcm_prepare(pcm);
        return true;
    }
    if (err == -ESTRPIPE) {
        while ((err = snd_pcm_resume(pcm)) == -EAGAIN)
            usleep(10000);
        if (err < 0)
            snd_pcm_prepare(pcm);
        return true;
    }
    LOG_ERR("audio: %s failed unrecoverably: %s", what, snd_strerror(err));
    return false;
}

/* Add nanoseconds to a monotonic timespec, normalising the carry. */
static void timespec_add_ns(struct timespec *t, long ns)
{
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L) {
        t->tv_nsec -= 1000000000L;
        t->tv_sec++;
    }
}

static double timespec_diff_ms(const struct timespec *a, const struct timespec *b)
{
    return (double)(a->tv_sec - b->tv_sec) * 1000.0
         + (double)(a->tv_nsec - b->tv_nsec) / 1000000.0;
}

static void *audio_thread(void *arg)
{
    syncn_audio *a = arg;

    /*
     * The loop is normally paced by the capture read blocking for one period.
     * We do not rely on that. A device that returns instantly — ALSA's null
     * device, or a driver that is misbehaving in exactly the way we would be
     * debugging — would otherwise spin the loop at thousands of hertz and flood
     * the outbox with uplink frames.
     *
     * So the tick is anchored to an absolute monotonic deadline. On healthy
     * hardware the blocking read consumes the period and the sleep below is a
     * no-op; on a device that does not block, the sleep restores the cadence.
     * Either way the door sees frames at 20 ms, which is what it gates on.
     */
    const long PERIOD_NS = (long)SYNCN_AUDIO_FRAME_MS * 1000000L;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);

    int16_t near_pcm[FRAME_SAMPLES];
    int16_t capture_buf[FRAME_SAMPLES * 2];  /* stereo, when the device gives it */
    int16_t far_pcm[FRAME_SAMPLES];
    int16_t play_buf[FRAME_SAMPLES * 2];  /* interleaved, when opened stereo */
    uint8_t downlink[SYNCN_AUDIO_FRAME_BYTES];
    uint8_t uplink[SYNCN_AUDIO_FRAME_BYTES];
    const int16_t silence_pcm[FRAME_SAMPLES * 2] = {0};

    /* Prime the playback ring so the first capture read does not immediately
     * underrun the speaker. */
    for (int i = 0; i < PRIME_PERIODS; i++) {
        snd_pcm_writei(a->playback, silence_pcm, FRAME_SAMPLES);
        /* The canceller must know about these too. Priming the speaker without
         * priming the reference starts the call with the two streams already
         * offset by the priming depth. */
        syncn_aec_play(a->aec, silence_pcm, FRAME_SAMPLES);
    }

    while (atomic_load_explicit(&a->running, memory_order_relaxed)) {

        /* ---- capture paces the loop: this blocks for one 20 ms period ---- */
        int16_t *const raw = (a->capture_channels == 2) ? capture_buf : near_pcm;
        snd_pcm_sframes_t n = snd_pcm_readi(a->capture, raw, FRAME_SAMPLES);
        if (n < 0) {
            if (!recover(a->capture, (int)n, "capture", &a->xruns_cap))
                break;
            memset(near_pcm, 0, sizeof near_pcm);
        } else {
            if (n < FRAME_SAMPLES) {
                const size_t missing = (size_t)(FRAME_SAMPLES - n) * a->capture_channels;
                memset(raw + (size_t)n * a->capture_channels, 0, missing * sizeof *raw);
            }
            if (a->capture_channels == 2)
                syncn_chanpick_run(a->chanpick, capture_buf, FRAME_SAMPLES, near_pcm);
            a->captured++;
        }

        struct timespec t_busy;
        clock_gettime(CLOCK_MONOTONIC, &t_busy);

        /* ---- what the far end is saying, from the jitter buffer ---------- */
        if (syncn_jitter_pop(a->jitter, downlink)) {
            syncn_alaw_decode(downlink, SYNCN_AUDIO_FRAME_BYTES, far_pcm);
        } else {
            /* Nothing to play. Silence rather than repeating the last frame:
             * on a narrowband voice link, repeated frames sound like a stutter,
             * which is worse than a brief gap. */
            memset(far_pcm, 0, sizeof far_pcm);
        }
        (void)apply_gain(far_pcm, FRAME_SAMPLES, a->speaker_gain);

        /* The telephony band. The door's stream carries its microphone's
         * hiss up to 4 kHz and rumble below the voice; neither is the
         * visitor. Before the canceller, so both its inputs share a band. */
        if (a->dl_highpass_on)
            syncn_biquad_run(&a->dl_highpass, far_pcm, FRAME_SAMPLES);
        if (a->lowpass_on) {
            syncn_biquad_run(&a->dl_lowpass[0], far_pcm, FRAME_SAMPLES);
            syncn_biquad_run(&a->dl_lowpass[1], far_pcm, FRAME_SAMPLES);
        }
        if (a->presence_on)
            syncn_biquad_run(&a->dl_presence, far_pcm, FRAME_SAMPLES);

        /*
         * Take our own voice back out of what the door sends. Its microphone
         * hears the panel talker and returns them 100-300 ms later at a
         * visitor's level; measured raw here, cleaned by the canceller whose
         * reference is what we sent on earlier ticks, and everything below
         * -- the gates, the speaker, the level in the log -- sees the clean
         * stream. The panel talker is no longer muted by their own return,
         * and no longer hears it.
         */
        const float far_raw_now = syncn_rms_dbfs(far_pcm, FRAME_SAMPLES);
        a->far_raw_dbfs = a->far_raw_dbfs * 0.9f + far_raw_now * 0.1f;
        if (a->ret_aec)
            syncn_aec_capture(a->ret_aec, far_pcm, FRAME_SAMPLES);

        /*
         * What the canceller recognised but did not fully remove. It takes
         * 13-16 dB off the return on the bench and the peaks of what is
         * left still cleared the downlink gate: played, 200 ms late, to the
         * person talking -- "a small echo", then "noise while he talks,
         * gone when he stops" -- and muting their microphone for as long
         * as it lasted. A frame the linear filter took return_echo_db out
         * of correlated with what we sent: nothing a visitor says does
         * that, and with nothing sent the filter's output is exactly zero,
         * so the measure reads 0 on a visitor whatever the denoiser then
         * takes (the whole removal cannot promise that: the denoiser takes
         * up to 15 dB off a quiet frame, and on the synthetic visitor 16 of
         * 150 loud frames cleared 8 dB that way). Two guards on top: the
         * frame has to be one we could be hearing back -- we sent voice
         * within the canceller's tail -- and loud enough that the downlink
         * gate would have played it. Muted here, before the energy the
         * gates read is taken, so the downlink gate stays shut on it and
         * the half-duplex gate leaves the talker's microphone open. The
         * hangover covers the frames the filter half-catches at the end of
         * a word.
         *
         * The bar is low because the door's path is not the synthetic one:
         * at 8 dB the gate never fired on the real door while the whole
         * removal read 13-16 dB, the door's own processing being nothing a
         * linear filter models well. ret= in the stats line reports the
         * whole removal and the linear share, mean over the frames judged,
         * which is what the bar is set from.
         */
        if (a->echo_gate_on) {
            const float far_now = syncn_rms_dbfs(far_pcm, FRAME_SAMPLES);
            const float floor   = a->downlink_ng_on ? a->cfg.downlink_noise_floor_dbfs : -60.0f;
            const bool  judged  = a->ref_active_left > 0 && far_raw_now > floor;
            bool echo = false;
            if (judged) {
                const float linear = syncn_aec_last_cancel_db(a->ret_aec);
                a->ret_total_sum  += far_raw_now - far_now;
                a->ret_linear_sum += linear;
                a->ret_judged++;
                echo = linear >= a->cfg.return_echo_db;
            }
            if (syncn_gate_run_active(&a->echo_gate, echo, far_pcm, FRAME_SAMPLES))
                a->echo_muted++;
        }
        if (a->ref_active_left > 0)
            a->ref_active_left--;
        if (a->dn_down)
            syncn_denoise_run(a->dn_down, far_pcm, FRAME_SAMPLES);

        a->far_energy = a->far_energy * 0.8 + energy_of(far_pcm, FRAME_SAMPLES) * 0.2;
        a->far_dbfs   = a->far_dbfs * 0.9f + syncn_rms_dbfs(far_pcm, FRAME_SAMPLES) * 0.1f;

        /*
         * The door's idle floor arrives at about -34 dBFS and was being played
         * at the panel in every pause. Mute it below a floor. This has to sit
         * AFTER far_energy is taken, because the half-duplex gate reads that
         * to know the visitor is talking, and a muted frame would blind it.
         */
        /*
         * The talker holds the floor. The door's return of the panel talker
         * was measured at the door's own floor -- raw -40 to -42 dBFS against
         * an idle floor of -42 -- and a linear canceller cannot take echo out
         * of noise: ret= read 0 dB on every judged frame of three calls, so
         * nothing above recognises the return, and its peaks still cleared
         * this gate at -37, each for 100 ms of hold, 150 of release and 150
         * of half-duplex hangover: 211 ticks of the talker's own microphone
         * muted in one call, "some of his words are being cut off". Once
         * the panel has been sending voice for 300 ms, the door's stream
         * has to reach hold_floor_dbfs, a visitor's level, to take the
         * floor, for as long as the voice keeps going and 600 ms after.
         * 300 ms and not the first frame: on the bench the panel's
         * microphone hears the visitor directly for the 100-200 ms before
         * their copy arrives over the network and the half-duplex gate
         * mutes it, and that must not count as the panel talking, or the
         * visitor's own words would need to clear the raised floor.
         * Applied by scaling the energy the gate reads, so the gate's own
         * hold and release are untouched. held= in the stats line counts
         * the ticks the floor was raised.
         */
        double gate_energy = a->far_energy;
        if (a->hold_on && a->hold_left > 0) {
            gate_energy *= a->hold_atten;
            a->held++;
        }
        if (a->downlink_ng_on &&
            syncn_noise_gate_run(&a->downlink_ng, gate_energy, far_pcm, FRAME_SAMPLES))
            a->downlink_ng_frames++;
        if (a->hold_left > 0)
            a->hold_left--;

        /*
         * While the panel has the floor, the speaker is muted. What the door
         * sends during the panel's sentence is mostly the panel talker coming
         * back (its microphone hears them, directly on the bench and through
         * its own speaker installed); played here, 150 ms late, the panel's
         * microphone picked it up again and the door heard the voice plus a
         * delayed copy of itself -- "noise", "not clear". Muting the mic
         * while the visitor talks was only half of half duplex. Decided on
         * the previous tick, applied to this one; the hangover covers the
         * return still arriving after the talker stops.
         */
        if (a->near_priority_on && a->duplex_active == SYNCN_DUPLEX_HALF &&
            syncn_gate_run_active(&a->speaker_gate, a->near_has_floor, far_pcm, FRAME_SAMPLES))
            a->speaker_muted++;

        /* ---- cancellation: reference first, then the microphone ---------- */
        syncn_aec_play(a->aec, far_pcm, FRAME_SAMPLES);

        /* The speaker is mono, but the device may be open in stereo to match
         * capture. Duplicating rather than leaving a channel silent keeps the
         * level right whichever output the panel is actually wired to. */
        const int16_t *play_from = far_pcm;
        if (a->playback_channels == 2) {
            for (int i = 0; i < FRAME_SAMPLES; i++) {
                play_buf[i * 2]     = far_pcm[i];
                play_buf[i * 2 + 1] = far_pcm[i];
            }
            play_from = play_buf;
        }

        snd_pcm_sframes_t w = snd_pcm_writei(a->playback, play_from, FRAME_SAMPLES);
        if (w < 0) {
            if (!recover(a->playback, (int)w, "playback", &a->xruns_play))
                break;
        } else {
            a->played++;
        }

        /*
         * Count clipping BEFORE anything touches the frame. After the
         * high-pass or the gain, a sample that was against the rail may no
         * longer look like one, and the question here is what the converter
         * delivered, not what we made of it.
         */
        unsigned clipped_here = 0;
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            if (near_pcm[i] >= 32700 || near_pcm[i] <= -32700)
                clipped_here++;
        }
        if (clipped_here) {
            a->clipped_samples += clipped_here;
            a->clipped_frames++;
        }

        /* Hum out before gain, so the gain lifts speech rather than mains;
         * hiss out likewise, so the gain lifts speech rather than the top
         * octave, where there is none. */
        if (a->highpass_on)
            syncn_biquad_run(&a->highpass, near_pcm, FRAME_SAMPLES);
        if (a->lowpass_on) {
            syncn_biquad_run(&a->lowpass[0], near_pcm, FRAME_SAMPLES);
            syncn_biquad_run(&a->lowpass[1], near_pcm, FRAME_SAMPLES);
        }
        if (a->presence_on)
            syncn_biquad_run(&a->presence, near_pcm, FRAME_SAMPLES);

        /*
         * Gain and limiter together, in float. The gain is set high on purpose
         * -- loud enough that speech peaks would hit the rail -- and the
         * limiter catches those peaks and only those, so the average, which is
         * what the door hears as loudness, keeps the full gain. They have to be
         * one pass: a gain that clamps to 16-bit first hands the limiter audio
         * that is already clipped, and that is exactly what happened.
         */
        if (a->limiter_on)
            syncn_limiter_run_gain(&a->limiter, near_pcm, FRAME_SAMPLES, a->mic_gain);
        else
            a->saturated_samples += apply_gain(near_pcm, FRAME_SAMPLES, a->mic_gain);

        /* With the gain where the door needs it, the room floor rides the
         * uplink at about -34 dBFS in every pause. Mute the pauses. Measured
         * here, after gain and before the denoiser: the floor was calibrated
         * on this signal, and judging the denoised one closed the gate on
         * quiet talkers. */
        if (a->uplink_ng_on &&
            syncn_noise_gate_run(&a->uplink_ng, energy_of(near_pcm, FRAME_SAMPLES),
                                 near_pcm, FRAME_SAMPLES))
            a->uplink_ng_frames++;

        /* What the microphone is actually delivering, after cleanup. Reported
         * in the stats line because "the door cannot hear me" is otherwise
         * indistinguishable from a protocol fault. */
        a->mic_dbfs = a->mic_dbfs * 0.9f
                    + syncn_rms_dbfs(near_pcm, FRAME_SAMPLES) * 0.1f;

        syncn_aec_capture(a->aec, near_pcm, FRAME_SAMPLES);
        if (a->dn_up)
            syncn_denoise_run(a->dn_up, near_pcm, FRAME_SAMPLES);

        /* The near side of the half-duplex comparison, if it is enabled:
         * what would go to the door this tick if nothing gated it. */
        a->near_dbfs = a->near_dbfs * 0.9f
                     + syncn_rms_dbfs(near_pcm, FRAME_SAMPLES) * 0.1f;

        /* ---- decide whether this tick's uplink carries voice or silence --- */
        bool send_silence = false;

        if (atomic_load_explicit(&a->muted, memory_order_relaxed)) {
            send_silence = true;
            a->muted_ticks++;
        } else if (a->duplex_active == SYNCN_DUPLEX_HALF) {
            /*
             * Half duplex: while the far end talks, our uplink fades to
             * silence so they cannot hear themselves. The gate has hysteresis
             * and a hangover, because a bare threshold closed 24% of the time
             * while the PANEL was talking on a real call -- the door's echo of
             * the panel voice sat right on the line. It fades rather than
             * switching, so it is silent rather than a click. The frame is
             * modified in place and encoded as-is: fully closed it is zeros,
             * which A-law turns into the same 0xD5 as fill_silence would.
             */
            /*
             * "Is the visitor talking?" is answered once, by the downlink
             * noise gate, and reused here. Deriving it a second time from
             * the raw level with its own threshold is how this gate got
             * stuck shut: it reopened below -34 and the door's idle floor
             * drifted to -33. With the downlink gate off, fall back to the
             * level.
             */
            const bool far_talking = a->downlink_ng_on
                                   ? !a->downlink_ng.closed
                                   : (a->far_energy > a->gate_energy);

            /*
             * Unless the panel is clearly louder. The door's microphone hears
             * the panel talker and sends them straight back, at -26 to -32
             * dBFS on the bench -- the same range as a real visitor, so no
             * threshold on the door's level alone can tell them apart. The
             * panel talker, after gain, sits 12-20 dB above their own return;
             * a visitor sits level with the panel's pickup of them. So: louder
             * side keeps the floor, by a margin. While both really talk at
             * once the louder wins, where before the visitor always did.
             */
            const bool near_wins = far_talking && a->near_priority_on &&
                                   a->near_dbfs > a->far_dbfs + a->cfg.near_priority_db;
            a->near_has_floor = near_wins;
            if (near_wins) {
                a->near_won++;
                syncn_gate_run_yield(&a->gate, near_pcm, FRAME_SAMPLES);
            } else if (syncn_gate_run_active(&a->gate, far_talking, near_pcm, FRAME_SAMPLES)) {
                a->gated++;
            }
        } else {
            a->near_has_floor = false;
        }

        float uplink_now = -96.0f;   /* this frame's wire level, unsmoothed */
        if (send_silence) {
            syncn_alaw_fill_silence(uplink, sizeof uplink);
            a->uplink_dbfs = a->uplink_dbfs * 0.9f + (-96.0f) * 0.1f;
        } else {
            syncn_alaw_encode(near_pcm, FRAME_SAMPLES, uplink);

            /* The wire level, on the samples the encoder is given. mic_dbfs is
             * read three steps earlier, before the echo canceller rewrites the
             * frame — so it is an upper bound on what the door receives, not a
             * measurement of it. */
            uplink_now = syncn_rms_dbfs(near_pcm, FRAME_SAMPLES);
            a->uplink_dbfs = a->uplink_dbfs * 0.9f + uplink_now * 0.1f;
        }

        /* The return canceller's reference is what the door will play and
         * its microphone will hear: exactly these samples, or silence. */
        if (a->ret_aec) {
            if (send_silence) {
                int16_t zeros[FRAME_SAMPLES];
                memset(zeros, 0, sizeof zeros);
                syncn_aec_play(a->ret_aec, zeros, FRAME_SAMPLES);
            } else {
                syncn_aec_play(a->ret_aec, near_pcm, FRAME_SAMPLES);
                /* Voice went out: for the length of the tail, what the door
                 * sends may be it coming back. The uplink noise gate has
                 * already replaced pauses with comfort noise, which is 25 dB
                 * under the floor and does not count. */
                if (a->uplink_dbfs > a->cfg.uplink_noise_floor_dbfs)
                    a->ref_active_left = (a->cfg.return_tail_ms > 0 ? a->cfg.return_tail_ms : 600)
                                       / SYNCN_AUDIO_FRAME_MS;
            }
        }

        /* The talker holds the floor (see the downlink gate above): voice
         * going out, this frame and the fourteen before it -- or this one
         * frame alone, if it is loud. A shout at the panel comes back from
         * the door within 100-300 ms at raw -20 dBFS, louder than any
         * visitor, and before 300 ms of voice have gone out: it opened the
         * gate, muted the microphone, the muting reset this count, and the
         * cycle chopped every shouted sentence into pieces while held= stayed
         * at 0. Nothing but a person at the panel puts a frame above
         * hold_loud_dbfs on the uplink: the visitor's direct pickup on the
         * bench, the one thing the 300 ms wait exists for, tops out around
         * -26. So a loud frame takes the floor at once. */
        if (!send_silence && a->uplink_dbfs > a->cfg.uplink_noise_floor_dbfs) {
            if (a->voiced_run < 1000)
                a->voiced_run++;
            if (a->voiced_run >= HOLD_AFTER_TICKS ||
                (a->cfg.hold_loud_dbfs < 0.0f && uplink_now > a->cfg.hold_loud_dbfs))
                a->hold_left = HOLD_FOR_TICKS;
        } else {
            a->voiced_run = 0;
        }

        /* ---- always emit, every 20 ms, no exceptions -------------------- */
        if (a->cb)
            a->cb(uplink, sizeof uplink, a->cb_user);

        a->ticks++;
        {
            struct timespec t_done;
            clock_gettime(CLOCK_MONOTONIC, &t_done);
            const long ns = (long)((t_done.tv_sec - t_busy.tv_sec) * 1000000000L
                                 + (t_done.tv_nsec - t_busy.tv_nsec));
            a->busy_ns_total += (double)ns;
            if (ns > a->busy_ns_max) a->busy_ns_max = ns;
        }

        /* ---- hold the 20 ms cadence ------------------------------------- */
        timespec_add_ns(&deadline, PERIOD_NS);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const double behind_ms = timespec_diff_ms(&now, &deadline);

        if (behind_ms > 200.0) {
            /* Badly behind — the panel was starved of CPU, or a device stalled.
             * Resynchronise rather than bursting a backlog of frames at the
             * door, which would only make the jitter worse at the far end. */
            if (a->ticks > 1)
                a->late_resyncs++;
            deadline = now;
        } else if (behind_ms < 0.0) {
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        }

        /* ---- auto mode: fall back to half duplex if echo stays bad ------ */
        if (a->cfg.duplex == SYNCN_DUPLEX_AUTO &&
            a->duplex_active == SYNCN_DUPLEX_FULL) {
            if (syncn_aec_residual(a->aec) > AUTO_RESIDUAL_LIMIT &&
                a->far_energy > a->gate_energy) {
                if (++a->auto_bad_ticks >= AUTO_BAD_TICKS) {
                    a->duplex_active = SYNCN_DUPLEX_HALF;
                    LOG_WARN("audio: residual echo stayed high for %d s — "
                             "falling back to half duplex",
                             AUTO_BAD_TICKS * (int)SYNCN_AUDIO_FRAME_MS / 1000);
                }
            } else if (a->auto_bad_ticks > 0) {
                a->auto_bad_ticks--;
            }
        }
    }

    if (atomic_load_explicit(&a->running, memory_order_relaxed)) {
        /* Still meant to be running, so this is a fault rather than a stop. */
        atomic_store(&a->failed, true);
        LOG_ERR("audio: engine stopped after %llu ticks — this call has no audio "
                "in either direction", (unsigned long long)a->ticks);
    } else {
        LOG_INFO("audio: thread stopped after %llu ticks",
                 (unsigned long long)a->ticks);
    }
    return NULL;
}

syncn_audio *syncn_audio_start(const syncn_audio_config *cfg,
                               syncn_audio_uplink_cb cb, void *user)
{
    syncn_audio *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;

    a->cfg     = *cfg;
    a->cb      = cb;
    a->cb_user = user;
    if (!a->cfg.capture_device)   a->cfg.capture_device  = "default";
    if (!a->cfg.playback_device)  a->cfg.playback_device = "default";
    if (a->cfg.aec_tail_ms <= 0)  a->cfg.aec_tail_ms     = 200;
    if (a->cfg.aec_delay_ms <= 0) a->cfg.aec_delay_ms    = 80;
    if (a->cfg.jitter_target_ms <= 0) a->cfg.jitter_target_ms = 60;

    a->mic_dbfs      = -96.0f;
    a->uplink_dbfs   = -96.0f;
    a->far_dbfs      = -96.0f;
    a->near_dbfs     = -96.0f;
    a->far_raw_dbfs  = -96.0f;
    a->near_priority_on = a->cfg.near_priority_db > 0.0f;
    a->limiter_on = a->cfg.uplink_ceiling_dbfs < 0.0f;
    if (a->limiter_on) {
        /*
         * Release: how long the gain takes to come back after a peak. Too
         * short and it recovers between syllables, so every syllable gets a
         * fresh attack and the modulation is audible as harshness -- 200 ms
         * did exactly that, limiting two-thirds of all samples on a real call.
         * Longer holds the gain down across a phrase, which sounds like one
         * quieter phrase rather than a flutter.
         */
        const float release = a->cfg.uplink_release_ms > 0.0f
                            ? a->cfg.uplink_release_ms : 400.0f;
        syncn_limiter_init(&a->limiter, a->cfg.uplink_ceiling_dbfs, release, (float)RATE);
        LOG_INFO("audio: uplink limiter at %.1f dBFS, %.0f ms release",
                 (double)a->cfg.uplink_ceiling_dbfs, (double)release);
    }
    {
        const float close = a->cfg.gate_threshold_dbfs != 0.0f
                          ? a->cfg.gate_threshold_dbfs : GATE_DEFAULT_DBFS;
        /* 400 ms: long enough that a dip inside the visitor's sentence, where
         * the downlink gate briefly closes, does not hand the uplink back
         * and let a burst of echo through. */
        const float hang  = a->cfg.gate_hangover_ms > 0.0f
                          ? a->cfg.gate_hangover_ms : 400.0f;
        /* 4 dB of hysteresis: the door's echo of the panel talker and a real
         * visitor were measured about 6 dB apart on this panel, so 4 leaves a
         * side for each. 5 ms ramps are inaudible and well inside one frame. */
        /* The half-duplex gate closes fast too (the visitor has started,
         * mute now) but releases over 100 ms, and sends comfort noise while
         * shut so the door never sees a dead line. */
        syncn_gate_init(&a->gate, close, 4.0f, hang, 5.0f, 100.0f,
                        a->cfg.comfort_noise_dbfs, (float)RATE);
        a->gate_energy = dbfs_to_energy(close);   /* auto mode still reads this */
        if (a->cfg.duplex == SYNCN_DUPLEX_HALF || a->cfg.duplex == SYNCN_DUPLEX_AUTO) {
            if (a->cfg.downlink_noise_floor_dbfs < 0.0f)
                LOG_INFO("audio: half-duplex gate follows the downlink noise gate, "
                         "%.0f ms hangover", (double)hang);
            else
                LOG_INFO("audio: half-duplex gate closes above %.0f dBFS, reopens below %.0f, "
                         "%.0f ms hangover", (double)close, (double)(close - 4.0f), (double)hang);
            if (a->near_priority_on) {
                /* 300 ms hangover: the door's return of the panel talker
                 * keeps arriving for about that long after they stop. 5 ms
                 * to mute, 50 ms to come back; no comfort noise on our own
                 * speaker. Thresholds unused: it is driven by the decision. */
                syncn_gate_init(&a->speaker_gate, close, 4.0f, 300.0f, 5.0f, 50.0f,
                                0.0f, (float)RATE);
                LOG_INFO("audio: the panel keeps the floor while %.0f dB louder than the door, "
                         "and the speaker is muted while it does",
                         (double)a->cfg.near_priority_db);
            }
        }
    }

    /*
     * The hold is what makes a noise gate usable on speech. Speech is not one
     * level: across a sentence the per-frame level spans 20 dB, and the quiet
     * syllables sit on the room floor where no threshold can separate them.
     * What separates them is time -- the gaps inside a sentence are under a
     * second, real pauses are longer. A 300 ms hold closed inside sentences
     * and ate the quiet words after each loud one, which the door heard as
     * "very, very low". 800 ms holds through a sentence.
     */
    a->uplink_ng_on = a->cfg.uplink_noise_floor_dbfs < 0.0f;
    if (a->uplink_ng_on) {
        const float hold = a->cfg.uplink_noise_hangover_ms > 0.0f
                         ? a->cfg.uplink_noise_hangover_ms : 300.0f;
        const float rel = a->cfg.noise_release_ms > 0.0f ? a->cfg.noise_release_ms : 400.0f;
        syncn_noise_gate_init(&a->uplink_ng, a->cfg.uplink_noise_floor_dbfs,
                              2.0f, hold, 5.0f, rel, a->cfg.comfort_noise_dbfs, (float)RATE);
        LOG_INFO("audio: uplink noise gate mutes below %.0f dBFS, reopens above %.0f, "
                 "%.0f ms hold, %.0f ms release, comfort noise %.0f dBFS",
                 (double)a->cfg.uplink_noise_floor_dbfs,
                 (double)(a->cfg.uplink_noise_floor_dbfs + 2.0f), (double)hold,
                 (double)rel, (double)a->cfg.comfort_noise_dbfs);
    }
    a->downlink_ng_on = a->cfg.downlink_noise_floor_dbfs < 0.0f;
    if (a->downlink_ng_on) {
        /* 3 dB of hysteresis, not 4: the quietest visitor measured was -29.2
         * and the door's idle floor -34.3, which leaves less room than the
         * uplink has. The hold is shorter than the uplink's because the
         * door's floor is only a couple of dB under the mute point, and every
         * ms of hold after the visitor stops is that floor being played. */
        const float hold = a->cfg.downlink_noise_hangover_ms > 0.0f
                         ? a->cfg.downlink_noise_hangover_ms : 200.0f;
        const float rel = a->cfg.noise_release_ms > 0.0f ? a->cfg.noise_release_ms : 400.0f;
        /* No comfort noise on the downlink: that is our own speaker, and the
         * point of gating it was to stop playing the door's floor. */
        syncn_noise_gate_init(&a->downlink_ng, a->cfg.downlink_noise_floor_dbfs,
                              2.0f, hold, 5.0f, rel, 0.0f, (float)RATE);
        LOG_INFO("audio: downlink noise gate mutes below %.0f dBFS, reopens above %.0f, "
                 "%.0f ms hold, %.0f ms release",
                 (double)a->cfg.downlink_noise_floor_dbfs,
                 (double)(a->cfg.downlink_noise_floor_dbfs + 2.0f), (double)hold, (double)rel);
        a->hold_on = a->cfg.hold_floor_dbfs < 0.0f &&
                     a->cfg.hold_floor_dbfs > a->cfg.downlink_noise_floor_dbfs &&
                     (a->cfg.duplex == SYNCN_DUPLEX_HALF || a->cfg.duplex == SYNCN_DUPLEX_AUTO);
        if (a->hold_on) {
            a->hold_atten = dbfs_to_energy(a->cfg.downlink_noise_floor_dbfs)
                          / dbfs_to_energy(a->cfg.hold_floor_dbfs);
            LOG_INFO("audio: the talker holds the floor: after %d ms of voice, or at once above "
                     "%.0f dBFS, the door's stream needs %.0f dBFS to take it, for %d ms after "
                     "the voice stops",
                     HOLD_AFTER_TICKS * SYNCN_AUDIO_FRAME_MS, (double)a->cfg.hold_loud_dbfs,
                     (double)a->cfg.hold_floor_dbfs, HOLD_FOR_TICKS * SYNCN_AUDIO_FRAME_MS);
        }
    }
    a->mic_gain      = db_to_linear(a->cfg.mic_gain_db);
    a->speaker_gain  = db_to_linear(a->cfg.speaker_gain_db);
    a->duplex_active = (a->cfg.duplex == SYNCN_DUPLEX_HALF)
                     ? SYNCN_DUPLEX_HALF : SYNCN_DUPLEX_FULL;

    a->jitter = syncn_jitter_create(a->cfg.jitter_target_ms);
    if (!a->jitter) {
        free(a);
        return NULL;
    }

    /*
     * Capture and playback are opened with the SAME channel count, and the
     * configuration is proven before it is trusted.
     *
     * Two things learned on the panel. Opening a stereo device as mono makes
     * ALSA average the channels, and this panel's microphone is wired to the
     * left only — the right reads -66 dBFS — so that average throws away 6 dB
     * of speech before anything else touches it. Stereo capture is how that is
     * avoided.
     *
     * But capture and playback on the RK809 are two halves of one codec sharing
     * an I2S configuration, not independent devices. Opening capture stereo and
     * playback mono is accepted at open time and then fails on the first read,
     * ten seconds later, with EIO. Separate arecord and aplay processes do not
     * show this because they go through ALSA's plug and dmix layers, which hide
     * the constraint; one process talking to the hardware does not.
     *
     * So: matched stereo first for the microphone's sake, matched mono as the
     * fallback, and a real read attempted before either is accepted. A silent
     * call is the worst possible failure here, and it is not one the daemon
     * should be able to reach by guessing.
     */
    /*
     * Mono, on both directions, unless explicitly configured otherwise.
     *
     * The microphone on this panel is wired to the left channel only, and
     * opening the device as mono makes ALSA average it with a dead right
     * channel. That was assumed to be the problem and is not: averaging with a
     * SILENT channel halves the amplitude and adds no noise, so it costs 6 dB
     * of level and nothing at all in signal-to-noise. A gain constant recovers
     * it exactly.
     *
     * Capturing in stereo to avoid that 6 dB does not work here. Capture and
     * playback on the RK809 are two halves of one codec sharing an I2S
     * configuration, and a stereo open inside one process fails on the first
     * read with EIO even when both directions match — while separate arecord
     * and aplay processes succeed, because ALSA's plug and dmix layers hide the
     * constraint.
     *
     * So: mono, and mic_gain_db carries the 6 dB. The setting exists for panels
     * whose codec does support it.
     */
    const unsigned ch = (a->cfg.capture_channels == 2) ? 2u : 1u;

    a->capture  = open_pcm(a->cfg.capture_device,  SND_PCM_STREAM_CAPTURE,  "capture", ch);
    a->playback = open_pcm(a->cfg.playback_device, SND_PCM_STREAM_PLAYBACK, "playback", ch);
    if (!a->capture || !a->playback) {
        syncn_audio_stop(a);
        return NULL;
    }
    a->capture_channels  = ch;
    a->playback_channels = ch;
    if (!a->capture || !a->playback) {
        syncn_audio_stop(a);
        return NULL;
    }

    if (a->capture_channels == 2) {
        a->chanpick = syncn_chanpick_create(
            syncn_channel_mode_from_name(a->cfg.capture_channel));
        LOG_INFO("audio: capture channel selection = %s",
                 a->cfg.capture_channel ? a->cfg.capture_channel : "auto");
    }

    /* Mains hum sat at roughly the same level as speech on this panel, so noise
     * suppression could not separate them and removed both. G.711 carries the
     * telephony band anyway, so nothing below ~150 Hz is lost that the door
     * could have transmitted. */
    if (a->cfg.highpass_hz > 0) {
        syncn_biquad_highpass(&a->highpass, (float)a->cfg.highpass_hz, (float)RATE, 0.707f);
        a->highpass_on = true;
        LOG_INFO("audio: high-pass at %d Hz on the microphone", a->cfg.highpass_hz);
    }
    if (a->cfg.lowpass_hz > 0) {
        /* Two Butterworth sections: 24 dB/octave, -6 dB at the corner. */
        syncn_biquad_lowpass(&a->lowpass[0], (float)a->cfg.lowpass_hz, (float)RATE, 0.707f);
        syncn_biquad_lowpass(&a->lowpass[1], (float)a->cfg.lowpass_hz, (float)RATE, 0.707f);
        syncn_biquad_lowpass(&a->dl_lowpass[0], (float)a->cfg.lowpass_hz, (float)RATE, 0.707f);
        syncn_biquad_lowpass(&a->dl_lowpass[1], (float)a->cfg.lowpass_hz, (float)RATE, 0.707f);
        a->lowpass_on = true;
    }
    if (a->cfg.downlink_highpass_hz > 0) {
        syncn_biquad_highpass(&a->dl_highpass, (float)a->cfg.downlink_highpass_hz, (float)RATE, 0.707f);
        a->dl_highpass_on = true;
    }
    if (a->lowpass_on || a->dl_highpass_on)
        LOG_INFO("audio: band %d-%d Hz on the door's stream, up to %d Hz on the microphone",
                 a->cfg.downlink_highpass_hz, a->cfg.lowpass_hz, a->cfg.lowpass_hz);
    if (a->cfg.presence_db != 0.0f && a->cfg.presence_hz > 0) {
        syncn_biquad_highshelf(&a->presence, (float)a->cfg.presence_hz, (float)RATE,
                               a->cfg.presence_db);
        syncn_biquad_highshelf(&a->dl_presence, (float)a->cfg.presence_hz, (float)RATE,
                               a->cfg.presence_db);
        a->presence_on = true;
        LOG_INFO("audio: presence %+.1f dB above %d Hz, both ways",
                 (double)a->cfg.presence_db, a->cfg.presence_hz);
    }

    const bool speex_denoise = a->cfg.denoise == SYNCN_DENOISE_SPEEX && a->cfg.aec_denoise;
    {
        syncn_aec_tuning tune;
        syncn_aec_default_tuning(&tune);
        tune.denoise = speex_denoise;
        if (a->cfg.aec_noise_suppress_db < 0)
            tune.noise_suppress_db = a->cfg.aec_noise_suppress_db;
        a->aec = syncn_aec_create_tuned(a->cfg.aec, RATE, FRAME_SAMPLES, a->cfg.aec_tail_ms,
                                        a->cfg.aec_delay_ms, &tune);
    }

    {
        const float wet = a->cfg.denoise_wet > 0.0f ? a->cfg.denoise_wet : 1.0f;
        if (a->cfg.denoise == SYNCN_DENOISE_RNNOISE) {
            a->dn_up = syncn_denoise_create(RATE, wet);
            if (!a->dn_up)
                LOG_WARN("audio: rnnoise not available in this build; microphone has no denoiser");
        }
        if (a->cfg.denoise_downlink == SYNCN_DENOISE_RNNOISE) {
            a->dn_down = syncn_denoise_create(RATE, wet);
            if (!a->dn_down)
                LOG_WARN("audio: rnnoise not available in this build; the door's stream has no denoiser");
        }
        if (a->dn_up || a->dn_down)
            LOG_INFO("audio: rnnoise on %s, %.0f%% wet",
                     a->dn_up && a->dn_down ? "both directions"
                                            : a->dn_up ? "the microphone" : "the door's stream",
                     (double)wet * 100.0);
    }

    if (a->cfg.return_cancel && a->cfg.aec != SYNCN_AEC_NONE) {
        /* Residual suppression a little harder than on the microphone: what
         * this one leaves behind is our own voice. Not too hard: on the
         * bench the panel's microphone hears the visitor before the gate
         * shuts, that goes out as reference, and -30/-20 then took 7 dB off
         * the visitor. The denoiser is on here too -- the door's idle floor
         * rides under the visitor, and "remove the noise" was asked of both
         * directions. The delay is inside the tail, so no reference offset. */
        syncn_aec_tuning tune;
        return_tuning(&a->cfg, &tune);
        const int tail = a->cfg.return_tail_ms > 0 ? a->cfg.return_tail_ms : 600;
        a->ret_aec = take_kept_return(tail, &tune);
        const bool kept = a->ret_aec != NULL;
        if (!a->ret_aec)
            a->ret_aec = syncn_aec_create_tuned(a->cfg.aec, RATE, FRAME_SAMPLES, tail, 0, &tune);
        if (a->ret_aec && syncn_aec_active(a->ret_aec) != SYNCN_AEC_NONE) {
            LOG_INFO("audio: return canceller on the door's stream, %d ms tail%s", tail,
                     kept ? ", kept from the last call" : "");
            if (a->cfg.return_echo_db > 0.0f) {
                /* 200 ms hangover: the return of a word keeps arriving for
                 * about that long after the filter last caught it cleanly.
                 * 5 ms to mute, 50 ms to come back; no comfort noise on our
                 * own speaker. Thresholds unused: driven by the decision. */
                syncn_gate_init(&a->echo_gate, 0.0f, 0.0f, 200.0f, 5.0f, 50.0f,
                                0.0f, (float)RATE);
                a->echo_gate_on = true;
                LOG_INFO("audio: frames it takes %.0f dB out of are kept off the speaker, "
                         "200 ms hangover", (double)a->cfg.return_echo_db);
            }
        } else {
            syncn_aec_destroy(a->ret_aec);
            a->ret_aec = NULL;
        }
    }

    /* Without cancellation, full duplex just means audible echo. Say so rather
     * than letting it be discovered on a call. */
    if (syncn_aec_active(a->aec) == SYNCN_AEC_NONE &&
        a->duplex_active == SYNCN_DUPLEX_FULL) {
        LOG_WARN("audio: full duplex with no echo cancellation — the far end "
                 "will hear themselves. Set duplex_mode=half to trade "
                 "interruption for a usable call.");
    }

    atomic_store(&a->running, true);
    atomic_store(&a->muted, false);

    if (pthread_create(&a->thread, NULL, audio_thread, a) != 0) {
        LOG_ERR("audio: cannot start the audio thread");
        atomic_store(&a->running, false);
        syncn_audio_stop(a);
        return NULL;
    }

    /*
     * Realtime priority. A missed 20 ms deadline is audible, and worse, it
     * breaks the uplink cadence the door gates its downlink on. Failure here is
     * not fatal — it just means audio competes with everything else, which on
     * an idle panel is usually fine.
     */
    struct sched_param sp = { .sched_priority = 10 };
    if (pthread_setschedparam(a->thread, SCHED_FIFO, &sp) != 0)
        LOG_WARN("audio: no realtime priority (needs rtprio in limits.conf); "
                 "audio may glitch under load");

    LOG_INFO("audio: started — %s duplex, aec=%s, jitter target %d ms",
             syncn_duplex_name(a->cfg.duplex),
             syncn_aec_backend_name(syncn_aec_active(a->aec)),
             a->cfg.jitter_target_ms);
    return a;
}

void syncn_audio_stop(syncn_audio *a)
{
    if (!a)
        return;

    if (atomic_load(&a->running)) {
        atomic_store(&a->running, false);
        pthread_join(a->thread, NULL);
    }
    if (a->capture)
        snd_pcm_close(a->capture);
    if (a->playback)
        snd_pcm_close(a->playback);
    syncn_aec_destroy(a->aec);
    if (a->ret_aec) {
        syncn_aec_tuning tune;
        return_tuning(&a->cfg, &tune);
        keep_return(a->ret_aec, a->cfg.return_tail_ms > 0 ? a->cfg.return_tail_ms : 600, &tune);
    }
    syncn_denoise_destroy(a->dn_up);
    syncn_denoise_destroy(a->dn_down);
    syncn_chanpick_destroy(a->chanpick);
    syncn_jitter_destroy(a->jitter);
    free(a);
}

void syncn_audio_push_downlink(syncn_audio *a, const uint8_t *alaw, size_t len)
{
    if (!a || !alaw)
        return;
    if (len != SYNCN_AUDIO_FRAME_BYTES) {
        /* The protocol fixes this at 160 bytes. Anything else means either a
         * framing bug on our side or a door we do not understand; either way,
         * feeding it to the decoder would produce noise. */
        LOG_DBG("audio: ignoring downlink frame of %zu bytes (expected %u)",
                len, SYNCN_AUDIO_FRAME_BYTES);
        return;
    }
    syncn_jitter_push(a->jitter, alaw);
}

void syncn_audio_set_mute(syncn_audio *a, bool muted)
{
    if (!a)
        return;
    atomic_store(&a->muted, muted);
    LOG_INFO("audio: microphone %s", muted ? "muted" : "unmuted");
}

bool syncn_audio_running(const syncn_audio *a)
{
    return a && atomic_load((atomic_bool *)&a->running)
             && !atomic_load((atomic_bool *)&a->failed);
}

bool syncn_audio_muted(const syncn_audio *a)
{
    return a && atomic_load((atomic_bool *)&a->muted);
}

void syncn_audio_get_stats(const syncn_audio *a, syncn_audio_stats *out)
{
    memset(out, 0, sizeof *out);
    if (!a)
        return;

    syncn_jitter_stats js;
    syncn_jitter_get_stats(a->jitter, &js);

    out->ticks              = a->ticks;
    out->frames_captured    = a->captured;
    out->frames_played      = a->played;
    out->downlink_received  = js.received;
    out->downlink_underruns = js.underruns;
    out->downlink_dropped   = js.dropped;
    out->downlink_trimmed   = js.trimmed;
    out->xruns_capture      = a->xruns_cap;
    out->xruns_playback     = a->xruns_play;
    out->gated_ticks        = a->gated;
    out->near_won_ticks     = a->near_won;
    out->speaker_muted_ticks = a->speaker_muted;
    out->echo_muted_ticks   = a->echo_muted;
    out->held_ticks         = a->held;
    out->return_judged      = a->ret_judged;
    out->return_total_db    = a->ret_judged ? (float)(a->ret_total_sum  / (double)a->ret_judged) : 0.0f;
    out->return_linear_db   = a->ret_judged ? (float)(a->ret_linear_sum / (double)a->ret_judged) : 0.0f;
    out->muted_ticks        = a->muted_ticks;
    out->late_resyncs       = a->late_resyncs;
    out->cpu_avg_pct        = a->ticks ? (float)(a->busy_ns_total / (double)a->ticks
                                                 / (SYNCN_AUDIO_FRAME_MS * 1e6) * 100.0) : 0.0f;
    out->cpu_max_pct        = (float)((double)a->busy_ns_max / (SYNCN_AUDIO_FRAME_MS * 1e6) * 100.0);
    out->mic_dbfs           = a->mic_dbfs;
    out->uplink_dbfs        = a->uplink_dbfs;
    out->clipped_samples    = a->clipped_samples;
    out->clipped_frames     = a->clipped_frames;
    out->saturated_samples  = a->saturated_samples;
    out->limited_samples    = a->limiter_on ? a->limiter.limited : 0;
    out->uplink_ng_frames   = a->uplink_ng_frames;
    out->downlink_ng_frames = a->downlink_ng_frames;
    out->far_dbfs           = a->far_dbfs;
    out->far_raw_dbfs       = a->far_raw_dbfs;
    out->near_dbfs          = a->near_dbfs;
    out->capture_channels   = a->capture_channels;
    out->capture_channel    = a->chanpick ? syncn_chanpick_decided(a->chanpick)
                                          : SYNCN_CH_MIX;
    out->jitter_depth       = js.depth;
    out->aec_residual       = syncn_aec_residual(a->aec);
    out->aec_backend        = syncn_aec_active(a->aec);
    out->duplex             = a->duplex_active;
    out->engine_running     = syncn_audio_running(a);
}
