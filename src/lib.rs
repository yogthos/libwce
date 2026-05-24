use std::cmp::Ordering;

pub const FORMAT_VERSION: u8 = 4;
pub const HEADER_SIZE: usize = 12;
pub const BLOCK_GROUPS: usize = 8;
pub const MAX_INLINE_GROUPS: usize = 16384;
pub const RICE_MAX_QUOTIENT: u32 = 256;

pub const PREDICTOR_RUNNING: u8 = 0;
pub const PREDICTOR_ZERO: u8 = 1;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    NoSpace,
    BadInput,
    BadMagic,
    BadVersion,
    Truncated,
    Corrupt,
}

impl core::fmt::Display for Error {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Error::NoSpace => write!(f, "output buffer too small"),
            Error::BadInput => write!(f, "invalid input parameters"),
            Error::BadMagic => write!(f, "bad magic bytes in header"),
            Error::BadVersion => write!(f, "unsupported format version"),
            Error::Truncated => write!(f, "truncated bitstream"),
            Error::Corrupt => write!(f, "corrupt bitstream"),
        }
    }
}

impl std::error::Error for Error {}

// -- bit I/O

pub struct BitReader<'a> {
    data: &'a [u8],
    pos: usize,
    reg: u64,
    bits_held: u8,
    truncated: bool,
}

impl<'a> BitReader<'a> {
    pub fn new(data: &'a [u8]) -> Self {
        Self { data, pos: 0, reg: 0, bits_held: 0, truncated: false }
    }


    fn refill(&mut self, min_bits: u8) {
        while self.bits_held < min_bits && self.bits_held <= 56 {
            if self.pos < self.data.len() {
                self.reg |= (self.data[self.pos] as u64) << self.bits_held;
                self.pos += 1;
            } else {
                self.truncated = true;
            }
            self.bits_held += 8;
        }
    }

    pub fn read_bits(&mut self, n: u8) -> u32 {
        let n = if n > 32 { 32 } else { n };
        if n == 0 { return 0; }
        if self.bits_held < n { self.refill(n); }
        let v = if n == 32 { self.reg as u32 } else { (self.reg & ((1u64 << n) - 1)) as u32 };
        self.reg >>= n;
        self.bits_held -= n;
        v
    }

    pub fn byte_align(&mut self) {
        let drop = self.bits_held & 7;
        self.reg >>= drop;
        self.bits_held -= drop;
    }

    pub fn is_truncated(&self) -> bool { self.truncated }
    pub fn bytes_consumed(&self) -> usize { self.pos }
}

pub struct BitWriter<'a> {
    data: &'a mut [u8],
    pos: usize,
    reg: u64,
    bits_held: u8,
    overflow: bool,
}

impl<'a> BitWriter<'a> {
    pub fn new(data: &'a mut [u8]) -> Self {
        Self { data, pos: 0, reg: 0, bits_held: 0, overflow: false }
    }


    pub fn write_bits(&mut self, value: u32, n: u8) {
        if self.overflow { return; }
        let n = if n > 32 { 32 } else { n };
        if n == 0 { return; }
        let mask = if n == 32 { 0xFFFF_FFFFu64 } else { (1u64 << n) - 1 };
        self.reg |= ((value as u64) & mask) << self.bits_held;
        self.bits_held += n;
        while self.bits_held >= 8 {
            if self.pos < self.data.len() {
                self.data[self.pos] = (self.reg & 0xFF) as u8;
                self.pos += 1;
            } else {
                self.overflow = true;
            }
            self.reg >>= 8;
            self.bits_held -= 8;
        }
    }

    pub fn flush(&mut self) {
        if self.bits_held > 0 {
            if self.pos < self.data.len() {
                self.data[self.pos] = (self.reg & 0xFF) as u8;
                self.pos += 1;
            } else {
                self.overflow = true;
            }
            self.reg = 0;
            self.bits_held = 0;
        }
    }

    pub fn is_overflow(&self) -> bool { self.overflow }
    pub fn set_overflow(&mut self) { self.overflow = true; }
    pub fn bytes_written(&self) -> usize { self.pos }
}

// zigzag

pub fn zigzag_encode(v: i32) -> u32 {
    let uv = v as u32;
    (uv << 1) ^ 0u32.wrapping_sub(uv >> 31)
}

