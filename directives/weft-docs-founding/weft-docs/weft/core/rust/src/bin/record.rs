// record.rs — Capture/replay tool (Rust, WO-P2-TOOLS T5)
// Mirror of tools/weft-record/weft_record.c. Same .weftrec v1 format.
use std::io::{Read, Write, Seek, SeekFrom};
use std::fs::{File, OpenOptions};
use std::time::{Duration, Instant};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::thread;
use weft_core::{pat, Weft};

const WREC_MAGIC: u32 = 0x43455257;
const WREC_FORMAT_VERSION: u16 = 1;
const WREC_HEADER_SIZE: u16 = 32;

// CRC-32/zlib (reflected poly 0xEDB88320)
fn crc32_compute(buf: &[u8]) -> u32 {
    let mut table = [0u32; 256];
    for i in 0..256u32 {
        let mut c = i;
        for _ in 0..8 { c = if c & 1 != 0 { 0xEDB88320 ^ (c >> 1) } else { c >> 1 }; }
        table[i as usize] = c;
    }
    let mut crc = 0xFFFFFFFFu32;
    for &b in buf { crc = table[((crc ^ b as u32) & 0xFF) as usize] ^ (crc >> 8); }
    crc ^ 0xFFFFFFFFu32
}

fn fill_payload(w: &Weft, seq: u32, payload_len: u32) {
    unsafe {
        let mut buf = vec![0u8; payload_len as usize];
        for i in 0..payload_len { buf[i as usize] = pat(seq, i); }
        w.w_write_payload(buf.as_ptr(), payload_len as usize).ok();
    }
}

fn cmd_capture(file: &str, hz: u64, payload_max: usize, secs: u64) -> i32 {
    let w = Arc::new(Weft::new(payload_max).unwrap());
    let stop = Arc::new(AtomicBool::new(false));
    let published = Arc::new(AtomicU64::new(0));
    let w2 = w.clone(); let stop2 = stop.clone(); let pub2 = published.clone();
    let writer = thread::spawn(move || {
        let period = Duration::from_nanos(1_000_000_000 / hz);
        let mut next = Instant::now() + period;
        let deadline = Instant::now() + Duration::from_secs(secs);
        let mut seq = 1u32;
        while !stop2.load(Ordering::Relaxed) && Instant::now() < deadline {
            unsafe { fill_payload(&w2, seq, payload_max as u32); w2.publish(seq, payload_max as u32); }
            pub2.fetch_add(1, Ordering::Relaxed);
            seq += 1;
            let now = Instant::now();
            if now < next { thread::sleep(next - now); }
            next += period;
        }
    });

    let mut f = match OpenOptions::new().write(true).create(true).truncate(true).read(true).open(file) {
        Ok(f) => f,
        Err(e) => { eprintln!("cannot open {}: {}", file, e); stop.store(true, Ordering::Relaxed); writer.join().unwrap(); return 1; }
    };

    // Write file header
    let mut header = [0u8; 32];
    header[0..4].copy_from_slice(&WREC_MAGIC.to_le_bytes());
    header[4..6].copy_from_slice(&WREC_FORMAT_VERSION.to_le_bytes());
    header[6..8].copy_from_slice(&WREC_HEADER_SIZE.to_le_bytes());
    // flags = 0 (bytes 8..12)
    header[12..16].copy_from_slice(&1u32.to_le_bytes()); // envelope_version = 1
    // frame_count = 0 (bytes 16..20) — patched at close
    let hdr_crc = crc32_compute(&header[0..20]);
    header[20..24].copy_from_slice(&hdr_crc.to_le_bytes());
    f.write_all(&header).unwrap();

    // Capture loop
    //
    // Stale-tracking per WO-P4-CLOSURE §3 C2 / WO-P5-RELEASE T0 C2: with a triad
    // of 3 buffers, the reader can see oscillating seqs across claims (buffer A
    // holds the most-recent writer-published seq; buffers B and C hold older
    // seqs). A naive `s != last_seq` predicate over-counts as fresh because
    // every claim sees a different (older) seq from the previous claim. The
    // correct predicate is: fresh iff `s > max_seq_seen_so_far`. The writer's
    // actual publish rate is `max_seq_seen / elapsed_s`, NOT `frame_count / elapsed_s`.
    let deadline = Instant::now() + Duration::from_secs(secs);
    let mut frame_count: u64 = 0;
    let mut stale_returns: u64 = 0;
    let mut max_seq_seen: u32 = 0;

    while Instant::now() < deadline {
        w.claim();
        let s = w.r_seq();
        let plen = w.r_payload_len();

        if s > max_seq_seen {
            let rec_len: u32 = 4 + 16 + plen + 4;
            let mut rec = vec![0u8; rec_len as usize];
            rec[0..4].copy_from_slice(&rec_len.to_le_bytes());
            // Read envelope + payload from live buffer
            unsafe {
                let env_ptr = w.r_live_ptr(0);
                if !env_ptr.is_null() {
                    std::ptr::copy_nonoverlapping(env_ptr, rec.as_mut_ptr().add(4), 16);
                }
                let payload_ptr = w.r_live_ptr(16);
                if !payload_ptr.is_null() && plen > 0 {
                    std::ptr::copy_nonoverlapping(payload_ptr, rec.as_mut_ptr().add(4 + 16), plen as usize);
                }
            }
            let rec_crc = crc32_compute(&rec[4..4 + 16 + plen as usize]);
            rec[4 + 16 + plen as usize..].copy_from_slice(&rec_crc.to_le_bytes());
            f.write_all(&rec).unwrap();

            frame_count += 1;
            max_seq_seen = s;
        } else {
            stale_returns += 1;
        }
    }

    stop.store(true, Ordering::Relaxed);
    writer.join().unwrap();

    // Patch frame_count + recompute header CRC (WO-P2-CLOSURE B3 fix)
    // Use OpenOptions with read+write+create (not truncate) to reopen safely.
    // The original bug was using File::create (O_WRONLY|O_TRUNC) which doesn't
    // support seek+write. The fix: open existing file with read+write.
    header[16..20].copy_from_slice(&(frame_count as u32).to_le_bytes());
    let hdr_crc = crc32_compute(&header[0..20]);
    header[20..24].copy_from_slice(&hdr_crc.to_le_bytes());
    // Use the same file handle — just seek and write (the handle was opened with read+write).
    f.seek(SeekFrom::Start(16)).unwrap();
    f.write_all(&header[16..24]).unwrap();  // frame_count + CRC
    f.flush().unwrap();

    eprintln!("capture: {} frames, {} stale returns, max_seq={} (frame_count patched)", frame_count, stale_returns, max_seq_seen);
    0
}

