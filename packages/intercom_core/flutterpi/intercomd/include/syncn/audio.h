/*
 * Audio engine: ALSA duplex, echo cancellation, jitter buffering, and the
 * 20 ms uplink cadence.
 *
 * One realtime thread owns both PCM devices. It is paced by the capture read,
 * which blocks for exactly one period, so the loop runs at the codec's own
 * clock rather than a timer we would have to chase.
 *
 * The uplink callback fires on EVERY tick, without exception — muted, gated,
 * capture broken, does not matter. That is deliberate and it is the whole
 * design: the door gates its downlink on seeing a continuous uplink stream, so
 * a gap in our transmission makes the door go silent. Emitting the frame
 * unconditionally means the cadence cannot be broken by a bug elsewhere, which
 * is a stronger guarantee than the watchdog timer it replaces.
 */
#ifndef SYNCN_AUDIO_H
#define SYNCN_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "syncn/aec.h"
#include "syncn/denoise.h"
#include "syncn/dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYNCN_DUPLEX_FULL = 0, /* both directions live; needs working cancellation */
    SYNCN_DUPLEX_HALF,     /* mute uplink while the far end talks              */
    SYNCN_DUPLEX_AUTO      /* start full, drop to half if echo stays bad       */
} syncn_duplex_mode;

const char *syncn_duplex_name(syncn_duplex_mode m);
syncn_duplex_mode syncn_duplex_from_name(const char *name);

typedef enum {
    SYNCN_DENOISE_OFF = 0,
    SYNCN_DENOISE_SPEEX,    /* the canceller's own band-by-band suppressor */
    SYNCN_DENOISE_RNNOISE   /* the neural one, see denoise.h; shipped     */
} syncn_denoise_mode;

const char *syncn_denoise_name(syncn_denoise_mode m);
syncn_denoise_mode syncn_denoise_from_name(const char *name);

