/* Minimal test harness. No dependencies — this has to build anywhere the
 * daemon builds, including on the panel itself. */
#ifndef SYNCN_TEST_H
#define SYNCN_TEST_H

#include <stdio.h>
#include <string.h>

extern int syncn_tests_run;
extern int syncn_tests_failed;
extern const char *syncn_current_suite;

#define SUITE(name)                                                            \
    do {                                                                       \
        syncn_current_suite = name;                                            \
        printf("\n  %s\n", name);                                              \
    } while (0)

#define CHECK(cond)                                                            \
    do {                                                                       \
        syncn_tests_run++;                                                     \
        if (!(cond)) {                                                         \
            syncn_tests_failed++;                                              \
            printf("    FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                      \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                         \
    do {                                                                       \
        syncn_tests_run++;                                                     \
        const long long a_ = (long long)(actual);                              \
        const long long e_ = (long long)(expected);                            \
        if (a_ != e_) {                                                        \
            syncn_tests_failed++;                                              \
            printf("    FAIL  %s:%d  %s: got %lld, expected %lld\n",           \
                   __FILE__, __LINE__, #actual, a_, e_);                       \
        }                                                                      \
    } while (0)

#define CHECK_EQ_MEM(actual, expected, len)                                    \
    do {                                                                       \
        syncn_tests_run++;                                                     \
        if (memcmp((actual), (expected), (len)) != 0) {                        \
            syncn_tests_failed++;                                              \
            printf("    FAIL  %s:%d  %s differs from %s over %zu bytes\n",     \
                   __FILE__, __LINE__, #actual, #expected, (size_t)(len));     \
        }                                                                      \
    } while (0)

void suite_frame(void);
void suite_alaw(void);
void suite_command(void);
void suite_discovery(void);
void suite_jitter(void);
void suite_h264(void);
void suite_dsp(void);
void suite_fdpass(void);
void suite_identity(void);
void suite_framepool(void);
void suite_limiter(void);
void suite_limiter_gain(void);
void suite_gate(void);
void suite_noise_gate(void);
void suite_gate_release(void);
void suite_gate_active(void);
void suite_video_proto(void);
void suite_ringer(void);
void suite_video(void);   /* built into its own binary, see video_main.c */

#endif /* SYNCN_TEST_H */
