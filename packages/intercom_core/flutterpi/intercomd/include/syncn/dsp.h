/*
 * Small fixed-function DSP for the capture path.
 *
 * Everything here exists because of what the YC-SM1011's microphone actually
 * delivers, measured on the bench:
 *
 *   - the capsule is wired to the LEFT channel only; the right reads -66 dBFS
 *   - a constant 60 Hz mains hum sits at roughly amplitude 670
 *   - speech peaks around 5000 and holds an RMS near 745
 *
 * Which means the hum and the voice are within a few dB of each other. Noise
 * suppression cannot separate them, so it removes both, and the door hears
 * silence while the panel dutifully transmits 50 frames a second of it.
 */
#ifndef SYNCN_DSP_H
#define SYNCN_DSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------- channel pick --- */

typedef enum {
    SYNCN_CH_AUTO = 0, /* measure both, keep the live one   */
    SYNCN_CH_LEFT,
    SYNCN_CH_RIGHT,
    SYNCN_CH_MIX       /* average — only correct when both carry signal */
} syncn_channel_mode;

const char *syncn_channel_mode_name(syncn_channel_mode m);
syncn_channel_mode syncn_channel_mode_from_name(const char *name);

/*
 * Extract one mono frame from interleaved stereo.
 *
 * SYNCN_CH_MIX is what ALSA does when a stereo device is opened as mono, and it
 * is wrong here: averaging a live channel with a dead one costs 6 dB of signal
 * for nothing. AUTO settles the question by measurement instead of assumption —
 * it watches both channels until one is clearly louder, then stays there.
 */
typedef struct syncn_chanpick syncn_chanpick;

syncn_chanpick *syncn_chanpick_create(syncn_channel_mode mode);
void            syncn_chanpick_destroy(syncn_chanpick *p);

void syncn_chanpick_run(syncn_chanpick *p, const int16_t *stereo, size_t frames,
                        int16_t *mono_out);

/* Which channel is in use once AUTO has decided. LEFT/RIGHT/MIX passthrough
 * report themselves immediately. */
syncn_channel_mode syncn_chanpick_decided(const syncn_chanpick *p);
bool               syncn_chanpick_is_settled(const syncn_chanpick *p);

/* ------------------------------------------------------------- high pass --- */

/*
 * Second-order high-pass, RBJ cookbook.
 *
 * Mains hum at 50 or 60 Hz is well below any useful speech, and on a narrowband
 * voice link nothing under about 100 Hz carries intelligibility at all. Removing
 * it before echo cancellation matters twice over: it stops the canceller
 * adapting to a signal that is not echo, and it lets noise suppression judge
 * speech against a quiet floor rather than a loud one.
 */
typedef struct {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} syncn_biquad;

void syncn_biquad_highpass(syncn_biquad *f, float cutoff_hz, float rate, float q);
/* Second-order low-pass, same cookbook. Cascade two for a telephony-band
 * edge: hiss lives in the top octave and speech does not. */
void syncn_biquad_lowpass(syncn_biquad *f, float cutoff_hz, float rate, float q);
/* High shelf: everything above the corner lifted (or cut) by gain_db. A
 * presence lift of a few dB is what makes a narrowband voice crisp rather
 * than muffled; the consonants live up there and carry no energy. */
void syncn_biquad_highshelf(syncn_biquad *f, float corner_hz, float rate, float gain_db);
void syncn_biquad_reset(syncn_biquad *f);

/* In place, on int16 samples, with saturation. */
void syncn_biquad_run(syncn_biquad *f, int16_t *samples, size_t count);

/* ------------------------------------------------------------- limiter --- */

/*
 * A peak limiter for the uplink.
 *
 * Why the panel talker is quiet at the door even after the gain went up:
 * speech peaks sit 12 to 15 dB above its average, and raw gain has to leave
 * room for the peaks, so the average -- which is what loudness is -- stays
 * low. Every telephone solves this the same way: lift the gain hard and catch
 * only the peaks. The average comes up by the full gain; the peaks are held
 * at a ceiling instead of hitting the rail and splattering.
 *
 * Instant attack, so no peak ever gets through above the ceiling. Release over
 * a few hundred milliseconds, so the gain does not chatter between syllables.
 * Below the ceiling it does nothing at all -- quiet speech is untouched.
 */
typedef struct {
    float ceiling;      /* linear, 0..32767                          */
    float envelope;     /* current peak estimate, decays over release */
    float release;      /* per-sample decay factor                   */
    uint64_t limited;   /* samples that were pulled down             */
} syncn_limiter;

void syncn_limiter_init(syncn_limiter *l, float ceiling_dbfs, float release_ms, float rate);
void syncn_limiter_run(syncn_limiter *l, int16_t *samples, size_t count);

/*
 * Gain and limiting in one pass, in float, clamped once at the end.
 *
 * The first version applied the gain with a 16-bit clamp and ran the limiter
 * afterwards, so on a loud word the clamp had already flattened the waveform
 * before the limiter saw it. The limiter then dutifully scaled down audio that
 * was already clipped -- 34,000 saturated samples in one call, heard at the
 * door as harshness. A limiter has to see the peak before anything else
 * truncates it, which means the gain must not round-trip through int16 first.
 */
void syncn_limiter_run_gain(syncn_limiter *l, int16_t *samples, size_t count,
                            float gain);

