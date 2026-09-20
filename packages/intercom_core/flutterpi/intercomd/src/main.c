/*
 * syncn-intercomd — SyncN panel intercom daemon.
 *
 * Stage one: the full protocol, two-way audio with echo cancellation, and the
 * door's video stream written to disk for verification. Decoding that stream to
 * the panel display comes next, once the diagnostics settle which decoder path
 * is usable.
 */
#include "syncn/call.h"
#include "syncn/config.h"
#include "syncn/util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_CONFIG "/etc/syncn/intercom.conf"

static syncn_call *g_call = NULL;

static void on_signal(int sig)
{
    (void)sig;
    if (g_call)
        syncn_call_stop(g_call);
}

static void usage(const char *argv0)
{
    printf(
"SyncN panel intercom daemon\n"
"\n"
"Usage: %s [options]\n"
"\n"
"  -c, --config <path>    configuration file (default %s)\n"
"      --door <ip>        door station address, overriding the config\n"
"      --port <n>         door call port (default 8189)\n"
"      --listen-port <n>  port we listen on (default: same as --port)\n"
"      --capture <dev>    ALSA capture device, e.g. plughw:0,0\n"
"      --playback <dev>   ALSA playback device\n"
"      --duplex <mode>    full | half | auto\n"
"      --aec <backend>    speex | none\n"
"      --mic-gain <dB>    microphone gain\n"
"      --spk-gain <dB>    speaker gain\n"
"      --video-dump <f>   write the door's H.264 stream here\n"
"  -l, --log <level>      error | warn | info | debug | trace\n"
"      --call             dial the door immediately on startup\n"
"      --batch            do not read keys from stdin (for systemd)\n"
"  -h, --help\n"
"\n"
"Interactive keys:\n"
"  c  call the door        a  answer a ringing call    u  unlock the door\n"
"  p  preview (video only) h  hang up                  m  mute the microphone\n"
"  s  print statistics     q  quit\n"
"\n"
"The panel always listens on the call port for door-initiated calls and answers\n"
"discovery broadcasts, whether or not a door address is configured.\n",
    argv0, DEFAULT_CONFIG);
}

int main(int argc, char **argv)
{
    const char *config_path = DEFAULT_CONFIG;
    const char *door = NULL, *capture = NULL, *playback = NULL;
    const char *duplex = NULL, *aec = NULL, *log_level = NULL, *video_dump = NULL;
    int port = 0, listen_port = 0, mic_gain = 0, spk_gain = 0;
    bool have_mic_gain = false, have_spk_gain = false;
    bool dial_on_start = false, interactive = true;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const bool has_next = (i + 1 < argc);

        if      ((!strcmp(a, "-c") || !strcmp(a, "--config")) && has_next) config_path = argv[++i];
        else if (!strcmp(a, "--door")       && has_next) door       = argv[++i];
        else if (!strcmp(a, "--port")       && has_next) port       = atoi(argv[++i]);
        else if (!strcmp(a, "--listen-port") && has_next) listen_port = atoi(argv[++i]);
        else if (!strcmp(a, "--capture")    && has_next) capture    = argv[++i];
        else if (!strcmp(a, "--playback")   && has_next) playback   = argv[++i];
        else if (!strcmp(a, "--duplex")     && has_next) duplex     = argv[++i];
        else if (!strcmp(a, "--aec")        && has_next) aec        = argv[++i];
        else if (!strcmp(a, "--video-dump") && has_next) video_dump = argv[++i];
        else if (!strcmp(a, "--mic-gain")   && has_next) { mic_gain = atoi(argv[++i]); have_mic_gain = true; }
        else if (!strcmp(a, "--spk-gain")   && has_next) { spk_gain = atoi(argv[++i]); have_spk_gain = true; }
        else if ((!strcmp(a, "-l") || !strcmp(a, "--log")) && has_next) log_level = argv[++i];
        else if (!strcmp(a, "--call"))  dial_on_start = true;
        else if (!strcmp(a, "--batch")) interactive = false;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else {
            fprintf(stderr, "unknown argument: %s\n\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    syncn_config cfg;
    syncn_config_defaults(&cfg);
    if (!syncn_config_load(&cfg, config_path))
        LOG_WARN("config: %s had errors; defaults were used where it failed", config_path);

    /* Command line wins over the file, so a technician can try a setting
     * without editing config and forgetting to put it back. */
    if (door)       snprintf(cfg.door_address,    sizeof cfg.door_address,    "%s", door);
    if (capture)    snprintf(cfg.capture_device,  sizeof cfg.capture_device,  "%s", capture);
    if (playback)   snprintf(cfg.playback_device, sizeof cfg.playback_device, "%s", playback);
    if (duplex)     snprintf(cfg.duplex_mode,     sizeof cfg.duplex_mode,     "%s", duplex);
    if (aec)        snprintf(cfg.aec,             sizeof cfg.aec,             "%s", aec);
    if (video_dump) snprintf(cfg.video_dump_path, sizeof cfg.video_dump_path, "%s", video_dump);
    if (log_level)  snprintf(cfg.log_level,       sizeof cfg.log_level,       "%s", log_level);
    if (port)        { cfg.door_port = port; if (!listen_port) cfg.listen_port = port; }
    if (listen_port)   cfg.listen_port = listen_port;
    if (have_mic_gain) cfg.mic_gain_db = mic_gain;
    if (have_spk_gain) cfg.speaker_gain_db = spk_gain;

    syncn_log_set_level(syncn_log_level_from_name(cfg.log_level));

    LOG_INFO("SyncN intercom starting");
    syncn_config_log(&cfg);

    g_call = syncn_call_create(&cfg);
    if (!g_call) {
        LOG_ERR("failed to start — see the errors above");
        return 1;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (dial_on_start && !syncn_call_dial(g_call))
        LOG_WARN("could not dial the door on startup; waiting for it to call us");

    syncn_call_run(g_call, interactive);

    syncn_call_destroy(g_call);
    g_call = NULL;
    LOG_INFO("SyncN intercom stopped");
    return 0;
}
