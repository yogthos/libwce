use wce::*;

fn quantize_coeff(c: i32, lossy_bits: u8) -> i32 {
    if lossy_bits >= 32 { return 0; }
    let mag = (c.unsigned_abs() >> lossy_bits) << lossy_bits;
    if c >= 0 { mag as i32 } else { (0u32.wrapping_sub(mag)) as i32 }
}

fn roundtrip(coeffs: &[i32], lossy_bits: u8) {
    let mut buf = vec![0u8; 16384];
    let out_len = encode(coeffs, lossy_bits, &mut buf).expect("encode");
    assert!(out_len >= HEADER_SIZE);
    let mut decoded = vec![0i32; coeffs.len()];
    let lb = decode(&buf[..out_len], &mut decoded).expect("decode");
    assert_eq!(lb, lossy_bits);
    for (i, &w) in coeffs.iter().enumerate() {
        assert_eq!(decoded[i], quantize_coeff(w, lossy_bits));
    }
}

// -- bitio

#[test]
fn bitio_write_single_bit() {
    let mut buf = [0u8; 1];
    { let mut bw = BitWriter::new(&mut buf); bw.write_bits(1, 1); bw.flush(); assert_eq!(bw.bytes_written(), 1); }
    assert_eq!(buf[0], 0x01);
}

#[test]
fn bitio_lsb_first_ordering() {
    let mut buf = [0u8; 2];
    { let mut bw = BitWriter::new(&mut buf); bw.write_bits(0xA, 4); bw.write_bits(0xB, 4); bw.write_bits(0xC, 4); bw.write_bits(0xD, 4); bw.flush(); }
    assert_eq!(buf[0], 0xBA); assert_eq!(buf[1], 0xDC);
}

#[test]
fn bitio_roundtrip_assorted_widths() {
    let vals: [u32; 8] = [0, 1, 31, 1023, 0x12345678, 7, 0, 0xFFFFFFFF];
    let widths: [u8; 8] = [1, 2, 5, 10, 32, 3, 7, 32];
    let mut buf = [0u8; 64];
    let written = {
        let mut bw = BitWriter::new(&mut buf);
        for (i, &v) in vals.iter().enumerate() { bw.write_bits(v, widths[i]); }
        bw.flush(); assert!(!bw.is_overflow()); bw.bytes_written()
    };
    let mut br = BitReader::new(&buf[..written]);
    for (i, &v) in vals.iter().enumerate() { assert_eq!(br.read_bits(widths[i]), v); }
    assert!(!br.is_truncated());
}

#[test]
fn bitio_overflow_flag() {
    let mut buf = [0u8; 2];
    let ov = { let mut bw = BitWriter::new(&mut buf); bw.write_bits(0xFFFF, 16); bw.write_bits(1, 1); bw.flush(); bw.is_overflow() };
    assert!(ov);
}

#[test]
fn bitio_truncated_returns_zero() {
    let mut br = BitReader::new(&[0x55]);
    assert_eq!(br.read_bits(8), 0x55); assert_eq!(br.read_bits(8), 0); assert!(br.is_truncated());
}

#[test]
fn bitio_zero_n_is_noop() {
    let mut br = BitReader::new(&[0xFF]);
    assert_eq!(br.read_bits(0), 0); assert_eq!(br.read_bits(8), 0xFF);
}

#[test]
fn bitio_byte_align() {
    let mut br = BitReader::new(&[0xF0, 0x55]);
    br.read_bits(3); br.byte_align(); assert_eq!(br.read_bits(8), 0x55);
}

#[test]
fn bitio_bytes_consumed() {
    let mut br = BitReader::new(&[1, 2, 3, 4]);
    assert_eq!(br.bytes_consumed(), 0); br.read_bits(8); assert_eq!(br.bytes_consumed(), 1); br.read_bits(4); assert_eq!(br.bytes_consumed(), 2);
}

// zigzag

#[test] fn zigzag_small() { assert_eq!(zigzag_encode(0), 0); assert_eq!(zigzag_encode(-1), 1); assert_eq!(zigzag_encode(1), 2); assert_eq!(zigzag_encode(-2), 3); assert_eq!(zigzag_encode(2), 4); }
#[test] fn zigzag_extremes() { assert_eq!(zigzag_encode(i32::MAX), 0xFFFFFFFE); assert_eq!(zigzag_encode(i32::MIN), 0xFFFFFFFF); }