pub fn zigzag_decode(u: u32) -> i32 {
    (u >> 1 ^ 0u32.wrapping_sub(u & 1)) as i32
}

// rice

pub fn write_rice(bw: &mut BitWriter, value: u32, k: u8) {
    let k = k.min(16);
    let q = value >> k;
    if q >= RICE_MAX_QUOTIENT { bw.set_overflow(); return; }
    let mut q = q;
    while q >= 31 { bw.write_bits(0x7FFF_FFFF, 31); q -= 31; }
    if q > 0 { bw.write_bits((1u32 << q) - 1, q as u8); }
    bw.write_bits(0, 1);
    if k > 0 { bw.write_bits(value & ((1u32 << k) - 1), k); }
}

pub fn read_rice(br: &mut BitReader, k: u8) -> u32 {
    let k = k.min(16);
    let mut q: u32 = 0;
    while q < RICE_MAX_QUOTIENT { if br.read_bits(1) == 0 { break; } q += 1; }
    if q == RICE_MAX_QUOTIENT { return u32::MAX; }
    let r = if k > 0 { br.read_bits(k) } else { 0 };
    (q << k) | r
}

// bpc

fn ceil_log2_plus1(v: u32) -> u8 { 32u8.saturating_sub(v.leading_zeros() as u8) }

pub fn compute_bpcs(coeffs: &[i32], lossy_bits: u8) -> Vec<u8> {
    let num_groups = coeffs.len() / 4;
    let mut bpcs = vec![0u8; num_groups];
    let lossy = lossy_bits.min(31);
    for g in 0..num_groups {
        let mut max_abs: u32 = 0;
        for i in 0..4 { max_abs = max_abs.max(coeffs[g * 4 + i].unsigned_abs()); }
        bpcs[g] = ceil_log2_plus1(max_abs).max(lossy).min(32);
    }
    bpcs
}

pub fn pick_rice_k_for_bpcs(bpcs: &[u8], k_max: u8) -> u8 {
    let k_max = k_max.min(16);
    if bpcs.len() < 2 { return 0; }
    let mut best_k: u8 = 0;
    let mut best_bits = u64::MAX;
    for k in 0..=k_max {
        let mut total: u64 = 0;
        let mut ok = true;
        for i in 1..bpcs.len() {
            let d = bpcs[i] as i32 - bpcs[i - 1] as i32;
            let q = zigzag_encode(d) >> k;
            if q >= RICE_MAX_QUOTIENT { ok = false; break; }
            total += q as u64 + 1 + k as u64;
        }
        if ok && total < best_bits { best_bits = total; best_k = k; }
    }
    best_k
}

pub fn encode_bpcs_dpcm(bw: &mut BitWriter, bpcs: &[u8], k: u8) {
    for i in 1..bpcs.len() {
        let d = bpcs[i] as i32 - bpcs[i - 1] as i32;
        write_rice(bw, zigzag_encode(d), k);
    }
}

pub fn decode_bpcs_dpcm(br: &mut BitReader, num_groups: usize, initial: u8, k: u8, bpcs: &mut [u8]) {
    if num_groups == 0 { return; }
    bpcs[0] = initial.min(32);
    let mut prev = bpcs[0] as i32;
    for i in 1..num_groups {
        let u = read_rice(br, k);
        if u == u32::MAX {
            for item in bpcs.iter_mut().skip(i) { *item = prev as u8; }
            return;
        }
        prev = (prev + zigzag_decode(u)).clamp(0, 32);
        bpcs[i] = prev as u8;
    }
}

// pack

pub fn pack_coeffs(bw: &mut BitWriter, coeffs: &[i32], bpcs: &[u8], lossy_bits: u8, num_groups: usize) {
    let lossy = lossy_bits.min(31);
    for g in 0..num_groups {
        let nbits = (bpcs[g] as i32 - lossy as i32).clamp(0, 32);
        for i in 0..4 {
            let c = coeffs[g * 4 + i];
            let m = c.unsigned_abs() >> lossy;
            if nbits > 0 { bw.write_bits(m, nbits as u8); }
            if m != 0 { bw.write_bits(if c < 0 { 1 } else { 0 }, 1); }
        }
    }
}

