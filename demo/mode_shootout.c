/*
 * libwce demo: encode-mode shootout.
 *
 * The new library exposes a 4-way mode space at the bitstream level:
 *
 *     predictor ∈ { RUNNING, ZERO } × use_flag ∈ { false, true }
 *
 *   RUNNING — DPCM with zigzag delta vs the previous group's BPC.
 *             Good when BPCs evolve smoothly within a band.
 *
 *   ZERO    — predict BPC = lossy_bits, encode unsigned residual.
 *             Good for sparse bands with occasional spikes (most
 *             groups sit on the deadzone floor).
 *
 *   use_flag — 1 bit per 8-group block flags "this block is all
 *              deadzone." Cheap shortcut when whole blocks are
 *              entirely zero; costs 1 bit/block when they aren't.
 *
 * For each synthetic sub-band this demo forces every (predictor,
 * use_flag) combo via wce_encode_with_options and prints the resulting
 * total byte count. The "auto" row shows what wce_encode picks on its
 * own — that should match the cheapest forced row.
 *
 * Output rows:
 *
 *     mode             total  ratio  ok
 *     --------------   -----  -----  --
 *     RUN, flag=off     XXX    Y.Yx  Y
 *     RUN, flag=on      ...
 *     ZERO, flag=off    ...
 *     ZERO, flag=on     ...
 *     auto              ...
 */

#include "wce.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_GROUPS  512
#define NUM_COEFFS  (NUM_GROUPS * 4)
#define LOSSY_BITS  3

/* Synthetic data */

static uint32_t xs_state = 0xC0FFEEu;
static uint32_t xs_next(void) {
    uint32_t x = xs_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return xs_state = x;
}
static double xs_uniform(void) {
    return ((double)(xs_next() | 1u) / (double)0xFFFFFFFFu);
}
static int32_t laplace_sample(double scale) {
    const double u = xs_uniform() - 0.5;
    const double sign = (u < 0) ? -1.0 : 1.0;
    const double mag  = -scale * log(1.0 - 2.0 * fabs(u));
    double v = sign * mag;
    if (v >  2.0e9) v =  2.0e9;
    if (v < -2.0e9) v = -2.0e9;
    return (int32_t)v;
}

/* Encode helpers */

static int32_t  coeffs[NUM_COEFFS];
static int32_t  decoded[NUM_COEFFS];
static uint8_t  buf[NUM_COEFFS * 5 + 64];

typedef struct {
    const char *name;
    size_t total_bytes;
    int    ok;
} mode_result_t;

/* Encode + decode + verify round-trip for the supplied options. */
static void run_combo(const char *name, const wce_encode_options_t *opts,
                       mode_result_t *r) {
    size_t out_len = 0;
    int rc = wce_encode_with_options(coeffs, NUM_COEFFS, LOSSY_BITS,
                                       opts, buf, sizeof(buf), &out_len);
    r->name = name;
    if (rc != WCE_OK) { r->total_bytes = 0; r->ok = 0; return; }
    r->total_bytes = out_len;

    int lb = -1;
    rc = wce_decode(buf, out_len, decoded, NUM_COEFFS, &lb);
    if (rc != WCE_OK) { r->ok = 0; return; }

    /* Round-trip check: the decoded coeffs should equal the truncated
     * (low lossy_bits zeroed) form of the input. */
    int ok = 1;
    for (size_t i = 0; i < NUM_COEFFS && ok; ++i) {
        const int32_t c = coeffs[i];
        const uint32_t abs_c = (c < 0) ? ((uint32_t)0 - (uint32_t)c) : (uint32_t)c;
        const uint32_t mag   = (abs_c >> LOSSY_BITS) << LOSSY_BITS;
        const int32_t expect = (c >= 0) ? (int32_t)mag
                                        : (int32_t)((uint32_t)0 - mag);
        if (decoded[i] != expect) ok = 0;
    }
    r->ok = ok;
}

int main(void) {
    /* Synthesize a Laplace-distributed band that the picker will find
     * interesting — enough variance that several modes pull ahead. */
    for (size_t i = 0; i < NUM_COEFFS; ++i)
        coeffs[i] = laplace_sample(8.0);

    const wce_encode_options_t opts_run_off = {
        .force_mode = true,
        .predictor  = WCE_PREDICTOR_RUNNING,
        .use_flag   = false,
        .rice_k     = 2,
    };
    const wce_encode_options_t opts_run_on = {
        .force_mode = true,
        .predictor  = WCE_PREDICTOR_RUNNING,
        .use_flag   = true,
        .rice_k     = 2,
    };
    const wce_encode_options_t opts_zero_off = {
        .force_mode = true,
        .predictor  = WCE_PREDICTOR_ZERO,
        .use_flag   = false,
        .rice_k     = 2,
    };
    const wce_encode_options_t opts_zero_on = {
        .force_mode = true,
        .predictor  = WCE_PREDICTOR_ZERO,
        .use_flag   = true,
        .rice_k     = 2,
    };

    mode_result_t rows[5];
    run_combo("RUN, flag=off ", &opts_run_off,  &rows[0]);
    run_combo("RUN, flag=on  ", &opts_run_on,   &rows[1]);
    run_combo("ZERO, flag=off", &opts_zero_off, &rows[2]);
    run_combo("ZERO, flag=on ", &opts_zero_on,  &rows[3]);
    run_combo("auto-pick     ", NULL,           &rows[4]);

    const size_t raw_bytes = NUM_COEFFS * sizeof(int32_t);

    printf("libwce mode shootout\n");
    printf("====================\n");
    printf("  groups          : %d  (%d coefficients)\n", NUM_GROUPS, NUM_COEFFS);
    printf("  lossy_bits      : %d\n", LOSSY_BITS);
    printf("  forced rice_k   : 2  (auto-pick chooses its own)\n");
    printf("  raw int32 bytes : %zu\n\n", raw_bytes);

    printf("  mode             total   ratio   ok\n");
    printf("  --------------   -----  ------   --\n");

    size_t best = 0;
    int best_ok = 0;
    for (size_t i = 0; i < 5; ++i) {
        const double ratio = (rows[i].total_bytes > 0)
            ? (double)raw_bytes / (double)rows[i].total_bytes : 0.0;
        printf("  %s   %5zu   %5.2fx   %s\n",
               rows[i].name, rows[i].total_bytes, ratio,
               rows[i].ok ? "Y" : "N");
        if (i < 4 && rows[i].ok &&
            (!best_ok || rows[i].total_bytes < rows[best].total_bytes)) {
            best = i;
            best_ok = 1;
        }
    }

    printf("\n  best forced: %s  (%zu bytes)\n", rows[best].name,
           rows[best].total_bytes);
    if (rows[4].ok) {
        if (rows[4].total_bytes == rows[best].total_bytes) {
            printf("  auto-pick matched the best forced mode (within 1 byte).\n");
        } else if (rows[4].total_bytes < rows[best].total_bytes) {
            printf("  auto-pick beat best forced by %lld bytes (better rice_k).\n",
                   (long long)rows[best].total_bytes - (long long)rows[4].total_bytes);
        } else {
            printf("  auto-pick paid %zu bytes vs best forced — review picker.\n",
                   rows[4].total_bytes - rows[best].total_bytes);
        }
    }

    return 0;
}
