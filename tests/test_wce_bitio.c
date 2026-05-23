#include "test_runner.h"
#include "wce.h"
#include <limits.h>
#include <string.h>

/* Bit I/O — LSB-first */

static void test_bw_write_single_bit(void) {
    uint8_t buf[1] = {0};
    wce_bitwriter_t bw;
    wce_bw_init(&bw, buf, sizeof(buf));
    wce_bw_write_bits(&bw, 1, 1);  /* lsb */
    wce_bw_flush(&bw);
    CHECK_EQ(buf[0], 0x01);
    CHECK_EQ(wce_bw_bytes_written(&bw), 1);
}

static void test_bw_lsb_first_ordering(void) {
    /* Write 4 nibbles, value=A,B,C,D each 4 bits.
     * Expected byte0 = (B<<4)|A, byte1 = (D<<4)|C (LSB-first). */
    uint8_t buf[2] = {0};
    wce_bitwriter_t bw;
    wce_bw_init(&bw, buf, sizeof(buf));
    wce_bw_write_bits(&bw, 0xA, 4);
    wce_bw_write_bits(&bw, 0xB, 4);
    wce_bw_write_bits(&bw, 0xC, 4);
    wce_bw_write_bits(&bw, 0xD, 4);
    wce_bw_flush(&bw);
    CHECK_EQ(buf[0], 0xBA);
    CHECK_EQ(buf[1], 0xDC);
}

static void test_bw_roundtrip_assorted_widths(void) {
    uint8_t buf[64] = {0};
    wce_bitwriter_t bw; wce_bw_init(&bw, buf, sizeof(buf));
    const uint32_t vals[] = {0, 1, 31, 1023, 0x12345678u, 7, 0, 0xFFFFFFFFu};
    const int      widths[] = {1, 2, 5, 10, 32, 3, 7, 32};
    const size_t   N = sizeof(vals)/sizeof(vals[0]);
    for (size_t i = 0; i < N; ++i)
        wce_bw_write_bits(&bw, vals[i], widths[i]);
    wce_bw_flush(&bw);
    CHECK(!wce_bw_overflow(&bw));

    wce_bitreader_t br; wce_br_init(&br, buf, wce_bw_bytes_written(&bw));
    for (size_t i = 0; i < N; ++i) {
        uint32_t got = wce_br_read_bits(&br, widths[i]);
        CHECK_EQ(got, vals[i]);
    }
    CHECK(!wce_br_truncated(&br));
}

static void test_bw_overflow_flag(void) {
    uint8_t buf[2] = {0};
    wce_bitwriter_t bw; wce_bw_init(&bw, buf, sizeof(buf));
    wce_bw_write_bits(&bw, 0xFFFF, 16);  /* fits exactly */
    wce_bw_write_bits(&bw, 1, 1);        /* overflows */
    wce_bw_flush(&bw);
    CHECK(wce_bw_overflow(&bw));
}

static void test_br_truncated_returns_zero(void) {
    uint8_t buf[1] = {0x55};
    wce_bitreader_t br; wce_br_init(&br, buf, sizeof(buf));
    /* Read past end. */
    uint32_t a = wce_br_read_bits(&br, 8);
    uint32_t b = wce_br_read_bits(&br, 8);  /* zero-pad */
    CHECK_EQ(a, 0x55);
    CHECK_EQ(b, 0);
    CHECK(wce_br_truncated(&br));
}

static void test_br_zero_n_is_noop(void) {
    uint8_t buf[1] = {0xFF};
    wce_bitreader_t br; wce_br_init(&br, buf, sizeof(buf));
    CHECK_EQ(wce_br_read_bits(&br, 0), 0);
    /* No side-effects. */
    CHECK_EQ(wce_br_read_bits(&br, 8), 0xFF);
}

static void test_br_byte_align(void) {
    uint8_t buf[2] = {0xF0, 0x55};
    wce_bitreader_t br; wce_br_init(&br, buf, sizeof(buf));
    wce_br_read_bits(&br, 3);
    wce_br_byte_align(&br);
    /* Next byte read should be 0x55. */
    CHECK_EQ(wce_br_read_bits(&br, 8), 0x55);
}

