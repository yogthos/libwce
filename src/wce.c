/*
 * libwce — patent-clean replacement.
 *
 * Sign handling uses unsigned arithmetic throughout to dodge
 * signed-overflow UB at INT32_MIN (0 - (uint32_t)INT32_MIN is
 * well-defined; -(int32_t)INT32_MIN is not).
 */

#include "wce.h"

#include <limits.h>
#include <math.h>
#include <string.h>

/* Bit I/O — LSB-first.
 *
 * reg accumulates bits low-to-high; the low n bits are returned and
 * shifted out per read. Bit i of byte j is bit (8*j + i) of the stream. */

void wce_br_init(wce_bitreader_t *br, const uint8_t *data, size_t len) {
    br->cur       = data;
    br->base      = data;
    br->end       = data ? data + len : NULL;
    br->reg       = 0;
    br->bits_held = 0;
    br->truncated = false;
}

/* Pull bytes into reg until we have min_bits or hit EOF. */
static void br_refill(wce_bitreader_t *br, int min_bits) {
    while (br->bits_held < min_bits && br->bits_held <= 56) {
        if (br->end != NULL && br->cur < br->end) {
            br->reg |= ((uint64_t)(*br->cur++)) << br->bits_held;
        } else {
            br->truncated = true;
            /* Zero-pad: no byte OR'd — the bit slot is implicitly 0
             * because prior shifts pushed zeros into the high bits. */
        }
        br->bits_held += 8;
    }
}

uint32_t wce_br_read_bits(wce_bitreader_t *br, int n) {
    if (n <= 0) return 0;
    if (n > 32) n = 32;
    if (br->bits_held < n) br_refill(br, n);
    uint32_t v;
    if (n == 32) {
        v = (uint32_t)br->reg;
    } else {
        v = (uint32_t)(br->reg & (((uint64_t)1 << n) - 1ULL));
    }
    br->reg       >>= n;
    br->bits_held -= n;
    return v;
}

void wce_br_byte_align(wce_bitreader_t *br) {
    const int drop = br->bits_held & 7;
    br->reg       >>= drop;
    br->bits_held -= drop;
}

bool wce_br_truncated(const wce_bitreader_t *br) {
    return br->truncated;
}

size_t wce_br_bytes_consumed(const wce_bitreader_t *br) {
    return br->base ? (size_t)(br->cur - br->base) : 0;
}

void wce_bw_init(wce_bitwriter_t *bw, uint8_t *buf, size_t cap) {
    bw->cur       = buf;
    bw->base      = buf;
    bw->end       = buf ? buf + cap : NULL;
    bw->reg       = 0;
    bw->bits_held = 0;
    bw->overflow  = false;
}

void wce_bw_write_bits(wce_bitwriter_t *bw, uint32_t value, int n) {
    if (bw->overflow) return;
    if (n <= 0) return;
    if (n > 32) n = 32;
    /* Mask high bits so OR-into-reg is safe. */
    const uint64_t mask = (n == 32) ? 0xFFFFFFFFULL
                                    : (((uint64_t)1 << n) - 1ULL);
    bw->reg |= ((uint64_t)value & mask) << bw->bits_held;
    bw->bits_held += n;
    while (bw->bits_held >= 8) {
        if (bw->end != NULL && bw->cur < bw->end) {
            *bw->cur++ = (uint8_t)(bw->reg & 0xFFu);
        } else {
            bw->overflow = true;
        }
        bw->reg       >>= 8;
        bw->bits_held -= 8;
    }
}

void wce_bw_flush(wce_bitwriter_t *bw) {
    if (bw->bits_held > 0) {
        if (bw->end != NULL && bw->cur < bw->end) {
            *bw->cur++ = (uint8_t)(bw->reg & 0xFFu);
        } else {
            bw->overflow = true;
        }
        bw->reg       = 0;
        bw->bits_held = 0;
    }
}

