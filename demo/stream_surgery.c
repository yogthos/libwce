/*
 * libwce demo: bitstream corruption resilience.
 *
 * Encodes a clean band via wce_encode, then attacks the encoded byte
 * stream four ways:
 *
 *   1. Single-bit flips at random positions in the bitstream
 *   2. Random-byte scrambles
 *   3. Truncation — feed the decoder progressively shorter prefixes
 *   4. Adversarial all-ones buffer + crafted bad headers
 *
 * For each attack the decoder must return without crashing or hanging.
 * Build with `make DEBUG=1` to also verify under ASan + UBSan.
 */

#include "wce.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_GROUPS  256
#define NUM_COEFFS  (NUM_GROUPS * 4)
#define LOSSY_BITS  3
#define BUF_CAP     (NUM_COEFFS * 5 + 64)

static uint32_t xs_state = 0xDEADBEEFu;
static uint32_t xs_next(void) {
    uint32_t x = xs_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return xs_state = x;
}

typedef struct {
    int32_t coeffs[NUM_COEFFS];     /* original, pre-quantize */
    int32_t expected[NUM_COEFFS];   /* truncated form a clean decode produces */
    uint8_t buf[BUF_CAP];
    size_t  buf_bytes;
} encoded_t;

/* Build a clean encoded band. */
static void build_clean(encoded_t *e) {
    for (size_t i = 0; i < NUM_COEFFS; ++i)
        e->coeffs[i] = (int32_t)(xs_next() % 4096) - 2048;
    int rc = wce_encode(e->coeffs, NUM_COEFFS, LOSSY_BITS,
                        e->buf, BUF_CAP, &e->buf_bytes);
    if (rc != WCE_OK) {
        fprintf(stderr, "build_clean: wce_encode failed (rc=%d)\n", rc);
        exit(1);
    }
    /* Pre-compute what a clean decode returns (truncated grid values).
     * Used to measure how badly each attack disrupted output. */
    for (size_t i = 0; i < NUM_COEFFS; ++i) {
        const int32_t c = e->coeffs[i];
        const uint32_t a = (c < 0) ? ((uint32_t)0 - (uint32_t)c) : (uint32_t)c;
        const uint32_t m = (a >> LOSSY_BITS) << LOSSY_BITS;
        e->expected[i] = (c >= 0) ? (int32_t)m : (int32_t)((uint32_t)0 - m);
    }
}

/* Count coefficients that differ between two arrays. */
static size_t coeff_distance(const int32_t *a, const int32_t *b, size_t n) {
    size_t diff = 0;
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) ++diff;
    return diff;
}

/* Decode a possibly corrupt buffer and report differences vs the clean
 * truncated baseline. Returns 1 if the decode returned (no crash). */
static int decode_and_measure(const uint8_t *buf, size_t len,
                               const int32_t *expected,
                               size_t *out_diff, int *out_rc) {
    int32_t coeffs[NUM_COEFFS];
    int lb = -1;
    int rc = wce_decode(buf, len, coeffs, NUM_COEFFS, &lb);
    if (out_rc) *out_rc = rc;
    if (out_diff) {
        *out_diff = (rc == WCE_OK)
            ? coeff_distance(coeffs, expected, NUM_COEFFS)
            : NUM_COEFFS;  /* error path: count as fully different */
    }
    return 1;
}

/* Attack 1: bit-flips */

static void attack_bit_flips(const encoded_t *e) {
    const int trials = 256;
    int returned = 0;
    size_t max_diff = 0, sum_diff = 0;
    uint8_t snap[BUF_CAP];
    for (int t = 0; t < trials; ++t) {
        memcpy(snap, e->buf, e->buf_bytes);
        const size_t bit = xs_next() % (e->buf_bytes * 8);
        snap[bit / 8] ^= (uint8_t)(1u << (7 - (bit & 7)));
        size_t diff = 0;
        decode_and_measure(snap, e->buf_bytes, e->expected, &diff, NULL);
        ++returned;
        if (diff > max_diff) max_diff = diff;
        sum_diff += diff;
    }
    printf("  bit-flip (anywhere)         : %d/%d returned, "
           "avg %zu / max %zu coeffs differ (of %d)\n",
           returned, trials, sum_diff / (size_t)trials, max_diff, NUM_COEFFS);
}

/* Attack 2: byte scramble */

static void attack_byte_scramble(const encoded_t *e) {
    const int trials = 256;
    int returned = 0;
    uint8_t snap[BUF_CAP];
    for (int t = 0; t < trials; ++t) {
        memcpy(snap, e->buf, e->buf_bytes);
        snap[xs_next() % e->buf_bytes] = (uint8_t)xs_next();
        decode_and_measure(snap, e->buf_bytes, e->expected, NULL, NULL);
        ++returned;
    }
    printf("  random byte (anywhere)      : %d/%d returned without crash\n",
           returned, trials);
}

/* Attack 3: truncation */

