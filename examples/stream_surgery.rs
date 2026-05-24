use wce::*;

const NUM_GROUPS: usize = 256;
const NUM_COEFFS: usize = NUM_GROUPS * 4;
const LOSSY_BITS: u8 = 3;
const BUF_CAP: usize = NUM_COEFFS * 5 + 64;

struct Rng { state: u32 }
impl Rng {
    fn next(&mut self) -> u32 { let mut x = self.state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; self.state = x; x }
}

struct Encoded { expected: Vec<i32>, buf: Vec<u8>, len: usize }

fn build_clean(rng: &mut Rng) -> Encoded {
    let coeffs: Vec<i32> = (0..NUM_COEFFS).map(|_| (rng.next() as i32 % 4096) - 2048).collect();
    let mut buf = vec![0u8; BUF_CAP];
    let len = encode(&coeffs, LOSSY_BITS, &mut buf).expect("encode failed");
    let expected: Vec<i32> = coeffs.iter().map(|&c| {
        let a = c.unsigned_abs(); let m = (a >> LOSSY_BITS) << LOSSY_BITS;
        if c >= 0 { m as i32 } else { (0u32.wrapping_sub(m)) as i32 }
    }).collect();
    Encoded { expected, buf, len }
}

fn coeff_distance(a: &[i32], b: &[i32]) -> usize { a.iter().zip(b).filter(|(x, y)| x != y).count() }

fn decode_measure(buf: &[u8], expected: &[i32]) -> (usize, bool) {
    let mut out = vec![0i32; NUM_COEFFS];
    match decode(buf, &mut out) {
        Ok(_) => (coeff_distance(&out, expected), true),
        Err(_) => (NUM_COEFFS, false),
    }
}

fn attack_bit_flips(e: &Encoded, rng: &mut Rng) {
    let trials = 256;
    let (mut sum_diff, mut max_diff) = (0usize, 0usize);
    for _ in 0..trials {
        let mut snap = e.buf.clone();
        let bit = rng.next() as usize % (e.len * 8);
        snap[bit / 8] ^= 1u8 << (7 - (bit & 7));
        let (d, _) = decode_measure(&snap[..e.len], &e.expected);
        sum_diff += d; max_diff = max_diff.max(d);
    }
    println!("  bit-flips      : {trials}/{} returned, avg {}/max {} coeffs differ (of {NUM_COEFFS})", trials, sum_diff / trials, max_diff);
}

fn attack_byte_scramble(e: &Encoded, rng: &mut Rng) {
    let trials = 256;
    for _ in 0..trials {
        let mut snap = e.buf.clone();
        let idx = rng.next() as usize % e.len;
        snap[idx] = rng.next() as u8;
        decode_measure(&snap[..e.len], &e.expected);
    }
    println!("  byte scramble  : {trials}/{trials} returned without crash");
}

fn attack_truncation(e: &Encoded) {
    let mut returned = 0usize;
    let steps = (0..=e.len).step_by(4).count();
    for cut in (0..=e.len).step_by(4) {
        decode_measure(&e.buf[..cut], &e.expected); returned += 1;
    }
    println!("  truncation     : {returned}/{steps} prefix lengths returned");
}

fn attack_adversarial() {
    let mut coeffs = vec![0i32; NUM_COEFFS];
    let mut returned = 0usize;

    // All-ones bomb
    let mut bombs = vec![0xFFu8; BUF_CAP];
    bombs[0] = b'W'; bombs[1] = b'C'; bombs[2] = b'E'; bombs[3] = 0;
    bombs[4] = (NUM_GROUPS & 0xFF) as u8; bombs[5] = ((NUM_GROUPS >> 8) & 0xFF) as u8;
    bombs[6] = 0; bombs[7] = 0;
    bombs[8] = FORMAT_VERSION; bombs[9] = LOSSY_BITS; bombs[10] = 0; bombs[11] = LOSSY_BITS;
    let _ = decode(&bombs, &mut coeffs); returned += 1;

    // Bad magic
    if decode(&[0u8; 12], &mut coeffs) == Err(Error::BadMagic) { returned += 1; }

    // Bad version
    let mut hdr = [0u8; 12];
    hdr[0]=b'W'; hdr[1]=b'C'; hdr[2]=b'E'; hdr[3]=0; hdr[8]=FORMAT_VERSION+99; hdr[9]=LOSSY_BITS; hdr[11]=LOSSY_BITS;
    if decode(&hdr, &mut coeffs) == Err(Error::BadVersion) { returned += 1; }

    // Bad lossy_bits
    hdr[4]=NUM_GROUPS as u8; hdr[5]=(NUM_GROUPS>>8) as u8; hdr[8]=FORMAT_VERSION; hdr[9]=32;
    if decode(&hdr, &mut coeffs) == Err(Error::BadInput) { returned += 1; }

    // Bad rice_k
    hdr[9]=LOSSY_BITS; hdr[10]=17;
    if decode(&hdr, &mut coeffs) == Err(Error::BadInput) { returned += 1; }

    // Bad initial_prev
    hdr[10]=0; hdr[11]=99;
    if decode(&hdr, &mut coeffs) == Err(Error::BadInput) { returned += 1; }

    // Size mismatch
    hdr[11]=LOSSY_BITS; hdr[4]=0xFF; hdr[5]=0xFF;
    if decode(&hdr, &mut coeffs) == Err(Error::BadInput) { returned += 1; }

    println!("  adversarial    : {returned}/7 cases returned expected code");
}

fn main() {
    let mut rng = Rng { state: 0xDEADBEEF };
    let e = build_clean(&mut rng);

    println!("libwce stream surgery demo (Rust)");
    println!("=================================");
    println!("  groups     : {NUM_GROUPS}  ({NUM_COEFFS} coefficients)");
    println!("  lossy_bits : {LOSSY_BITS}");
    println!("  encoded    : {} bytes\n", e.len);
    println!("  attacks:");
    attack_bit_flips(&e, &mut rng);
    attack_byte_scramble(&e, &mut rng);
    attack_truncation(&e);
    attack_adversarial();
    println!("\n  every decode call returned.");
}