bool wce_bw_overflow(const wce_bitwriter_t *bw) {
    return bw->overflow;
}

size_t wce_bw_bytes_written(const wce_bitwriter_t *bw) {
    return (size_t)(bw->cur - bw->base);
}


/* Rice-k: q = v>>k (unary ones + terminator zero), then k remainder bits.
 * Terminator-zero convention degrades truncated streams to zero rather
 * than unbounded run. Reader caps at WCE_RICE_MAX_QUOTIENT. */

void wce_bw_write_rice(wce_bitwriter_t *bw, uint32_t value, int k) {
    if (k < 0)  k = 0;
    if (k > 16) k = 16;
    uint32_t q = value >> k;
    if (q >= WCE_RICE_MAX_QUOTIENT) { bw->overflow = true; return; }
    /* Emit q one-bits, up to 31 per chunk. */
    while (q >= 31) {
        wce_bw_write_bits(bw, 0x7FFFFFFFu, 31);
        q -= 31;
    }
    if (q > 0) wce_bw_write_bits(bw, (1ULL << q) - 1u, (int)q);
    wce_bw_write_bits(bw, 0u, 1);  /* terminator */
    if (k > 0) wce_bw_write_bits(bw, value & ((1ULL << k) - 1u), k);
}

uint32_t wce_br_read_rice(wce_bitreader_t *br, int k) {
    if (k < 0)  k = 0;
    if (k > 16) k = 16;
    uint32_t q = 0;
    /* Count one-bits up to the terminator zero. Bounded by max-quotient. */
    while (q < WCE_RICE_MAX_QUOTIENT) {
        if (wce_br_read_bits(br, 1) == 0u) break;
        ++q;
    }
    if (q == WCE_RICE_MAX_QUOTIENT) return UINT32_MAX;
    uint32_t r = (k > 0) ? wce_br_read_bits(br, k) : 0u;
    return (q << k) | r;
}

uint32_t wce_zigzag_encode(int32_t v) {
    /* (v << 1) ^ (v >> 31) via unsigned ops, avoids signed-shift UB. */
    const uint32_t uv      = (uint32_t)v;
    const uint32_t sign_lo = uv >> 31;
    const uint32_t sign    = (uint32_t)0 - sign_lo;
    return (uv << 1) ^ sign;
}

int32_t wce_zigzag_decode(uint32_t u) {
    /* (u >> 1) ^ -(u & 1) */
    const uint32_t lsb  = u & 1u;
    const uint32_t mag  = u >> 1;
    return (int32_t)(mag ^ ((uint32_t)0 - lsb));
}


/* BPC computation + DPCM coding. */

static uint8_t ceil_log2_plus1(uint32_t v) {
    /* ceil(log2(v+1)): 0→0, 1→1, 2→2, 4→3, … */
    uint8_t b = 0;
    while (v > 0) { ++b; v >>= 1; }
    return b;
}

void wce_compute_bpcs(const int32_t *coeffs, size_t num_groups,
                      uint8_t *bpcs_out, uint8_t lossy_bits) {
    if (!coeffs || !bpcs_out) return;
    if (lossy_bits > 31) lossy_bits = 31;
    for (size_t g = 0; g < num_groups; ++g) {
        uint32_t m = 0;
        for (int i = 0; i < 4; ++i) {
            const int32_t c = coeffs[g * 4u + (size_t)i];
            const uint32_t abs_c = (c < 0)
                ? ((uint32_t)0 - (uint32_t)c)
                : (uint32_t)c;
            if (abs_c > m) m = abs_c;
        }
        uint8_t bpc = ceil_log2_plus1(m);
        if (bpc < lossy_bits) bpc = lossy_bits;
        if (bpc > 32) bpc = 32;
        bpcs_out[g] = bpc;
    }
}

