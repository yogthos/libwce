/*
 * libwce demo: end-to-end image compression.
 *
 * Pipeline:
 *   1. Read 8-bit grayscale PGM (P5).
 *   2. 1-level Haar wavelet transform → LL, HL, LH, HH sub-bands.
 *   3. Per sub-band, run wce_encode at the preset's lossy_bits.
 *   4. Decode each sub-band, apply CENTERED reconstruction, inverse Haar.
 *   5. Write reconstructed PGM; print PSNR + compressed size.
 *
 * Container format (.wce):
 *   [0..3]   magic "WCE3"
 *   [4..5]   width  (u16 LE)
 *   [6..7]   height (u16 LE)
 *   [8..11]  per-band lossy_bits (LL, HL, LH, HH)
 *   [12..43] per-band scale_b (4 × IEEE 754 double LE) — Laplacian
 *            scale estimated by the encoder, used by the decoder for
 *            wce_dequantize_optimal reconstruction
 *   [44..59] per-band payload bytes (4 × u32 LE)
 *   [60..]   concatenated LL.wce, HL.wce, LH.wce, HH.wce bitstreams
 *
 * Usage:  image_compress <input.pgm> [output.pgm]
 */

#include "wce.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PGM I/O */

typedef struct {
    int w, h;
    uint8_t *pixels;
} pgm_t;

static int pgm_read(const char *path, pgm_t *img) {
    FILE *f = fopen(path, "rb");
    int maxval = 0;
    if (!f) { perror(path); return -1; }
    rewind(f);
    {
        char magic[3] = {0};
        if (fread(magic, 1, 2, f) != 2 ||
            magic[0] != 'P' || magic[1] != '5') {
            fprintf(stderr, "%s: not a P5 PGM\n", path);
            fclose(f); return -1;
        }
    }
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) { fclose(f); return -1; }
        if (c == '#') { while ((c = fgetc(f)) != EOF && c != '\n') {} continue; }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        ungetc(c, f); break;
    }
    if (fscanf(f, "%d %d %d", &img->w, &img->h, &maxval) != 3) {
        fclose(f); return -1;
    }
    fgetc(f);
    if (maxval != 255) {
        fprintf(stderr, "%s: only maxval=255 supported (got %d)\n",
                path, maxval);
        fclose(f); return -1;
    }
    img->pixels = (uint8_t *)malloc((size_t)img->w * (size_t)img->h);
    if (!img->pixels) { fclose(f); return -1; }
    if (fread(img->pixels, 1, (size_t)img->w * (size_t)img->h, f)
            != (size_t)img->w * (size_t)img->h) {
        free(img->pixels); fclose(f); return -1;
    }
    fclose(f);
    return 0;
}

static int pgm_write(const char *path, const pgm_t *img) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    fprintf(f, "P5\n%d %d\n255\n", img->w, img->h);
    fwrite(img->pixels, 1, (size_t)img->w * (size_t)img->h, f);
    fclose(f);
    return 0;
}

/* Haar wavelet (1-level, in-place) */

static void haar_forward_1d(double *row, int n, double *scratch) {
    const int half = n / 2;
    const double s = 1.0 / sqrt(2.0);
    int i;
    for (i = 0; i < half; ++i) {
        const double a = row[2*i];
        const double b = row[2*i + 1];
        scratch[i]        = (a + b) * s;
        scratch[half + i] = (a - b) * s;
    }
    if (n & 1) scratch[half + half] = row[n - 1];  /* odd: copy lone element to H */
    memcpy(row, scratch, sizeof(double) * (size_t)n);
}

static void haar_inverse_1d(double *row, int n, double *scratch) {
    const int half = n / 2;
    const double s = 1.0 / sqrt(2.0);
    int i;
    for (i = 0; i < half; ++i) {
        const double L = row[i];
        const double H = row[half + i];
        scratch[2*i]     = (L + H) * s;
        scratch[2*i + 1] = (L - H) * s;
    }
    if (n & 1) scratch[n - 1] = row[half + half];  /* odd: restore lone element */
    memcpy(row, scratch, sizeof(double) * (size_t)n);
}