pub fn unpack_coeffs(br: &mut BitReader, bpcs: &[u8], lossy_bits: u8, num_groups: usize, out: &mut [i32]) {
    let lossy = lossy_bits.min(31);
    for g in 0..num_groups {
        let nbits = (bpcs[g] as i32 - lossy as i32).clamp(0, 32);
        for i in 0..4 {
            let m = if nbits > 0 { br.read_bits(nbits as u8) } else { 0 };
            let mag = m << lossy;
            out[g * 4 + i] = if m == 0 { 0 } else {
                let sign = br.read_bits(1);
                if mag > i32::MAX as u32 { if sign != 0 { i32::MIN } else { i32::MAX } }
                else if sign != 0 { (0u32.wrapping_sub(mag)) as i32 }
                else { mag as i32 }
            };
        }
    }
}

// quantize

pub fn quantize(coeffs: &mut [i32], lossy_bits: u8) {
    if lossy_bits == 0 { return; }
    if lossy_bits >= 32 { coeffs.fill(0); return; }
    let mask = !((1u32 << lossy_bits) - 1);
    for c in coeffs.iter_mut() {
        if *c > 0 { *c = ((*c as u32) & mask) as i32; }
        else if *c < 0 {
            let abs = 0u32.wrapping_sub(*c as u32);
            *c = (0u32.wrapping_sub(abs & mask)) as i32;
        }
    }
}

pub fn estimate_laplacian_scale(coeffs: &[i32]) -> f64 {
    if coeffs.is_empty() { return 0.0; }
    coeffs.iter().map(|&c| c.unsigned_abs() as f64).sum::<f64>() / coeffs.len() as f64
}

pub fn dequantize_optimal(coeffs: &mut [i32], lossy_bits: u8, scale_b: f64) {
    if lossy_bits == 0 || lossy_bits >= 32 { return; }
    if scale_b.partial_cmp(&0.0) != Some(Ordering::Greater) || !scale_b.is_finite() { return; }
    let step = (1u64 << lossy_bits) as f64;
    let u = step / scale_b;
    if u == 0.0 { return; }
    let offset_d = if u > 709.0 { scale_b } else { scale_b - step / u.exp_m1() };
    let offset = (offset_d.clamp(0.0, step - 1.0) + 0.5) as u32;
    if offset == 0 { return; }
    for c in coeffs.iter_mut() {
        if *c > 0 {
            let mag = (*c as u32) + offset;
            *c = if mag > i32::MAX as u32 { i32::MAX } else { mag as i32 };
        } else if *c < 0 {
            let abs = 0u32.wrapping_sub(*c as u32) + offset;
            *c = if abs >= 0x8000_0000 { i32::MIN } else { (0u32.wrapping_sub(abs)) as i32 };
        }
    }
}

// codec

pub struct EncodeOptions {
    pub predictor: u8,
    pub sparse_flag: bool,
    pub rice_k: u8,
}

pub fn encode(coeffs: &[i32], lossy_bits: u8, out: &mut [u8]) -> Result<usize, Error> {
    encode_with_options(coeffs, lossy_bits, None, out)
}

const PICK_K_MAX: usize = 6;
const HIST_RUN_BINS: usize = 65;
const HIST_ZERO_BINS: usize = 33;

