# libwce — Patent Posture

This document records the patent-avoidance choices made in libwce. It
is intended as an auditable trail for legal review, not legal advice.
Independent counsel should be engaged before any commercial deployment.

## What we are avoiding

### JPEG XS essential patents

JPEG XS (ISO/IEC 21122) is administered as a patent pool by **Vectis**
on behalf of **intoPIX and Fraunhofer IIS** (announced March 2022, in
force as of 2026). The pool covers, among other claims, none of which
are individually published:

- The **line-based wavelet transform** with the XS-specific buffering
  schedule (sub-line latency).
- The **four-substream entropy layout** — significance flags, bit-plane
  counts, data planes, signs — and the inter-substream synchronization.
- The **seven prediction modes** for the BPC stream (raw, horizontal,
  vertical, zero-prediction, vertical+LUT, flgband+zero-prediction,
  flgband+vertical+LUT).
- **Precinct-based rate allocation** for sub-line latency.
- The **deadzone-midpoint dequantization formula** as expressed by
  bitwise-OR of bit `lossy_bits - 1` into the magnitude.

### ANS family

Although Jarek Duda intended ANS for the public domain, **Microsoft was
granted US 11,234,023 B2** in January 2022 on "Features of range
asymmetric number system encoding and decoding" — a live patent that
introduces non-zero risk for any rANS implementation in a commercial
product. libwce uses neither rANS nor tANS.

## Algorithm choices

Each design choice is paired with the public-domain prior art it relies
on.

### Quantization & reconstruction

- **`wce_quantize`** — symmetric truncate-toward-zero by integer shift:
  `c → sign(c) · (|c| >> lossy_bits) << lossy_bits`. Basic integer
  arithmetic; prior art predates digital signal processing.

- **`wce_dequantize(WCE_RECON_TRUNCATE)`** — no-op. Coefficients remain
  on the quantization grid. No reconstruction offset.

- **`wce_dequantize(WCE_RECON_CENTERED)`** — adds `step/2` to non-zero
  magnitudes via **integer addition**, not bitwise OR. The output is
  mathematically equivalent to a midpoint reconstruction, but the
  *mechanism* differs from the XS-specific bitwise-OR formulation.
  Midpoint reconstruction itself is textbook prior art (Lloyd 1957,
  Max 1960). Whether the equivalence-of-output triggers a
  doctrine-of-equivalents reading is a legal question that requires
  counsel; see the CENTERED caveat below.

- **`wce_dequantize_optimal`** — Lloyd-Max optimal reconstruction for a
  Laplacian source. Offset is `b − step / (eᵘ − 1)` where `b` is the
  estimated band scale and `u = step/b`. **Different formula, different
  output values** from the XS midpoint. The derivation is standard
  rate-distortion theory (Berger 1971; Gray 1990).

### Entropy coding

- **No ANS.** Neither rANS nor tANS appears in the codec.

- **Rice-Golomb coding** (Rice 1979, Golomb 1966) for variable-length
  integers. Both patents long expired.

- **Zigzag mapping** of signed integers (`(v << 1) ^ (v >> 31)`).
  Folklore in compression literature; no patent.

- **DPCM with running prev** for BPC delta coding. The DPCM concept
  predates digital coding by decades; no patent.

- **Zero-prediction** mode (predict `bpc = lossy_bits`, encode
  unsigned residual). The "predict constant, encode residual" idea is
  unpatentable prior art (used in countless DPCM systems including
  JPEG-LS). This mode is one of two predictor choices selected per
  band by the encoder.

### Bitstream layout

- **Two streams** — a 12-byte fixed header and a single contiguous
  payload bitstream. Distinct from the XS four-substream layout.

- **Coefficient-major packing** — for each group, magnitudes followed
  by inline signs, group by group. Distinct from the XS plane-
  interleaved nibbles (each bit-plane encoded as one nibble across the
  four coefficients of a group, planes packed MSB-first).