static void haar_forward_2d(double *buf, int w, int h) {
    double *scratch = (double *)malloc(sizeof(double) * (size_t)(w > h ? w : h));
    int i;
    for (i = 0; i < h; ++i) haar_forward_1d(buf + (size_t)i * w, w, scratch);
    for (i = 0; i < w; ++i) {
        int j;
        double tmp[2048];
        if (h > 2048) { fprintf(stderr, "haar_forward_2d: column too tall\n"); exit(1); }
        for (j = 0; j < h; ++j) tmp[j] = buf[(size_t)j * w + i];
        haar_forward_1d(tmp, h, scratch);
        for (j = 0; j < h; ++j) buf[(size_t)j * w + i] = tmp[j];
    }
    free(scratch);
}

static void haar_inverse_2d(double *buf, int w, int h) {
    double *scratch = (double *)malloc(sizeof(double) * (size_t)(w > h ? w : h));
    int i;
    for (i = 0; i < w; ++i) {
        int j;
        double tmp[2048];
        if (h > 2048) { fprintf(stderr, "haar_inverse_2d: column too tall\n"); exit(1); }
        for (j = 0; j < h; ++j) tmp[j] = buf[(size_t)j * w + i];
        haar_inverse_1d(tmp, h, scratch);
        for (j = 0; j < h; ++j) buf[(size_t)j * w + i] = tmp[j];
    }
    for (i = 0; i < h; ++i) haar_inverse_1d(buf + (size_t)i * w, w, scratch);
    free(scratch);
}

/* Sub-band coding */

typedef struct {
    int      cols, rows;
    uint8_t  lossy_bits;
    double   scale_b;        /* estimated Laplacian scale, sent in container */
    uint8_t *buf;
    size_t   buf_bytes;
    size_t   buf_cap;
    size_t   num_coeffs;     /* padded to multiple of 4 */
} subband_codec_t;

static void subband_init(subband_codec_t *sb, int cols, int rows,
                         uint8_t lossy_bits) {
    sb->cols = cols;
    sb->rows = rows;
    sb->lossy_bits = lossy_bits;
    sb->num_coeffs = ((size_t)cols * (size_t)rows + 3u) & ~(size_t)3u;
    /* Generous upper bound: header + ~33 bits per coeff in the worst case. */
    sb->buf_cap   = sb->num_coeffs * 5u + 64u;
    sb->buf       = (uint8_t *)malloc(sb->buf_cap);
    sb->buf_bytes = 0;
}

static void subband_free(subband_codec_t *sb) {
    free(sb->buf);
    sb->buf = NULL;
}

/* Encode one band: pad to multiple of 4, estimate Laplacian scale on the
 * pre-quantize coefficients (decoder needs it for OPTIMAL recon), then
 * hand to wce_encode. */
static int subband_encode(subband_codec_t *sb, int32_t *coeffs) {
    size_t real = (size_t)sb->cols * (size_t)sb->rows;
    for (; real < sb->num_coeffs; ++real) coeffs[real] = 0;
    sb->scale_b = wce_estimate_laplacian_scale(coeffs, sb->num_coeffs);
    int rc = wce_encode(coeffs, sb->num_coeffs, (int)sb->lossy_bits,
                        sb->buf, sb->buf_cap, &sb->buf_bytes);
    return rc;
}

/* Decode + Lloyd-Max optimal reconstruction. */
static int subband_decode(const subband_codec_t *sb, int32_t *coeffs_out) {
    int lb_out = -1;
    int rc = wce_decode(sb->buf, sb->buf_bytes, coeffs_out,
                        sb->num_coeffs, &lb_out);
    if (rc != WCE_OK) return rc;
    wce_dequantize_optimal(coeffs_out, sb->num_coeffs, sb->lossy_bits,
                           sb->scale_b);
    return WCE_OK;
}

/* Container file */

#define DEMO_CONTAINER_HEADER 60