int wce_pick_rice_k_for_bpcs(const uint8_t *bpcs, size_t num_groups,
                              int k_max) {
    if (!bpcs || num_groups < 2) return 0;
    if (k_max < 0)  k_max = 0;
    if (k_max > 16) k_max = 16;

    int best_k = -1;
    uint64_t best_bits = UINT64_MAX;
    for (int k = 0; k <= k_max; ++k) {
        uint64_t total = 0;
        bool ok = true;
        for (size_t i = 1; i < num_groups; ++i) {
            const int32_t  d  = (int32_t)bpcs[i] - (int32_t)bpcs[i - 1];
            const uint32_t u  = wce_zigzag_encode(d);
            const uint32_t q  = u >> k;
            if (q >= WCE_RICE_MAX_QUOTIENT) { ok = false; break; }
            total += (uint64_t)q + 1u + (uint64_t)k;
        }
        if (ok && total < best_bits) {
            best_bits = total;
            best_k = k;
        }
    }
    return best_k;
}

void wce_encode_bpcs_dpcm(wce_bitwriter_t *bw,
                          const uint8_t *bpcs, size_t num_groups, int k) {
    if (!bw || !bpcs) return;
    for (size_t i = 1; i < num_groups; ++i) {
        const int32_t  d = (int32_t)bpcs[i] - (int32_t)bpcs[i - 1];
        const uint32_t u = wce_zigzag_encode(d);
        wce_bw_write_rice(bw, u, k);
    }
}

/* Coeff-major: per coeff, emit nb magnitude bits, then 1 sign bit
 * iff decoded magnitude ≠ 0. */
void wce_pack_coeffs(wce_bitwriter_t *bw,
                     const int32_t *coeffs, const uint8_t *bpcs,
                     int lossy_bits, size_t num_groups) {
    if (!bw || !coeffs || !bpcs) return;
    if (lossy_bits < 0)  lossy_bits = 0;
    if (lossy_bits > 31) lossy_bits = 31;
    for (size_t g = 0; g < num_groups; ++g) {
        const int bpc = (int)bpcs[g];
        int nb = bpc - lossy_bits;
        if (nb < 0) nb = 0;
        if (nb > 32) nb = 32;
        for (int i = 0; i < 4; ++i) {
            const int32_t c = coeffs[g * 4u + (size_t)i];
            const uint32_t abs_c = (c < 0)
                ? ((uint32_t)0 - (uint32_t)c)
                : (uint32_t)c;
            const uint32_t m_shifted = abs_c >> lossy_bits;
            if (nb > 0) wce_bw_write_bits(bw, m_shifted, nb);
            if (m_shifted != 0) {
                wce_bw_write_bits(bw, (c < 0) ? 1u : 0u, 1);
            }
        }
    }
}

void wce_unpack_coeffs(wce_bitreader_t *br,
                       const uint8_t *bpcs, int lossy_bits,
                       size_t num_groups, int32_t *coeffs_out) {
    if (!br || !bpcs || !coeffs_out) return;
    if (lossy_bits < 0)  lossy_bits = 0;
    if (lossy_bits > 31) lossy_bits = 31;
    for (size_t g = 0; g < num_groups; ++g) {
        const int bpc = (int)bpcs[g];
        int nb = bpc - lossy_bits;
        if (nb < 0) nb = 0;
        if (nb > 32) nb = 32;
        for (int i = 0; i < 4; ++i) {
            uint32_t m_shifted = (nb > 0) ? wce_br_read_bits(br, nb) : 0u;
            const uint32_t mag = m_shifted << lossy_bits;
            int32_t out;
            if (m_shifted == 0) {
                out = 0;
            } else {
                const uint32_t sign = wce_br_read_bits(br, 1);
                if (mag > (uint32_t)INT32_MAX) {
                    out = sign ? INT32_MIN : INT32_MAX;
                } else {
                    out = sign ? (int32_t)((uint32_t)0 - mag) : (int32_t)mag;
                }
            }
            coeffs_out[g * 4u + (size_t)i] = out;
        }
    }
}

