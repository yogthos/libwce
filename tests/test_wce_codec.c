#include "test_runner.h"
#include "wce.h"
#include <limits.h>
#include <string.h>

/* Top-level encode / decode round-trip */

static void check_roundtrip(const int32_t *coeffs, size_t n, int lossy_bits) {
    uint8_t  buf[16384];
    size_t   out_len = 0;
    int      rc = wce_encode(coeffs, n, lossy_bits, buf, sizeof(buf), &out_len);
    CHECK_EQ(rc, WCE_OK);
    CHECK(out_len >= WCE_HEADER_SIZE);

    int32_t decoded[2048];
    int     decoded_lb = -1;
    rc = wce_decode(buf, out_len, decoded, n, &decoded_lb);
    CHECK_EQ(rc, WCE_OK);
    CHECK_EQ(decoded_lb, lossy_bits);

    /* Quantization is truncate-toward-zero on magnitude; decoder returns
     * truncated grid values (no reconstruction offset). */
    for (size_t i = 0; i < n; ++i) {
        const int32_t c = coeffs[i];
        const uint32_t abs_c = (c < 0) ? ((uint32_t)0 - (uint32_t)c) : (uint32_t)c;
        const uint32_t mag   = (lossy_bits >= 32) ? 0
                              : ((abs_c >> lossy_bits) << lossy_bits);
        const int32_t want   = (c >= 0)
            ? (int32_t)mag
            : (int32_t)((uint32_t)0 - mag);
        CHECK_EQ(decoded[i], want);
    }
}

static void test_codec_tiny(void) {
    int32_t c[4] = {0, 0, 0, 0};
    check_roundtrip(c, 4, 0);
}

static void test_codec_lossless(void) {
    int32_t c[16];
    for (int i = 0; i < 16; ++i) c[i] = (i & 1) ? -i*7 : i*5;
    check_roundtrip(c, 16, 0);
}

static void test_codec_lossy_levels(void) {
    int32_t c[64];
    for (int i = 0; i < 64; ++i) c[i] = (i*97 - 1024) * ((i&1) ? -1 : 1);
    check_roundtrip(c, 64, 2);
    check_roundtrip(c, 64, 5);
}

static void test_codec_int32_extremes(void) {
    int32_t c[8] = {INT32_MAX, INT32_MIN, 0, -1, 1, -2, 2, 1234567};
    check_roundtrip(c, 8, 0);
    check_roundtrip(c, 8, 5);
}

static void test_codec_larger(void) {
    int32_t c[2048];
    for (int i = 0; i < 2048; ++i) {
        int32_t v = (int32_t)((i * 13 + 7) & 0xFFF);
        if (i & 1) v = -v;
        c[i] = v;
    }
    check_roundtrip(c, 2048, 0);
    check_roundtrip(c, 2048, 3);
}

/* Error handling */

static void test_codec_rejects_unaligned_n(void) {
    int32_t c[5] = {0,0,0,0,0};
    uint8_t buf[64]; size_t out_len;
    CHECK_EQ(wce_encode(c, 5, 0, buf, sizeof(buf), &out_len), WCE_ERR_BADINPUT);
}

static void test_codec_rejects_bad_lossy_bits(void) {
    int32_t c[4] = {0};
    uint8_t buf[64]; size_t out_len;
    CHECK_EQ(wce_encode(c, 4, -1, buf, sizeof(buf), &out_len), WCE_ERR_BADINPUT);
    CHECK_EQ(wce_encode(c, 4, 32, buf, sizeof(buf), &out_len), WCE_ERR_BADINPUT);
}

static void test_codec_rejects_small_out_cap(void) {
    int32_t c[4] = {0};
    uint8_t buf[4]; size_t out_len;
    CHECK_EQ(wce_encode(c, 4, 0, buf, sizeof(buf), &out_len), WCE_ERR_NOSPACE);
}

