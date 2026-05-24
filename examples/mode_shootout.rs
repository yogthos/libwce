use std::time::Instant;
use wce::*;

const NUM_GROUPS: usize = 512;
const NUM_COEFFS: usize = NUM_GROUPS * 4;
const LOSSY_BITS: u8 = 3;

struct Rng { state: u32 }
impl Rng {
    fn next(&mut self) -> u32 {
        let mut x = self.state;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        self.state = x; x
    }
    fn uniform(&mut self) -> f64 { (self.next() | 1) as f64 / 0x100000000u64 as f64 }
}

fn laplace_sample(rng: &mut Rng, scale: f64) -> i32 {
    let u = rng.uniform() - 0.5;
    let sign = if u < 0.0 { -1.0 } else { 1.0 };
    let mag = -scale * (1.0 - 2.0 * u.abs()).ln();
    (sign * mag).clamp(-2e9, 2e9) as i32
}

fn run_combo(name: &str, coeffs: &[i32], opts: Option<&EncodeOptions>, buf: &mut [u8]) -> (String, usize, bool) {
    match encode_with_options(coeffs, LOSSY_BITS, opts, buf) {
        Ok(n) => {
            let mut dec = vec![0i32; NUM_COEFFS];
            if decode(&buf[..n], &mut dec).is_err() { return (name.into(), 0, false); }
            let ok = coeffs.iter().zip(&dec).all(|(&c, &d)| {
                let a = c.unsigned_abs(); let m = (a >> LOSSY_BITS) << LOSSY_BITS;
                let e = if c >= 0 { m as i32 } else { (0u32.wrapping_sub(m)) as i32 };
                d == e
            });
            (name.into(), n, ok)
        }
        Err(_) => (name.into(), 0, false),
    }
}

fn main() {
    let mut rng = Rng { state: 0xC0FFEE };
    let coeffs: Vec<i32> = (0..NUM_COEFFS).map(|_| laplace_sample(&mut rng, 8.0)).collect();
    let mut buf = vec![0u8; NUM_COEFFS * 5 + 64];

    let t0 = Instant::now();
    let results = [
        run_combo("RUN, flag=off ", &coeffs, Some(&EncodeOptions{predictor:PREDICTOR_RUNNING, sparse_flag:false, rice_k:2}), &mut buf),
        run_combo("RUN, flag=on  ", &coeffs, Some(&EncodeOptions{predictor:PREDICTOR_RUNNING, sparse_flag:true,  rice_k:2}), &mut buf),
        run_combo("ZERO, flag=off", &coeffs, Some(&EncodeOptions{predictor:PREDICTOR_ZERO,    sparse_flag:false, rice_k:2}), &mut buf),
        run_combo("ZERO, flag=on ", &coeffs, Some(&EncodeOptions{predictor:PREDICTOR_ZERO,    sparse_flag:true,  rice_k:2}), &mut buf),
        run_combo("auto-pick     ", &coeffs, None, &mut buf),
    ];
    let elapsed = t0.elapsed();

    let raw = (NUM_COEFFS * 4) as f64;
    println!("libwce mode shootout (Rust)");
    println!("============================");
    println!("  groups     : {NUM_GROUPS}  ({NUM_COEFFS} coefficients)");
    println!("  lossy_bits : {LOSSY_BITS}");
    println!("  time       : {elapsed:.1?}\n");
    println!("  mode              total   ratio   ok");
    println!("  ---------------   -----  ------   --");

    let mut best = (0usize, 0usize, 0usize);
    for (i, (name, bytes, ok)) in results.iter().enumerate() {
        let r = raw / *bytes as f64;
        println!("  {name}   {bytes:>5}   {r:>5.2}x   {}", if *ok {"Y"} else {"N"});
        if i < 4 && *ok && (best.1 == 0 || *bytes < best.1) { best = (i, *bytes, 0); }
    }

    let (_, auto_bytes, auto_ok) = &results[4];
    println!();
    if *auto_ok {
        if best.1 == 0 {
            println!("  auto-pick succeeded (all forced modes failed)");
        } else if *auto_bytes <= best.1 {
            println!("  auto-pick matched or beat all forced modes");
        } else {
            println!("  auto-pick paid {} bytes vs best forced", auto_bytes - best.1);
        }
    }
}
