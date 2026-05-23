#include "test_runner.h"
#include "wce.h"
#include <limits.h>
#include <string.h>

/* wce_compute_bpcs */

static void test_bpc_all_zero(void) {
    int32_t c[8] = {0};
    uint8_t bpc[2] = {99, 99};
    wce_compute_bpcs(c, 2, bpc, 0);
    CHECK_EQ(bpc[0], 0);
    CHECK_EQ(bpc[1], 0);
}

static void test_bpc_lossy_floor(void) {
    /* All-zero coeffs, lossy_bits=3 → BPC clamped to 3. */
    int32_t c[4] = {0};
    uint8_t bpc[1] = {99};
    wce_compute_bpcs(c, 1, bpc, 3);
    CHECK_EQ(bpc[0], 3);
}

static void test_bpc_correct_widths(void) {
    int32_t c[16] = {
         1,   0,  0,  0,   /* max abs=1   → bpc=1 */
         0,   0,  3,  0,   /* max abs=3   → bpc=2 */
        -7,   1,  0, -2,   /* max abs=7   → bpc=3 */
         0,  -1024, 0, 100  /* max abs=1024 → bpc=11 */
    };
    uint8_t bpc[4];
    wce_compute_bpcs(c, 4, bpc, 0);
    CHECK_EQ(bpc[0], 1);
    CHECK_EQ(bpc[1], 2);
    CHECK_EQ(bpc[2], 3);
    CHECK_EQ(bpc[3], 11);
}

static void test_bpc_int32_min(void) {
    int32_t c[4] = {INT32_MIN, 0, 0, 0};
    uint8_t bpc[1];
    wce_compute_bpcs(c, 1, bpc, 0);
    CHECK_EQ(bpc[0], 32);
}

/* DPCM round-trip */

static void encode_decode_roundtrip(const uint8_t *bpcs, size_t n) {
    uint8_t buf[1024];
    wce_bitwriter_t bw; wce_bw_init(&bw, buf, sizeof(buf));
    const int k = wce_pick_rice_k_for_bpcs(bpcs, n, 6);
    CHECK(k >= 0 && k <= 6);
    wce_encode_bpcs_dpcm(&bw, bpcs, n, k);
    wce_bw_flush(&bw);
    CHECK(!wce_bw_overflow(&bw));

    uint8_t out[256];
    wce_bitreader_t br; wce_br_init(&br, buf, wce_bw_bytes_written(&bw));
    wce_decode_bpcs_dpcm(&br, n, bpcs[0], k, out);
    CHECK_EQ_BYTES(out, bpcs, n);
}

static void test_dpcm_smooth_sequence(void) {
    const uint8_t bpcs[] = {5, 5, 5, 6, 6, 6, 7, 7, 8, 8};
    encode_decode_roundtrip(bpcs, sizeof(bpcs));
}

static void test_dpcm_jumpy_sequence(void) {
    const uint8_t bpcs[] = {0, 8, 0, 16, 4, 12, 2, 20, 1, 5};
    encode_decode_roundtrip(bpcs, sizeof(bpcs));
}

static void test_dpcm_long_constant(void) {
    /* 100 identical BPCs: every delta is 0, so the DPCM stream is just
     * 99 Rice-coded zeros — should compress hard. */
    uint8_t bpcs[100];
    for (size_t i = 0; i < 100; ++i) bpcs[i] = 7;
    encode_decode_roundtrip(bpcs, 100);
}

static void test_dpcm_single_group(void) {
    /* num_groups=1: no deltas, just verify initial round-trips. */
    uint8_t bpcs[1] = {5};
    uint8_t buf[16];
    wce_bitwriter_t bw; wce_bw_init(&bw, buf, sizeof(buf));
    wce_encode_bpcs_dpcm(&bw, bpcs, 1, 0);
    wce_bw_flush(&bw);
    /* No bits should have been emitted. */
    CHECK_EQ(wce_bw_bytes_written(&bw), 0);
    uint8_t out[1];
    wce_bitreader_t br; wce_br_init(&br, buf, 0);
    wce_decode_bpcs_dpcm(&br, 1, 5, 0, out);
    CHECK_EQ(out[0], 5);
}

static void test_dpcm_decoder_caps_out_of_range(void) {
    /* Initial > 32 is clamped. */
    uint8_t out[1];
    wce_bitreader_t br; wce_br_init(&br, NULL, 0);
    wce_decode_bpcs_dpcm(&br, 1, 99, 0, out);
    CHECK_EQ(out[0], 32);
}

static void test_dpcm_decoder_clamps_negative_delta(void) {
    /* Encode a sequence that would underflow if decoded naively. */
    uint8_t bpcs[3] = {2, 0, 0};
    uint8_t buf[16];
    wce_bitwriter_t bw; wce_bw_init(&bw, buf, sizeof(buf));
    wce_encode_bpcs_dpcm(&bw, bpcs, 3, 0);
    wce_bw_flush(&bw);
    uint8_t out[3];
    wce_bitreader_t br; wce_br_init(&br, buf, wce_bw_bytes_written(&bw));
    wce_decode_bpcs_dpcm(&br, 3, 2, 0, out);
    CHECK_EQ(out[0], 2);
    CHECK_EQ(out[1], 0);
    CHECK_EQ(out[2], 0);
}

static void test_dpcm_decoder_handles_rice_corruption(void) {
    /* All-ones bitstream → first Rice read returns UINT32_MAX → decoder
     * pins remaining BPCs at the initial. */
    uint8_t buf[64]; memset(buf, 0xFF, sizeof(buf));
    uint8_t out[5];
    wce_bitreader_t br; wce_br_init(&br, buf, sizeof(buf));
    wce_decode_bpcs_dpcm(&br, 5, 7, 0, out);
    CHECK_EQ(out[0], 7);
    CHECK_EQ(out[1], 7);
    CHECK_EQ(out[2], 7);
    CHECK_EQ(out[3], 7);
    CHECK_EQ(out[4], 7);
}

/* Rice-k picker sanity */

static void test_pick_k_constant_picks_zero(void) {
    /* All identical → every delta=0 → smallest k=0 wins. */
    uint8_t bpcs[20];
    for (size_t i = 0; i < 20; ++i) bpcs[i] = 5;
    CHECK_EQ(wce_pick_rice_k_for_bpcs(bpcs, 20, 6), 0);
}

static void test_pick_k_big_jumps_picks_larger(void) {
    /* Large alternating deltas → bigger k packs more efficiently. */
    const uint8_t bpcs[] = {0, 16, 0, 16, 0, 16, 0, 16};
    int k = wce_pick_rice_k_for_bpcs(bpcs, 8, 6);
    CHECK(k >= 3);  /* not k=0; specific best may shift with corpus */
}

void run_wce_bpc_tests(void) {
    printf("wce_bpc:\n");
    test_bpc_all_zero();
    test_bpc_lossy_floor();
    test_bpc_correct_widths();
    test_bpc_int32_min();
    test_dpcm_smooth_sequence();
    test_dpcm_jumpy_sequence();
    test_dpcm_long_constant();
    test_dpcm_single_group();
    test_dpcm_decoder_caps_out_of_range();
    test_dpcm_decoder_clamps_negative_delta();
    test_dpcm_decoder_handles_rice_corruption();
    test_pick_k_constant_picks_zero();
    test_pick_k_big_jumps_picks_larger();
}