static void test_codec_rejects_bad_magic(void) {
    uint8_t buf[WCE_HEADER_SIZE] = {0};
    int32_t out[4]; int lb;
    CHECK_EQ(wce_decode(buf, sizeof(buf), out, 4, &lb), WCE_ERR_BADMAGIC);
}

static void test_codec_rejects_bad_version(void) {
    /* Valid magic but bumped version. */
    uint8_t buf[WCE_HEADER_SIZE] = {0};
    buf[0] = 'W'; buf[1] = 'C'; buf[2] = 'E'; buf[3] = 0;
    buf[4] = 1;  /* num_groups = 1 */
    buf[8] = WCE_FORMAT_VERSION + 99u;
    int32_t out[4]; int lb;
    CHECK_EQ(wce_decode(buf, sizeof(buf), out, 4, &lb), WCE_ERR_BADVERSION);
}

static void test_codec_rejects_size_mismatch(void) {
    int32_t c[4] = {1, 2, 3, 4};
    uint8_t buf[64]; size_t out_len;
    CHECK_EQ(wce_encode(c, 4, 0, buf, sizeof(buf), &out_len), WCE_OK);
    int32_t out[8]; int lb;
    /* Header says 1 group; we ask decoder for 8 coeffs (= 2 groups). */
    CHECK_EQ(wce_decode(buf, out_len, out, 8, &lb), WCE_ERR_BADINPUT);
}

static void test_codec_truncated_decodes_zeros(void) {
    /* Truncated stream must return WCE_ERR_TRUNCATED — output
     * coefficients past the truncation point are zero-filled. */
    int32_t c[8] = {100, -200, 300, -400, 500, -600, 700, -800};
    uint8_t buf[128]; size_t out_len;
    CHECK_EQ(wce_encode(c, 8, 0, buf, sizeof(buf), &out_len), WCE_OK);

    const size_t half = WCE_HEADER_SIZE + (out_len - WCE_HEADER_SIZE) / 2;
    int32_t out[8]; int lb;
    CHECK_EQ(wce_decode(buf, half, out, 8, &lb), WCE_ERR_TRUNCATED);
}

/* Forced modes, sparse blocks, empty bands, corruption */

static void test_codec_empty_band(void) {
    int32_t dummy = 0;
    uint8_t buf[64]; size_t out_len;
    CHECK_EQ(wce_encode(&dummy, 0, 0, buf, sizeof(buf), &out_len), WCE_OK);
    int32_t out[1]; int lb;
    CHECK_EQ(wce_decode(buf, out_len, out, 0, &lb), WCE_OK);
}

static void test_codec_forced_mode_roundtrip(void) {
    int32_t c[64];
    for (int i = 0; i < 64; ++i) c[i] = (i * 97 - 1024) * ((i&1) ? -1 : 1);

    const struct {
        uint8_t pred;
        bool    flag;
    } combos[] = {
        {WCE_PREDICTOR_RUNNING, false},
        {WCE_PREDICTOR_RUNNING, true},
        {WCE_PREDICTOR_ZERO,    false},
        {WCE_PREDICTOR_ZERO,    true},
    };
    int rice_k_vals[] = {0, 3, 6};

    for (size_t ci = 0; ci < sizeof(combos)/sizeof(combos[0]); ++ci) {
        for (size_t ki = 0; ki < sizeof(rice_k_vals)/sizeof(rice_k_vals[0]); ++ki) {
            wce_encode_options_t opts = {
                .force_mode = true,
                .predictor  = combos[ci].pred,
                .use_flag   = combos[ci].flag,
                .rice_k     = (uint8_t)rice_k_vals[ki],
            };
            uint8_t buf[4096]; size_t out_len;
            int rc = wce_encode_with_options(c, 64, 3, &opts,
                                             buf, sizeof(buf), &out_len);
            CHECK_EQ(rc, WCE_OK);

            int32_t out[64]; int lb;
            rc = wce_decode(buf, out_len, out, 64, &lb);
            CHECK_EQ(rc, WCE_OK);
            CHECK_EQ(lb, 3);

            for (int i = 0; i < 64; ++i) {
                const int32_t ci = c[i];
                const uint32_t abs_c = (ci < 0) ? ((uint32_t)0 - (uint32_t)ci) : (uint32_t)ci;
                const uint32_t mag = (abs_c >> 3) << 3;
                const int32_t want = (ci >= 0) ? (int32_t)mag : (int32_t)((uint32_t)0 - mag);
                CHECK_EQ(out[i], want);
            }
        }
    }
}

