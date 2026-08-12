// SPDX-License-Identifier: MIT
#ifndef _SYNCN_INTERCOM_VIDEO_H
#define _SYNCN_INTERCOM_VIDEO_H

#include "pluginregistry.h"

#define SYNCN_INTERCOM_VIDEO_CHANNEL "syncn_intercom/video"

enum plugin_init_result syncn_intercom_video_init(struct flutterpi *flutterpi, void **userdata_out);
void syncn_intercom_video_deinit(struct flutterpi *flutterpi, void *userdata);

#endif  // _SYNCN_INTERCOM_VIDEO_H
