/*
 * A separate binary from the main unit tests, because it links speexdsp,
 * which the protocol library does not need and a machine without it must
 * still build the rest.
 */
#include "test.h"

int syncn_tests_run = 0;
int syncn_tests_failed = 0;
const char *syncn_current_suite = "";

void suite_return(void);
void suite_denoise(void);

int main(void)
{
    printf("SyncN echo canceller tests\n");

    suite_return();
    suite_denoise();

    printf("\n%d checks, %d failed\n", syncn_tests_run, syncn_tests_failed);
    return syncn_tests_failed == 0 ? 0 : 1;
}