typedef struct {
    const char       *capture_device;   /* NULL -> "default"          */
    const char       *playback_device;  /* NULL -> "default"          */
    syncn_aec_backend aec;
    int               aec_tail_ms;      /* 0 -> 200                   */
    int               aec_delay_ms;     /* 0 -> 80, the playback latency */
    syncn_duplex_mode duplex;
    int               capture_channels;   /* 1 (default) or 2           */
    const char       *capture_channel;   /* auto | left | right | mix  */
    int               highpass_hz;       /* microphone; 0 disables; 250 shipped */
    /* The telephony band, both ways. Hiss lives in the top octave and speech
     * does not: two cascaded second-order sections at lowpass_hz on the
     * microphone and on the door's stream, and a high-pass on the door's
     * stream too. 0 disables either. */
    int               lowpass_hz;             /* 3700 shipped */
    int               downlink_highpass_hz;   /* 250 shipped  */
    /* Presence: a high shelf both ways. "Slightly muffled" once the band
     * limit and the denoisers had taken the noise; the consonants live
     * above 2 kHz and carry almost no energy. 0 dB disables. */
    int               presence_hz;            /* 2000 shipped */
    float             presence_db;            /* 3 shipped    */
    int               mic_gain_db;
    int               speaker_gain_db;
    int               jitter_target_ms; /* 0 -> 60                    */
    float             gate_threshold_dbfs; /* half-duplex gate closes above; 0 -> -30 */
    float             gate_hangover_ms;    /* stays closed after far goes quiet; 0 -> 150 */
    float             uplink_ceiling_dbfs; /* peak limiter; 0 disables; -1 typical */
    float             uplink_release_ms;   /* limiter release; 0 -> 400 */
    float             uplink_noise_floor_dbfs;   /* mute mic below; 0 disables */
    float             downlink_noise_floor_dbfs; /* mute door below; 0 disables */
    float             uplink_noise_hangover_ms;   /* 0 -> 300 */
    float             downlink_noise_hangover_ms; /* 0 -> 200 */
    float             noise_release_ms;      /* gate close fade; 0 -> 400  */
    float             comfort_noise_dbfs;    /* sent while our uplink is gated; 0 = silence */

    /* Half duplex: the panel keeps the floor while its own level, after the
     * canceller and the noise gate, exceeds the door's by this many dB. The
     * door's microphone hears the panel talker and sends them back; without
     * this that echo muted the panel. While the panel has the floor the
     * speaker is muted too, so that return is not played and re-captured.
     * 0 disables; 4 shipped. */
    float             near_priority_db;

    /* Speex denoiser on the microphone, bounded to this much attenuation.
     * Off it was, when the microphone was at -41 dBFS and the suppressor could
     * not tell quiet speech from the floor; at +20 dB of gain speech sits 20 dB
     * above the floor and it can. */
    bool              aec_denoise;
    int               aec_noise_suppress_db;  /* negative; -10 shipped */

    /* Which denoiser, both ways. rnnoise replaces the two speex denoisers
     * above and in the return canceller; speex keeps them; off is neither.
     * denoise_wet: 0..1, how much of rnnoise's output replaces the input. */
    syncn_denoise_mode denoise;            /* the microphone; rnnoise shipped */
    syncn_denoise_mode denoise_downlink;   /* the door's stream; speex shipped: the
                                              network twice over missed the deadline */
    float             denoise_wet;

    /*
     * The return canceller. The door's microphone hears the panel talker --
     * through its own speaker installed, straight through the air on the
     * bench -- and sends them back 100-300 ms later at the level a visitor
     * arrives at. No threshold tells the two apart, and comparing levels
     * failed on a bench where the panel hears the visitor too. What does
     * tell them apart: the return is a delayed copy of exactly what we sent.
     * A second speex canceller, reference = our uplink, takes it out of the
     * door's stream before any gate reads it. tests/test_return.c: 17 dB.
     */
    bool              return_cancel;     /* on shipped */
    int               return_tail_ms;    /* covers the round trip; 600 shipped */
    int               return_noise_suppress_db;  /* denoiser on the door's stream; -15 shipped */
    /* What the canceller cannot take out, keep off the speaker. The return
     * peaks above the downlink gate's floor after cancellation, it was
     * played, and the talker at the panel heard themselves a beat late --
     * "a small echo"; with the speaker shut it was gone. A frame the
     * linear filter took this many dB out of, among the frames that could
     * be our voice coming back (we sent voice within the tail, and the
     * frame is above the downlink floor), correlated with what we sent:
     * a copy of it whatever is left, muted with a hangover instead of
     * judged by level. With nothing sent the filter's output is zero and a
     * visitor reads 0. 0 disables; 3 shipped, low because the door's path
     * is not one a linear filter models well (8 never fired). */
    float             return_echo_db;
    /* The talker holds the floor. The return canceller above cannot take
     * the door's return out of the door's own floor (ret= read 0 dB on the
     * bench), and its peaks opened the downlink gate for 400 ms at a time:
     * the talker's microphone muted, "some of his words are being cut
     * off". After 300 ms of continuous voice from the panel, or at once on
     * a frame above hold_loud_dbfs, the door's stream has to reach this
     * level to take the floor, for 600 ms past the last voiced frame. The
     * return of a shout came back at raw -20, then raw -16, then raw -10:
     * -28, -22 and -12 were each crossed in turn. -1 shipped: the hold is
     * absolute, nothing the door sends takes the floor from the panel
     * talker, and the visitor speaks the moment the panel pauses 600 ms.
     * 0 disables. Only with a downlink noise gate, and only above its
     * floor. */
    float             hold_floor_dbfs;
    /* A single uplink frame above this takes the floor without the 300 ms
     * wait. Only a person at the panel reaches it; the bench's direct
     * pickup of the visitor tops out around -26. Without it a shout's
     * return, back within 100-300 ms at raw -20, muted the microphone
     * before the 300 ms were up and reset the count. 0 disables; -18
     * shipped. */
    float             hold_loud_dbfs;
} syncn_audio_config;

void syncn_audio_config_defaults(syncn_audio_config *cfg);

/*
 * Called from the audio thread once every 20 ms with 160 bytes of A-law ready
 * to go uplink. Must be cheap and thread-safe — it runs on a realtime thread,
 * so no allocation, no logging, no blocking.
 */
typedef void (*syncn_audio_uplink_cb)(const uint8_t *alaw, size_t len, void *user);

typedef struct syncn_audio syncn_audio;

syncn_audio *syncn_audio_start(const syncn_audio_config *cfg,
                               syncn_audio_uplink_cb cb, void *user);
void         syncn_audio_stop(syncn_audio *a);

/* Hand a received 0xCC payload to the jitter buffer. Safe from any thread. */
void syncn_audio_push_downlink(syncn_audio *a, const uint8_t *alaw, size_t len);