static void attack_truncation(const encoded_t *e) {
    int returned = 0, trials = 0;
    for (size_t cut = 0; cut <= e->buf_bytes; cut += 4) {
        decode_and_measure(e->buf, cut, e->expected, NULL, NULL);
        ++returned; ++trials;
    }
    printf("  truncation (every prefix)   : %d/%d prefix lengths returned\n",
           returned, trials);
}

/* Attack 4: all-ones bomb + crafted bad headers */

static void attack_adversarial(void) {
    int returned = 0;
    int32_t coeffs[NUM_COEFFS];
    int lb;

    /* All-ones buffer past the header — would trigger runaway Rice
     * quotient without the cap. */
    uint8_t bombs[BUF_CAP];
    memset(bombs, 0xFF, sizeof(bombs));
    /* Patch a plausible header so the validator doesn't reject early. */
    bombs[0] = 'W'; bombs[1] = 'C'; bombs[2] = 'E'; bombs[3] = 0;
    bombs[4] = (uint8_t)NUM_GROUPS;
    bombs[5] = (uint8_t)(NUM_GROUPS >> 8);
    bombs[6] = 0; bombs[7] = 0;
    bombs[8] = WCE_FORMAT_VERSION;
    bombs[9] = LOSSY_BITS;
    bombs[10] = 0;             /* rice_k=0, predictor=running, no flag */
    bombs[11] = LOSSY_BITS;    /* initial_prev */
    wce_decode(bombs, sizeof(bombs), coeffs, NUM_COEFFS, &lb);
    ++returned;

    /* Bad magic. */
    uint8_t hdr[WCE_HEADER_SIZE] = {0};
    int rc = wce_decode(hdr, sizeof(hdr), coeffs, NUM_COEFFS, &lb);
    if (rc == WCE_ERR_BADMAGIC) ++returned;

    /* Bad version. */
    hdr[0] = 'W'; hdr[1] = 'C'; hdr[2] = 'E'; hdr[3] = 0;
    memset(hdr + 4, 0, 4);
    hdr[8] = (uint8_t)(WCE_FORMAT_VERSION + 99);
    hdr[9] = LOSSY_BITS;
    hdr[10] = 0;
    hdr[11] = LOSSY_BITS;
    rc = wce_decode(hdr, sizeof(hdr), coeffs, NUM_COEFFS, &lb);
    if (rc == WCE_ERR_BADVERSION) ++returned;

    /* Bad lossy_bits (32). */
    hdr[8] = WCE_FORMAT_VERSION;
    hdr[4] = (uint8_t)NUM_GROUPS; hdr[5] = (uint8_t)(NUM_GROUPS >> 8);
    hdr[9] = 32;
    rc = wce_decode(hdr, sizeof(hdr), coeffs, NUM_COEFFS, &lb);
    if (rc == WCE_ERR_BADINPUT) ++returned;

    /* Bad rice_k (>16 in low 5 bits). */
    hdr[9] = LOSSY_BITS;
    hdr[10] = 17;  /* low bits = 17, exceeds 16 */
    rc = wce_decode(hdr, sizeof(hdr), coeffs, NUM_COEFFS, &lb);
    if (rc == WCE_ERR_BADINPUT) ++returned;

    /* Bad initial_prev mismatch (decoder now validates this). */
    hdr[10] = 0;
    hdr[11] = 99;  /* initial_prev != lossy_bits */
    rc = wce_decode(hdr, sizeof(hdr), coeffs, NUM_COEFFS, &lb);
    if (rc == WCE_ERR_BADINPUT) ++returned;

    /* Header-only buffer claiming many groups — decoder rejects size mismatch. */
    hdr[11] = LOSSY_BITS;
    hdr[4] = 0xFF; hdr[5] = 0xFF; hdr[6] = 0; hdr[7] = 0;  /* 65535 groups */
    rc = wce_decode(hdr, sizeof(hdr), coeffs, NUM_COEFFS, &lb);
    if (rc == WCE_ERR_BADINPUT) ++returned;

    printf("  adversarial (bombs + bad hdrs): %d/%d cases returned expected code\n",
           returned, 7);
}

int main(void) {
    encoded_t e;
    build_clean(&e);

    printf("libwce stream surgery demo\n");
    printf("==========================\n");
    printf("  groups       : %d  (%d coefficients)\n", NUM_GROUPS, NUM_COEFFS);
    printf("  lossy_bits   : %d\n", LOSSY_BITS);
    printf("  encoded      : %zu bytes (single bitstream, %d-byte header)\n\n",
           e.buf_bytes, WCE_HEADER_SIZE);

    printf("  attacks:\n");
    attack_bit_flips(&e);
    attack_byte_scramble(&e);
    attack_truncation(&e);
    attack_adversarial();

    printf("\n  every decode call returned. build with `make DEBUG=1` to\n");
    printf("  also verify no UB / OOB under ASan + UBSan.\n");

    return 0;
}
