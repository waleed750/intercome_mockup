#include "test.h"

int syncn_tests_run = 0;
int syncn_tests_failed = 0;
const char *syncn_current_suite = "";

int main(void)
{
    printf("SyncN intercom protocol tests\n");

    suite_frame();
    suite_alaw();
    suite_command();
    suite_discovery();
    suite_jitter();
    suite_h264();
    suite_dsp();
    suite_fdpass();
    suite_identity();
    suite_framepool();
    suite_limiter();
    suite_limiter_gain();
    suite_gate();
    suite_noise_gate();
    suite_gate_release();
    suite_gate_active();
    suite_video_proto();
    suite_ringer();

    printf("\n%d checks, %d failed\n", syncn_tests_run, syncn_tests_failed);
    return syncn_tests_failed == 0 ? 0 : 1;
}