/* False once the engine has stopped on its own — a call with no audio. */
bool syncn_audio_running(const syncn_audio *a);

void syncn_audio_set_mute(syncn_audio *a, bool muted);
bool syncn_audio_muted(const syncn_audio *a);

typedef struct {
    uint64_t ticks;             /* 20 ms iterations completed                 */
    uint64_t frames_captured;
    uint64_t frames_played;
    uint64_t downlink_received;
    uint64_t downlink_underruns;/* buffer empty when a frame was needed       */
    uint64_t downlink_dropped;  /* buffer full; far end is ahead of us        */
    uint64_t downlink_trimmed;  /* frames discarded to pull latency back down */
    uint64_t xruns_capture;
    uint64_t xruns_playback;
    uint64_t gated_ticks;       /* uplink replaced with silence by the gate   */
    /* Frames the door was above its floor but the panel kept the floor by
     * being clearly louder. Climbs while the panel talks over the door's
     * pickup of them; must stay flat while only a visitor talks. */
    uint64_t near_won_ticks;
    /* Frames the speaker was muted because the panel had the floor. */
    uint64_t speaker_muted_ticks;
    /* Frames the return canceller recognised as our own voice coming back
     * and kept off the speaker (return_echo_db). Climbs while the panel
     * talks, stays flat while a visitor does. */
    uint64_t echo_muted_ticks;
    /* The frames the echo judgement looked at, and what the return canceller
     * took out of them on average: the whole removal, and the linear
     * filter's share of it. */
    uint64_t return_judged;
    /* Ticks the talker held the floor: the downlink gate read a raised
     * floor. Climbs while the panel talks, flat while a visitor does. */
    uint64_t held_ticks;
    float    return_total_db, return_linear_db;
    uint64_t muted_ticks;
    uint64_t late_resyncs;      /* tick fell >200 ms behind and was reset      */
    /* Processing time per tick as a share of the 20 ms budget, mean and
     * worst since the call started. Above 100 the audio drops out. */
    float    cpu_avg_pct, cpu_max_pct;
    float    mic_dbfs;          /* after the high-pass and gain, BEFORE the AEC */

    /*
     * What actually goes on the wire: measured after the echo canceller has
     * rewritten the frame and after the mute and half-duplex decisions, on the
     * exact samples handed to the A-law encoder.
     *
     * Both numbers are reported because they answer different questions. The
     * door hearing nothing with a healthy mic_dbfs and a dead uplink_dbfs is
     * the echo canceller eating the speech; both low is a quiet microphone.
     * Reporting only the first made "the door can barely hear me" impossible
     * to attribute — it was measured three steps upstream of the wire.
     */
    float    uplink_dbfs;

    /* Capture samples against the converter's rails, and how many 20 ms frames
     * contained at least one. Non-zero means audio was lost before any of this
     * daemon's processing saw it, which no amount of tuning recovers. */
    uint64_t clipped_samples;
    uint64_t clipped_frames;

    /* Samples mic_gain_db pushed past the rails after the converter. Non-zero
     * means the gain is too high, which clipped_samples cannot tell you. */
    uint64_t saturated_samples;

    /* Samples the limiter pulled down to the ceiling. Non-zero is normal on
     * loud words; it is the limiter doing its job rather than the rail. */
    uint64_t limited_samples;

    /* Frames each noise gate muted. Should climb in pauses and not during
     * speech; climbing during speech means the floor is set too high. */
    uint64_t uplink_ng_frames;
    uint64_t downlink_ng_frames;

    /* The door's level after speaker_gain_db and the return canceller: what
     * the half-duplex gate sees. */
    float    far_dbfs;
    /* The same before the return canceller. The gap between the two is the
     * panel talker coming back; it should open while the panel talks and
     * close while a visitor does. */
    float    far_raw_dbfs;
    /* The panel's level after the canceller and the noise gate: the other
     * side of that comparison. */
    float    near_dbfs;
    unsigned capture_channels;
    syncn_channel_mode capture_channel;
    bool     engine_running;
    unsigned jitter_depth;      /* frames currently buffered                  */
    float    aec_residual;
    syncn_aec_backend aec_backend;
    syncn_duplex_mode duplex;
} syncn_audio_stats;

void syncn_audio_get_stats(const syncn_audio *a, syncn_audio_stats *out);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_AUDIO_H */