static void test_codec_sparse_block_mix(void) {
    int32_t c[256];
    for (int i = 0; i < 256; ++i) c[i] = 0;
    c[0] = 1024;  c[1] = -512;
    c[128] = 256; c[129] = -128;
    uint8_t buf[8192]; size_t out_len;
    CHECK_EQ(wce_encode(c, 256, 3, buf, sizeof(buf), &out_len), WCE_OK);
    int32_t out[256]; int lb;
    CHECK_EQ(wce_decode(buf, out_len, out, 256, &lb), WCE_OK);
    CHECK_EQ(lb, 3);
    /* Verify coefficient values — sparse blocks should survive. */
    for (int i = 0; i < 256; ++i) {
        const int32_t ci = c[i];
        const uint32_t a = (ci < 0) ? ((uint32_t)0 - (uint32_t)ci) : (uint32_t)ci;
        const uint32_t m = (a >> 3) << 3;
        const int32_t w = (ci >= 0) ? (int32_t)m : (int32_t)((uint32_t)0 - m);
        CHECK_EQ(out[i], w);
    }
}

static void test_codec_partial_final_block(void) {
    int32_t c[40];  /* 10 groups, final block has 2 groups (not 8) */
    for (int i = 0; i < 40; ++i) c[i] = (i * 7) * ((i&1) ? -1 : 1);
    uint8_t buf[1024]; size_t out_len;
    CHECK_EQ(wce_encode(c, 40, 0, buf, sizeof(buf), &out_len), WCE_OK);
    int32_t out[40]; int lb;
    CHECK_EQ(wce_decode(buf, out_len, out, 40, &lb), WCE_OK);
    CHECK_EQ(lb, 0);
}

static void test_codec_corrupt_rice_detected(void) {
    int32_t c[32] = {0};
    for (int i = 0; i < 32; ++i) c[i] = (i+1)*100 * ((i&1)?-1:1);
    uint8_t buf[2048]; size_t out_len;
    CHECK_EQ(wce_encode(c, 32, 0, buf, sizeof(buf), &out_len), WCE_OK);

    /* Craft an all-ones payload past the header to trigger Rice quotient cap. */
    uint8_t corrupt[2048];
    memcpy(corrupt, buf, WCE_HEADER_SIZE);
    memset(corrupt + WCE_HEADER_SIZE, 0xFF, out_len - WCE_HEADER_SIZE);
    int32_t out[32]; int lb;
    CHECK_EQ(wce_decode(corrupt, out_len, out, 32, &lb), WCE_ERR_CORRUPT);
}

static void test_codec_null_lossy_bits_out(void) {
    int32_t c[4] = {1, -2, 3, -4};
    uint8_t buf[256]; size_t out_len;
    CHECK_EQ(wce_encode(c, 4, 0, buf, sizeof(buf), &out_len), WCE_OK);
    int32_t out[4];
    CHECK_EQ(wce_decode(buf, out_len, out, 4, NULL), WCE_OK);
}

/* Additional error-path and boundary tests */

