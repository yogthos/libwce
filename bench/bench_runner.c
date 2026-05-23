/*
 * bench_runner — sweep codecs × corpora × lossy_bits, emit CSV.
 *
 * For each (codec, corpus, lossy_bits) triple:
 *   1. encode once, record bytes_out
 *   2. measure encode/decode throughput by running each in a loop until
 *      we have at least MIN_RUNTIME_NS of measured time; report ns/coeff
 *   3. decode once, compute coeff-domain MSE (orig vs round-trip)
 *   4. emit a CSV row
 *
 * CSV columns:
 *   codec, corpus, n, lossy_bits, bytes_out, ratio, enc_mbps, dec_mbps,
 *   coeff_mse, ok
 *
 * `ratio` is n*4 (raw int32 bytes) divided by bytes_out — a reasonable
 * upper bound on compression. Real images would compare against raw
 * uint8/16 source instead.
 */

#define _POSIX_C_SOURCE 199309L

#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double coeff_mse(const int32_t *a, const int32_t *b, size_t n) {
    double acc = 0.0;
    size_t i;
    for (i = 0; i < n; ++i) {
        double d = (double)a[i] - (double)b[i];
        acc += d * d;
    }
    return acc / (double)n;
}

#define MIN_RUNTIME_NS 50000000ull   /* 50 ms per measurement */
#define MAX_ITERS      100000u

/* Measure throughput by running `op` repeatedly until MIN_RUNTIME_NS
 * elapses. Returns megabytes-per-second based on coeff_bytes per call. */
static double measure_mbps(int (*op)(void), size_t coeff_bytes) {
    uint64_t t0 = now_ns();
    uint64_t elapsed = 0;
    uint64_t iters = 0;
    while (elapsed < MIN_RUNTIME_NS && iters < MAX_ITERS) {
        if (op() != WCE_BENCH_OK) return 0.0;
        ++iters;
        elapsed = now_ns() - t0;
    }
    if (elapsed == 0) return 0.0;
    /* bytes/sec → mb/sec */
    double bytes = (double)iters * (double)coeff_bytes;
    return (bytes * 1e3) / (double)elapsed; /* (bytes/ns)*1e3 = MB/s */
}

/* Per-run state, since measure_mbps takes a nullary fn pointer. */
static const wce_bench_codec_t *g_codec;
static const wce_bench_corpus_t *g_corpus;
static int g_lossy_bits;
static uint8_t *g_enc_buf;
static size_t   g_enc_cap;
static size_t   g_enc_len;
static int32_t *g_dec_buf;

static int op_encode(void) {
    return g_codec->encode(g_corpus->coeffs, g_corpus->n, g_lossy_bits,
                           g_enc_buf, g_enc_cap, &g_enc_len);
}
static int op_decode(void) {
    return g_codec->decode(g_enc_buf, g_enc_len, g_dec_buf,
                           g_corpus->n, g_lossy_bits);
}

static void run_one(const wce_bench_codec_t *codec,
                    const wce_bench_corpus_t *corpus,
                    int lossy_bits, FILE *out) {
    const size_t coeff_bytes = corpus->n * sizeof(int32_t);
    const size_t enc_cap     = coeff_bytes * 4 + 256;

    g_codec = codec;
    g_corpus = corpus;
    g_lossy_bits = lossy_bits;
    g_enc_cap = enc_cap;
    g_enc_buf = (uint8_t *)malloc(enc_cap);
    g_dec_buf = (int32_t *)malloc(coeff_bytes);
    g_enc_len = 0;

    int rc = op_encode();
    if (rc == WCE_BENCH_ERR_UNIMPLEMENTED) {
        fprintf(out, "%s,%s,%zu,%d,,,,,,unimplemented\n",
                codec->name, corpus->name, corpus->n, lossy_bits);
        goto done;
    }
    if (rc != WCE_BENCH_OK) {
        fprintf(out, "%s,%s,%zu,%d,,,,,,encode_err_%d\n",
                codec->name, corpus->name, corpus->n, lossy_bits, rc);
        goto done;
    }
    const size_t bytes_out = g_enc_len;

    rc = op_decode();
    if (rc != WCE_BENCH_OK) {
        fprintf(out, "%s,%s,%zu,%d,%zu,,,,,decode_err_%d\n",
                codec->name, corpus->name, corpus->n, lossy_bits,
                bytes_out, rc);
        goto done;
    }
    const double mse = coeff_mse(corpus->coeffs, g_dec_buf, corpus->n);

    double enc_mbps = measure_mbps(op_encode, coeff_bytes);
    double dec_mbps = measure_mbps(op_decode, coeff_bytes);

    double ratio = (double)coeff_bytes / (double)bytes_out;

    fprintf(out, "%s,%s,%zu,%d,%zu,%.2f,%.1f,%.1f,%.3f,ok\n",
            codec->name, corpus->name, corpus->n, lossy_bits,
            bytes_out, ratio, enc_mbps, dec_mbps, mse);
    fflush(out);

done:
    free(g_enc_buf); g_enc_buf = NULL;
    free(g_dec_buf); g_dec_buf = NULL;
}

int main(int argc, char **argv) {
    const char *out_path = (argc > 1) ? argv[1] : NULL;
    FILE *out = out_path ? fopen(out_path, "w") : stdout;
    if (!out) { perror(out_path); return 1; }

    const wce_bench_codec_t *codecs[] = {
        &wce_bench_codec_new,
    };
    const size_t NCODECS = sizeof(codecs) / sizeof(codecs[0]);

    wce_bench_corpus_t corpora[16];
    const size_t NCORPORA = wce_bench_corpus_build(corpora, 16);

    const int lossy_bits_grid[] = { 0, 2, 4, 6 };
    const size_t NL = sizeof(lossy_bits_grid) / sizeof(lossy_bits_grid[0]);

    fprintf(out, "codec,corpus,n,lossy_bits,bytes_out,ratio,enc_mbps,dec_mbps,coeff_mse,status\n");

    for (size_t ci = 0; ci < NCODECS; ++ci) {
        for (size_t ki = 0; ki < NCORPORA; ++ki) {
            for (size_t li = 0; li < NL; ++li) {
                run_one(codecs[ci], &corpora[ki], lossy_bits_grid[li], out);
            }
        }
    }

    for (size_t i = 0; i < NCORPORA; ++i) wce_bench_corpus_free(&corpora[i]);
    if (out != stdout) fclose(out);
    return 0;
}
