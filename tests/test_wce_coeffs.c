#include "test_runner.h"
#include "wce.h"
#include <limits.h>
#include <string.h>

static void roundtrip(const int32_t *coeffs_in, size_t num_groups, int lossy_bits) {
    uint8_t bpcs[256];
    int32_t reconstructed[1024];
    uint8_t buf[8192];

    wce_compute_bpcs(coeffs_in, num_groups, bpcs, (uint8_t)lossy_bits);

    wce_bitwriter_t bw; wce_bw_init(&bw, buf, sizeof(buf));
    wce_pack_coeffs(&bw, coeffs_in, bpcs, lossy_bits, num_groups);
    wce_bw_flush(&bw);
    CHECK(!wce_bw_overflow(&bw));

    wce_bitreader_t br; wce_br_init(&br, buf, wce_bw_bytes_written(&bw));
    wce_unpack_coeffs(&br, bpcs, lossy_bits, num_groups, reconstructed);
    CHECK(!wce_br_truncated(&br));

    /* Expected: |c| with bottom lossy_bits zeroed (truncate-toward-zero). */
    for (size_t i = 0; i < num_groups * 4; ++i) {
        const int32_t c = coeffs_in[i];
        const uint32_t abs_c = (c < 0) ? ((uint32_t)0 - (uint32_t)c) : (uint32_t)c;
        const uint32_t mag   = (lossy_bits >= 32) ? 0
                              : ((abs_c >> lossy_bits) << lossy_bits);
        const int32_t expect = (c >= 0) ? (int32_t)mag : (int32_t)((uint32_t)0 - mag);
        CHECK_EQ(reconstructed[i], expect);
    }
}

static void test_coeffs_all_zero(void) {
    int32_t c[16] = {0};
    roundtrip(c, 4, 0);
    roundtrip(c, 4, 3);
}

static void test_coeffs_small_lossless(void) {
    int32_t c[8] = {1, -1, 2, -2, 0, 7, -7, 100};
    roundtrip(c, 2, 0);
}

static void test_coeffs_lossy(void) {
    int32_t c[16] = {
         1, -1,  7,  -8,
        16,  -16, 100, -100,
         0,   0,   0,    0,
       1024, -1024, 5000, -5000
    };
    roundtrip(c, 4, 3);
    roundtrip(c, 4, 5);
}

static void test_coeffs_int32_extremes(void) {
    int32_t c[4] = {INT32_MAX, INT32_MIN, 0, 1};
    roundtrip(c, 1, 0);
    roundtrip(c, 1, 5);
}

static void test_coeffs_lossy_kills_small(void) {
    /* lossy_bits=3 → step=8 → coeffs with |c|<8 quantize to zero. */
    int32_t c[4] = {7, -7, 4, -4};
    uint8_t bpcs[1];
    wce_compute_bpcs(c, 1, bpcs, 3);
    /* All four coeffs are |.| < 8, so quantize to 0, so bpc = lossy = 3. */
    CHECK_EQ(bpcs[0], 3);

    uint8_t buf[64];
    wce_bitwriter_t bw; wce_bw_init(&bw, buf, sizeof(buf));
    wce_pack_coeffs(&bw, c, bpcs, 3, 1);
    wce_bw_flush(&bw);

    int32_t out[4];
    wce_bitreader_t br; wce_br_init(&br, buf, wce_bw_bytes_written(&bw));
    wce_unpack_coeffs(&br, bpcs, 3, 1, out);
    for (int i = 0; i < 4; ++i) CHECK_EQ(out[i], 0);
}

static void test_coeffs_no_signs_for_zero_coeffs(void) {
    /* Zero coefficients consume no sign bits. Verify by encoding mixed
     * zero/nonzero and checking the byte count matches the bit-counted
     * expectation. */
    int32_t c[4] = {16, 0, 0, -16};
    uint8_t bpcs[1];
    wce_compute_bpcs(c, 1, bpcs, 3);  /* lossy=3, max|c|=16 → bpc=5 */
    CHECK_EQ(bpcs[0], 5);
    /* Expected: 4 coeffs × (bpc-lossy=2) mag bits each + 2 sign bits
     *         = 8 + 2 = 10 bits = 2 bytes (after flush). */
    uint8_t buf[64];
    wce_bitwriter_t bw; wce_bw_init(&bw, buf, sizeof(buf));
    wce_pack_coeffs(&bw, c, bpcs, 3, 1);
    wce_bw_flush(&bw);
    CHECK_EQ(wce_bw_bytes_written(&bw), 2);
}

