#include "syncn/denoise.h"
#include "syncn/util.h"

#include <stdlib.h>
#include <string.h>

#ifdef SYNCN_HAVE_RNNOISE
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include <rnnoise.h>
#pragma GCC diagnostic pop
#include <speex/speex_resampler.h>

#define RNN_RATE   48000
#define RNN_FRAME  480          /* rnnoise_get_frame_size(): 10 ms at 48 kHz */
#define UP_CAP     (RNN_FRAME * 8)
#define OUT_CAP    4096

struct syncn_denoise {
    DenoiseState        *st;
    SpeexResamplerState *up, *down;
    int                  rate;
    float                wet;
    float                voice;

    /* 48 kHz samples waiting for a whole frame. */
    float  hi[UP_CAP];
    size_t hi_len;
    /* Denoised 8 kHz samples waiting to go out, and a dry copy of the input
     * aligned with them for the wet/dry mix. */
    float  out[OUT_CAP];
    float  dry[OUT_CAP];
    size_t out_len;
    /* Input, delayed by what the pipeline holds, for the dry side. */
    float  dry_fifo[OUT_CAP];
    size_t dry_len;
    int    primed;
};

int syncn_denoise_available(void) { return 1; }

syncn_denoise *syncn_denoise_create(int rate, float wet)
{
    syncn_denoise *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->rate = rate;
    d->wet  = wet < 0.0f ? 0.0f : wet > 1.0f ? 1.0f : wet;

    int err = 0;
    d->st   = rnnoise_create(NULL);
    d->up   = speex_resampler_init(1, (spx_uint32_t)rate, RNN_RATE, 5, &err);
    d->down = speex_resampler_init(1, RNN_RATE, (spx_uint32_t)rate, 5, &err);
    if (!d->st || !d->up || !d->down) {
        syncn_denoise_destroy(d);
        return NULL;
    }
    return d;
}

void syncn_denoise_destroy(syncn_denoise *d)
{
    if (!d)
        return;
    if (d->st)   rnnoise_destroy(d->st);
    if (d->up)   speex_resampler_destroy(d->up);
    if (d->down) speex_resampler_destroy(d->down);
    free(d);
}

float syncn_denoise_voice(const syncn_denoise *d) { return d ? d->voice : 0.0f; }

void syncn_denoise_run(syncn_denoise *d, int16_t *pcm, size_t count)
{
    if (!d || !pcm || count == 0)
        return;

    /* Up to 48 kHz. Samples stay in 16-bit scale: that is what the network
     * was trained on. */
    float in[1024];
    if (count > sizeof in / sizeof in[0])
        count = sizeof in / sizeof in[0];
    for (size_t i = 0; i < count; i++)
        in[i] = (float)pcm[i];

    /* Keep a dry copy in step with the output. */
    if (d->dry_len + count <= OUT_CAP) {
        memcpy(d->dry_fifo + d->dry_len, in, count * sizeof in[0]);
        d->dry_len += count;
    }

    spx_uint32_t in_len  = (spx_uint32_t)count;
    spx_uint32_t out_len = (spx_uint32_t)(UP_CAP - d->hi_len);
    speex_resampler_process_float(d->up, 0, in, &in_len, d->hi + d->hi_len, &out_len);
    d->hi_len += out_len;

    /* Whole frames through the network, then back down. */
    size_t done = 0;
    while (d->hi_len - done >= RNN_FRAME) {
        float clean[RNN_FRAME];
        d->voice = rnnoise_process_frame(d->st, clean, d->hi + done);
        done += RNN_FRAME;

        spx_uint32_t cin  = RNN_FRAME;
        spx_uint32_t cout = (spx_uint32_t)(OUT_CAP - d->out_len);
        speex_resampler_process_float(d->down, 0, clean, &cin, d->out + d->out_len, &cout);
        d->out_len += cout;
    }
    if (done) {
        memmove(d->hi, d->hi + done, (d->hi_len - done) * sizeof d->hi[0]);
        d->hi_len -= done;
    }

    /*
     * Exactly `count` out. The pipeline holds one frame plus the resamplers'
     * few samples, so the first call cannot fill; pad the front with silence
     * once and the delay is fixed from then on. The dry copy is aligned to
     * the same delay by dropping from its front what the padding stands in
     * for.
     */
    if (!d->primed) {
        if (d->out_len < count) {
            const size_t pad = count - d->out_len;
            memmove(d->out + pad, d->out, d->out_len * sizeof d->out[0]);
            memset(d->out, 0, pad * sizeof d->out[0]);
            d->out_len += pad;
            /* The dry side is `pad` samples ahead of the wet side now. */
            memmove(d->dry_fifo + pad, d->dry_fifo, d->dry_len * sizeof d->dry_fifo[0]);
            memset(d->dry_fifo, 0, pad * sizeof d->dry_fifo[0]);
            d->dry_len += pad;
            if (d->dry_len > OUT_CAP) d->dry_len = OUT_CAP;
        }
        d->primed = 1;
    }
    if (d->out_len < count) {
        /* Should not happen after priming; hold the input rather than glitch. */
        return;
    }

    const float wet = d->wet, dryw = 1.0f - d->wet;
    for (size_t i = 0; i < count; i++) {
        const float dryv = i < d->dry_len ? d->dry_fifo[i] : 0.0f;
        float v = d->out[i] * wet + dryv * dryw;
        if (v > 32767.0f)  v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm[i] = (int16_t)v;
    }
    memmove(d->out, d->out + count, (d->out_len - count) * sizeof d->out[0]);
    d->out_len -= count;
    if (d->dry_len >= count) {
        memmove(d->dry_fifo, d->dry_fifo + count, (d->dry_len - count) * sizeof d->dry_fifo[0]);
        d->dry_len -= count;
    } else {
        d->dry_len = 0;
    }
}

#else  /* !SYNCN_HAVE_RNNOISE */

int syncn_denoise_available(void) { return 0; }
syncn_denoise *syncn_denoise_create(int rate, float wet) { (void)rate; (void)wet; return NULL; }
void syncn_denoise_destroy(syncn_denoise *d) { (void)d; }
void syncn_denoise_run(syncn_denoise *d, int16_t *pcm, size_t count) { (void)d; (void)pcm; (void)count; }
float syncn_denoise_voice(const syncn_denoise *d) { (void)d; return 0.0f; }

#endif
