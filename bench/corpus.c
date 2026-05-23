/*
 * Corpus generation for the bench harness.
 *
 * Synthetic Laplacian-distributed coefficient bands at several scales.
 * Real-image subbands can be added later by carving demo/Cthulhu.pgm
 * (or other PGMs) through a Haar/CDF DWT and dumping the subbands.
 */

#include "bench.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Deterministic xorshift32 — independent of libc rand state. */
typedef struct { uint32_t s; } xs32_t;
static uint32_t xs_next(xs32_t *r) {
    uint32_t x = r->s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (r->s = x);
}
static double xs_uniform(xs32_t *r) {
    /* Divide by 2^32 to produce u ∈ [0, 1). Using 0x100000000ull avoids
     * the u=1.0 edge case (0xFFFFFFFE / 0xFFFFFFFF ≈ 1.0). */
    return ((double)(xs_next(r) | 1u) / (double)0x100000000ull);
}
static int32_t laplace_i32(xs32_t *r, double scale) {
    const double u = xs_uniform(r) - 0.5;
    const double sign = (u < 0) ? -1.0 : 1.0;
    const double mag  = -scale * log(1.0 - 2.0 * fabs(u));
    double v = sign * mag;
    if (v >  2.0e9) v =  2.0e9;
    if (v < -2.0e9) v = -2.0e9;
    return (int32_t)v;
}

static void fill_laplace(int32_t *out, size_t n, double scale, uint32_t seed) {
    xs32_t r = { seed };
    size_t i;
    for (i = 0; i < n; ++i) out[i] = laplace_i32(&r, scale);
}

static int32_t *alloc_band(size_t n) {
    return (int32_t *)malloc(sizeof(int32_t) * n);
}

size_t wce_bench_corpus_build(wce_bench_corpus_t *out, size_t max) {
    size_t k = 0;

    /* Small bands — match mode_shootout's NUM_GROUPS=512 → 2048 coeffs. */
    const size_t SMALL = 2048;

    /* Larger bands — closer to a real HL subband at 1080p tile. */
    const size_t LARGE = 32768;

    struct { const char *name; size_t n; double scale; uint32_t seed; } recipes[] = {
        { "laplace_s2_2k",   SMALL,    2.0,  0xC0FFEE1u },
        { "laplace_s8_2k",   SMALL,    8.0,  0xC0FFEE2u },
        { "laplace_s32_2k",  SMALL,   32.0,  0xC0FFEE3u },
        { "laplace_s128_2k", SMALL,  128.0,  0xC0FFEE4u },
        { "laplace_s2_32k",  LARGE,    2.0,  0xC0FFEE7u },
        { "laplace_s8_32k",  LARGE,    8.0,  0xC0FFEE5u },
        { "laplace_s32_32k", LARGE,   32.0,  0xC0FFEE6u },
        { "laplace_s128_32k",LARGE,  128.0,  0xC0FFEE8u },
    };
    const size_t N = sizeof(recipes) / sizeof(recipes[0]);

    for (size_t i = 0; i < N && k < max; ++i) {
        int32_t *buf = alloc_band(recipes[i].n);
        if (!buf) continue;
        fill_laplace(buf, recipes[i].n, recipes[i].scale, recipes[i].seed);
        out[k].name   = recipes[i].name;
        out[k].coeffs = buf;
        out[k].n      = recipes[i].n;
        ++k;
    }

    return k;
}

void wce_bench_corpus_free(wce_bench_corpus_t *c) {
    free(c->coeffs);
    c->coeffs = NULL;
    c->n = 0;
}