void wce_decode_bpcs_dpcm(wce_bitreader_t *br,
                          size_t num_groups, uint8_t initial, int k,
                          uint8_t *bpcs_out) {
    if (!br || !bpcs_out || num_groups == 0) return;
    if (initial > 32) initial = 32;
    bpcs_out[0] = initial;
    int32_t prev = (int32_t)initial;
    for (size_t i = 1; i < num_groups; ++i) {
        const uint32_t u = wce_br_read_rice(br, k);
        if (u == UINT32_MAX) {
            /* Corrupt stream — pin remaining BPCs at the last good value. */
            for (size_t j = i; j < num_groups; ++j) bpcs_out[j] = (uint8_t)prev;
            return;
        }
        const int32_t d   = wce_zigzag_decode(u);
        int32_t       cur = prev + d;
        if (cur < 0)  cur = 0;
        if (cur > 32) cur = 32;
        bpcs_out[i] = (uint8_t)cur;
        prev = cur;
    }
}


/* Top-level encode / decode. */

static void write_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v       & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t read_u32le(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* True if every BPC in [g0, g_end) equals lossy_bits (sparse block). */
static bool block_is_sparse(const uint8_t *bpcs, size_t g0, size_t g_end,
                             uint8_t lossy_bits) {
    for (size_t g = g0; g < g_end; ++g) {
        if (bpcs[g] != lossy_bits) return false;
    }
    return true;
}

/* BPC delta predictor. Picked per-band by the encoder. */
typedef enum {
    WCE_PRED_RUNNING = 0,
    WCE_PRED_ZERO    = 1
} wce_predictor_t;

/* Max Rice-k in the cost sweep. BPC deltas ≤ 64 zigzagged → k=6 suffices. */
#define WCE_PICK_K_MAX 6

/* Histogram bins for cost sweep. BPC ∈ [0,32] → running delta ∈ [-32,32]
 * (zigzag ≤ 64, 65 bins), zero residual ∈ [0,32] (33 bins). */
#define WCE_HIST_RUN_BINS  65
#define WCE_HIST_ZERO_BINS 33

/* Cost sweep over 4 mode combos × (K_MAX+1) Rice-k values.
 * One pass builds 3 histograms (RUN off, RUN on, ZERO), then bins
 * are iterated per-k to get shifted-sums S(k). Costs:
 *   RUN/off = (1+k)*N + S_run_off
 *   RUN/on  = blocks + (1+k)*(N - sparse) + S_run_on
 *   ZERO/off, ZERO/on similar — sparse groups all land in bin 0.
 * costs_out[combo][k]. */
static void compute_all_combo_costs(const uint8_t *bpcs, size_t num_groups,
                                      uint8_t lossy_bits,
                                      uint64_t costs_out[4][WCE_PICK_K_MAX + 1]) {
    uint32_t hist_run_off[WCE_HIST_RUN_BINS];
    uint32_t hist_run_on [WCE_HIST_RUN_BINS];
    uint32_t hist_zero   [WCE_HIST_ZERO_BINS];
    memset(hist_run_off, 0, sizeof(hist_run_off));
    memset(hist_run_on,  0, sizeof(hist_run_on));
    memset(hist_zero,    0, sizeof(hist_zero));

    int32_t prev_off = (int32_t)lossy_bits;
    int32_t prev_on  = (int32_t)lossy_bits;
    size_t  num_blocks = 0;
    size_t  sparse_group_count = 0;

    for (size_t g0 = 0; g0 < num_groups; g0 += WCE_BLOCK_GROUPS) {
        size_t g_end = g0 + WCE_BLOCK_GROUPS;
        if (g_end > num_groups) g_end = num_groups;
        ++num_blocks;

        const bool sparse = block_is_sparse(bpcs, g0, g_end, lossy_bits);

        if (sparse) {
            sparse_group_count += (g_end - g0);
            for (size_t g = g0; g < g_end; ++g) {
                const int32_t  d  = (int32_t)bpcs[g] - prev_off;
                const uint32_t zz = wce_zigzag_encode(d);
                if (zz < WCE_HIST_RUN_BINS) ++hist_run_off[zz];
                {
                const int32_t zd = (int32_t)bpcs[g] - (int32_t)lossy_bits;
                if ((uint32_t)zd < WCE_HIST_ZERO_BINS) ++hist_zero[zd];
            }
                prev_off = (int32_t)bpcs[g];
            }
            prev_on = (int32_t)lossy_bits;
        } else {
            for (size_t g = g0; g < g_end; ++g) {
                const uint32_t zz_off = wce_zigzag_encode((int32_t)bpcs[g] - prev_off);
                const uint32_t zz_on  = wce_zigzag_encode((int32_t)bpcs[g] - prev_on);
                if (zz_off < WCE_HIST_RUN_BINS) ++hist_run_off[zz_off];
                if (zz_on  < WCE_HIST_RUN_BINS) ++hist_run_on [zz_on];
                {
                const int32_t zd = (int32_t)bpcs[g] - (int32_t)lossy_bits;
                if ((uint32_t)zd < WCE_HIST_ZERO_BINS) ++hist_zero[zd];
            }
                prev_off = prev_on = (int32_t)bpcs[g];
            }
        }
    }

    const uint64_t total_groups       = (uint64_t)num_groups;
    const uint64_t non_sparse_groups  = total_groups - (uint64_t)sparse_group_count;
    const uint64_t blocks             = (uint64_t)num_blocks;

    for (int k = 0; k <= WCE_PICK_K_MAX; ++k) {
        uint64_t S_run_off = 0, S_run_on = 0, S_zero = 0;
        for (size_t v = 0; v < WCE_HIST_RUN_BINS; ++v) {
            const uint64_t shifted = (uint64_t)(v >> k);
            S_run_off += (uint64_t)hist_run_off[v] * shifted;
            S_run_on  += (uint64_t)hist_run_on [v] * shifted;
        }
        for (size_t v = 0; v < WCE_HIST_ZERO_BINS; ++v) {
            S_zero += (uint64_t)hist_zero[v] * (uint64_t)(v >> k);
        }
        const uint64_t k1 = 1ull + (uint64_t)k;

        /* RUN, flag-off */
        costs_out[0][k] = k1 * total_groups + S_run_off;
        /* RUN, flag-on */
        costs_out[1][k] = blocks + k1 * non_sparse_groups + S_run_on;
        /* ZERO, flag-off */
        costs_out[2][k] = k1 * total_groups + S_zero;
        /* ZERO, flag-on — shifted-sum is identical to flag-off case
         * because sparse groups all have delta 0, contributing 0 to
         * every (v>>k). The only difference is the (1+k)·count term. */
        costs_out[3][k] = blocks + k1 * non_sparse_groups + S_zero;
    }
}

int wce_encode(const int32_t *coeffs, size_t num_coeffs, int lossy_bits,
               uint8_t *out, size_t out_cap, size_t *out_len) {
    return wce_encode_with_options(coeffs, num_coeffs, lossy_bits, NULL,
                                    out, out_cap, out_len);
}

int wce_encode_with_options(const int32_t *coeffs, size_t num_coeffs,
                            int lossy_bits,
                            const wce_encode_options_t *opts,
                            uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!coeffs || !out || !out_len) return WCE_ERR_BADINPUT;
    if ((num_coeffs & 3u) != 0)      return WCE_ERR_BADINPUT;
    if (lossy_bits < 0 || lossy_bits > 31) return WCE_ERR_BADINPUT;
    if (out_cap < WCE_HEADER_SIZE)   return WCE_ERR_NOSPACE;

    const size_t num_groups = num_coeffs >> 2;

    uint8_t bpcs_inline[WCE_MAX_INLINE_GROUPS];
    if (num_groups > WCE_MAX_INLINE_GROUPS) return WCE_ERR_BADINPUT;
    uint8_t *bpcs = bpcs_inline;

    wce_compute_bpcs(coeffs, num_groups, bpcs, (uint8_t)lossy_bits);

    int rice_k = 0;
    wce_predictor_t predictor = WCE_PRED_RUNNING;
    bool use_flag = false;

    if (opts && opts->force_mode) {
        if (opts->rice_k > 16) return WCE_ERR_BADINPUT;
        if (opts->predictor != WCE_PREDICTOR_RUNNING &&
            opts->predictor != WCE_PREDICTOR_ZERO) return WCE_ERR_BADINPUT;
        rice_k    = (int)opts->rice_k;
        predictor = (opts->predictor == WCE_PREDICTOR_ZERO)
                    ? WCE_PRED_ZERO : WCE_PRED_RUNNING;
        use_flag  = opts->use_flag;
    } else if (num_groups > 0) {
        uint64_t costs[4][WCE_PICK_K_MAX + 1];
        compute_all_combo_costs(bpcs, num_groups, (uint8_t)lossy_bits, costs);

        uint64_t best_cost = UINT64_MAX;
        int      best_combo = 0;
        int      best_k     = 0;
        for (int c = 0; c < 4; ++c) {
            for (int k = 0; k <= WCE_PICK_K_MAX; ++k) {
                if (costs[c][k] < best_cost) {
                    best_cost  = costs[c][k];
                    best_combo = c;
                    best_k     = k;
                }
            }
        }
        rice_k    = best_k;
        predictor = (best_combo & 2) ? WCE_PRED_ZERO : WCE_PRED_RUNNING;
        use_flag  = (best_combo & 1) ? true : false;
    }

    const uint8_t initial_prev = (uint8_t)lossy_bits;

    wce_bitwriter_t bw;
    wce_bw_init(&bw, out + WCE_HEADER_SIZE, out_cap - WCE_HEADER_SIZE);

    int32_t prev = (int32_t)initial_prev;
    for (size_t g0 = 0; g0 < num_groups; g0 += WCE_BLOCK_GROUPS) {
        size_t g_end = g0 + WCE_BLOCK_GROUPS;
        if (g_end > num_groups) g_end = num_groups;

        bool sparse = false;
        if (use_flag) {
            sparse = block_is_sparse(bpcs, g0, g_end, (uint8_t)lossy_bits);
            wce_bw_write_bits(&bw, sparse ? 1u : 0u, 1);
            if (sparse) {
                prev = lossy_bits;
                continue;
            }
        }

        for (size_t g = g0; g < g_end; ++g) {
            uint32_t u;
            if (predictor == WCE_PRED_ZERO) {
                u = (uint32_t)((int32_t)bpcs[g] - lossy_bits);
            } else {
                const int32_t d = (int32_t)bpcs[g] - prev;
                u = wce_zigzag_encode(d);
            }
            wce_bw_write_rice(&bw, u, rice_k);
            prev = (int32_t)bpcs[g];
        }
        wce_pack_coeffs(&bw, coeffs + g0 * 4, bpcs + g0,
                        lossy_bits, g_end - g0);
    }
    wce_bw_flush(&bw);

    if (wce_bw_overflow(&bw)) return WCE_ERR_NOSPACE;

    out[0] = 'W';
    out[1] = 'C';
    out[2] = 'E';
    out[3] = 0;
    write_u32le(out + 4, (uint32_t)num_groups);
    out[8]  = (uint8_t)WCE_FORMAT_VERSION;
    out[9]  = (uint8_t)lossy_bits;
    /* rice_k in low 5 bits (0..16); bit 6 = predictor; bit 7 = flag-mode. */
    out[10] = (uint8_t)((rice_k & 0x1F)
                       | (predictor == WCE_PRED_ZERO ? 0x40 : 0)
                       | (use_flag ? 0x80 : 0));
    out[11] = initial_prev;

    *out_len = WCE_HEADER_SIZE + wce_bw_bytes_written(&bw);
    return WCE_OK;
}