static void test_br_bytes_consumed(void) {
    uint8_t buf[4] = {1,2,3,4};
    wce_bitreader_t br; wce_br_init(&br, buf, sizeof(buf));
    CHECK_EQ(wce_br_bytes_consumed(&br), 0);
    wce_br_read_bits(&br, 8);
    CHECK_EQ(wce_br_bytes_consumed(&br), 1);
    wce_br_read_bits(&br, 4);
    /* refill loaded byte 2; we've consumed 2 bytes from the buffer. */
    CHECK_EQ(wce_br_bytes_consumed(&br), 2);
}

/* Zigzag */

static void test_zigzag_small_values(void) {
    CHECK_EQ(wce_zigzag_encode(0),  0);
    CHECK_EQ(wce_zigzag_encode(-1), 1);
    CHECK_EQ(wce_zigzag_encode(1),  2);
    CHECK_EQ(wce_zigzag_encode(-2), 3);
    CHECK_EQ(wce_zigzag_encode(2),  4);
}

static void test_zigzag_extremes(void) {
    /* INT32_MAX → 0xFFFFFFFE; INT32_MIN → 0xFFFFFFFF. */
    CHECK_EQ(wce_zigzag_encode(INT32_MAX), 0xFFFFFFFEu);
    CHECK_EQ(wce_zigzag_encode(INT32_MIN), 0xFFFFFFFFu);
}

static void test_zigzag_roundtrip(void) {
    const int32_t vals[] = {0, 1, -1, 100, -100, INT32_MAX, INT32_MIN,
                             -42, 0x1234567, -0x1234567};
    for (size_t i = 0; i < sizeof(vals)/sizeof(vals[0]); ++i) {
        uint32_t e = wce_zigzag_encode(vals[i]);
        int32_t  d = wce_zigzag_decode(e);
        CHECK_EQ((uint32_t)d, (uint32_t)vals[i]);
    }
}

/* Rice */

static void test_rice_k0_unary(void) {
    /* k=0: value v encoded as v one-bits then a zero. */
    uint8_t buf[8] = {0};
    wce_bitwriter_t bw; wce_bw_init(&bw, buf, sizeof(buf));
    wce_bw_write_rice(&bw, 3, 0);
    wce_bw_flush(&bw);
    /* LSB-first: 1,1,1,0 → byte = 0b00000111 = 0x07 */
    CHECK_EQ(buf[0], 0x07);

    wce_bitreader_t br; wce_br_init(&br, buf, sizeof(buf));
    CHECK_EQ(wce_br_read_rice(&br, 0), 3);
}

static void test_rice_k2_split(void) {
    /* k=2: v=11 → q=2, r=3 → bits: 1,1,0, then r(2 bits)=11 → 1,1.
     * LSB-first: bits = [1,1,0,1,1] → byte = 0b00011011 = 0x1B */
    uint8_t buf[4] = {0};
    wce_bitwriter_t bw; wce_bw_init(&bw, buf, sizeof(buf));
    wce_bw_write_rice(&bw, 11, 2);
    wce_bw_flush(&bw);
    CHECK_EQ(buf[0], 0x1B);

    wce_bitreader_t br; wce_br_init(&br, buf, sizeof(buf));
    CHECK_EQ(wce_br_read_rice(&br, 2), 11);
}

static void test_rice_roundtrip_many(void) {
    /* For each (value, k) pair, only round-trip values whose quotient fits
     * under WCE_RICE_MAX_QUOTIENT — that's the documented decoder bound. */
    uint8_t buf[1024];
    const uint32_t vals[] = {0, 1, 2, 5, 10, 31, 63, 100, 200, 1024, 65535};
    const size_t N = sizeof(vals)/sizeof(vals[0]);
    for (int k = 0; k <= 12; ++k) {
        wce_bitwriter_t bw; wce_bw_init(&bw, buf, sizeof(buf));
        size_t encoded[64]; size_t ne = 0;
        for (size_t i = 0; i < N; ++i) {
            if ((vals[i] >> k) >= WCE_RICE_MAX_QUOTIENT) continue;
            wce_bw_write_rice(&bw, vals[i], k);
            encoded[ne++] = i;
        }
        wce_bw_flush(&bw);
        CHECK(!wce_bw_overflow(&bw));

        wce_bitreader_t br;
        wce_br_init(&br, buf, wce_bw_bytes_written(&bw));
        for (size_t e = 0; e < ne; ++e) {
            uint32_t got = wce_br_read_rice(&br, k);
            CHECK_EQ(got, vals[encoded[e]]);
        }
    }
}

