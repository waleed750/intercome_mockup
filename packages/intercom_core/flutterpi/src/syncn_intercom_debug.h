// SPDX-License-Identifier: MIT
//
// File-based diagnostic logging for the syncn_intercom plugins, independent
// of journald/LOG_ERROR. Added because journald's capture of this process's
// stdout/stderr was found to be unreliable on-device (a journald restart
// after the process had already started orphans its log pipe -- the process
// keeps running fine, but nothing it writes to stdout/stderr ever reaches
// the journal again). Writing straight to a file sidesteps that entirely:
// `cat /tmp/syncn_intercom_debug.log` on the panel always reflects what this
// process actually did, regardless of journald's state.
#ifndef _SYNCN_INTERCOM_DEBUG_H
#define _SYNCN_INTERCOM_DEBUG_H

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#define SYNCN_INTERCOM_DEBUG_LOG_PATH "/tmp/syncn_intercom_debug.log"

static inline void syncn_intercom_debug_log(const char *tag, const char *fmt, ...) {
    FILE *f = fopen(SYNCN_INTERCOM_DEBUG_LOG_PATH, "a");
    if (f == NULL) {
        return;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);
    char timestr[16];
    strftime(timestr, sizeof(timestr), "%H:%M:%S", &tm_info);

    fprintf(f, "[%s.%03ld] %s: ", timestr, ts.tv_nsec / 1000000, tag);

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    fprintf(f, "\n");
    fclose(f);
}

#endif  // _SYNCN_INTERCOM_DEBUG_H