pub fn encode_with_options(
    coeffs: &[i32], lossy_bits: u8, opts: Option<&EncodeOptions>, out: &mut [u8],
) -> Result<usize, Error> {
    if lossy_bits > 31 { return Err(Error::BadInput); }
    if out.len() < HEADER_SIZE { return Err(Error::NoSpace); }
    if coeffs.len() & 3 != 0 { return Err(Error::BadInput); }
    let num_groups = coeffs.len() >> 2;
    if num_groups > MAX_INLINE_GROUPS { return Err(Error::BadInput); }

    let bpcs = compute_bpcs(coeffs, lossy_bits);

    let (predictor, use_flag, rice_k) = mode_select(&bpcs, num_groups, lossy_bits, opts)?;

    let (header_buf, payload_buf) = out.split_at_mut(HEADER_SIZE);
    let mut bw = BitWriter::new(payload_buf);
    let mut prev = lossy_bits as i32;
    let mut g0 = 0;
    while g0 < num_groups {
        let g_end = (g0 + BLOCK_GROUPS).min(num_groups);
        if use_flag {
            let sparse = is_sparse(&bpcs, g0, g_end, lossy_bits);
            bw.write_bits(sparse as u32, 1);
            if sparse { prev = lossy_bits as i32; g0 = g_end; continue; }
        }
        for g in g0..g_end {
            let u = if predictor {
                (bpcs[g] as i32 - lossy_bits as i32).max(0) as u32
            } else {
                zigzag_encode(bpcs[g] as i32 - prev)
            };
            write_rice(&mut bw, u, rice_k);
            prev = bpcs[g] as i32;
        }
        pack_coeffs(&mut bw, &coeffs[g0 * 4..], &bpcs[g0..], lossy_bits, g_end - g0);
        g0 = g_end;
    }
    bw.flush();
    if bw.is_overflow() { return Err(Error::NoSpace); }
    let payload_len = bw.bytes_written();

    header_buf[0] = b'W'; header_buf[1] = b'C'; header_buf[2] = b'E'; header_buf[3] = 0;
    write_u32le(&mut header_buf[4..8], num_groups as u32);
    header_buf[8] = FORMAT_VERSION;
    header_buf[9] = lossy_bits;
    header_buf[10] = (rice_k & 0x1F) | if predictor { 0x40 } else { 0 } | if use_flag { 0x80 } else { 0 };
    header_buf[11] = lossy_bits; // initial prev == lossy_bits for the first group
    Ok(HEADER_SIZE + payload_len)
}

fn mode_select(bpcs: &[u8], num_groups: usize, lossy_bits: u8, opts: Option<&EncodeOptions>) -> Result<(bool, bool, u8), Error> {
    if let Some(o) = opts {
        if o.rice_k > 16 { return Err(Error::BadInput); }
        if o.predictor != PREDICTOR_RUNNING && o.predictor != PREDICTOR_ZERO { return Err(Error::BadInput); }
        return Ok((o.predictor == PREDICTOR_ZERO, o.sparse_flag, o.rice_k));
    }
    if num_groups == 0 { return Ok((false, false, 0)); }
    let costs = compute_all_combo_costs(bpcs, num_groups, lossy_bits);
    let (mut best_cost, mut best_combo, mut best_k) = (u64::MAX, 0usize, 0u8);
    for c in 0..4 {
        for k in 0..=PICK_K_MAX {
            if costs[c][k] < best_cost { best_cost = costs[c][k]; best_combo = c; best_k = k as u8; }
        }
    }
    Ok(((best_combo & 2) != 0, (best_combo & 1) != 0, best_k))
}

pub fn decode(input: &[u8], coeffs: &mut [i32]) -> Result<u8, Error> {
    if input.len() < HEADER_SIZE { return Err(Error::Truncated); }
    if &input[..4] != b"WCE\0" { return Err(Error::BadMagic); }
    if input[8] != FORMAT_VERSION { return Err(Error::BadVersion); }
    let num_groups = read_u32le(&input[4..8]) as usize;
    let lossy = input[9];
    let flags = input[10];
    let rice_k = flags & 0x1F;
    let predictor = flags & 0x40 != 0;
    let use_flag = flags & 0x80 != 0;
    let initial_prev = input[11];

    if lossy > 31 || rice_k > 16 || coeffs.len() & 3 != 0 || num_groups * 4 != coeffs.len() || initial_prev != lossy {
        return Err(Error::BadInput);
    }
    if num_groups > MAX_INLINE_GROUPS { return Err(Error::BadInput); }
    if num_groups == 0 { return Ok(lossy); }

    let mut br = BitReader::new(&input[HEADER_SIZE..]);
    let mut bpcs = vec![0u8; num_groups];
    let mut prev = initial_prev.min(32) as i32;
    let mut g0 = 0;
    while g0 < num_groups {
        let g_end = (g0 + BLOCK_GROUPS).min(num_groups);
        if use_flag && br.read_bits(1) != 0 {
            for g in g0..g_end {
                bpcs[g] = lossy;
                coeffs[g * 4..][..4].fill(0);
            }
            prev = lossy as i32;
        } else {
            for g in g0..g_end {
                let u = read_rice(&mut br, rice_k);
                if u == u32::MAX {
                    for g2 in g..num_groups { bpcs[g2] = prev as u8; coeffs[g2 * 4..][..4].fill(0); }
                    return Err(Error::Corrupt);
                }
                prev = if predictor {
                    (lossy as u32 + u).min(32) as i32
                } else {
                    (prev + zigzag_decode(u)).clamp(0, 32)
                };
                bpcs[g] = prev as u8;
            }
            unpack_coeffs(&mut br, &bpcs[g0..], lossy, g_end - g0, &mut coeffs[g0 * 4..]);
            if br.is_truncated() {
                for g in g_end..num_groups { coeffs[g * 4..][..4].fill(0); }
                return Err(Error::Truncated);
            }
        }
        g0 = g_end;
    }
    Ok(lossy)
}

