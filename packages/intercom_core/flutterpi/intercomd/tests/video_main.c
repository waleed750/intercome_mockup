/*
 * A separate binary from the main unit tests, because it links fake Rockchip
 * hardware and a fake DMA heap in place of the real ones. Those substitutions
 * are link-time, so the pipeline under test is the shipping source with no
 * conditional compilation in it and no test-only branch to get wrong.
 */
#include "test.h"

int syncn_tests_run = 0;
int syncn_tests_failed = 0;
const char *syncn_current_suite = "";

int main(void)
{
    printf("SyncN video pipeline tests\n");

    suite_video();

    printf("\n%d checks, %d failed\n", syncn_tests_run, syncn_tests_failed);
    return syncn_tests_failed == 0 ? 0 : 1;
}
