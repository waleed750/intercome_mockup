#include "syncn/config.h"
#include "syncn/discovery.h"
#include "syncn/util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool parse_bool(const char *v)
{
    return v && (strcasecmp(v, "on") == 0 || strcasecmp(v, "true") == 0 ||
                 strcasecmp(v, "yes") == 0 || strcmp(v, "1") == 0);
}

static void copy(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

void syncn_config_defaults(syncn_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);

    cfg->door_port   = SYNCN_CALL_PORT;
    cfg->listen_port = SYNCN_CALL_PORT;
    copy(cfg->appid,    sizeof cfg->appid,    SYNCN_DEFAULT_APPID);
    copy(cfg->group_ip, sizeof cfg->group_ip, SYNCN_DEFAULT_GROUP_IP);

    copy(cfg->capture_device,  sizeof cfg->capture_device,  "default");
    copy(cfg->playback_device, sizeof cfg->playback_device, "default");
    copy(cfg->duplex_mode,     sizeof cfg->duplex_mode,     "full");
    copy(cfg->aec,             sizeof cfg->aec,             "speex");
    copy(cfg->capture_channel, sizeof cfg->capture_channel, "auto");
    cfg->capture_channels = 1;
    cfg->highpass_hz = 250;
    cfg->lowpass_hz  = 3400;
    cfg->downlink_highpass_hz = 250;
    cfg->presence_hz = 2000;
    cfg->presence_db = 0.0f;
    /* ALSA's mono downmix averages the live microphone channel with a dead one,
     * which halves the level and costs nothing in signal-to-noise. This is the
     * 6 dB back. */
    cfg->mic_gain_db = 6;
    cfg->aec_tail_ms      = 200;
    cfg->aec_delay_ms     = 80;
    cfg->jitter_target_ms = 60;
    cfg->gate_threshold_dbfs = -30.0f;
    cfg->gate_hangover_ms    = 150.0f;
    cfg->uplink_ceiling_dbfs = -1.0f;
    cfg->uplink_release_ms   = 400.0f;
    cfg->uplink_noise_floor_dbfs   = -34.0f;
    cfg->downlink_noise_floor_dbfs = -37.0f;
    cfg->uplink_noise_hangover_ms   = 150.0f;
    cfg->downlink_noise_hangover_ms = 100.0f;
    cfg->noise_release_ms   = 150.0f;
    cfg->comfort_noise_dbfs = -55.0f;
    cfg->near_priority_db   = 0.0f;
    copy(cfg->denoise, sizeof cfg->denoise, "speex");
    copy(cfg->denoise_downlink, sizeof cfg->denoise_downlink, "speex");
    cfg->denoise_wet        = 100;
    cfg->aec_denoise        = true;
    cfg->aec_noise_suppress_db = -18;
    cfg->return_cancel      = true;
    cfg->return_tail_ms     = 600;
    cfg->return_noise_suppress_db = -15;
    cfg->return_echo_db     = 3.0f;
    cfg->hold_floor_dbfs    = -1.0f;
    cfg->hold_loud_dbfs     = -18.0f;
    cfg->ring         = true;
    cfg->ring_gain_db = 0.0f;

    copy(cfg->video_present,   sizeof cfg->video_present,   "file");
    copy(cfg->video_dump_path, sizeof cfg->video_dump_path, "/var/lib/syncn/door.h264");
    cfg->video_max_inflight = 4;
    cfg->video_idle_preview = false;

    copy(cfg->log_level, sizeof cfg->log_level, "info");
}

/* Trim in place and return the start. */
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        e--;
    *e = '\0';
    return s;
}

