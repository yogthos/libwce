/*
 * libwce bench adapter.
 *
 * Encodes via wce_encode, decodes via wce_decode, then applies OPTIMAL
 * (Laplacian Lloyd-Max) reconstruction with a scale estimated from the
 * decoded coefficients. The bench has no out-of-band channel for the
 * encoder to transmit scale_b, so we estimate post-decode — slightly
 * biased downward by the truncate, but consistent and good enough for
 * a coefficient-MSE signal.
 */

#include "bench.h"
#include "wce.h"

static int new_encode(const int32_t *coeffs, size_t n, int lossy_bits,
                      uint8_t *out, size_t out_cap, size_t *out_len) {
    int rc = wce_encode(coeffs, n, lossy_bits, out, out_cap, out_len);
    if (rc == WCE_OK)                return WCE_BENCH_OK;
    if (rc == WCE_ERR_NOSPACE)       return WCE_BENCH_ERR_NOSPACE;
    return WCE_BENCH_ERR_BADINPUT;
}

static int new_decode(const uint8_t *in, size_t in_len, int32_t *coeffs_out,
                      size_t n, int lossy_bits) {
    int stored_lb = -1;
    int rc = wce_decode(in, in_len, coeffs_out, n, &stored_lb);
    if (rc != WCE_OK)                       return WCE_BENCH_ERR_BADINPUT;
    if (stored_lb != lossy_bits)            return WCE_BENCH_ERR_BADINPUT;
    const double scale_b = wce_estimate_laplacian_scale(coeffs_out, n);
    wce_dequantize_optimal(coeffs_out, n, (uint8_t)lossy_bits, scale_b);
    return WCE_BENCH_OK;
}

const wce_bench_codec_t wce_bench_codec_new = {
    .name   = "wce",
    .encode = new_encode,
    .decode = new_decode,
};