fn cmd_replay(file: &str) -> i32 {
    let mut f = match File::open(file) {
        Ok(f) => f,
        Err(e) => { eprintln!("cannot open {}: {}", file, e); return 1; }
    };

    let mut header = [0u8; 32];
    if f.read_exact(&mut header).is_err() {
        eprintln!("replay: short read on header");
        return 1;
    }

    let magic = u32::from_le_bytes(header[0..4].try_into().unwrap());
    let fmt_ver = u16::from_le_bytes(header[4..6].try_into().unwrap());
    let frame_count = u32::from_le_bytes(header[16..20].try_into().unwrap());
    let stored_crc = u32::from_le_bytes(header[20..24].try_into().unwrap());

    if magic != WREC_MAGIC { eprintln!("replay: bad magic 0x{:08X}", magic); return 1; }
    if fmt_ver != WREC_FORMAT_VERSION { eprintln!("replay: bad format_version {}", fmt_ver); return 1; }

    let computed_crc = crc32_compute(&header[0..20]);
    if computed_crc != stored_crc {
        eprintln!("replay: header CRC mismatch (computed=0x{:08X}, stored=0x{:08X})", computed_crc, stored_crc);
        return 1;
    }

    eprintln!("replay: header OK (fmt_ver={}, frame_count={})", fmt_ver, frame_count);

    let mut actual_count: u64 = 0;
    loop {
        let mut rec_len_buf = [0u8; 4];
        match f.read_exact(&mut rec_len_buf) {
            Ok(_) => {}
            Err(_) => break, // EOF
        }
        let rec_len = u32::from_le_bytes(rec_len_buf);
        if rec_len < 24 { eprintln!("replay: record {}: rec_len={} too short", actual_count, rec_len); break; }

        let payload_len = (rec_len - 4 - 16 - 4) as usize;
        let mut verify_buf = vec![0u8; 16 + payload_len];
        if f.read_exact(&mut verify_buf).is_err() {
            eprintln!("replay: record {}: short read", actual_count); break;
        }

        let mut crc_buf = [0u8; 4];
        if f.read_exact(&mut crc_buf).is_err() {
            eprintln!("replay: record {}: short read on CRC", actual_count); break;
        }
        let stored_rec_crc = u32::from_le_bytes(crc_buf);

        // Check envelope magic
        let env_magic = u32::from_le_bytes(verify_buf[0..4].try_into().unwrap());
        if env_magic != 0x54464557 { eprintln!("replay: record {}: bad envelope magic", actual_count); break; }

        // Verify CRC
        let computed_rec_crc = crc32_compute(&verify_buf);
        if computed_rec_crc != stored_rec_crc {
            eprintln!("replay: record {}: CRC mismatch", actual_count); break;
        }

        actual_count += 1;
    }

    if frame_count > 0 && actual_count != frame_count as u64 {
        eprintln!("replay: frame_count mismatch (header={}, actual={})", frame_count, actual_count);
        return 1;
    }

    eprintln!("replay: {} records validated, all CRCs OK", actual_count);
    0
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 { eprintln!("usage: record <capture|replay> <file> [--hz N] [--payload N] [--secs N]"); std::process::exit(2); }

    match args[1].as_str() {
        "capture" => {
            let file = &args[2];
            let mut hz = 120u64; let mut payload = 64usize; let mut secs = 30u64;
            let mut i = 3;
            while i < args.len() {
                match args[i].as_str() {
                    "--hz" => { if i + 1 < args.len() { hz = args[i + 1].parse().unwrap_or(120); } }
                    "--payload" => { if i + 1 < args.len() { payload = args[i + 1].parse().unwrap_or(64); } }
                    "--secs" => { if i + 1 < args.len() { secs = args[i + 1].parse().unwrap_or(30); } }
                    _ => {}
                }
                i += 1;
            }
            std::process::exit(cmd_capture(file, hz, payload, secs));
        }
        "replay" => { std::process::exit(cmd_replay(&args[2])); }
        _ => { eprintln!("unknown command: {}", args[1]); std::process::exit(2); }
    }
}
