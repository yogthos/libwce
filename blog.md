{:title "libwce: the entropy layer of a wavelet codec, on its own." :layout :post, :tags ["programming" "c" "entropy-coding" "compression"]}

Most image codecs you know about such as JPEG, JPEG 2000, JPEG XS, WebP are like layer cakes. You have transform sitting on top, entropy coding at the bottom, and rate control floats somewhere in the middle. And then there's a metadata layer wrapping it all up. The interesting bits are hidden under tons of framing code, profile parsers, and standards plumbing. If you just want to see how wavelet coefficients become bits, you have to dig deep.

I wrote `libwce` as a bare-bones implementation, consisting of only a single pair of `.c` and `.h` files, about a thousand lines total. It just implements a patent-clean Bit-Plane Count (BPC)-style entropy layer in the spirit of JPEG XS, and nothing else. No container, no rate control, and no patent encumbered code. The library has zero dependencies, relying solely on C99 stdlib.

### A two-minute primer on BPC coding

After the wavelet transform, you end up with a 2D array of signed integer coefficients, most of which are near zero, with a long Laplacian tail. The purpose of the entropy layer is to compress this array down to a small number of significant bits.

BPC coding is carried out in groups of four coefficients at a time. For each group, you have to determine the smallest `bpc` such that every coefficient can be held. This is the bit-plane count representing the index above which all coefficient bits in the group are zero. In libwce, all the `bpc` values are written first into a single bitstream, then for each group the four coefficients are emitted coeff-major: the magnitude bits of each coefficient followed immediately by a single sign bit when that coefficient is nonzero.

That takes care of all the data processing. The real compression comes when you go to encode the `bpc` values. Neighboring groups tend to have similar sizes, so instead of writing each `bpc` as a raw 6-bit number, you estimate it from its neighbors and write the small residual which tends to be tiny.

Here, libwce uses two predictors — RUNNING (DPCM delta vs the previous group's `bpc`, zigzag-mapped and Rice-coded) and ZERO (unsigned residual against `lossy_bits`) — each optionally combined with a 1-bit-per-8-group sparse-block flag that short-circuits all-deadzone blocks. That gives four predictor × flag combinations, and the encoder sweeps Rice-k across seven values inside each, picking the best per band via a single-pass cost search. All combinations give the same decoded result, but the bitstreams they produce are quite different. Some work better for textured parts, some for flat parts, and others for sub-bands which are mostly zeros.

### What it looks like to use

Here's a complete decoder for one sub-band:

```c
int32_t coeffs[N];
int     lossy_bits;

wce_decode(buf, buf_len, coeffs, N, &lossy_bits);
wce_dequantize_optimal(coeffs, N, lossy_bits, scale_b);
```

The library itself is stateless, and only works with buffers you give it. There's no `malloc`, IO, or hidden globals, and it never touches memory unless you pass it a pointer.

### Compressing an image end-to-end

The repo has 3 demos. The most fun one is `image_compress`, which is a full codec built on top of libwce. It uses Haar wavelet in, libwce in the middle, and inverse Haar on the way out which run across four quality presets.

```
  preset          lossy_bits      payload     .wce file    ratio    PSNR
                  LL  HL  LH  HH    bytes        bytes
  near-lossless    2   4   4   5    146537       146597    1.52x   49.06 dB
  balanced         4   6   6   7     92631        92691    2.40x   37.54 dB
  aggressive       6   8   8   9     49516        49576    4.48x   28.79 dB
  very lossy       8  10  10  11     21923        21983   10.11x   21.62 dB
```

The whole process consisting of DWT, sub-band coding, quantization, writing to a container takes under 500 lines of demo code. If you open the four reconstituted PGMs side by side and you'll see quality degrade before your eyes.

- **q1** will be indistinguishable from the original.
- **q2** has some minor smoothing in flat areas, but you have to squint to see it.
- **q3** starts to show noticeable wavelet ringing around edges.
- **q4** is blocky in a recognizable wavelet way, looking eldritch but legible.

You picked the entropy layer. Everything else consists of straightforward C code which you can read in an afternoon.

### The mode shootout

The second demo, `mode_shootout`, runs a synthetic Laplacian sub-band through every predictor × flag combination and displays the winner.

```
  mode             total   ratio   ok
  --------------   -----  ------   --
  RUN, flag=off      658   12.45x   Y
  RUN, flag=on       666   12.30x   Y
  ZERO, flag=off     652   12.56x   Y
  ZERO, flag=on      660   12.41x   Y
  auto-pick          612   13.39x   Y

  best forced: ZERO, flag=off  (652 bytes)
  auto-pick beat best forced by 40 bytes (better rice_k).
```

This is precisely the kind of thing that's a pain to do within the confines of a full codec. You’d have to fiddle with instrumenting internals, disable rate control, then mock the entire framing layer. With libwce, mode comparison is just how the API works. Run the same sub-band through `wce_encode_with_options` with each predictor × flag combination, then count the bytes and pick the winner — which is exactly what `wce_encode` itself does internally.

### Doing error correction

The third demo, `stream_surgery`, does 256 random bitflips and 256 random byte scrambles across the encoded bitstream, 300 truncation points covering every 4-byte prefix, and a set of adversarial cases including all-ones “unary bombs” and crafted bad headers.

```
  bit-flip (anywhere)           : 256/256 returned, avg 36 / max 1024 coeffs differ (of 1024)
  random byte (anywhere)        : 256/256 returned without crash
  truncation (every prefix)     : 300/300 prefix lengths returned
  adversarial (bombs + bad hdrs): 7 cases returned cleanly
```

 Every case gets successfully decoded without a hangup or a crash.

### What it isn't

libwce is not a full codec. It lacks container format, rate control, and JPEG XS compliance. It purposely diverges from ISO/IEC 21122 — replacing XS's per-band prediction modes with a DPCM-Rice + zero-prediction pair, swapping the deadzone-midpoint reconstruction for Laplacian Lloyd-Max, and avoiding all ANS-family entropy coders — to sidestep the JPEG XS and ANS patent pools.

The goal is to provide an entropy layer that you can wire into your own pipeline. Maybe you’re developing a custom wavelet codec for low-latency video streaming, or researching prediction strategies for sub-band coefficients, integrating compression into an embedded or FPGA project, or fuzzing the BPC family for security work. That’s the niche.

## Where to find it

The repo is at [https://github.com/yogthos/libwce](https://github.com/yogthos/libwce). Clone it, type `make test demos`, and play with it. It's written so you read and understand the source, not just use.