#[test]
fn zigzag_roundtrip() {
    for &v in &[0, 1, -1, 100, -100, i32::MAX, i32::MIN, -42, 0x1234567, -0x1234567] {
        assert_eq!(zigzag_decode(zigzag_encode(v)), v);
    }
}

// rice

#[test]
fn rice_k0_unary() {
    let mut buf = [0u8; 8];
    { let mut bw = BitWriter::new(&mut buf); write_rice(&mut bw, 3, 0); bw.flush(); }
    let mut br = BitReader::new(&buf); assert_eq!(read_rice(&mut br, 0), 3);
}

#[test]
fn rice_k2_split() {
    let mut buf = [0u8; 4];
    { let mut bw = BitWriter::new(&mut buf); write_rice(&mut bw, 11, 2); bw.flush(); }
    let mut br = BitReader::new(&buf); assert_eq!(read_rice(&mut br, 2), 11);
}

#[test]
fn rice_roundtrip_many() {
    let vals = [0u32, 1, 2, 5, 10, 31, 63, 100, 200, 1024, 65535];
    let mut buf = [0u8; 1024];
    for k in 0..=12u8 {
        let filtered: Vec<u32> = vals.iter().copied().filter(|&v| (v >> k) < RICE_MAX_QUOTIENT).collect();
        let written = { let mut bw = BitWriter::new(&mut buf); for &v in &filtered { write_rice(&mut bw, v, k); } bw.flush(); assert!(!bw.is_overflow()); bw.bytes_written() };
        let mut br = BitReader::new(&buf[..written]);
        for &e in &filtered { assert_eq!(read_rice(&mut br, k), e); }
    }
}

#[test]
fn rice_large_k16() {
    let mut buf = [0u8; 64];
    let w = { let mut bw = BitWriter::new(&mut buf); write_rice(&mut bw, 0x123456, 16); bw.flush(); bw.bytes_written() };
    let mut br = BitReader::new(&buf[..w]); assert_eq!(read_rice(&mut br, 16), 0x123456);
}

#[test]
fn rice_all_ones() {
    let mut br = BitReader::new(&[0xFFu8; 64]);
    assert_eq!(read_rice(&mut br, 4), u32::MAX); assert!(!br.is_truncated()); assert_eq!(br.bytes_consumed(), 32);
}

#[test]
fn rice_q255_boundary() {
    let tests = [(255u32, 0u8), (255 << 1, 1), (255 << 2, 2), (255 << 4, 4), (255 << 8, 8)];
    let mut buf = [0u8; 512];
    for &(val, k) in &tests {
        let w = { let mut bw = BitWriter::new(&mut buf); write_rice(&mut bw, val, k); bw.flush(); assert!(!bw.is_overflow()); bw.bytes_written() };
        assert_eq!(read_rice(&mut BitReader::new(&buf[..w]), k), val);
    }
    let ov = { let mut bw = BitWriter::new(&mut buf); write_rice(&mut bw, 256, 0); bw.flush(); (bw.is_overflow(), bw.bytes_written()) };
    assert!(ov.0); assert_eq!(ov.1, 0);
}

#[test]
fn rice_truncated() {
    let mut br = BitReader::new(&[]);
    assert_eq!(read_rice(&mut br, 3), 0); assert!(br.is_truncated());
}

// bpc

#[test] fn bpc_all_zero() { assert_eq!(&compute_bpcs(&[0i32; 8], 0)[..], &[0, 0]); }
#[test] fn bpc_lossy_floor() { assert_eq!(compute_bpcs(&[0i32; 4], 3)[0], 3); }

#[test]
fn bpc_correct_widths() {
    let c = [1,0,0,0, 0,0,3,0, -7,1,0,-2, 0,-1024,0,100];
    let b = compute_bpcs(&c, 0); assert_eq!(&b[..], &[1, 2, 3, 11]);
}

#[test] fn bpc_int32_min() { assert_eq!(compute_bpcs(&[i32::MIN, 0, 0, 0], 0)[0], 32); }

