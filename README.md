# libwce — Wavelet Coefficient Entropy Codec

A minimal, zero-dependency Rust library for low-latency entropy coding of
wavelet coefficient bands. Patent-clean alternative to JPEG XS-style
BPC entropy layers.

```rust
use wce::*;

let mut coeffs = vec![0i32; 2048];  // your wavelet sub-band
let scale = estimate_laplacian_scale(&coeffs);

let mut buf = vec![0u8; 8192];
let out_len = encode(&coeffs, /*lossy_bits=*/3, &mut buf).unwrap();
// … transmit `&buf[..out_len]` and `scale` …

let mut decoded = vec![0i32; 2048];
let lossy_bits = decode(&buf[..out_len], &mut decoded).unwrap();
dequantize_optimal(&mut decoded, lossy_bits, scale);
```

## What it does

`encode` turns an `&[i32]` of wavelet coefficients into a self-contained
bitstream. `decode` inverts it, returning truncated grid values.
`dequantize_optimal` applies Laplacian Lloyd-Max reconstruction for
minimum coefficient-domain MSE.

The encoder picks between 4 mode combinations on each band:

|  | sparse_flag=off | sparse_flag=on (1 bit/8-group block) |
|---|---|---|
| RUNNING predictor (DPCM + zigzag) | smooth bpc sequences | smooth + sparse-block shortcut |
| ZERO predictor (`bpc − lossy`, unsigned) | sparse bands with occasional spikes | very sparse bands |

…across 7 Rice-k parameter values, via a single-pass histogram-based
cost picker. The chosen mode is encoded in the 12-byte header.

## Patent posture

The codec is designed to avoid the JPEG XS patent pool (Vectis /
intoPIX / Fraunhofer IIS) and all ANS-family entropy coders (notably
Microsoft US 11,234,023 B2). See [`PATENTS.md`](PATENTS.md) for the
full rationale.

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
encode 350–3700 MB/s, decode 380–5600 MB/s.

## API overview

| Category | Functions |
|---|---|
| **Top-level codec** | `encode`, `decode`, `encode_with_options` |
| **Quantization** | `quantize` |
| **Reconstruction** | `dequantize_optimal`, `estimate_laplacian_scale` |
| **Bit I/O** | `BitReader`, `BitWriter` — `read_bits`, `write_bits`, `flush`, `byte_align`, etc. |
| **Rice / zigzag** | `write_rice`, `read_rice`, `zigzag_encode`, `zigzag_decode` |
| **BPC primitives** | `compute_bpcs`, `pick_rice_k_for_bpcs`, `encode_bpcs_dpcm`, `decode_bpcs_dpcm` |
| **Coeff pack** | `pack_coeffs`, `unpack_coeffs` |

Most callers want only `encode` / `decode` / `dequantize_optimal`. The
other entry points are exposed for testing and integration into custom
codecs.

Grid-truncated values (skip reconstruction): just don't call
`dequantize_optimal` — the decoder leaves coefficients on the
quantization grid.

## Building

```sh
cargo test                    # run the unit-test suite (81 tests)
cargo build --examples        # build the demo + bench binaries
cargo run --example mode_shootout
cargo run --release --example image_compress -- demo/Cthulhu.pgm demo/Cthulhu
cargo run --release --example bench        # benchmark harness, CSV to stdout
```

## Demos

Under `examples/`:

- **`image_compress`** — reads `demo/Cthulhu.pgm`, runs a 1-level Haar
  DWT, encodes each sub-band, decodes back with optimal reconstruction,
  writes reconstructed PGMs and `.wce` container files at four quality
  presets. Prints PSNR and ratio.
- **`mode_shootout`** — enumerates the 4 (predictor × sparse-flag) mode
  combos via `encode_with_options`, then shows the auto-pick outcome
  alongside. Demonstrates mode selection.
- **`stream_surgery`** — corruption-resilience demo. Bit-flips, byte
  scrambles, prefix truncation, and crafted bad-header attacks against
  `decode`. Every call must return.
- **`bench`** — sweeps encode/decode over synthetic Laplacian corpora
  across lossy_bits values, measures throughput in MB/s, emits CSV.

## Design notes

- **Zero dependencies.** Only the Rust standard library. No external
  crates.
- **No heap allocation in the hot path.** The library allocates a small
  BPC buffer on first use; all encode/decode buffers are caller-owned.
  BitReader/BitWriter borrow slices — no ownership transfer.
- **Non-owning bit readers/writers.** Hold `&[u8]` / `&mut [u8]` slices
  into caller-provided buffers. Truncated reads zero-pad.
  `RICE_MAX_QUOTIENT` caps adversarial unary runs.
- **Self-contained per-band bitstream.** 12-byte header + payload, with
  magic `"WCE\0"`, format version, lossy_bits, rice_k + mode bits, and
  initial-prev seed. Each band is independently decodable.
- **Thread-safe.** All functions operate only on caller-supplied state.
  No global mutation. `Send` + `Sync` compatible.

## License

MIT.
