/*
 * bench/bench.h — benchmark harness interface.
 *
 * Single-codec benchmark for libwce. The runner sweeps the codec over
 * a built-in synthetic Laplacian corpus × lossy_bits values and emits
 * CSV: bytes_out, ratio, encode MB/s, decode MB/s, coefficient MSE.
 *
 * The codec_t struct is kept as a function-pointer table so the harness
 * can be retargeted at a different implementation without touching the
 * runner.
 */

#ifndef WCE_BENCH_H
#define WCE_BENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes. */
#define WCE_BENCH_OK              0
#define WCE_BENCH_ERR_NOSPACE    -1
#define WCE_BENCH_ERR_BADINPUT   -2
#define WCE_BENCH_ERR_UNIMPLEMENTED -3

typedef struct {
    const char *name;

    /* Encode `n` int32 coefficients at the given lossy_bits into `out`.
     * On success writes total byte count to *out_len. */
    int (*encode)(const int32_t *coeffs, size_t n, int lossy_bits,
                  uint8_t *out, size_t out_cap, size_t *out_len);

    /* Decode back to `n` coefficients. `lossy_bits` matches encode. */
    int (*decode)(const uint8_t *in, size_t in_len, int32_t *coeffs_out,
                  size_t n, int lossy_bits);
} wce_bench_codec_t;

extern const wce_bench_codec_t wce_bench_codec_new;


/* Corpus = a named array of int32 coefficients. */
typedef struct {
    const char *name;
    int32_t    *coeffs;
    size_t      n;
} wce_bench_corpus_t;

/* Build the built-in corpora. Caller frees with wce_bench_corpus_free.
 * Returns count, fills `out` (caller-allocated, must hold >= max). */
size_t wce_bench_corpus_build(wce_bench_corpus_t *out, size_t max);
void   wce_bench_corpus_free(wce_bench_corpus_t *c);


#ifdef __cplusplus
}
#endif

#endif /* WCE_BENCH_H */