int wce_decode(const uint8_t *in, size_t in_len,
               int32_t *coeffs_out, size_t num_coeffs,
               int *lossy_bits_out) {
    if (!in || !coeffs_out)          return WCE_ERR_BADINPUT;
    if (in_len < WCE_HEADER_SIZE)    return WCE_ERR_TRUNCATED;
    if (in[0] != 'W' || in[1] != 'C' || in[2] != 'E' || in[3] != 0)
        return WCE_ERR_BADMAGIC;
    if (in[8] != WCE_FORMAT_VERSION) return WCE_ERR_BADVERSION;

    const uint32_t hdr_num_groups = read_u32le(in + 4);
    const uint8_t  lossy_bits     = in[9];
    const uint8_t  rice_k_byte    = in[10];
    const uint8_t  rice_k         = (uint8_t)(rice_k_byte & 0x1F);
    const wce_predictor_t predictor = (rice_k_byte & 0x40u)
        ? WCE_PRED_ZERO : WCE_PRED_RUNNING;
    const bool     use_flag       = (rice_k_byte & 0x80u) != 0;
    const uint8_t  initial_prev   = in[11];

    if (lossy_bits > 31)                  return WCE_ERR_BADINPUT;
    if (rice_k > 16)                      return WCE_ERR_BADINPUT;
    if ((num_coeffs & 3u) != 0)           return WCE_ERR_BADINPUT;
    if ((size_t)hdr_num_groups * 4u != num_coeffs)
        return WCE_ERR_BADINPUT;

    /* initial_prev must equal lossy_bits for valid encoder-produced streams. */
    if (initial_prev != lossy_bits)       return WCE_ERR_BADINPUT;

    const size_t num_groups = (size_t)hdr_num_groups;

    uint8_t bpcs_inline[WCE_MAX_INLINE_GROUPS];
    if (num_groups > WCE_MAX_INLINE_GROUPS) return WCE_ERR_BADINPUT;

    wce_bitreader_t br;
    wce_br_init(&br, in + WCE_HEADER_SIZE, in_len - WCE_HEADER_SIZE);

    if (lossy_bits_out) *lossy_bits_out = (int)lossy_bits;
    if (num_groups == 0) return WCE_OK;

    int32_t prev = (initial_prev > 32) ? 32 : (int32_t)initial_prev;

    for (size_t g0 = 0; g0 < num_groups; g0 += WCE_BLOCK_GROUPS) {
        size_t g_end = g0 + WCE_BLOCK_GROUPS;
        if (g_end > num_groups) g_end = num_groups;

        if (use_flag) {
            const uint32_t flag = wce_br_read_bits(&br, 1);
            if (flag) {
                for (size_t g = g0; g < g_end; ++g) {
                    bpcs_inline[g] = (uint8_t)lossy_bits;
                    for (int i = 0; i < 4; ++i)
                        coeffs_out[g * 4 + (size_t)i] = 0;
                }
                prev = lossy_bits;
                continue;
            }
        }

        for (size_t g = g0; g < g_end; ++g) {
            const uint32_t u = wce_br_read_rice(&br, (int)rice_k);
            if (u == UINT32_MAX) {
                /* Corrupt — pin remaining bpcs at prev and zero coeffs. */
                for (size_t g2 = g; g2 < num_groups; ++g2) {
                    bpcs_inline[g2] = (uint8_t)prev;
                    for (int i = 0; i < 4; ++i)
                        coeffs_out[g2 * 4 + (size_t)i] = 0;
                }
                return WCE_ERR_CORRUPT;
            }
            int32_t cur;
            if (predictor == WCE_PRED_ZERO) {
                const uint32_t cur_u = (uint32_t)lossy_bits + u;
                cur = (cur_u > 32u) ? 32 : (int32_t)cur_u;
            } else {
                const int32_t d = wce_zigzag_decode(u);
                cur = prev + d;
            }
            if (cur < 0)  cur = 0;
            if (cur > 32) cur = 32;
            bpcs_inline[g] = (uint8_t)cur;
            prev = cur;
        }
        wce_unpack_coeffs(&br, bpcs_inline + g0, (int)lossy_bits,
                            g_end - g0, coeffs_out + g0 * 4);
        if (wce_br_truncated(&br)) {
            for (size_t g = g_end; g < num_groups; ++g)
                for (int i = 0; i < 4; ++i)
                    coeffs_out[g * 4 + (size_t)i] = 0;
            return WCE_ERR_TRUNCATED;
        }
    }
    return WCE_OK;
}