- **Signs interleaved with magnitudes**, gated on non-zero decoded
  magnitude. Not a separate sign substream.

- **LSB-first bit ordering** within each byte.

- **8-group sparse-flag option** — 1 bit per block of 8 groups signals
  "this block is entirely in the deadzone." Structurally similar to
  XS's "flgband" concept but distinguished by: (a) inline placement in
  the single payload stream rather than a separate parallel substream;
  (b) selection as one of two encoder modes (chosen per band via 1
  mode bit) rather than membership in a seven-mode menu;
  (c) LSB-first encoding inline rather than synchronized parallel
  bits in a dedicated `flg` substream. The structural overlap is
  closer here than for other components — see the caveat below.

### Mode selection

- **Per-band selection** between RUNNING-predictor + sparse-flag and
  ZERO-predictor + sparse-flag (4 combinations × Rice-k), with the
  encoder picking the smallest-cost combination via a histogram-based
  cost search. Brute-force cost search is common practice across the
  compression literature; no novelty claim.

- **1 mode bit + 1 predictor bit** packed into the header's `rice_k`
  byte. Format choice, no patent angle.

## Building-block prior art

| Component | Status |
|---|---|
| Rice (1979), Golomb (1966) | Patents expired decades ago |
| DPCM, integer truncate, sign-magnitude | Pre-digital; no patent |
| Lloyd-Max quantization (1957/60) | Textbook; no patent |
| LSB-first bit packing | Folklore; no patent |
| MQ-coder / IBM Q-coder | Expired ≥2009 |
| EBCOT (Taubman, 1999) | Expired ~2019-2020 |
| SPIHT (Said-Pearlman, 1996) | Expired ~2017 |
| LOCO-I / JPEG-LS (HP, 1997) | Expired ~2017-2018 |
| CDF 9/7, Le Gall 5/3 wavelets | Expired ~2016-2017 |

These are reference data points — not all are used by libwce, but they
inform the prior-art landscape it relies on.

## Known caveats

1. **The XS patent pool does not publish individual patent numbers.**
   Vectis publishes a license but not a claim chart. Before commercial
   deployment we recommend obtaining the claim chart from Vectis and
   asking counsel to map it against this implementation.

2. **The sparse-flag option** is the closest structural cousin to the
   XS flgband concept. Even though the surrounding code differs
   (inline single-stream vs. parallel sub-stream, integrated mode
   selection vs. separate selection), counsel should sanity-check this
   area specifically.

3. **CENTERED reconstruction** produces output numerically identical
   to the XS deadzone-midpoint formula. While the mechanism differs,
   doctrine-of-equivalents is a real concern. For maximum patent
   distance, **prefer `OPTIMAL` or `TRUNCATE` modes** in commercial
   deployments. `CENTERED` is offered for callers who accept the
   equivalence-of-output risk.

4. **Microsoft rANS patent** (US 11,234,023 B2). Although libwce uses
   no ANS family encoder, downstream callers who replace the entropy
   layer with rANS would re-introduce this exposure.

## Sources

- [JPEG XS Patent Pool announcement (Vectis, 2022)](https://www.businesswire.com/news/home/20220301005145/en/intoPIX-and-Fraunhofer-IIS-Team-with-Vectis-to-Offer-JPEG-XS-Patent-Pool-License)
- [Microsoft US 11,234,023 B2 — rANS patent](https://patents.google.com/patent/US11234023B2/en)
- [SPIHT family — US 6,671,413 (expired 2020)](https://patents.google.com/patent/US6671413B1/en)
- [LOCO-I / JPEG-LS technical report (HP)](http://shiftleft.com/mirrors/www.hpl.hp.com/techreports/98/HPL-98-193.pdf)
- [Image and Video Coding for Ultra-Low Latency — ACM CSur 2022](https://dl.acm.org/doi/full/10.1145/3512342)
