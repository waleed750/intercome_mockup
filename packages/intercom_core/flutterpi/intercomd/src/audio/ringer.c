#include "syncn/ringer.h"
#include "syncn/util.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef SYNCN_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

/* Timing of one cycle, shared with the synth. */
#define DING_MS   350
#define DONG_MS   500
#define GAP_MS    1900

/* ----------------------------------------------------------- the thread --- */

#ifdef SYNCN_HAVE_ALSA

static pthread_t   g_thread;
static bool        g_joinable;   /* a thread was created and not yet reaped  */
static atomic_bool g_running;    /* ...and is still playing                  */
static atomic_bool g_stop;
static char        g_device[128];
static float       g_gain_db;

static void *ring_thread(void *arg)
{
    (void)arg;

    snd_pcm_t *pcm = NULL;
    if (snd_pcm_open(&pcm, g_device, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        LOG_ERR("ringer: cannot open %s — ringing silently", g_device);
        atomic_store(&g_running, false);
        return NULL;
    }
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           1, SYNCN_RING_RATE, 1, 100000) < 0) {
        LOG_ERR("ringer: %s refuses %d Hz mono — ringing silently", g_device, SYNCN_RING_RATE);
        snd_pcm_close(pcm);
        atomic_store(&g_running, false);
        return NULL;
    }

    /* One cycle, rendered once. */
    const size_t cap = (size_t)SYNCN_RING_RATE * (DING_MS + DONG_MS + GAP_MS) / 1000 + 16;
    int16_t *cycle = calloc(cap, sizeof *cycle);
    if (!cycle) {
        snd_pcm_close(pcm);
        atomic_store(&g_running, false);
        return NULL;
    }
    const size_t n = syncn_ringer_synth(cycle, cap, g_gain_db);

    /* Written in 50 ms pieces so a stop is honoured within 50 ms, which is
     * what makes the answer button feel like it did something. */
    const size_t piece = SYNCN_RING_RATE / 20;
    while (!atomic_load(&g_stop)) {
        for (size_t off = 0; off < n && !atomic_load(&g_stop); off += piece) {
            const size_t want = (n - off < piece) ? n - off : piece;
            snd_pcm_sframes_t w = snd_pcm_writei(pcm, cycle + off, want);
            if (w < 0) {
                w = snd_pcm_recover(pcm, (int)w, 1);
                if (w < 0) {
                    LOG_WARN("ringer: playback error: %s", snd_strerror((int)w));
                    goto out;
                }
            }
        }
    }

out:
    /* Drop, not drain: the person has answered and the engine wants the
     * device now, not after the dong finishes. */
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    free(cycle);
    atomic_store(&g_running, false);
    return NULL;
}

bool syncn_ringer_start(const char *device, float gain_db)
{
    if (atomic_load(&g_running))
        return true;

    snprintf(g_device, sizeof g_device, "%s", (device && *device) ? device : "default");
    g_gain_db = gain_db;
    atomic_store(&g_stop, false);
    atomic_store(&g_running, true);

    /* Reap a previous thread that ended on its own (device error) before
     * starting another, or the handle is overwritten and never joined. */
    if (g_joinable) {
        pthread_join(g_thread, NULL);
        g_joinable = false;
    }

    if (pthread_create(&g_thread, NULL, ring_thread, NULL) != 0) {
        atomic_store(&g_running, false);
        LOG_ERR("ringer: could not start the ring thread");
        return false;
    }
    g_joinable = true;
    LOG_INFO("ringer: ringing on %s", g_device);
    return true;
}

void syncn_ringer_stop(void)
{
    if (!g_joinable)
        return;
    atomic_store(&g_stop, true);
    pthread_join(g_thread, NULL);
    g_joinable = false;
    atomic_store(&g_stop, false);
    atomic_store(&g_running, false);
}

bool syncn_ringer_active(void) { return atomic_load(&g_running); }

#else /* !SYNCN_HAVE_ALSA */

bool syncn_ringer_start(const char *device, float gain_db)
{ (void)device; (void)gain_db; LOG_WARN("ringer: built without ALSA"); return false; }
void syncn_ringer_stop(void) {}
bool syncn_ringer_active(void) { return false; }

#endif
