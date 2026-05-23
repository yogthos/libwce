# libwce — Wavelet Coefficient Entropy Codec

A minimal, zero-dependency C99 library for low-latency entropy coding of
wavelet coefficient bands. Patent-clean alternative to JPEG XS-style
BPC entropy layers.

```c
#include "wce.h"

int32_t  coeffs[2048];  /* your wavelet sub-band */
uint8_t  buf[8192];
size_t   out_len;
double   scale_b = wce_estimate_laplacian_scale(coeffs, 2048);

wce_encode(coeffs, 2048, /*lossy_bits=*/3, buf, sizeof(buf), &out_len);
/* … transmit `buf[0..out_len]` and `scale_b` … */

int32_t decoded[2048];
int     lossy_bits;
wce_decode(buf, out_len, decoded, 2048, &lossy_bits);
wce_dequantize_optimal(decoded, 2048, lossy_bits, scale_b);
```

## What it does

`wce_encode` turns a band of `int32_t` wavelet coefficients into a
self-contained bitstream. `wce_decode` inverts it, returning truncated
grid values. `wce_dequantize_optimal` applies Laplacian Lloyd-Max
reconstruction for minimum coefficient-domain MSE.

The encoder picks between 4 mode combinations on each band:

|  | flag-off | flag-on (1 bit/8-group block) |
|---|---|---|
| RUNNING predictor (DPCM + zigzag) | smooth bpc sequences | smooth + sparse-block shortcut |
| ZERO predictor (`bpc − lossy`, unsigned) | sparse bands with occasional spikes | very sparse bands |

…across 7 Rice-k parameter values, via a single-pass histogram-based
cost picker. The chosen mode is encoded in the 12-byte header.

## Patent posture

The codec is designed to avoid the JPEG XS patent pool (Vectis /
intoPIX / Fraunhofer IIS) and all ANS-family entropy coders (notably
Microsoft US 11,234,023 B2). See [`PATENTS.md`](PATENTS.md) for the
full rationale — what's avoided, what's used, building-block prior
art, and known caveats.

## Performance

End-to-end on the demo Cthulhu PGM through a 1-level Haar DWT at four
quality presets:

| Preset | bytes_out | ratio | PSNR |
|---|---|---|---|
| near-lossless | 146 KB | 1.52× | 49.06 dB |
| balanced      |  92 KB | 2.40× | 37.54 dB |
| aggressive    |  49 KB | 4.48× | 28.79 dB |
| very lossy    |  21 KB | 10.11× | 21.62 dB |

Single-thread throughput on a 24-cell synthetic Laplacian corpus
(scales 2..128 × band sizes 2K and 32K × lossy_bits 0/2/4/6):
encode 430–4900 MB/s, decode 350–6000 MB/s.

## API overview

| Category | Functions |
|---|---|
| **Top-level codec** | `wce_encode`, `wce_decode`, `wce_encode_with_options` |
| **Quantization** | `wce_quantize` |
| **Reconstruction** | `wce_dequantize_optimal`, `wce_estimate_laplacian_scale` |
| **Bit I/O** | `wce_br_init`, `wce_br_read_bits`, `wce_br_byte_align`, `wce_br_truncated`, `wce_br_bytes_consumed`, `wce_bw_init`, `wce_bw_write_bits`, `wce_bw_flush`, `wce_bw_overflow`, `wce_bw_bytes_written` |
| **Rice / zigzag** | `wce_bw_write_rice`, `wce_br_read_rice`, `wce_zigzag_encode`, `wce_zigzag_decode` |
| **BPC primitives** | `wce_compute_bpcs`, `wce_pick_rice_k_for_bpcs`, `wce_encode_bpcs_dpcm`, `wce_decode_bpcs_dpcm` |
| **Coeff pack** | `wce_pack_coeffs`, `wce_unpack_coeffs` |

Most callers want only `wce_encode` / `wce_decode` /
`wce_dequantize_optimal`. The other entry points are exposed for
testing and integration into custom codecs.

Callers who want grid-truncated values (skip reconstruction) simply
don't call `wce_dequantize_optimal` — the decoder leaves coefficients
on the quantization grid by default.

## Building

```sh
make          # build libwce.a
make test     # run the unit-test suite
make demos    # build the three demo binaries
make bench    # benchmark harness, CSV to stdout
make fuzz     # 1M random inputs under ASan + UBSan (standalone)
make clean    # remove build artifacts

# Sanitizer build:
make test DEBUG=1
make demos DEBUG=1
```

`make fuzz-libfuzzer` builds a libFuzzer harness instead — requires a
clang that ships libclang_rt.fuzzer (e.g., Homebrew LLVM; Apple's
bundled clang does not).

## Demos

Under `demo/`:

- **`image_compress`** — reads `demo/Cthulhu.pgm`, runs a 1-level Haar
  DWT, encodes each sub-band through `wce_encode`, decodes back with
  OPTIMAL reconstruction, writes reconstructed PGMs and `.wce`
  container files at four quality presets. Prints PSNR and ratio.
- **`mode_shootout`** — enumerates the 4 (predictor × flag) mode
  combos via `wce_encode_with_options`, then shows the auto-pick
  outcome alongside. Demonstrates the value of mode selection.
- **`stream_surgery`** — corruption-resilience demo. Bit-flips, byte
  scrambles, prefix truncation, and crafted bad-header attacks against
  `wce_decode`. Every call must return; rebuild with `DEBUG=1` to also
  verify under ASan + UBSan.

## Design notes

- **Zero dependencies.** Only C99 stdlib (`stdint.h`, `stddef.h`,
  `stdbool.h`, `math.h` for `exp()` in optimal reconstruction).
- **No dynamic allocation.** The library never calls `malloc`. All
  buffers are caller-owned; encode/decode use a ~16 KB stack scratch
  for BPCs.
- **Non-owning bit readers/writers.** Hold pointers into caller-
  provided byte buffers; truncated reads zero-pad rather than
  segfault. `WCE_RICE_MAX_QUOTIENT` caps adversarial unary runs.
- **Self-contained per-band bitstream.** 12-byte header + payload,
  with magic `"WCE\0"`, format version, lossy_bits, rice_k + mode
  bits, and initial-prev seed. Each band is independently decodable.
- **Thread-safety.** All encode/decode functions are reentrant and
  operate only on caller-supplied state. No global mutation.

## License

MIT.
