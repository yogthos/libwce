#![allow(clippy::too_many_arguments)]

use wce::*;
use std::io::Write;
use std::fs;

struct Pgm { w: usize, h: usize, pixels: Vec<u8> }

fn skip_ws(data: &[u8], pos: &mut usize) {
    loop {
        while *pos < data.len() && (data[*pos] == b' ' || data[*pos] == b'\t' || data[*pos] == b'\n' || data[*pos] == b'\r') { *pos += 1; }
        if *pos >= data.len() || data[*pos] != b'#' { break; }
        while *pos < data.len() && data[*pos] != b'\n' { *pos += 1; }
    }
}

fn pgm_read(path: &str) -> Option<Pgm> {
    let data = fs::read(path).ok()?;
    let mut pos = 0;
    if data.get(pos..pos+2) != Some(b"P5") { return None; } pos += 2;
    skip_ws(&data, &mut pos);
    let w = parse_num(&data, &mut pos);
    skip_ws(&data, &mut pos);
    let h = parse_num(&data, &mut pos);
    skip_ws(&data, &mut pos);
    let maxval = parse_num(&data, &mut pos);
    if maxval != 255 { return None; }
    skip_ws(&data, &mut pos);
    if pos + w * h > data.len() { return None; }
    Some(Pgm { w, h, pixels: data[pos..pos + w * h].to_vec() })
}

fn parse_num(data: &[u8], pos: &mut usize) -> usize {
    let mut n = 0;
    while *pos < data.len() && data[*pos].is_ascii_digit() { n = n * 10 + (data[*pos] - b'0') as usize; *pos += 1; }
    n
}

fn pgm_write(path: &str, img: &Pgm) {
    let mut f = fs::File::create(path).unwrap();
    write!(f, "P5\n{} {}\n255\n", img.w, img.h).unwrap();
    f.write_all(&img.pixels).unwrap();
}

fn haar_1d(row: &mut [f64], forward: bool) {
    let n = row.len() & !1;
    let half = n / 2;
    let s = 1.0 / (2.0f64).sqrt();
    let tmp = row.to_vec();
    if forward {
        for i in 0..half {
            let (a, b) = (tmp[2*i], tmp[2*i+1]);
            row[i] = (a + b) * s;
            row[half + i] = (a - b) * s;
        }
    } else {
        for i in 0..half {
            let (l, h) = (tmp[i], tmp[half + i]);
            row[2*i] = (l + h) * s;
            row[2*i+1] = (l - h) * s;
        }
    }
}

fn haar_2d(buf: &mut [f64], w: usize, h: usize, forward: bool) {
    for i in 0..h { haar_1d(&mut buf[i*w..][..w], forward); }
    for i in 0..w {
        let mut col: Vec<f64> = (0..h).map(|j| buf[j*w + i]).collect();
        haar_1d(&mut col, forward);
        for j in 0..h { buf[j*w + i] = col[j]; }
    }
}

fn extract_subband(full: &[f64], w: usize, x0: usize, y0: usize, sw: usize, sh: usize, scale: f64) -> Vec<i32> {
    let mut out = vec![0i32; sw * sh];
    for y in 0..sh {
        for x in 0..sw {
            let v = full[(y0 + y) * w + (x0 + x)] * scale;
            out[y * sw + x] = (if v < 0.0 { v - 0.5 } else { v + 0.5 }) as i32;
        }
    }
    out
}

fn inject_subband(full: &mut [f64], w: usize, x0: usize, y0: usize, sw: usize, sh: usize, src: &[i32], scale: f64) {
    for y in 0..sh {
        for x in 0..sw {
            full[(y0 + y) * w + (x0 + x)] = src[y * sw + x] as f64 / scale;
        }
    }
}