// helpers -----------------------------------------------------------------

fn write_u32le(p: &mut [u8], v: u32) {
    p[0] = v as u8; p[1] = (v >> 8) as u8; p[2] = (v >> 16) as u8; p[3] = (v >> 24) as u8;
}

fn read_u32le(p: &[u8]) -> u32 {
    p[0] as u32 | (p[1] as u32) << 8 | (p[2] as u32) << 16 | (p[3] as u32) << 24
}

fn is_sparse(bpcs: &[u8], g0: usize, g_end: usize, lossy_bits: u8) -> bool {
    bpcs[g0..g_end].iter().all(|&b| b == lossy_bits)
}

fn compute_all_combo_costs(bpcs: &[u8], num_groups: usize, lossy_bits: u8) -> [[u64; PICK_K_MAX + 1]; 4] {
    let mut hist_run_off = [0u32; HIST_RUN_BINS];
    let mut hist_run_on = [0u32; HIST_RUN_BINS];
    let mut hist_zero = [0u32; HIST_ZERO_BINS];
    let (mut prev_off, mut prev_on) = (lossy_bits as i32, lossy_bits as i32);
    let (mut num_blocks, mut sparse_count) = (0usize, 0usize);

    let mut g0 = 0;
    while g0 < num_groups {
        let g_end = (g0 + BLOCK_GROUPS).min(num_groups);
        num_blocks += 1;
        let sparse = is_sparse(bpcs, g0, g_end, lossy_bits);
        if sparse {
            sparse_count += g_end - g0;
            for g in g0..g_end {
                let zz = zigzag_encode(bpcs[g] as i32 - prev_off);
                if (zz as usize) < HIST_RUN_BINS { hist_run_off[zz as usize] += 1; }
                let zd = bpcs[g] as i32 - lossy_bits as i32;
                if (zd as u32 as usize) < HIST_ZERO_BINS { hist_zero[zd as usize] += 1; }
                prev_off = bpcs[g] as i32;
            }
            prev_on = lossy_bits as i32;
        } else {
            for g in g0..g_end {
                let (zz_off, zz_on) = (zigzag_encode(bpcs[g] as i32 - prev_off), zigzag_encode(bpcs[g] as i32 - prev_on));
                if (zz_off as usize) < HIST_RUN_BINS { hist_run_off[zz_off as usize] += 1; }
                if (zz_on as usize) < HIST_RUN_BINS { hist_run_on[zz_on as usize] += 1; }
                let zd = bpcs[g] as i32 - lossy_bits as i32;
                if (zd as u32 as usize) < HIST_ZERO_BINS { hist_zero[zd as usize] += 1; }
                prev_off = bpcs[g] as i32;
                prev_on = bpcs[g] as i32;
            }
        }
        g0 = g_end;
    }

    let (total, non_sparse, blocks) = (num_groups as u64, (num_groups - sparse_count) as u64, num_blocks as u64);
    let mut costs = [[0u64; PICK_K_MAX + 1]; 4];
    for k in 0..=PICK_K_MAX {
        let (mut s_run_off, mut s_run_on, mut s_zero) = (0u64, 0u64, 0u64);
        for v in 0..HIST_RUN_BINS { s_run_off += hist_run_off[v] as u64 * (v >> k) as u64; s_run_on += hist_run_on[v] as u64 * (v >> k) as u64; }
        for v in 0..HIST_ZERO_BINS { s_zero += hist_zero[v] as u64 * (v >> k) as u64; }
        let k1 = 1 + k as u64;
        costs[0][k] = k1 * total + s_run_off;
        costs[1][k] = blocks + k1 * non_sparse + s_run_on;
        costs[2][k] = k1 * total + s_zero;
        costs[3][k] = blocks + k1 * non_sparse + s_zero;
    }
    costs
}