fn dpcm_rt(bpcs: &[u8]) {
    let k = pick_rice_k_for_bpcs(bpcs, 6); assert!(k <= 6);
    let mut buf = [0u8; 1024]; let mut out = vec![0u8; bpcs.len()];
    let w = { let mut bw = BitWriter::new(&mut buf); encode_bpcs_dpcm(&mut bw, bpcs, k); bw.flush(); assert!(!bw.is_overflow()); bw.bytes_written() };
    decode_bpcs_dpcm(&mut BitReader::new(&buf[..w]), bpcs.len(), bpcs[0], k, &mut out);
    assert_eq!(out, bpcs);
}

#[test] fn dpcm_smooth() { dpcm_rt(&[5,5,5,6,6,6,7,7,8,8]); }
#[test] fn dpcm_jumpy() { dpcm_rt(&[0,8,0,16,4,12,2,20,1,5]); }
#[test] fn dpcm_long_constant() { dpcm_rt(&vec![7u8; 100]); }

#[test]
fn dpcm_single_group() {
    let mut buf = [0u8; 16];
    let w = { let mut bw = BitWriter::new(&mut buf); encode_bpcs_dpcm(&mut bw, &[5], 0); bw.flush(); bw.bytes_written() };
    assert_eq!(w, 0);
    decode_bpcs_dpcm(&mut BitReader::new(&[]), 1, 5, 0, &mut [0]);
    // initial written directly
}

#[test]
fn dpcm_caps_out_of_range() {
    let mut out = [0u8];
    decode_bpcs_dpcm(&mut BitReader::new(&[]), 1, 99, 0, &mut out);
    assert_eq!(out[0], 32);
}

#[test]
fn dpcm_clamps_negative_delta() {
    let mut buf = [0u8; 16]; let mut out = [0u8; 3];
    let w = { let mut bw = BitWriter::new(&mut buf); encode_bpcs_dpcm(&mut bw, &[2,0,0], 0); bw.flush(); bw.bytes_written() };
    decode_bpcs_dpcm(&mut BitReader::new(&buf[..w]), 3, 2, 0, &mut out);
    assert_eq!(out, [2, 0, 0]);
}

#[test]
fn dpcm_rice_corruption() {
    let mut out = [0u8; 5];
    decode_bpcs_dpcm(&mut BitReader::new(&[0xFFu8; 64]), 5, 7, 0, &mut out);
    assert_eq!(out, [7, 7, 7, 7, 7]);
}

#[test] fn pick_k_constant() { assert_eq!(pick_rice_k_for_bpcs(&vec![5u8; 20], 6), 0); }
#[test] fn pick_k_big_jumps() { assert!(pick_rice_k_for_bpcs(&[0,16,0,16,0,16,0,16], 6) >= 3); }

// pack

fn pack_rt(coeffs: &[i32], lossy_bits: u8) {
    let num_groups = coeffs.len() / 4;
    let bpcs = compute_bpcs(coeffs, lossy_bits);
    let mut buf = [0u8; 8192]; let mut out = vec![0i32; coeffs.len()];
    let w = { let mut bw = BitWriter::new(&mut buf); pack_coeffs(&mut bw, coeffs, &bpcs, lossy_bits, num_groups); bw.flush(); assert!(!bw.is_overflow()); bw.bytes_written() };
    unpack_coeffs(&mut BitReader::new(&buf[..w]), &bpcs, lossy_bits, num_groups, &mut out);
    for (i, &c) in coeffs.iter().enumerate() { assert_eq!(out[i], quantize_coeff(c, lossy_bits), "bad at {i}"); }
}

#[test] fn coeffs_all_zero() { pack_rt(&[0i32; 16], 0); pack_rt(&[0i32; 16], 3); }
#[test] fn coeffs_small_lossless() { pack_rt(&[1,-1,2,-2,0,7,-7,100], 0); }
#[test] fn coeffs_lossy() { pack_rt(&[1,-1,7,-8,16,-16,100,-100,0,0,0,0,1024,-1024,5000,-5000], 3); pack_rt(&[1,-1,7,-8,16,-16,100,-100,0,0,0,0,1024,-1024,5000,-5000], 5); }
#[test] fn coeffs_int32_extremes() { pack_rt(&[i32::MAX, i32::MIN, 0, 1], 0); pack_rt(&[i32::MAX, i32::MIN, 0, 1], 5); }

