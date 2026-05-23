/*
 * libwce fuzz target.
 *
 * Two attack surfaces exercised on each fuzzed input:
 *
 *   - DECODE — feed the bytes straight to wce_decode and verify no crash
 *     or sanitizer fault. This exercises the header parser, Rice quotient
 *     cap, bpc/coeff stream readers, and CENTERED reconstruction under
 *     entirely attacker-controlled input.
 *
 *   - ROUND-TRIP — treat the bytes as raw int32 coefficients, call
 *     wce_encode then wce_decode. This exercises the cost picker, mode
 *     selection and the encode path with arbitrary coefficient values.
 *
 * The first byte of the input selects which surface: low bit chooses
 * decode-only (1) or round-trip (0); bits 1-6 of the same byte choose
 * num_coeffs (multiple of 4, capped at 256).
 *
 * Builds two ways:
 *   - Default (standalone): own main(), drives LLVMFuzzerTestOneInput
 *     with deterministic xorshift random inputs. Works with any C
 *     compiler; recommended on Apple clang which lacks libFuzzer.
 *         make fuzz
 *
 *   - libFuzzer mode: define WCE_FUZZ_LIBFUZZER, build with a clang
 *     that ships libclang_rt.fuzzer (e.g., Homebrew LLVM). libFuzzer
 *     supplies main; this file provides LLVMFuzzerTestOneInput.
 *         make fuzz-libfuzzer
 */

#include "wce.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COEFFS 256
#define OUT_CAP    (MAX_COEFFS * 6 + 64)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;

    const uint8_t hdr = data[0];
    const int     mode = hdr & 1;
    const int     submode = (hdr >> 4) & 3;  /* 0=decode, 1=rt-auto, 2=rt-forced */
    size_t        n   = (size_t)((hdr >> 1) & 0x7) * 4u + 4u;  /* 4..32 */
    if (n > MAX_COEFFS) n = MAX_COEFFS;

    if (mode == 0) {
        /* Decode-only: arbitrary attacker input. */
        if (size < 1) return 0;
        int32_t coeffs[MAX_COEFFS];
        int     lb = -1;
        (void)wce_decode(data, size, coeffs, n, &lb);
        return 0;
    }

    /* Round-trip: bytes after the header are reinterpreted as int32
     * coefficients (zero-padded if short). lossy_bits picked from byte 1. */
    if (size < 2) return 0;
    const int lossy_bits = (int)(data[1] % 32);

    int32_t coeffs[MAX_COEFFS];
    memset(coeffs, 0, sizeof(coeffs));
    const size_t avail = (size - 2) / sizeof(int32_t);
    const size_t take  = (avail < n) ? avail : n;
    if (take > 0) memcpy(coeffs, data + 2, take * sizeof(int32_t));

    uint8_t out[OUT_CAP];
    size_t  out_len = 0;

    if (submode == 2) {
        /* Round-trip with forced-mode: randomly pick a predictor/flag combo. */
        wce_encode_options_t opts = {
            .force_mode = true,
            .predictor  = (data[0] & 0x08) ? WCE_PREDICTOR_ZERO : WCE_PREDICTOR_RUNNING,
            .use_flag   = (data[0] & 0x10) != 0,
            .rice_k     = (uint8_t)(data[1] % 17),
        };
        int rc = wce_encode_with_options(coeffs, n, lossy_bits,
                                          &opts, out, sizeof(out), &out_len);
        if (rc != WCE_OK) return 0;
    } else {
        int rc = wce_encode(coeffs, n, lossy_bits, out, sizeof(out), &out_len);
        if (rc != WCE_OK) return 0;
    }

    int32_t decoded[MAX_COEFFS];
    int     lb_out = -1;
    (void)wce_decode(out, out_len, decoded, n, &lb_out);
    return 0;
}

#ifndef WCE_FUZZ_LIBFUZZER

/* Standalone driver: deterministic xorshift fills a buffer and pumps
 * variable-length prefixes into the fuzz entry point. Compatible with
 * any toolchain — no libFuzzer runtime required. */

static uint32_t xs_state = 0xCAFEBABEu;
static uint32_t xs_next(void) {
    uint32_t x = xs_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (xs_state = x);
}

int main(int argc, char **argv) {
    const int iters = (argc > 1) ? atoi(argv[1]) : 1000000;
    if (argc > 2) xs_state = (uint32_t)strtoul(argv[2], NULL, 0);
    if (xs_state == 0) xs_state = 1;

    uint8_t buf[4096];
    size_t  total_calls = 0;

    for (int i = 0; i < iters; ++i) {
        for (size_t j = 0; j < sizeof(buf); ++j) buf[j] = (uint8_t)xs_next();
        const size_t len = (xs_next() % sizeof(buf)) + 1;
        LLVMFuzzerTestOneInput(buf, len);
        ++total_calls;
    }
    fprintf(stderr, "fuzz: %zu inputs processed without crash\n", total_calls);
    return 0;
}

#endif /* WCE_FUZZ_LIBFUZZER */
