#include "test_runner.h"
#include "wce.h"
#include <limits.h>
#include <string.h>

/* wce_quantize */

static void test_q_lossy_zero_no_op(void) {
    int32_t a[5] = {0, 7, -3, 1000, -16000};
    int32_t b[5];
    memcpy(b, a, sizeof(a));
    wce_quantize(a, 5, 0);
    CHECK_EQ_BYTES(a, b, sizeof(a));
}

static void test_q_lossy_32_zeros_all(void) {
    int32_t a[3] = {7, -3, INT32_MIN};
    wce_quantize(a, 3, 32);
    CHECK_EQ(a[0], 0); CHECK_EQ(a[1], 0); CHECK_EQ(a[2], 0);
}

static void test_q_lossy_31_keeps_only_int32_min(void) {
    int32_t a[5] = {0, 1, -1, INT32_MIN, INT32_MAX};
    wce_quantize(a, 5, 31);
    CHECK_EQ(a[0], 0);
    CHECK_EQ(a[1], 0);
    CHECK_EQ(a[2], 0);
    CHECK_EQ(a[3], INT32_MIN);
    CHECK_EQ(a[4], 0);
}

static void test_q_truncates_positive(void) {
    int32_t a[6] = {0, 1, 2, 3, 4, 7};
    wce_quantize(a, 6, 2);  /* step = 4, mask = ~3 */
    CHECK_EQ(a[0], 0); CHECK_EQ(a[1], 0); CHECK_EQ(a[2], 0);
    CHECK_EQ(a[3], 0); CHECK_EQ(a[4], 4); CHECK_EQ(a[5], 4);
}

static void test_q_truncates_negative_symmetrically(void) {
    int32_t a[6] = {-1, -2, -3, -4, -7, -8};
    wce_quantize(a, 6, 2);  /* truncate toward zero on magnitude */
    CHECK_EQ(a[0],  0); CHECK_EQ(a[1],  0); CHECK_EQ(a[2],  0);
    CHECK_EQ(a[3], -4); CHECK_EQ(a[4], -4); CHECK_EQ(a[5], -8);
}

static void test_q_int32_min_no_ub(void) {
    /* abs(INT32_MIN) overflows in signed math; impl must use unsigned. */
    int32_t a[1] = {INT32_MIN};
    wce_quantize(a, 1, 3);  /* INT32_MIN = 0x80000000, mask = ~7 → 0x80000000 */
    CHECK_EQ(a[0], INT32_MIN);
}

static void test_q_zero_stays_zero(void) {
    int32_t a[1] = {0};
    wce_quantize(a, 1, 5);
    CHECK_EQ(a[0], 0);
}

/* Quantize round-trip */

static void test_quantize_lands_on_grid(void) {
    int32_t a[8] = {0, 1, 7, 8, 9, 15, 16, -17};
    wce_quantize(a, 8, 3);
    int i;
    for (i = 0; i < 8; ++i) CHECK_EQ((a[i] & 7), 0);
}

/* Lloyd-Max optimal reconstruction */

static void test_est_scale_empty(void) {
    CHECK(wce_estimate_laplacian_scale(NULL, 0) == 0.0);
    int32_t empty[1] = {0};
    CHECK(wce_estimate_laplacian_scale(empty, 0) == 0.0);
}

static void test_est_scale_mean_abs(void) {
    int32_t a[5] = {10, -10, 20, -20, 0};
    /* mean(|.|) = (10+10+20+20+0)/5 = 12 */
    double b = wce_estimate_laplacian_scale(a, 5);
    CHECK_NEAR(b, 12.0, 1e-9);
}

static void test_dq_optimal_zero_scale_noop(void) {
    int32_t a[3] = {8, -8, 16};
    int32_t b[3]; memcpy(b, a, sizeof(a));
    wce_dequantize_optimal(a, 3, 3, 0.0);
    CHECK_EQ_BYTES(a, b, sizeof(a));
}

static void test_dq_optimal_lossy_zero_noop(void) {
    int32_t a[3] = {5, -5, 7};
    int32_t b[3]; memcpy(b, a, sizeof(a));
    wce_dequantize_optimal(a, 3, 0, 10.0);
    CHECK_EQ_BYTES(a, b, sizeof(a));
}