#[test]
fn coeffs_lossy_kills_small() {
    let c = [7i32, -7, 4, -4]; let bpcs = compute_bpcs(&c, 3); assert_eq!(bpcs[0], 3);
    let mut buf = [0u8; 64]; let mut out = [0i32; 4];
    let w = { let mut bw = BitWriter::new(&mut buf); pack_coeffs(&mut bw, &c, &bpcs, 3, 1); bw.flush(); bw.bytes_written() };
    unpack_coeffs(&mut BitReader::new(&buf[..w]), &bpcs, 3, 1, &mut out);
    assert_eq!(out, [0, 0, 0, 0]);
}

#[test]
fn coeffs_no_signs_for_zeros() {
    let c = [16i32, 0, 0, -16]; let bpcs = compute_bpcs(&c, 3); assert_eq!(bpcs[0], 5);
    let mut buf = [0u8; 64];
    let w = { let mut bw = BitWriter::new(&mut buf); pack_coeffs(&mut bw, &c, &bpcs, 3, 1); bw.flush(); bw.bytes_written() };
    assert_eq!(w, 2);
}

#[test]
fn coeffs_large_roundtrip() {
    let mut c = [0i32; 256];
    for i in 0..256 {
        c[i] = if i < 4 { [i32::MAX, i32::MIN, 0x7FFFFFFF, 0x80000001u32 as i32][i] }
        else { let v = i as i32 * 13 + 1; if i & 1 != 0 { -v } else { if i % 7 == 0 { 0 } else { v } } };
    }
    pack_rt(&c, 0); pack_rt(&c, 2); pack_rt(&c, 5);
}

#[test]
fn coeffs_lossy_bits_31() { pack_rt(&[i32::MIN, i32::MAX, -1, 1, 0, 0x40000000u32 as i32, -0x40000000i64 as i32, 42], 31); }

#[test]
fn coeffs_saturates_int32_max() {
    let c = [i32::MAX-1, i32::MAX, i32::MIN+1, i32::MIN]; let bpcs = compute_bpcs(&c, 1); assert_eq!(bpcs[0], 32);
    let mut buf = [0u8; 256]; let mut out = [0i32; 4];
    let w = { let mut bw = BitWriter::new(&mut buf); pack_coeffs(&mut bw, &c, &bpcs, 1, 1); bw.flush(); assert!(!bw.is_overflow()); bw.bytes_written() };
    unpack_coeffs(&mut BitReader::new(&buf[..w]), &bpcs, 1, 1, &mut out);
    assert_eq!(out, [2147483646, 2147483646, -2147483646, i32::MIN]);
}

// quantize

