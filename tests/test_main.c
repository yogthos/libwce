#include "test_runner.h"

int tests_run    = 0;
int tests_failed = 0;

int main(void) {
    extern void run_quantize_tests(void);
    extern void run_wce_bitio_tests(void);
    extern void run_wce_bpc_tests(void);
    extern void run_wce_coeffs_tests(void);
    extern void run_wce_codec_tests(void);

    printf("libwce tests\n");
    printf("============\n");

    run_quantize_tests();
    run_wce_bitio_tests();
    run_wce_bpc_tests();
    run_wce_coeffs_tests();
    run_wce_codec_tests();

    printf("--------------------\n");
    printf("ran %d, failed %d\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
