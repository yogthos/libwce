use wce::*;
use std::time::Instant;

struct Rng { state: u32 }
impl Rng {
    fn next(&mut self) -> u32 { let mut x = self.state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; self.state = x; x }
    fn uniform(&mut self) -> f64 { (self.next() | 1) as f64 / 0x100000000u64 as f64 }
}

fn laplace_i32(rng: &mut Rng, scale: f64) -> i32 {
    let u = rng.uniform() - 0.5;
    let sign = if u < 0.0 { -1.0 } else { 1.0 };
    let mag = -scale * (1.0 - 2.0 * u.abs()).ln();
    (sign * mag).clamp(-2e9, 2e9) as i32
}

fn fill_laplace(out: &mut [i32], scale: f64, seed: u32) {
    let mut rng = Rng { state: seed };
    for v in out { *v = laplace_i32(&mut rng, scale); }
}

fn coeff_mse(a: &[i32], b: &[i32]) -> f64 {
    a.iter().zip(b).map(|(&x, &y)| { let d = x as f64 - y as f64; d * d }).sum::<f64>() / a.len() as f64
}

fn measure_mbps<F: FnMut()>(mut op: F, coeff_bytes: usize) -> f64 {
    let min_ns = 50_000_000u128;
    let start = Instant::now();
    let mut iters = 0u64;
    while start.elapsed().as_nanos() < min_ns && iters < 100_000 {
        op(); iters += 1;
    }
    let elapsed = start.elapsed().as_nanos();
    if elapsed == 0 || iters == 0 { return 0.0; }
    (iters as f64 * coeff_bytes as f64 * 1e3) / elapsed as f64
}

fn main() {
    let corpora: Vec<(&str, usize, f64, u32)> = vec![
        ("laplace_s2_2k",   2048,   2.0,  0xC0FFEE1),
        ("laplace_s8_2k",   2048,   8.0,  0xC0FFEE2),
        ("laplace_s32_2k",  2048,  32.0,  0xC0FFEE3),
        ("laplace_s128_2k", 2048, 128.0,  0xC0FFEE4),
        ("laplace_s2_32k",  32768,  2.0,  0xC0FFEE7),
        ("laplace_s8_32k",  32768,  8.0,  0xC0FFEE5),
        ("laplace_s32_32k", 32768, 32.0,  0xC0FFEE6),
        ("laplace_s128_32k",32768,128.0,  0xC0FFEE8),
    ];
    let lossy_bits_grid = [0i32, 2, 4, 6];

    println!("codec,corpus,n,lossy_bits,bytes_out,ratio,enc_mbps,dec_mbps,coeff_mse,status");

    for (name, n, scale, seed) in &corpora {
        let mut coeffs = vec![0i32; *n];
        fill_laplace(&mut coeffs, *scale, *seed);

        for &lb in &lossy_bits_grid {
            let lbs = lb as u8;
            let mut enc_buf = vec![0u8; coeffs.len() * 16 + 256];
            let enc_len = match encode(&coeffs, lbs, &mut enc_buf) {
                Ok(n) => n,
                Err(_) => { println!("wce,{name},{n},{lb},,,,,,encode_err"); continue; }
            };

            let mut dec_buf = vec![0i32; *n];
            let scale = estimate_laplacian_scale(&coeffs);
            if decode(&enc_buf[..enc_len], &mut dec_buf).is_err() {
                println!("wce,{name},{n},{lb},{enc_len},,,,decode_err"); continue;
            }
            let mut dec_buf_recon = dec_buf.clone();
            dequantize_optimal(&mut dec_buf_recon, lbs, scale);
            let mse = coeff_mse(&coeffs, &dec_buf_recon);
            let coeff_bytes = coeffs.len() * 4;
            let ratio = coeff_bytes as f64 / enc_len as f64;

            let enc_buf2 = enc_buf.clone();
            let enc_mbps = measure_mbps(|| { let mut b = enc_buf2.clone(); if encode(&coeffs, lbs, &mut b).is_err() { return; } }, coeff_bytes);
            let dec_mbps = measure_mbps(|| {
                let mut out = vec![0i32; *n];
                if decode(&enc_buf[..enc_len], &mut out).is_err() { return; }
                dequantize_optimal(&mut out, lbs, scale);
            }, coeff_bytes);

            println!("wce,{name},{n},{lb},{enc_len},{ratio:.2},{enc_mbps:.1},{dec_mbps:.1},{mse:.3},ok");
        }
    }
}