#[test] fn q_lossy_zero_noop() { let mut a = [0,7,-3,1000,-16000]; let orig = a; quantize(&mut a, 0); assert_eq!(a, orig); }
#[test] fn q_lossy_32_zeros_all() { let mut a = [7,-3,i32::MIN]; quantize(&mut a, 32); assert_eq!(a, [0,0,0]); }
#[test] fn q_lossy_31_keeps_min() { let mut a = [0,1,-1,i32::MIN,i32::MAX]; quantize(&mut a, 31); assert_eq!(a[3], i32::MIN); assert_eq!(a[4], 0); }
#[test] fn q_truncates_positive() { let mut a = [0,1,2,3,4,7]; quantize(&mut a, 2); assert_eq!(a, [0,0,0,0,4,4]); }
#[test] fn q_truncates_negative() { let mut a = [-1,-2,-3,-4,-7,-8]; quantize(&mut a, 2); assert_eq!(a, [0,0,0,-4,-4,-8]); }
#[test] fn q_int32_min_no_ub() { let mut a = [i32::MIN]; quantize(&mut a, 3); assert_eq!(a[0], i32::MIN); }
#[test] fn q_zero_stays_zero() { let mut a = [0]; quantize(&mut a, 5); assert_eq!(a[0], 0); }
#[test] fn quantize_lands_on_grid() { let mut a = [0,1,7,8,9,15,16,-17]; quantize(&mut a, 3); for v in &a { assert_eq!(v & 7, 0); } }
#[test] fn est_scale_empty() { assert_eq!(estimate_laplacian_scale(&[]), 0.0); }
#[test] fn est_scale_mean_abs() { assert!((estimate_laplacian_scale(&[10,-10,20,-20,0]) - 12.0).abs() < 1e-9); }
#[test] fn dq_zero_scale_noop() { let mut a = [8,-8,16]; let orig = a; dequantize_optimal(&mut a, 3, 0.0); assert_eq!(a, orig); }
#[test] fn dq_lossy_zero_noop() { let mut a = [5,-5,7]; let orig = a; dequantize_optimal(&mut a, 0, 10.0); assert_eq!(a, orig); }
#[test] fn dq_zero_stays_zero() { let mut a = [0,0,0,0]; dequantize_optimal(&mut a, 3, 10.0); assert_eq!(a, [0,0,0,0]); }
#[test] fn dq_fine_midpoint() { let mut a = [8,-8]; dequantize_optimal(&mut a, 3, 10000.0); assert_eq!(a, [12,-12]); }
#[test] fn dq_coarse_zero() { let mut a = [256]; dequantize_optimal(&mut a, 8, 2.0); assert!(a[0] >= 257 && a[0] <= 259); }
#[test] fn dq_saturates_max() { let mut a = [i32::MAX-2]; dequantize_optimal(&mut a, 3, 10000.0); assert_eq!(a[0], i32::MAX); }
#[test] fn dq_saturates_min() { let mut a = [i32::MIN]; dequantize_optimal(&mut a, 3, 10000.0); assert_eq!(a[0], i32::MIN); }
#[test] fn dq_lossy_31() { let mut a = [0,i32::MIN]; dequantize_optimal(&mut a, 31, 100.0); assert_eq!(a, [0,i32::MIN]); }
#[test] fn dq_nan_noop() { let mut a = [8,-8]; let orig = a; dequantize_optimal(&mut a, 3, f64::NAN); assert_eq!(a, orig); }
#[test] fn dq_inf_noop() { let mut a = [8,-8]; let orig = a; dequantize_optimal(&mut a, 3, f64::INFINITY); assert_eq!(a, orig); }

// codec

#[test] fn codec_tiny() { roundtrip(&[0,0,0,0], 0); }

#[test] fn codec_lossless() { let mut c = [0i32; 16]; for i in 0..16 { c[i] = if i&1!=0 { -(i as i32*7) } else { i as i32*5 }; } roundtrip(&c, 0); }

#[test] fn codec_lossy_levels() {
    let mut c = [0i32; 64]; for i in 0..64 { c[i] = (i as i32*97-1024) * if i&1!=0 { -1 } else { 1 }; }
    roundtrip(&c, 2); roundtrip(&c, 5);
}

#[test] fn codec_int32_extremes() { roundtrip(&[i32::MAX,i32::MIN,0,-1,1,-2,2,1234567], 0); roundtrip(&[i32::MAX,i32::MIN,0,-1,1,-2,2,1234567], 5); }

#[test] fn codec_larger() { let mut c = [0i32; 2048]; for i in 0..2048 { let v = ((i*13+7)&0xFFF) as i32; c[i] = if i&1!=0 { -v } else { v }; } roundtrip(&c, 0); roundtrip(&c, 3); }

#[test] fn codec_rejects_unaligned() { let mut b = [0u8; 64]; assert_eq!(encode(&[0i32; 5], 0, &mut b), Err(Error::BadInput)); }
#[test] fn codec_rejects_bad_lossy() { let mut b = [0u8; 64]; assert_eq!(encode(&[0i32; 4], 32, &mut b), Err(Error::BadInput)); assert_eq!(encode(&[0i32; 4], 99, &mut b), Err(Error::BadInput)); }
#[test] fn codec_rejects_small_cap() { let mut b = [0u8; 4]; assert_eq!(encode(&[0i32; 4], 0, &mut b), Err(Error::NoSpace)); }
#[test] fn codec_rejects_bad_magic() { assert_eq!(decode(&[0u8; 12], &mut [0i32; 4]), Err(Error::BadMagic)); }

#[test] fn codec_rejects_bad_version() {
    let mut b = [0u8; 12]; b[..4].copy_from_slice(b"WCE\0"); b[4] = 1; b[8] = FORMAT_VERSION+99;
    assert_eq!(decode(&b, &mut [0i32; 4]), Err(Error::BadVersion));
}

