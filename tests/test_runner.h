/*
 * Minimal test runner for libwce.
 * Each test module defines a void run_<phase>_tests(void) function.
 */

#ifndef WCE_TEST_RUNNER_H
#define WCE_TEST_RUNNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int tests_run;
extern int tests_failed;

static inline void check_(bool cond, const char *expr,
                          const char *file, int line) {
    tests_run++;
    if (!cond) {
        tests_failed++;
        fprintf(stderr, "  FAIL %s:%d  %s\n", file, line, expr);
    }
}

static inline void check_eq_u64_(unsigned long long got,
                                 unsigned long long want,
                                 const char *expr,
                                 const char *file, int line) {
    tests_run++;
    if (got != want) {
        tests_failed++;
        fprintf(stderr,
                "  FAIL %s:%d  %s\n    got  = %llu (0x%llx)\n"
                "    want = %llu (0x%llx)\n",
                file, line, expr, got, got, want, want);
    }
}

static inline void check_eq_bytes_(const void *got, const void *want,
                                   size_t n, const char *expr,
                                   const char *file, int line) {
    tests_run++;
    if (memcmp(got, want, n) != 0) {
        tests_failed++;
        fprintf(stderr, "  FAIL %s:%d  %s (%zu bytes differ)\n",
                file, line, expr, n);
    }
}

#define CHECK(expr) \
    check_((expr), #expr, __FILE__, __LINE__)

#define CHECK_EQ(got, want) \
    check_eq_u64_((unsigned long long)(got), \
                  (unsigned long long)(want), \
                  #got " == " #want, __FILE__, __LINE__)

#define CHECK_EQ_BYTES(got, want, n) \
    check_eq_bytes_((got), (want), (n), \
                    #got " ~ " #want, __FILE__, __LINE__)

static inline void check_near_(double got, double want, double eps,
                               const char *expr, const char *file, int line) {
    tests_run++;
    const double diff = (got > want) ? got - want : want - got;
    if (diff > eps) {
        tests_failed++;
        fprintf(stderr, "  FAIL %s:%d  %s\n    got  = %.15g\n"
                "    want = %.15g  (eps = %.15g)\n",
                file, line, expr, got, want, eps);
    }
}

#define CHECK_NEAR(got, want, eps) \
    check_near_((got), (want), (eps), \
                #got " ≈ " #want, __FILE__, __LINE__)

#endif /* WCE_TEST_RUNNER_H */