static void write_u16le(FILE *f, uint16_t v) {
    fputc((int)(v & 0xFF),        f);
    fputc((int)((v >> 8) & 0xFF), f);
}
static void write_u32le(FILE *f, uint32_t v) {
    fputc((int)(v & 0xFF),         f);
    fputc((int)((v >> 8)  & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f);
    fputc((int)((v >> 24) & 0xFF), f);
}
static void write_double_le(FILE *f, double v) {
    /* On both target platforms (x86-64 Linux, arm64 macOS) double is
     * IEEE 754 little-endian. Write the underlying byte representation. */
    uint8_t bytes[8];
    memcpy(bytes, &v, sizeof(bytes));
    fwrite(bytes, 1, 8, f);
}

static size_t write_wce_file(const char *path, int W, int H,
                             const subband_codec_t *sb_ll,
                             const subband_codec_t *sb_hl,
                             const subband_codec_t *sb_lh,
                             const subband_codec_t *sb_hh) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    fwrite("WCE3", 1, 4, f);
    write_u16le(f, (uint16_t)W);
    write_u16le(f, (uint16_t)H);
    fputc(sb_ll->lossy_bits, f);
    fputc(sb_hl->lossy_bits, f);
    fputc(sb_lh->lossy_bits, f);
    fputc(sb_hh->lossy_bits, f);
    const subband_codec_t *bands[4] = {sb_ll, sb_hl, sb_lh, sb_hh};
    int i;
    for (i = 0; i < 4; ++i) write_double_le(f, bands[i]->scale_b);
    for (i = 0; i < 4; ++i) write_u32le(f, (uint32_t)bands[i]->buf_bytes);
    for (i = 0; i < 4; ++i) fwrite(bands[i]->buf, 1, bands[i]->buf_bytes, f);
    const long end = ftell(f);
    fclose(f);
    return end < 0 ? 0 : (size_t)end;
}

/* Sub-band layout */

static void extract_subband(const double *full, int W, int H,
                            int x0, int y0, int sw, int sh,
                            int32_t *out, double scale) {
    (void)H;
    int y;
    for (y = 0; y < sh; ++y) {
        int x;
        for (x = 0; x < sw; ++x) {
            const double v = full[(size_t)(y0 + y) * W + (x0 + x)] * scale;
            const double r = v < 0 ? v - 0.5 : v + 0.5;
            out[(size_t)y * sw + x] = (int32_t)r;
        }
    }
}

static void inject_subband(double *full, int W, int H,
                           int x0, int y0, int sw, int sh,
                           const int32_t *src, double scale) {
    (void)H;
    int y;
    for (y = 0; y < sh; ++y) {
        int x;
        for (x = 0; x < sw; ++x) {
            full[(size_t)(y0 + y) * W + (x0 + x)] =
                (double)src[(size_t)y * sw + x] / scale;
        }
    }
}

/* PSNR */

static double psnr(const pgm_t *a, const pgm_t *b) {
    const size_t n = (size_t)a->w * (size_t)a->h;
    double mse = 0.0;
    size_t i;
    for (i = 0; i < n; ++i) {
        const double d = (double)a->pixels[i] - (double)b->pixels[i];
        mse += d * d;
    }
    mse /= (double)n;
    if (mse <= 0.0) return INFINITY;
    return 10.0 * log10(255.0 * 255.0 / mse);
}

/* Driver */

typedef struct {
    const char *name;
    const char *suffix;
    uint8_t     lb_ll, lb_hl, lb_lh, lb_hh;
} preset_t;

typedef struct {
    const char *name;
    size_t      total_bytes;     /* sum of per-band encoded bytes */
    size_t      file_bytes;      /* on-disk .wce file including header */
    double      ratio;           /* raw / file_bytes */
    double      psnr_db;
    size_t      sb_totals[4];    /* LL, HL, LH, HH */
    char        pgm_path[256];
    char        wce_path[256];
} preset_result_t;