bool syncn_config_load(syncn_config *cfg, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_INFO("config: %s not found — using defaults", path);
        return true;
    }

    char line[512], section[64] = "";
    int lineno = 0;
    bool ok = true;

    while (fgets(line, sizeof line, f)) {
        lineno++;

        /* Strip comments. Both ; and # are accepted; a trailing comment after a
         * value is common in hand-edited config, so handle it. */
        for (char *p = line; *p; p++) {
            if (*p == ';' || *p == '#') {
                *p = '\0';
                break;
            }
        }

        char *s = trim(line);
        if (!*s)
            continue;

        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close) {
                LOG_ERR("config: %s:%d: unterminated section header", path, lineno);
                ok = false;
                continue;
            }
            *close = '\0';
            snprintf(section, sizeof section, "%s", trim(s + 1));
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            LOG_ERR("config: %s:%d: expected key = value", path, lineno);
            ok = false;
            continue;
        }
        *eq = '\0';
        const char *key = trim(s);
        const char *val = trim(eq + 1);

#define MATCH(sec, k) (strcmp(section, sec) == 0 && strcmp(key, k) == 0)

        if      (MATCH("door",  "address"))          copy(cfg->door_address, sizeof cfg->door_address, val);
        else if (MATCH("door",  "port"))             cfg->door_port = atoi(val);
        else if (MATCH("door",  "listen_port"))      cfg->listen_port = atoi(val);
        else if (MATCH("panel", "alias"))            copy(cfg->alias,    sizeof cfg->alias,    val);
        else if (MATCH("panel", "serial"))           copy(cfg->serial,   sizeof cfg->serial,   val);
        else if (MATCH("panel", "appid"))            copy(cfg->appid,    sizeof cfg->appid,    val);
        else if (MATCH("panel", "group_ip"))         copy(cfg->group_ip, sizeof cfg->group_ip, val);
        else if (MATCH("audio", "capture_device"))   copy(cfg->capture_device,  sizeof cfg->capture_device,  val);
        else if (MATCH("audio", "playback_device"))  copy(cfg->playback_device, sizeof cfg->playback_device, val);
        else if (MATCH("audio", "duplex_mode"))      copy(cfg->duplex_mode, sizeof cfg->duplex_mode, val);
        else if (MATCH("audio", "aec"))              copy(cfg->aec,         sizeof cfg->aec,         val);
        else if (MATCH("audio", "aec_tail_ms"))      cfg->aec_tail_ms      = atoi(val);
        else if (MATCH("audio", "aec_delay_ms"))     cfg->aec_delay_ms     = atoi(val);
        else if (MATCH("audio", "capture_channel"))  copy(cfg->capture_channel, sizeof cfg->capture_channel, val);
        else if (MATCH("audio", "capture_channels")) cfg->capture_channels = atoi(val);
        else if (MATCH("audio", "highpass_hz"))      cfg->highpass_hz      = atoi(val);
        else if (MATCH("audio", "jitter_target_ms")) cfg->jitter_target_ms = atoi(val);
        else if (MATCH("audio", "gate_threshold_dbfs")) cfg->gate_threshold_dbfs = (float)atof(val);
        else if (MATCH("audio", "gate_hangover_ms"))    cfg->gate_hangover_ms    = (float)atof(val);
        else if (MATCH("audio", "uplink_ceiling_dbfs")) cfg->uplink_ceiling_dbfs = (float)atof(val);
        else if (MATCH("audio", "uplink_release_ms"))   cfg->uplink_release_ms   = (float)atof(val);
        else if (MATCH("audio", "uplink_noise_floor_dbfs"))   cfg->uplink_noise_floor_dbfs   = (float)atof(val);
        else if (MATCH("audio", "downlink_noise_floor_dbfs")) cfg->downlink_noise_floor_dbfs = (float)atof(val);
        else if (MATCH("audio", "uplink_noise_hangover_ms"))   cfg->uplink_noise_hangover_ms   = (float)atof(val);
        else if (MATCH("audio", "downlink_noise_hangover_ms")) cfg->downlink_noise_hangover_ms = (float)atof(val);
        else if (MATCH("audio", "noise_release_ms"))   cfg->noise_release_ms   = (float)atof(val);
        else if (MATCH("audio", "comfort_noise_dbfs")) cfg->comfort_noise_dbfs = (float)atof(val);
        else if (MATCH("audio", "near_priority_db"))   cfg->near_priority_db   = (float)atof(val);
        else if (MATCH("audio", "denoise"))            copy(cfg->denoise, sizeof cfg->denoise, val);
        else if (MATCH("audio", "denoise_downlink"))   copy(cfg->denoise_downlink, sizeof cfg->denoise_downlink, val);
        else if (MATCH("audio", "denoise_wet"))        cfg->denoise_wet        = atoi(val);
        else if (MATCH("audio", "aec_denoise"))        cfg->aec_denoise        = parse_bool(val);
        else if (MATCH("audio", "aec_noise_suppress_db")) cfg->aec_noise_suppress_db = atoi(val);
        else if (MATCH("audio", "return_cancel"))      cfg->return_cancel      = parse_bool(val);
        else if (MATCH("audio", "return_tail_ms"))     cfg->return_tail_ms     = atoi(val);
        else if (MATCH("audio", "return_noise_suppress_db")) cfg->return_noise_suppress_db = atoi(val);
        else if (MATCH("audio", "return_echo_db"))     cfg->return_echo_db     = (float)atof(val);
        else if (MATCH("audio", "hold_floor_dbfs"))    cfg->hold_floor_dbfs    = (float)atof(val);
        else if (MATCH("audio", "hold_loud_dbfs"))     cfg->hold_loud_dbfs     = (float)atof(val);
        else if (MATCH("audio", "lowpass_hz"))         cfg->lowpass_hz         = atoi(val);
        else if (MATCH("audio", "downlink_highpass_hz")) cfg->downlink_highpass_hz = atoi(val);
        else if (MATCH("audio", "presence_hz"))        cfg->presence_hz        = atoi(val);
        else if (MATCH("audio", "presence_db"))        cfg->presence_db        = (float)atof(val);
        else if (MATCH("audio", "ring"))               cfg->ring         = parse_bool(val);
        else if (MATCH("audio", "ring_gain_db"))       cfg->ring_gain_db = (float)atof(val);
        else if (MATCH("audio", "mic_gain_db"))      cfg->mic_gain_db      = atoi(val);
        else if (MATCH("audio", "speaker_gain_db"))  cfg->speaker_gain_db  = atoi(val);
        else if (MATCH("video", "present"))          copy(cfg->video_present,   sizeof cfg->video_present,   val);
        else if (MATCH("video", "dump_path"))        copy(cfg->video_dump_path, sizeof cfg->video_dump_path, val);
        else if (MATCH("video", "max_inflight"))     cfg->video_max_inflight = atoi(val);
        else if (MATCH("video", "idle_preview"))     cfg->video_idle_preview = parse_bool(val);
        else if (MATCH("log",   "level"))            copy(cfg->log_level, sizeof cfg->log_level, val);
        else
            LOG_WARN("config: %s:%d: ignoring unknown key [%s] %s", path, lineno, section, key);

#undef MATCH
    }

    fclose(f);
    LOG_INFO("config: loaded %s", path);
    return ok;
}

void syncn_config_log(const syncn_config *cfg)
{
    LOG_INFO("config: door=%s:%d  alias=%s  serial=%s",
             cfg->door_address[0] ? cfg->door_address : "(learn from discovery)",
             cfg->door_port,
             cfg->alias[0] ? cfg->alias : "(hostname)",
             cfg->serial[0] ? cfg->serial : "(from MAC)");
    LOG_INFO("config: audio capture=%s playback=%s duplex=%s aec=%s tail=%dms jitter=%dms",
             cfg->capture_device, cfg->playback_device, cfg->duplex_mode,
             cfg->aec, cfg->aec_tail_ms, cfg->jitter_target_ms);
    LOG_INFO("config: video present=%s dump=%s idle_preview=%s",
             cfg->video_present, cfg->video_dump_path,
             cfg->video_idle_preview ? "on" : "off");
}