void wce_quantize(int32_t *coeffs, size_t num_coeffs, uint8_t lossy_bits) {
    if (!coeffs) return;
    if (lossy_bits == 0) return;
    if (lossy_bits >= 32) {
        for (size_t i = 0; i < num_coeffs; ++i) coeffs[i] = 0;
        return;
    }
    const uint32_t mask = ~(((uint32_t)1 << lossy_bits) - 1u);
    for (size_t i = 0; i < num_coeffs; ++i) {
        const int32_t c = coeffs[i];
        if (c > 0) {
            coeffs[i] = (int32_t)((uint32_t)c & mask);
        } else if (c < 0) {
            const uint32_t abs_c = (uint32_t)0 - (uint32_t)c;
            const uint32_t q = abs_c & mask;
            coeffs[i] = (int32_t)((uint32_t)0 - q);
        }
    }
}

double wce_estimate_laplacian_scale(const int32_t *coeffs, size_t num_coeffs) {
    if (!coeffs || num_coeffs == 0) return 0.0;
    double acc = 0.0;
    for (size_t i = 0; i < num_coeffs; ++i) {
        const int32_t c = coeffs[i];
        const uint32_t abs_c = (c < 0)
            ? ((uint32_t)0 - (uint32_t)c)
            : (uint32_t)c;
        acc += (double)abs_c;
    }
    return acc / (double)num_coeffs;
}