static int run_preset(const pgm_t *orig, const double *dwt_input,
                      int W, int H, int hw, int hh,
                      double scale_ll, double scale_d,
                      const preset_t *p, const char *base_out_path,
                      preset_result_t *out) {
    const size_t sb_pixels = (size_t)hw * (size_t)hh;
    const size_t sb_alloc  = (sb_pixels + 3u) & ~(size_t)3u;

    int32_t *ll = (int32_t *)calloc(sb_alloc, sizeof(int32_t));
    int32_t *hl = (int32_t *)calloc(sb_alloc, sizeof(int32_t));
    int32_t *lh = (int32_t *)calloc(sb_alloc, sizeof(int32_t));
    int32_t *hh_band = (int32_t *)calloc(sb_alloc, sizeof(int32_t));
    if (!ll || !hl || !lh || !hh_band) {
        fprintf(stderr, "run_preset: out of memory\n");
        free(ll); free(hl); free(lh); free(hh_band);
        return -1;
    }

    extract_subband(dwt_input, W, H, 0,  0,  hw, hh, ll,      scale_ll);
    extract_subband(dwt_input, W, H, hw, 0,  hw, hh, hl,      scale_d);
    extract_subband(dwt_input, W, H, 0,  hh, hw, hh, lh,      scale_d);
    extract_subband(dwt_input, W, H, hw, hh, hw, hh, hh_band, scale_d);

    subband_codec_t sb_ll, sb_hl, sb_lh, sb_hh;
    subband_init(&sb_ll, hw, hh, p->lb_ll);
    subband_init(&sb_hl, hw, hh, p->lb_hl);
    subband_init(&sb_lh, hw, hh, p->lb_lh);
    subband_init(&sb_hh, hw, hh, p->lb_hh);

    int rc = subband_encode(&sb_ll, ll);
    if (rc != WCE_OK) { fprintf(stderr, "encode LL failed: %d\n", rc); goto cleanup_preset; }
    rc = subband_encode(&sb_hl, hl);
    if (rc != WCE_OK) { fprintf(stderr, "encode HL failed: %d\n", rc); goto cleanup_preset; }
    rc = subband_encode(&sb_lh, lh);
    if (rc != WCE_OK) { fprintf(stderr, "encode LH failed: %d\n", rc); goto cleanup_preset; }
    rc = subband_encode(&sb_hh, hh_band);
    if (rc != WCE_OK) { fprintf(stderr, "encode HH failed: %d\n", rc); goto cleanup_preset; }

    int32_t *ll_d = (int32_t *)calloc(sb_alloc, sizeof(int32_t));
    int32_t *hl_d = (int32_t *)calloc(sb_alloc, sizeof(int32_t));
    int32_t *lh_d = (int32_t *)calloc(sb_alloc, sizeof(int32_t));
    int32_t *hh_d = (int32_t *)calloc(sb_alloc, sizeof(int32_t));
    if (!ll_d || !hl_d || !lh_d || !hh_d) {
        fprintf(stderr, "run_preset: out of memory (decoder)\n");
        free(ll_d); free(hl_d); free(lh_d); free(hh_d);
        goto cleanup_preset;
    }
    rc = subband_decode(&sb_ll, ll_d);
    if (rc != WCE_OK) { fprintf(stderr, "decode LL failed: %d\n", rc); goto cleanup_decode; }
    rc = subband_decode(&sb_hl, hl_d);
    if (rc != WCE_OK) { fprintf(stderr, "decode HL failed: %d\n", rc); goto cleanup_decode; }
    rc = subband_decode(&sb_lh, lh_d);
    if (rc != WCE_OK) { fprintf(stderr, "decode LH failed: %d\n", rc); goto cleanup_decode; }
    rc = subband_decode(&sb_hh, hh_d);
    if (rc != WCE_OK) { fprintf(stderr, "decode HH failed: %d\n", rc); goto cleanup_decode; }

    double *recon_buf = (double *)calloc(sizeof(double),
                                          (size_t)W * (size_t)H);
    if (!recon_buf) { goto cleanup_decode; }
    inject_subband(recon_buf, W, H, 0,  0,  hw, hh, ll_d, scale_ll);
    inject_subband(recon_buf, W, H, hw, 0,  hw, hh, hl_d, scale_d);
    inject_subband(recon_buf, W, H, 0,  hh, hw, hh, lh_d, scale_d);
    inject_subband(recon_buf, W, H, hw, hh, hw, hh, hh_d, scale_d);
    haar_inverse_2d(recon_buf, W, H);

    pgm_t recon;
    recon.w = W; recon.h = H;
    recon.pixels = (uint8_t *)malloc((size_t)W * (size_t)H);
    {
        size_t i;
        for (i = 0; i < (size_t)W * (size_t)H; ++i) {
            double v = recon_buf[i] + 128.0;
            if (v < 0) v = 0; else if (v > 255) v = 255;
            recon.pixels[i] = (uint8_t)(v + 0.5);
        }
    }

    {
        const char *dot = strrchr(base_out_path, '.');
        const size_t stem_len = dot ? (size_t)(dot - base_out_path)
                                    : strlen(base_out_path);
        snprintf(out->pgm_path, sizeof(out->pgm_path), "%.*s_%s%s",
                 (int)stem_len, base_out_path, p->suffix,
                 dot ? dot : ".pgm");
        snprintf(out->wce_path, sizeof(out->wce_path), "%.*s_%s.wce",
                 (int)stem_len, base_out_path, p->suffix);
    }
    pgm_write(out->pgm_path, &recon);
    out->file_bytes = write_wce_file(out->wce_path, W, H,
                                      &sb_ll, &sb_hl, &sb_lh, &sb_hh);

    out->name        = p->name;
    out->sb_totals[0] = sb_ll.buf_bytes;
    out->sb_totals[1] = sb_hl.buf_bytes;
    out->sb_totals[2] = sb_lh.buf_bytes;
    out->sb_totals[3] = sb_hh.buf_bytes;
    out->total_bytes = out->sb_totals[0] + out->sb_totals[1]
                     + out->sb_totals[2] + out->sb_totals[3];
    out->ratio       = (double)((size_t)W * (size_t)H)
                     / (double)out->file_bytes;
    out->psnr_db     = psnr(orig, &recon);

    printf("  wrote %s + %s   (%.2fx on disk, PSNR %.2f dB)\n",
           out->pgm_path, out->wce_path, out->ratio, out->psnr_db);

    free(ll_d); free(hl_d); free(lh_d); free(hh_d);
    free(recon.pixels); free(recon_buf);
cleanup_decode:
    free(ll_d); free(hl_d); free(lh_d); free(hh_d);
cleanup_preset:
    free(ll); free(hl); free(lh); free(hh_band);
    subband_free(&sb_ll); subband_free(&sb_hl);
    subband_free(&sb_lh); subband_free(&sb_hh);
    return 0;
}

