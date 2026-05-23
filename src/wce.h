/*
 * libwce — Wavelet Coefficient Entropy codec.
 *
 * Zero-dependency C99. No malloc, no globals. Patent-clean:
 * avoids JPEG-XS substream layout, XS prediction, XS deadzone
 * reconstruction, and all ANS-family entropy coding.
 */

#ifndef WCE_H
#define WCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WCE_VERSION_MAJOR 0
#define WCE_VERSION_MINOR 2
#define WCE_VERSION_PATCH 0

/* Wire-format version. Increment on any bitstream-incompatible change. */
#define WCE_FORMAT_VERSION 4u

/* Groups per sparse-flag block. 8 = 32 coefficients per block. */
#define WCE_BLOCK_GROUPS 8

/* Max groups the encode/decode functions accept (stack-allocated BPC buffer).
 * Equates to 65536 coefficients. Callers needing larger bands must split. */
#define WCE_MAX_INLINE_GROUPS 16384

/* Cap on Rice unary-run length. Decoder returns UINT32_MAX on hit. */
#ifndef WCE_RICE_MAX_QUOTIENT
#define WCE_RICE_MAX_QUOTIENT 256u
#endif


/* Bit I/O — LSB-first, non-owning buffers. Truncated reads zero-pad;
 * writes past end set overflow and discard. */

typedef struct {
    const uint8_t *cur;
    const uint8_t *base;
    const uint8_t *end;
    uint64_t       reg;
    int            bits_held;
    bool           truncated;
} wce_bitreader_t;

typedef struct {
    uint8_t *cur;
    uint8_t *base;
    uint8_t *end;
    uint64_t reg;
    int      bits_held;
    bool     overflow;
} wce_bitwriter_t;

void     wce_br_init(wce_bitreader_t *br, const uint8_t *data, size_t len);
uint32_t wce_br_read_bits(wce_bitreader_t *br, int n);
void     wce_br_byte_align(wce_bitreader_t *br);
bool     wce_br_truncated(const wce_bitreader_t *br);
size_t   wce_br_bytes_consumed(const wce_bitreader_t *br);

void   wce_bw_init(wce_bitwriter_t *bw, uint8_t *buf, size_t cap);
void   wce_bw_write_bits(wce_bitwriter_t *bw, uint32_t value, int n);
void   wce_bw_flush(wce_bitwriter_t *bw);
bool   wce_bw_overflow(const wce_bitwriter_t *bw);
size_t wce_bw_bytes_written(const wce_bitwriter_t *bw);


/* Rice-k: quotient (unary) + k remainder bits. k ∈ [0,16].
 * Zigzag: signed → unsigned (0→0, -1→1, 1→2, …) for Rice coding. */

void     wce_bw_write_rice(wce_bitwriter_t *bw, uint32_t value, int k);
uint32_t wce_br_read_rice(wce_bitreader_t *br, int k);

uint32_t wce_zigzag_encode(int32_t v);
int32_t  wce_zigzag_decode(uint32_t u);


/* BPC per group of 4 coeffs: ceil(log2(max|coeff| + 1)), clamped to
 * ≥ lossy_bits. DPCM deltas between groups are then Rice-coded. */

void wce_compute_bpcs(const int32_t *coeffs, size_t num_groups,
                      uint8_t *bpcs_out, uint8_t lossy_bits);

/* Pick the Rice-k that minimises bits for the given delta sequence.
 * Searches k in [0, k_max] inclusive. Returns the best k. */
int wce_pick_rice_k_for_bpcs(const uint8_t *bpcs, size_t num_groups,
                              int k_max);

/* Encode (num_groups-1) deltas: zigzag(bpc[i] - bpc[i-1]) Rice-k coded. */
void wce_encode_bpcs_dpcm(wce_bitwriter_t *bw,
                          const uint8_t *bpcs, size_t num_groups, int k);

/* Decode (num_groups-1) deltas, applying them to the supplied initial
 * value. bpcs_out[0] = initial, bpcs_out[i] = bpcs_out[i-1] + delta.
 * Clamps each step into [0, 32] — anything out of range indicates a
 * corrupt stream and we cap rather than crash. */