static void test_rice_large_value_at_k16(void) {
    /* Wide k handles million-class values cleanly. */
    uint8_t buf[64] = {0};
    wce_bitwriter_t bw; wce_bw_init(&bw, buf, sizeof(buf));
    wce_bw_write_rice(&bw, 0x123456u, 16);
    wce_bw_flush(&bw);
    wce_bitreader_t br; wce_br_init(&br, buf, wce_bw_bytes_written(&bw));
    CHECK_EQ(wce_br_read_rice(&br, 16), 0x123456u);
}

static void test_rice_adversarial_all_ones(void) {
    /* All-ones stream → unbounded unary run → quotient hits cap.
     * Reader must return UINT32_MAX, not loop forever. */
    uint8_t buf[64];
    memset(buf, 0xFF, sizeof(buf));
    wce_bitreader_t br; wce_br_init(&br, buf, sizeof(buf));
    uint32_t v = wce_br_read_rice(&br, 4);
    CHECK_EQ(v, UINT32_MAX);
    /* Cap-hit on a full buffer should not set truncated. */
    CHECK(!wce_br_truncated(&br));
    /* Should have consumed exactly 32 bytes (256 unary bits at cap). */
    CHECK_EQ(wce_br_bytes_consumed(&br), 32);
}

static void test_rice_boundary_q255(void) {
    /* q=255 is the largest decodable quotient. Roundtrip at several k. */
    const uint32_t tests[][2] = {{255u << 0, 0}, {255u << 1, 1}, {255u << 2, 2},
                                  {255u << 4, 4}, {255u << 8, 8}};
    uint8_t buf[512];
    for (size_t ti = 0; ti < sizeof(tests)/sizeof(tests[0]); ++ti) {
        uint32_t val = tests[ti][0];
        int      k   = (int)tests[ti][1];
        wce_bitwriter_t bw; wce_bw_init(&bw, buf, sizeof(buf));
        wce_bw_write_rice(&bw, val, k);
        wce_bw_flush(&bw);
        CHECK(!wce_bw_overflow(&bw));
        wce_bitreader_t br; wce_br_init(&br, buf, wce_bw_bytes_written(&bw));
        CHECK_EQ(wce_br_read_rice(&br, k), val);
    }
    /* q=256 at k=0 → writer sets the overflow flag (guard in wce_bw_write_rice). */
    memset(buf, 0, sizeof(buf));
    wce_bitwriter_t bw; wce_bw_init(&bw, buf, sizeof(buf));
    wce_bw_write_rice(&bw, 256u, 0);  /* q=256 ≥ MAX_QUOTIENT → overflow set */
    wce_bw_flush(&bw);
    CHECK(wce_bw_overflow(&bw));
    /* After discarding, zero bytes should have been written. */
    CHECK_EQ(wce_bw_bytes_written(&bw), 0);
}

static void test_rice_truncated_input(void) {
    /* Empty buffer → zero-pad → decodes to 0 (immediate terminator). */
    wce_bitreader_t br; wce_br_init(&br, NULL, 0);
    uint32_t v = wce_br_read_rice(&br, 3);
    CHECK_EQ(v, 0);
    CHECK(wce_br_truncated(&br));
}

void run_wce_bitio_tests(void) {
    printf("wce_bitio:\n");
    test_bw_write_single_bit();
    test_bw_lsb_first_ordering();
    test_bw_roundtrip_assorted_widths();
    test_bw_overflow_flag();
    test_br_truncated_returns_zero();
    test_br_zero_n_is_noop();
    test_br_byte_align();
    test_br_bytes_consumed();
    test_zigzag_small_values();
    test_zigzag_extremes();
    test_zigzag_roundtrip();
    test_rice_k0_unary();
    test_rice_k2_split();
    test_rice_roundtrip_many();
    test_rice_large_value_at_k16();
    test_rice_adversarial_all_ones();
    test_rice_boundary_q255();
    test_rice_truncated_input();
}
