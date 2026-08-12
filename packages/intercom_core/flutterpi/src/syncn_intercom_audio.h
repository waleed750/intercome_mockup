// SPDX-License-Identifier: MIT
#ifndef _SYNCN_INTERCOM_AUDIO_H
#define _SYNCN_INTERCOM_AUDIO_H

#include "pluginregistry.h"

#define SYNCN_INTERCOM_AUDIO_METHOD_CHANNEL "syncn_intercom/audio"
#define SYNCN_INTERCOM_AUDIO_EVENT_CHANNEL "syncn_intercom/audio_uplink"

enum plugin_init_result syncn_intercom_audio_init(struct flutterpi *flutterpi, void **userdata_out);
void syncn_intercom_audio_deinit(struct flutterpi *flutterpi, void *userdata);

#endif  // _SYNCN_INTERCOM_AUDIO_H