void wce_dequantize_optimal(int32_t *coeffs, size_t num_coeffs,
                            uint8_t lossy_bits, double scale_b) {
    if (!coeffs) return;
    if (lossy_bits == 0 || lossy_bits >= 32) return;
    if (!(scale_b > 0.0) || !isfinite(scale_b)) return;  /* NaN, Inf, ≤0 → no-op */

    const double step = (double)(1u << lossy_bits);
    const double u    = step / scale_b;

    if (u == 0.0) return;  /* scale_b >> step → delta → 0 */

    /* offset = b - step / (exp(u) - 1). u > 709 → exp overflows → use b.
     * u → 0 gives step/2 via the series expansion. */
    double offset_d = (u > 709.0)
        ? scale_b
        : scale_b - step / expm1(u);
    if (offset_d < 0.0) offset_d = 0.0;
    if (offset_d > step - 1.0) offset_d = step - 1.0;

    const uint32_t offset = (uint32_t)(offset_d + 0.5);
    if (offset == 0) return;

    for (size_t i = 0; i < num_coeffs; ++i) {
        const int32_t c = coeffs[i];
        if (c > 0) {
            const uint32_t mag = (uint32_t)c + offset;
            coeffs[i] = (mag > (uint32_t)INT32_MAX)
                ? INT32_MAX
                : (int32_t)mag;
        } else if (c < 0) {
            const uint32_t abs_c = (uint32_t)0 - (uint32_t)c;
            const uint32_t mag = abs_c + offset;
            if (mag >= (uint32_t)0x80000000u) {
                coeffs[i] = INT32_MIN;
            } else {
                coeffs[i] = (int32_t)((uint32_t)0 - mag);
            }
        }
    }
}