#[test]
fn codec_rejects_size_mismatch() {
    let mut b = [0u8; 64]; let n = encode(&[1,2,3,4], 0, &mut b).unwrap();
    assert_eq!(decode(&b[..n], &mut [0i32; 8]), Err(Error::BadInput));
}

#[test] fn codec_truncated() { let c = [100,-200,300,-400,500,-600,700,-800]; let mut b = [0u8; 128]; let n = encode(&c, 0, &mut b).unwrap(); let h = HEADER_SIZE + (n-HEADER_SIZE)/2; assert_eq!(decode(&b[..h], &mut [0i32; 8]), Err(Error::Truncated)); }

#[test] fn codec_empty_band() { let mut b = [0u8; 64]; let n = encode(&[], 0, &mut b).unwrap(); assert_eq!(decode(&b[..n], &mut []), Ok(0)); }

#[test] fn codec_forced_mode_roundtrip() {
    let mut c = [0i32; 64]; for i in 0..64 { c[i] = (i as i32*97-1024) * if i&1!=0 { -1 } else { 1 }; }
    for &(pred, flag) in &[(PREDICTOR_RUNNING, false), (PREDICTOR_RUNNING, true), (PREDICTOR_ZERO, false), (PREDICTOR_ZERO, true)] {
        for &rk in &[0u8, 3, 6] {
            let mut b = [0u8; 4096];
            let n = encode_with_options(&c, 3, Some(&EncodeOptions{predictor:pred, sparse_flag:flag, rice_k:rk}), &mut b).unwrap();
            let mut out = [0i32; 64]; assert_eq!(decode(&b[..n], &mut out), Ok(3));
            for (i, &ci) in c.iter().enumerate() { assert_eq!(out[i], quantize_coeff(ci, 3), "bad at {i}"); }
        }
    }
}

#[test] fn codec_sparse_block_mix() { let mut c = [0i32; 256]; c[0]=1024; c[1]=-512; c[128]=256; c[129]=-128; roundtrip(&c, 3); }

#[test] fn codec_partial_final_block() { let mut c = [0i32; 40]; for i in 0..40 { c[i] = (i as i32*7) * if i&1!=0 { -1 } else { 1 }; } roundtrip(&c, 0); }

#[test] fn codec_corrupt_rice() { let mut c = [0i32; 32]; for i in 0..32 { c[i] = (i as i32+1)*100 * if i&1!=0 { -1 } else { 1 }; } let mut b = [0u8; 2048]; let n = encode(&c, 0, &mut b).unwrap(); b[HEADER_SIZE..n].fill(0xFF); assert_eq!(decode(&b[..n], &mut [0i32; 32]), Err(Error::Corrupt)); }

#[test] fn codec_forced_bad_input() {
    let c = [1,2,3,4]; let mut b = [0u8; 256];
    assert_eq!(encode_with_options(&c, 0, Some(&EncodeOptions{predictor:99,sparse_flag:false,rice_k:0}), &mut b), Err(Error::BadInput));
    assert_eq!(encode_with_options(&c, 0, Some(&EncodeOptions{predictor:PREDICTOR_RUNNING,sparse_flag:false,rice_k:17}), &mut b), Err(Error::BadInput));
    assert_eq!(encode_with_options(&c, 0, Some(&EncodeOptions{predictor:PREDICTOR_RUNNING,sparse_flag:false,rice_k:99}), &mut b), Err(Error::BadInput));
}

#[test] fn codec_decode_bad_lossy() { let mut h = [0u8; 12]; h[..4].copy_from_slice(b"WCE\0"); h[4]=1; h[8]=FORMAT_VERSION; h[9]=33; assert_eq!(decode(&h, &mut [0i32; 4]), Err(Error::BadInput)); }
#[test] fn codec_too_many_groups() { assert_eq!(encode(&vec![0i32; (MAX_INLINE_GROUPS+1)*4], 0, &mut [0u8; 64]), Err(Error::BadInput)); }

#[test] fn codec_initial_prev_mismatch() { let c = [1,2,3,4]; let mut b = [0u8; 256]; let n = encode(&c, 3, &mut b).unwrap(); b[11] = 99; assert_eq!(decode(&b[..n], &mut [0i32; 4]), Err(Error::BadInput)); }