fn psnr(a: &Pgm, b: &Pgm) -> f64 {
    let n = a.pixels.len();
    let mse: f64 = a.pixels.iter().zip(&b.pixels).map(|(&x, &y)| { let d = x as f64 - y as f64; d*d }).sum::<f64>() / n as f64;
    if mse <= 0.0 { f64::INFINITY } else { 10.0 * (255.0f64.powi(2) / mse).log10() }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let in_path = args.get(1).map_or("demo/Cthulhu.pgm", |s| s.as_str());
    let out_stem = args.get(2).map_or("demo/Cthulhu", |s| s.as_str());

    let orig = pgm_read(in_path).expect("failed to read PGM");
    if orig.w & 1 != 0 || orig.h & 1 != 0 { eprintln!("image dims must be even"); return; }
    let (w, h) = (orig.w, orig.h);
    let (hw, hh) = (w/2, h/2);

    let mut dwt: Vec<f64> = orig.pixels.iter().map(|&p| p as f64 - 128.0).collect();
    haar_2d(&mut dwt, w, h, true);

    let presets = [
        ("near-lossless", "q1", 2u8, 4u8, 4u8, 5u8),
        ("balanced",      "q2", 4u8, 6u8, 6u8, 7u8),
        ("aggressive",    "q3", 6u8, 8u8, 8u8, 9u8),
        ("very lossy",    "q4", 8u8, 10u8, 10u8, 11u8),
    ];

    let (scale_ll, scale_d) = (4.0, 8.0);
    let n_pixels = hw * hh;
    let pad = (n_pixels + 3) & !3;

    println!("libwce image compression demo (Rust)");
    println!("=====================================");
    println!("  input : {in_path}  ({w}x{h}, {} raw bytes)", orig.pixels.len());

    for &(name, suffix, lb_ll, lb_hl, lb_lh, lb_hh) in &presets {
        let mut ll = extract_subband(&dwt, w, 0, 0, hw, hh, scale_ll); ll.resize(pad, 0);
        let mut hl = extract_subband(&dwt, w, hw, 0, hw, hh, scale_d); hl.resize(pad, 0);
        let mut lh = extract_subband(&dwt, w, 0, hh, hw, hh, scale_d); lh.resize(pad, 0);
        let mut hh_band = extract_subband(&dwt, w, hw, hh, hw, hh, scale_d); hh_band.resize(pad, 0);

        let encode_band = |coeffs: &[i32], real: usize, lb: u8| -> (Vec<u8>, usize, f64) {
            let scale = estimate_laplacian_scale(&coeffs[..real]);
            let mut buf = vec![0u8; coeffs.len() * 5 + 64];
            let n = encode(coeffs, lb, &mut buf).unwrap();
            (buf, n, scale)
        };

        let (b_ll, n_ll, sc_ll) = encode_band(&ll, n_pixels, lb_ll);
        let (b_hl, n_hl, sc_hl) = encode_band(&hl, n_pixels, lb_hl);
        let (b_lh, n_lh, sc_lh) = encode_band(&lh, n_pixels, lb_lh);
        let (b_hh, n_hh, sc_hh) = encode_band(&hh_band, n_pixels, lb_hh);

        let decode_band = |buf: &[u8], n: usize, lb: u8, scale: f64| -> Vec<i32> {
            let mut out = vec![0i32; pad];
            decode(&buf[..n], &mut out).unwrap();
            dequantize_optimal(&mut out, lb, scale);
            out
        };

        let ll_d = decode_band(&b_ll, n_ll, lb_ll, sc_ll);
        let hl_d = decode_band(&b_hl, n_hl, lb_hl, sc_hl);
        let lh_d = decode_band(&b_lh, n_lh, lb_lh, sc_lh);
        let hh_d = decode_band(&b_hh, n_hh, lb_hh, sc_hh);

        let mut recon = vec![0.0f64; w * h];
        inject_subband(&mut recon, w, 0, 0, hw, hh, &ll_d, scale_ll);
        inject_subband(&mut recon, w, hw, 0, hw, hh, &hl_d, scale_d);
        inject_subband(&mut recon, w, 0, hh, hw, hh, &lh_d, scale_d);
        inject_subband(&mut recon, w, hw, hh, hw, hh, &hh_d, scale_d);
        haar_2d(&mut recon, w, h, false);

        let recon_img = Pgm {
            w, h,
            pixels: recon.iter().map(|&v| (v + 128.0).clamp(0.0, 255.0).round() as u8).collect(),
        };

        let pgm_path = format!("{out_stem}_{suffix}.pgm");
        let wce_path = format!("{out_stem}_{suffix}.wce");
        pgm_write(&pgm_path, &recon_img);

        {
            let mut f = fs::File::create(&wce_path).unwrap();
            f.write_all(b"WCE3").unwrap();
            f.write_all(&(w as u16).to_le_bytes()).unwrap();
            f.write_all(&(h as u16).to_le_bytes()).unwrap();
            f.write_all(&[lb_ll, lb_hl, lb_lh, lb_hh]).unwrap();
            f.write_all(&sc_ll.to_le_bytes()).unwrap();
            f.write_all(&sc_hl.to_le_bytes()).unwrap();
            f.write_all(&sc_lh.to_le_bytes()).unwrap();
            f.write_all(&sc_hh.to_le_bytes()).unwrap();
            f.write_all(&(n_ll as u32).to_le_bytes()).unwrap();
            f.write_all(&(n_hl as u32).to_le_bytes()).unwrap();
            f.write_all(&(n_lh as u32).to_le_bytes()).unwrap();
            f.write_all(&(n_hh as u32).to_le_bytes()).unwrap();
            f.write_all(&b_ll[..n_ll]).unwrap();
            f.write_all(&b_hl[..n_hl]).unwrap();
            f.write_all(&b_lh[..n_lh]).unwrap();
            f.write_all(&b_hh[..n_hh]).unwrap();
        }

        let total_bytes = n_ll + n_hl + n_lh + n_hh;
        let file_bytes = 60 + total_bytes;
        let ratio = orig.pixels.len() as f64 / file_bytes as f64;
        let p = psnr(&orig, &recon_img);

        println!("  {name:>13}  lb={lb_ll}/{lb_hl}/{lb_lh}/{lb_hh}  {total_bytes:>6}B payload  {file_bytes:>7}B file  {ratio:>5.2}x  {p:>5.2} dB");
    }
}
