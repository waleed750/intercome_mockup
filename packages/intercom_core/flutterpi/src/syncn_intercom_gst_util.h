// SPDX-License-Identifier: MIT
#ifndef _SYNCN_INTERCOM_GST_UTIL_H
#define _SYNCN_INTERCOM_GST_UTIL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdbool.h>
#include <time.h>

#include <gst/gst.h>
#include <pthread.h>

#include "syncn_intercom_debug.h"

struct syncn_gst_teardown_job {
    const char *tag;
    GstElement *pipeline;
};

static inline void *syncn_gst_teardown_worker(void *userdata) {
    struct syncn_gst_teardown_job *job = userdata;

    syncn_intercom_debug_log(job->tag, "bounded_teardown: setting pipeline to NULL");
    gst_element_set_state(job->pipeline, GST_STATE_NULL);
    gst_object_unref(job->pipeline);
    syncn_intercom_debug_log(job->tag, "bounded_teardown: settled and unref'd pipeline");

    g_free(job);
    return NULL;
}

static inline bool syncn_gst_bounded_teardown(const char *tag, GstElement *pipeline, int timeout_sec) {
    if (pipeline == NULL) {
        return true;
    }

    struct syncn_gst_teardown_job *job = g_new0(struct syncn_gst_teardown_job, 1);
    job->tag = tag;
    job->pipeline = pipeline;

    pthread_t thread;
    int create_ret = pthread_create(&thread, NULL, syncn_gst_teardown_worker, job);
    if (create_ret != 0) {
        syncn_intercom_debug_log(tag, "bounded_teardown: pthread_create failed (%d); falling back to synchronous teardown", create_ret);
        g_free(job);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return true;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_sec;

    int join_ret = pthread_timedjoin_np(thread, NULL, &deadline);
    if (join_ret == 0) {
        return true;
    }

    if (join_ret == ETIMEDOUT) {
        syncn_intercom_debug_log(tag, "bounded_teardown: TIMED OUT after %ds; ABANDONING pipeline", timeout_sec);
    } else {
        syncn_intercom_debug_log(tag, "bounded_teardown: pthread_timedjoin_np failed (%d); ABANDONING pipeline", join_ret);
    }
    pthread_detach(thread);
    return false;
}

#endif  // _SYNCN_INTERCOM_GST_UTIL_H