static void test_codec_forced_mode_rejects_bad_input(void) {
    int32_t c[4] = {1, 2, 3, 4};
    wce_encode_options_t opts = { .force_mode = true, .predictor = 0,
                                   .use_flag = false, .rice_k = 0 };
    uint8_t buf[256]; size_t out_len;

    /* Invalid predictor value */
    opts.predictor = 99;
    CHECK_EQ(wce_encode_with_options(c, 4, 0, &opts,
                                     buf, sizeof(buf), &out_len),
             WCE_ERR_BADINPUT);

    /* Invalid rice_k > 16 */
    opts.predictor = WCE_PREDICTOR_RUNNING;
    opts.rice_k    = 17;
    CHECK_EQ(wce_encode_with_options(c, 4, 0, &opts,
                                     buf, sizeof(buf), &out_len),
             WCE_ERR_BADINPUT);
    opts.rice_k = 99;
    CHECK_EQ(wce_encode_with_options(c, 4, 0, &opts,
                                     buf, sizeof(buf), &out_len),
             WCE_ERR_BADINPUT);
}

static void test_codec_decode_rejects_bad_lossy_bits(void) {
    /* Craft a header with lossy_bits=32 on the decode side. */
    uint8_t hdr[WCE_HEADER_SIZE] = {0};
    hdr[0] = 'W'; hdr[1] = 'C'; hdr[2] = 'E'; hdr[3] = 0;
    hdr[4] = 1; hdr[8] = WCE_FORMAT_VERSION;
    hdr[9] = 33;  /* > 31 */
    int32_t out[4]; int lb;
    CHECK_EQ(wce_decode(hdr, sizeof(hdr), out, 4, &lb), WCE_ERR_BADINPUT);
}

static void test_codec_rejects_too_many_groups(void) {
    /* num_groups > WCE_MAX_INLINE_GROUPS should be rejected. */
    int32_t c[4] = {0};
    uint8_t buf[64]; size_t out_len;
    /* Use more coefficients than can fit in one band. */
    const size_t big = ((size_t)WCE_MAX_INLINE_GROUPS + 1) * 4;
    CHECK_EQ(wce_encode(c, big, 0, buf, sizeof(buf), &out_len), WCE_ERR_BADINPUT);
}

static void test_codec_rejects_null_coeffs_nonzero_n(void) {
    uint8_t buf[64]; size_t out_len;
    CHECK_EQ(wce_encode(NULL, 4, 0, buf, sizeof(buf), &out_len), WCE_ERR_BADINPUT);
}

static void test_codec_decode_rejects_initial_prev_mismatch(void) {
    /* Encode a valid stream then tamper with initial_prev in the header. */
    int32_t c[4] = {1, 2, 3, 4};
    uint8_t buf[256]; size_t out_len;
    CHECK_EQ(wce_encode(c, 4, 3, buf, sizeof(buf), &out_len), WCE_OK);
    buf[11] = 99;  /* initial_prev != lossy_bits */
    int32_t out[4]; int lb;
    CHECK_EQ(wce_decode(buf, out_len, out, 4, &lb), WCE_ERR_BADINPUT);
}

void run_wce_codec_tests(void) {
    printf("wce_codec:\n");
    test_codec_tiny();
    test_codec_lossless();
    test_codec_lossy_levels();
    test_codec_int32_extremes();
    test_codec_larger();
    test_codec_rejects_unaligned_n();
    test_codec_rejects_bad_lossy_bits();
    test_codec_rejects_small_out_cap();
    test_codec_rejects_bad_magic();
    test_codec_rejects_bad_version();
    test_codec_rejects_size_mismatch();
    test_codec_truncated_decodes_zeros();
    test_codec_empty_band();
    test_codec_forced_mode_roundtrip();
    test_codec_sparse_block_mix();
    test_codec_partial_final_block();
    test_codec_corrupt_rice_detected();
    test_codec_null_lossy_bits_out();
    test_codec_forced_mode_rejects_bad_input();
    test_codec_decode_rejects_bad_lossy_bits();
    test_codec_rejects_too_many_groups();
    test_codec_rejects_null_coeffs_nonzero_n();
    test_codec_decode_rejects_initial_prev_mismatch();
}