void wce_decode_bpcs_dpcm(wce_bitreader_t *br,
                          size_t num_groups, uint8_t initial, int k,
                          uint8_t *bpcs_out);


/* Pack/unpack: coeff-major (per-coeff: magnitude bits, then sign bit
 * if magnitude ≠ 0). Signs interleaved with magnitudes, not in a
 * separate substream. */

void wce_pack_coeffs(wce_bitwriter_t *bw,
                     const int32_t *coeffs, const uint8_t *bpcs,
                     int lossy_bits, size_t num_groups);

void wce_unpack_coeffs(wce_bitreader_t *br,
                       const uint8_t *bpcs, int lossy_bits,
                       size_t num_groups, int32_t *coeffs_out);


/* Top-level codec. num_coeffs must be a multiple of 4.
 *
 * Header: magic "WCE\0", u32le num_groups, u8 version, u8 lossy_bits,
 * u8 rice_k|predictor|flag, u8 initial_bpc. Then variable payload. */

#define WCE_HEADER_SIZE 12

#define WCE_OK                0
#define WCE_ERR_NOSPACE      -1
#define WCE_ERR_BADINPUT     -2
#define WCE_ERR_BADMAGIC     -3
#define WCE_ERR_BADVERSION   -4
#define WCE_ERR_TRUNCATED    -5
#define WCE_ERR_CORRUPT      -6

int wce_encode(const int32_t *coeffs, size_t num_coeffs, int lossy_bits,
               uint8_t *out, size_t out_cap, size_t *out_len);

int wce_decode(const uint8_t *in, size_t in_len,
               int32_t *coeffs_out, size_t num_coeffs,
               int *lossy_bits_out);


/* Force specific mode. NULL (or !force_mode) → auto-pick.
 * Useful for benchmarking; production callers use wce_encode. */

#define WCE_PREDICTOR_RUNNING 0   /* zigzag delta vs running prev BPC */
#define WCE_PREDICTOR_ZERO    1   /* unsigned residual vs lossy_bits */

typedef struct {
    bool    force_mode;   /* false ⇒ all other fields ignored, auto-pick */
    uint8_t predictor;    /* WCE_PREDICTOR_RUNNING or WCE_PREDICTOR_ZERO */
    bool    use_flag;     /* true ⇒ emit 1 flag bit per 8-group block */
    uint8_t rice_k;       /* 0..16 */
} wce_encode_options_t;

int wce_encode_with_options(const int32_t *coeffs, size_t num_coeffs,
                            int lossy_bits,
                            const wce_encode_options_t *opts,
                            uint8_t *out, size_t out_cap, size_t *out_len);

/* Uniform scalar quantizer, power-of-2 step. Truncate toward zero.
 * Symmetric: zero stays zero, no deadzone widening. No-op at lb=0;
 * zeros everything at lb≥32. */
void wce_quantize(int32_t *coeffs, size_t num_coeffs, uint8_t lossy_bits);

/* Callers wanting grid-truncated values skip reconstruction.
 * For minimum-MSE, use wce_dequantize_optimal with scale estimated
 * from the unquantized band. */


/* Lloyd-Max optimal reconstruction for Laplacian source.
 *
 * offset = b - step / (exp(step/b) - 1)  (scale_b must come from
 * unquantized coefficients). */

/* Mean absolute value — the maximum-likelihood estimator of b for a
 * zero-mean Laplacian source. Returns 0.0 on empty input. */
double wce_estimate_laplacian_scale(const int32_t *coeffs, size_t num_coeffs);

/* In-place optimal reconstruction. lossy_bits must match wce_quantize.
 * scale_b <= 0 falls through as no-op. */
void wce_dequantize_optimal(int32_t *coeffs, size_t num_coeffs,
                            uint8_t lossy_bits, double scale_b);


#ifdef __cplusplus
}
#endif

#endif /* WCE_H */
