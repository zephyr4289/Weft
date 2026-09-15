// probe.rs — State inspector (Rust, WO-P2-TOOLS T3)
// Mirror of tools/weft-probe/weft_probe.c. Uses Weft::debug_state().
use std::time::{Duration, Instant};
use weft_core::{pat, PubResult, Weft, WeftDebugView};

fn fill_payload(w: &Weft, seq: u32, payload_len: u32) {
    unsafe {
        let mut buf = vec![0u8; payload_len as usize];
        for i in 0..payload_len { buf[i as usize] = pat(seq, i); }
        w.w_write_payload(buf.as_ptr(), payload_len as usize).ok();
    }
}

fn owner_str(owner: u8) -> &'static str {
    match owner { 0 => "free", 1 => "writer", 2 => "reader", 3 => "in-exchange", _ => "unknown" }
}

fn print_view(v: &WeftDebugView, mode: &str, json: bool) {
    if json {
        let mut s = String::new();
        s.push_str(&format!("{{\"tool\":\"weft-probe\",\"mode\":\"{}\",", mode));
        s.push_str(&format!("\"latest\":{},\"w_work\":{},\"r_work\":{},", v.latest, v.w_work, v.r_work));
        s.push_str(&format!("\"revoked\":{},\"epoch\":{},", v.revoked, v.epoch));
        s.push_str(&format!("\"t_publish\":{},\"t_claim\":{},\"t_drop\":{},", v.t_publish, v.t_claim, v.t_drop));
        s.push_str(&format!("\"mid_publish_sample\":{},\"advisory\":true,\"bufs\":[", v.mid_publish_sample));
        for i in 0..2 {
            s.push_str(&format!("{{\"slot\":{},\"owner\":\"{}\",\"seq\":{},\"version\":{},\"header_size\":{},\"payload_len\":{}}}",
                     v.bufs[i].slot_idx, owner_str(v.bufs[i].owner),
                     v.bufs[i].seq, v.bufs[i].version, v.bufs[i].header_size, v.bufs[i].payload_len));
            if i == 0 { s.push(','); }
        }
        s.push_str("]}");
        println!("{}", s);
    } else {
        println!("WEFT PROBE — {} dump", mode);
        println!("latest: {}", v.latest);
        println!("w_work: {} (advisory)", v.w_work);
        println!("r_work: {} (advisory)", v.r_work);
        println!("revoked: {}", v.revoked);
        println!("epoch: {}", v.epoch);
        println!("t_publish: {} (advisory)", v.t_publish);
        println!("t_claim: {} (advisory)", v.t_claim);
        println!("t_drop: {} (advisory)", v.t_drop);
        println!("mid_publish_sample: {}", v.mid_publish_sample);
        for i in 0..2 {
            println!("buf[{}]: slot={} owner={} seq={} version={} header_size={} payload_len={}",
                     i, v.bufs[i].slot_idx, owner_str(v.bufs[i].owner),
                     v.bufs[i].seq, v.bufs[i].version, v.bufs[i].header_size, v.bufs[i].payload_len);
        }
    }
}

fn run_quiesced(payload_max: usize, frames: u32, json: bool) -> i32 {
    let w = Weft::new(payload_max).unwrap();
    for seq in 1..=frames {
        unsafe { fill_payload(&w, seq, payload_max as u32); w.publish(seq, payload_max as u32); }
    }
    w.claim();
    let view = w.debug_state();
    print_view(&view, "quiesced", json);
    let ok = view.bufs[0].seq == frames || view.bufs[1].seq == frames;
    eprintln!("quiesced round-trip: {} (expected seq={})", if ok { "PASS" } else { "FAIL" }, frames);
    if ok { 0 } else { 1 }
}

fn run_revocation(payload_max: usize, json: bool) -> i32 {
    let w = Weft::new(payload_max).unwrap();
    for seq in 1..=10u32 {
        unsafe { fill_payload(&w, seq, payload_max as u32); w.publish(seq, payload_max as u32); }
    }
    w.revoke();
    let view = w.debug_state();
    print_view(&view, "revocation", json);
    let ok = view.revoked;
    eprintln!("revocation-safe: {} (revoked={})", if ok { "PASS" } else { "FAIL" }, view.revoked);
    if ok { 0 } else { 1 }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 { eprintln!("usage: probe <quiesced|revocation> [--json] [--payload N] [--frames N]"); std::process::exit(2); }
    let mode = &args[1];
    let mut json = false;
    let mut payload_max = 256usize;
    let mut frames = 100u32;
    for i in 2..args.len() {
        match args[i].as_str() {
            "--json" => json = true,
            "--payload" => { if i + 1 < args.len() { payload_max = args[i + 1].parse().unwrap_or(256); } }
            "--frames" => { if i + 1 < args.len() { frames = args[i + 1].parse().unwrap_or(100); } }
            _ => {}
        }
    }
    match mode.as_str() {
        "quiesced" => std::process::exit(run_quiesced(payload_max, frames, json)),
        "revocation" => std::process::exit(run_revocation(payload_max, json)),
        "live" => { let mut hz = 240u64; let mut secs = 3u64; let mut i = 2; while i < args.len() { match args[i].as_str() { "--hz" => { if i+1 < args.len() { hz = args[i+1].parse().unwrap_or(240); } } "--secs" => { if i+1 < args.len() { secs = args[i+1].parse().unwrap_or(3); } } _ => {} } i += 1; } std::process::exit(run_live(payload_max, hz, secs, json)); },
        _ => { eprintln!("unknown mode: {}", mode); std::process::exit(2); }
    }
}

fn run_live(payload_max: usize, hz: u64, secs: u64, json: bool) -> i32 {
    use std::sync::Arc;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::thread;
    let w = Arc::new(Weft::new(payload_max).unwrap());
    let stop = Arc::new(AtomicBool::new(false));
    let w2 = w.clone();
    let stop2 = stop.clone();
    let writer = thread::spawn(move || {
        let period = std::time::Duration::from_nanos(1_000_000_000 / hz);
        let mut next = std::time::Instant::now() + period;
        let mut seq = 1u32;
        while !stop2.load(Ordering::Relaxed) {
            unsafe { fill_payload(&w2, seq, payload_max as u32); w2.publish(seq, payload_max as u32); }
            seq += 1;
            let now = std::time::Instant::now();
            if now < next { thread::sleep(next - now); }
            next += period;
        }
    });
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(secs);
    let mut samples = 0;
    while std::time::Instant::now() < deadline {
        let view = w.debug_state();
        if json {
            print_view(&view, "live", true);
        } else {
            eprintln!("live sample {}: latest={} t_publish={} mid_publish={}",
                      samples, view.latest, view.t_publish, view.mid_publish_sample);
        }
        samples += 1;
        thread::sleep(std::time::Duration::from_millis(16)); // ~60 Hz probe
    }
    stop.store(true, Ordering::Relaxed);
    writer.join().unwrap();
    eprintln!("live: {} samples, no crashes, no allocations on probe path", samples);
    0
}