int main(int argc, char **argv) {
    const char *in_path  = argc > 1 ? argv[1] : "demo/Cthulhu.pgm";
    const char *out_path = argc > 2 ? argv[2] : "demo/Cthulhu_reconstructed.pgm";

    pgm_t orig = {0};
    if (pgm_read(in_path, &orig) != 0) return 1;
    if ((orig.w & 1) || (orig.h & 1)) {
        fprintf(stderr, "image dims must be even (got %dx%d)\n",
                orig.w, orig.h);
        return 1;
    }
    const int W = orig.w, H = orig.h;
    const int hw = W / 2, hh = H / 2;

    double *dwt = (double *)malloc(sizeof(double) * (size_t)W * (size_t)H);
    {
        size_t i;
        for (i = 0; i < (size_t)W * (size_t)H; ++i)
            dwt[i] = (double)orig.pixels[i] - 128.0;
    }
    haar_forward_2d(dwt, W, H);

    const double scale_ll = 4.0;
    const double scale_d  = 8.0;

    const preset_t presets[] = {
        { "near-lossless", "q1",    2, 4, 4, 5 },
        { "balanced",      "q2",    4, 6, 6, 7 },
        { "aggressive",    "q3",    6, 8, 8, 9 },
        { "very lossy",    "q4",    8, 10, 10, 11 },
    };
    const size_t n_presets = sizeof(presets) / sizeof(presets[0]);
    preset_result_t results[16] = {0};

    const size_t raw_bytes = (size_t)W * (size_t)H;

    printf("libwce image compression demo\n");
    printf("=============================\n");
    printf("  input          : %s  (%d x %d, %zu raw bytes)\n",
           in_path, W, H, raw_bytes);
    printf("  scheme         : 1-level Haar DWT, wce_encode per band\n");
    printf("  reconstruction : OPTIMAL (Laplacian Lloyd-Max)\n");
    printf("  presets        : %zu\n\n", n_presets);

    size_t i;
    for (i = 0; i < n_presets; ++i) {
        run_preset(&orig, dwt, W, H, hw, hh,
                   scale_ll, scale_d,
                   &presets[i], out_path, &results[i]);
    }

    printf("\n  preset          lossy_bits      payload     .wce file    ratio    PSNR\n");
    printf("                  LL  HL  LH  HH    bytes        bytes\n");
    printf("  -------------  --- --- --- ---   -------      --------   ------   -------\n");
    for (i = 0; i < n_presets; ++i) {
        printf("  %-13s  %3u %3u %3u %3u   %7zu      %7zu   %5.2fx   %5.2f dB\n",
               presets[i].name,
               presets[i].lb_ll, presets[i].lb_hl,
               presets[i].lb_lh, presets[i].lb_hh,
               results[i].total_bytes, results[i].file_bytes,
               results[i].ratio, results[i].psnr_db);
    }
    printf("\n  (raw .pgm pixels: %zu bytes; .wce ratio includes %d-byte header)\n",
           raw_bytes, DEMO_CONTAINER_HEADER);

    free(orig.pixels); free(dwt);
    return 0;
}
