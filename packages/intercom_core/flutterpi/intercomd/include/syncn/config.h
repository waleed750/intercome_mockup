/* Configuration: /etc/syncn/intercom.conf, INI style.
 * Every value has a working default; nothing site-specific is compiled in. */
#ifndef SYNCN_CONFIG_H
#define SYNCN_CONFIG_H

#include <stdbool.h>
#include "syncn/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYNCN_STR_MAX 128

typedef struct {
    /* [door] — blank address means "learn it from the discovery broadcast" */
    char door_address[SYNCN_STR_MAX];
    int  door_port;
    /* Port we listen on. Same as door_port in the field; separable so both ends
     * can be run on one machine for testing. */
    int  listen_port;

    /* [panel] — blank alias/serial are filled in at startup */
    char alias[SYNCN_STR_MAX];
    char serial[SYNCN_STR_MAX];
    char appid[SYNCN_STR_MAX];
    char group_ip[SYNCN_STR_MAX];

    /* [audio] */
    char capture_device[SYNCN_STR_MAX];
    char playback_device[SYNCN_STR_MAX];
    char duplex_mode[32];
    char aec[32];
    int  aec_tail_ms;
    int  aec_delay_ms;
    int  jitter_target_ms;

    /* Half-duplex gate: mute the uplink while the door's level, after
     * speaker_gain_db, is above this. 0 means the built-in -34. */
    float gate_threshold_dbfs;
    float gate_hangover_ms;
    float uplink_ceiling_dbfs;   /* 0 = no limiter */
    float uplink_release_ms;     /* 0 = 400 */
    float uplink_noise_floor_dbfs;    /* 0 = off */
    float downlink_noise_floor_dbfs;  /* 0 = off */
    float uplink_noise_hangover_ms;
    float downlink_noise_hangover_ms;
    float noise_release_ms;
    float comfort_noise_dbfs;
    /* The panel keeps the floor while this many dB louder than the door.
     * 0 = off. */
    float near_priority_db;
    /* Speex denoiser on the microphone, and how much it may take. */
    bool  aec_denoise;
    int   aec_noise_suppress_db;
    /* Which denoiser both ways: rnnoise (shipped), speex, off; and how much
     * of rnnoise's output replaces the input, in percent. */
    char  denoise[16];
    char  denoise_downlink[16];
    int   denoise_wet;
    /* Cancel the door's echo of our own uplink from its stream. */
    bool  return_cancel;
    int   return_tail_ms;
    int   return_noise_suppress_db;
    float return_echo_db;
    float hold_floor_dbfs;
    float hold_loud_dbfs;
    /* The telephony band: low-pass both ways, high-pass on the door's stream
     * (the microphone's is highpass_hz below). 0 disables. */
    int   lowpass_hz;
    int   downlink_highpass_hz;
    /* A high shelf both ways, for crispness. 0 dB disables. */
    int   presence_hz;
    float presence_db;

    /* The ring on an incoming call: on by default, level trim in dB. */
    bool  ring;
    float ring_gain_db;
    int  capture_channels;
    char capture_channel[32];
    int  highpass_hz;
    int  mic_gain_db;
    int  speaker_gain_db;

    /* [video] */
    char video_present[32];   /* overlay | texture | file */
    char video_dump_path[SYNCN_STR_MAX];
    int  video_max_inflight;

    /* Keep the door's picture on screen whenever the panel is idle: open a
     * preview by itself, reopen it whenever the door drops it, and give it up
     * the instant a real call arrives. */
    bool video_idle_preview;

    /* [log] */
    char log_level[32];
} syncn_config;

void syncn_config_defaults(syncn_config *cfg);

/* Missing file is not an error — defaults stand. Returns false only on a
 * malformed file, and says which line. */
bool syncn_config_load(syncn_config *cfg, const char *path);

void syncn_config_log(const syncn_config *cfg);

#ifdef __cplusplus
}
#endif
#endif /* SYNCN_CONFIG_H */