static void test_coeffs_large_roundtrip(void) {
    /* 256 coefficients = 64 groups. Mix of patterns including extremes. */
    int32_t c[256];
    for (size_t i = 0; i < 256; ++i) {
        int32_t v;
        if (i < 4) {
            v = (i == 0) ? INT32_MAX : (i == 1) ? INT32_MIN :
                (i == 2) ? 0x7FFFFFFF : 0x80000001;
        } else {
            v = (int32_t)(i * 13 + 1);
            if (i & 1) v = -v;
            if (i % 7 == 0) v = 0;
        }
        c[i] = v;
    }
    roundtrip(c, 64, 0);
    roundtrip(c, 64, 2);
    roundtrip(c, 64, 5);
}

static void test_coeffs_lossy_bits_31(void) {
    /* lossy_bits=31 → only the MSB survives. INT32_MIN stays, everything
     * else quantizes to 0 (since positive values max out at INT32_MAX whose
     * MSB is 0). */
    int32_t c[8] = {INT32_MIN, INT32_MAX, -1, 1, 0, 0x40000000, -0x40000000, 42};
    roundtrip(c, 2, 31);
}

static void test_coeffs_saturates_int32_max(void) {
    /* When |coeff| shifted by lossy_bits is just below overflow,
     * the unpack path must saturate correctly for both signs. */
    uint8_t bpcs[1];
    /* lossy_bits=1: step=2. 0x7FFFFFFE >> 1 = 0x3FFFFFFF → << 1 = 0x7FFFFFFE.
     * 0x7FFFFFFF >> 1 = 0x3FFFFFFF → << 1 = 0x7FFFFFFE (both within range). */
    int32_t c[4] = {INT32_MAX - 1, INT32_MAX, INT32_MIN + 1, INT32_MIN};
    wce_compute_bpcs(c, 1, bpcs, 1);  /* bpc=32 */
    CHECK_EQ(bpcs[0], 32);

    uint8_t buf[256];
    wce_bitwriter_t bw; wce_bw_init(&bw, buf, sizeof(buf));
    wce_pack_coeffs(&bw, c, bpcs, 1, 1);
    wce_bw_flush(&bw);
    CHECK(!wce_bw_overflow(&bw));

    int32_t out[4];
    wce_bitreader_t br; wce_br_init(&br, buf, wce_bw_bytes_written(&bw));
    wce_unpack_coeffs(&br, bpcs, 1, 1, out);
    CHECK(!wce_br_truncated(&br));

    /* After lossy_bits=1 truncation:
     * c[0]=0x7FFFFFFE → mag=0x7FFFFFFE → int32_max
     * c[1]=0x7FFFFFFF → mag=0x7FFFFFFE → int32_max (truncated)
     * c[2]=0x80000001 → -0x7FFFFFFE → -2147483646
     * c[3]=0x80000000 → -0x80000000 → INT32_MIN */
    CHECK_EQ(out[0], 2147483646);
    CHECK_EQ(out[1], 2147483646);
    CHECK_EQ(out[2], -2147483646);
    CHECK_EQ(out[3], INT32_MIN);
}

void run_wce_coeffs_tests(void) {
    printf("wce_coeffs:\n");
    test_coeffs_all_zero();
    test_coeffs_small_lossless();
    test_coeffs_lossy();
    test_coeffs_int32_extremes();
    test_coeffs_lossy_kills_small();
    test_coeffs_no_signs_for_zero_coeffs();
    test_coeffs_large_roundtrip();
    test_coeffs_lossy_bits_31();
    test_coeffs_saturates_int32_max();
}