static void test_dq_optimal_zero_stays_zero(void) {
    int32_t a[4] = {0, 0, 0, 0};
    wce_dequantize_optimal(a, 4, 3, 10.0);
    CHECK_EQ(a[0], 0); CHECK_EQ(a[1], 0);
    CHECK_EQ(a[2], 0); CHECK_EQ(a[3], 0);
}

static void test_dq_optimal_fine_quant_approaches_midpoint(void) {
    /* When step << scale_b, offset → step/2 (the midpoint limit).
     * lossy=3 step=8, scale_b=10000 → offset ≈ 4. */
    int32_t a[2] = {8, -8};
    wce_dequantize_optimal(a, 2, 3, 10000.0);
    CHECK_EQ(a[0],  12);
    CHECK_EQ(a[1], -12);
}

static void test_dq_optimal_coarse_quant_approaches_zero(void) {
    /* When step >> scale_b, offset → scale_b (small).
     * lossy=8 step=256, scale_b=2 → exp(128) huge, offset ≈ scale_b ≈ 2.
     * Magnitude 256 + 2 = 258. */
    int32_t a[1] = {256};
    wce_dequantize_optimal(a, 1, 8, 2.0);
    CHECK(a[0] >= 257 && a[0] <= 259);
}

static void test_dq_optimal_saturates_int32_max(void) {
    int32_t a[1] = {INT32_MAX - 2};
    wce_dequantize_optimal(a, 1, 3, 10000.0);  /* offset ≈ 4 */
    CHECK_EQ(a[0], INT32_MAX);
}

static void test_dq_optimal_saturates_int32_min(void) {
    int32_t a[1] = {INT32_MIN};
    wce_dequantize_optimal(a, 1, 3, 10000.0);
    CHECK_EQ(a[0], INT32_MIN);
}

static void test_dq_optimal_lossy_31(void) {
    /* lossy_bits=31 → step=2^31. scale_b small → exp(u) huge → offset ≈ scale_b.
     * coeff after dequant is on the 2^31 grid with a small offset added. */
    int32_t a[2] = {0, INT32_MIN};
    wce_dequantize_optimal(a, 2, 31, 100.0);
    CHECK_EQ(a[0], 0);           /* zero stays zero */
    CHECK_EQ(a[1], INT32_MIN);   /* negative saturates */
}

static void test_dq_optimal_nan_scale_noop(void) {
    /* NaN scale_b must be a no-op, not UB. */
    int32_t a[2] = {8, -8};
    int32_t orig[2]; memcpy(orig, a, sizeof(a));
    wce_dequantize_optimal(a, 2, 3, 0.0 / 0.0);
    CHECK_EQ_BYTES(a, orig, sizeof(a));
}

static void test_dq_optimal_inf_scale_noop(void) {
    /* +Inf scale_b must be a no-op. */
    int32_t a[2] = {8, -8};
    int32_t orig[2]; memcpy(orig, a, sizeof(a));
    wce_dequantize_optimal(a, 2, 3, 1.0 / 0.0);
    CHECK_EQ_BYTES(a, orig, sizeof(a));
}

void run_quantize_tests(void) {
    printf("quantize:\n");
    test_q_lossy_zero_no_op();
    test_q_lossy_32_zeros_all();
    test_q_lossy_31_keeps_only_int32_min();
    test_q_truncates_positive();
    test_q_truncates_negative_symmetrically();
    test_q_int32_min_no_ub();
    test_q_zero_stays_zero();
    test_quantize_lands_on_grid();
    test_est_scale_empty();
    test_est_scale_mean_abs();
    test_dq_optimal_zero_scale_noop();
    test_dq_optimal_lossy_zero_noop();
    test_dq_optimal_zero_stays_zero();
    test_dq_optimal_fine_quant_approaches_midpoint();
    test_dq_optimal_coarse_quant_approaches_zero();
    test_dq_optimal_saturates_int32_max();
    test_dq_optimal_saturates_int32_min();
    test_dq_optimal_lossy_31();
    test_dq_optimal_nan_scale_noop();
    test_dq_optimal_inf_scale_noop();
}