/* ---------------------------------------------------------- half-duplex gate ---

 * Mutes the uplink while the far end is talking.
 *
 * The first version was a bare comparison against one threshold, and a real
 * call showed what that does: the door's own echo of the panel talker comes
 * back at almost exactly the threshold, so the gate closed 24% of the time
 * WHILE THE PANEL WAS TALKING -- 20 ms of silence dropped into the middle of
 * every few words. That is heard as harsh, and no amount of limiter tuning
 * touches it because it is not the limiter.
 *
 * Three things a gate needs so that cannot happen:
 *
 *   hysteresis   it closes at one level and reopens at a lower one, so a
 *                signal sitting on the threshold picks a side and stays there
 *   hangover     once closed it stays closed for a while after the far end
 *                goes quiet, so the gap between a visitor's words does not
 *                reopen it and let a burst of echo through
 *   ramps        the transition fades over a few milliseconds rather than
 *                switching between frames, so it is silent rather than a click
 */
typedef struct {
    double close_energy;    /* far-end mean-square above which we mute       */
    double open_energy;     /* ...and below which we unmute (lower)          */
    int    hangover_frames; /* frames to stay muted after far goes quiet     */
    int    attack_samples;  /* fade length when opening: fast               */
    int    release_samples; /* fade length when closing: slow               */

    /*
     * What a closed gate sends. Zero is pure digital silence, and this door
     * reads a long run of it as an idle line: it stopped sending audio for
     * the rest of a call after our noise gate had been shut for a while.
     * Comfort noise at about -55 dBFS keeps the line alive and is inaudible
     * next to speech -- the same reason telephony has sent it since the
     * 1990s. Linear amplitude; 0 disables.
     */
    float  comfort_amp;
    unsigned noise_seed;

    bool   closed;
    int    hang_left;
    float  level;           /* current gain 0..1, ramps between              */
    uint64_t closures;      /* how many times it shut -- flicker shows here  */
} syncn_gate;

/*
 * attack_ms is how fast it opens (5 ms: a word must not lose its first
 * consonant). release_ms is how slowly it closes, and this is the number that
 * decides how the tail after speech sounds. A short release plateaus at full
 * gain for the whole hangover and then drops -- the room floor is heard for
 * the hangover, then cut. A long release fades the floor out instead, and a
 * quiet word that arrives mid-fade is attenuated rather than lost.
 */
void syncn_gate_init(syncn_gate *g, float close_dbfs, float hysteresis_db,
                     float hangover_ms, float attack_ms, float release_ms,
                     float comfort_dbfs, float rate);

bool syncn_gate_run(syncn_gate *g, double far_energy, int16_t *uplink, size_t count);

/*
 * The same gate, driven by a decision made elsewhere rather than by a level.
 *
 * `far_talking` true closes it (after which the hangover applies as usual);
 * false lets it reopen once the hangover has run. No thresholds are consulted.
 *
 * This exists because the half-duplex gate and the downlink noise gate were
 * both answering "is the visitor talking?" from the same signal with
 * different thresholds, and the half-duplex one reopened at -34 while the
 * door's idle floor drifted to -33: closed once, it stayed closed to the end
 * of the call and the panel talker was muted. The downlink noise gate makes
 * that call well -- it stays flat through a visitor's sentence -- so the
 * half-duplex gate now follows it. One answer, used twice.
 */
bool syncn_gate_run_active(syncn_gate *g, bool far_talking, int16_t *uplink, size_t count);

/*
 * The near end has the floor: open now, hangover cancelled, then apply.
 *
 * Used when the panel talker is clearly louder than what the door is sending.
 * On the bench the door's microphone hears the panel talker directly and sends
 * them back at -26 to -32 dBFS, which the downlink gate cannot tell from a
 * visitor; without this the panel's own voice muted the panel 30-95% of the
 * time. Going through the hangover would still cost the first 150 ms of every
 * phrase, so this opens at once (over the 5 ms attack).
 */
bool syncn_gate_run_yield(syncn_gate *g, int16_t *uplink, size_t count);

/*
 * The same gate the other way up: a NOISE gate. Mutes when the signal's own
 * level falls BELOW the floor, reopens once it rises clearly above it.
 *
 * Why it exists: with the gain where the door needs it, the panel's room
 * floor rides the uplink at about -36 dBFS in every pause, and the door's own
 * idle floor arrives at -34 and is played at the panel. Both sides reported
 * the same thing -- "no noise while someone talks, then it comes back". That
 * is noise masked by speech and exposed in the gaps. Muting the gaps is the
 * whole of the fix, and hysteresis plus hangover is what stops it chopping the
 * quiet ends of words.
 *
 * syncn_gate_init() is reused: close_dbfs is the floor below which it mutes,
 * and it reopens `hysteresis_db` ABOVE that.
 */
void syncn_noise_gate_init(syncn_gate *g, float floor_dbfs, float hysteresis_db,
                           float hangover_ms, float attack_ms, float release_ms,
                           float comfort_dbfs, float rate);
bool syncn_noise_gate_run(syncn_gate *g, double own_energy, int16_t *frame, size_t count);

/* ---------------------------------------------------------------- levels --- */

/* RMS in dBFS, for logging what the microphone is actually delivering.
 * Returns -100.0 for digital silence. */
float syncn_rms_dbfs(const int16_t *samples, size_t count);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_DSP_H */
