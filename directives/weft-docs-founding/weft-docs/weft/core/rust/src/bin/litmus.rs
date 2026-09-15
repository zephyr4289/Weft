// litmus.rs — L1–L8 litmus runner (Rust)
//
// Per 04-LITMUS.md (procedures + verdicts) and 05-CONTRACTS.md (CLI + JSON output).
// Direct port of core/c/litmus_runner.c. Same protocol, same adversarial parameters.
//
// CLI: ./litmus <TEST_ID> [key=value ...]
// Output: exactly ONE JSON line on stdout (the LAST line); diagnostics to stderr.
// Exit: 0 pass · 1 fail · 2 usage/contract error.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Duration;
use weft_core::{envelope_decode, envelope_encode, envelope_encode_v1, negotiate, pat, xorshift32, PubResult, Weft};

// ---------------------------------------------------------------------------
// CLI parsing (mirror of C)
// ---------------------------------------------------------------------------

#[derive(Default)]
struct Cli {
    test_id: String,
    holds_ms: Vec<i32>,
    writer_hz: i32,
    reader_hz: i32,
    frames: i32,
    publishes: i32,
    claims: i32,
    payload_max: i32,
    bound: i32,
    window_s: f64,
    timeout_ms: i32,
    tolerance: f64,
    trials: i32,
    max_delay_us: i32,
    seed: u32,
    min_claims: i32,  // v1.1 (A4): per-language exposure floor
}

fn parse_args(args: &[String]) -> Result<Cli, String> {
    if args.len() < 2 {
        return Err(format!("usage: {} <TEST_ID> [key=value ...]", args[0]));
    }
    let mut c = Cli::default();
    c.test_id = args[1].clone();
    for arg in &args[2..] {
        let eq = arg.find('=');
        let eq = match eq { Some(i) => i, None => return Err(format!("bad arg: {}", arg)) };
        let key = &arg[..eq];
        let val = &arg[eq + 1..];
        match key {
            "holds_ms" => {
                c.holds_ms = val.split(',').filter_map(|s| s.parse::<i32>().ok()).collect();
            }
            "writer_hz" => c.writer_hz = val.parse().map_err(|e: std::num::ParseIntError| e.to_string())?,
            "reader_hz" => c.reader_hz = val.parse().map_err(|e: std::num::ParseIntError| e.to_string())?,
            "frames" => c.frames = val.parse().map_err(|e: std::num::ParseIntError| e.to_string())?,
            "publishes" => c.publishes = val.parse().map_err(|e: std::num::ParseIntError| e.to_string())?,
            "claims" => c.claims = val.parse().map_err(|e: std::num::ParseIntError| e.to_string())?,
            "payload_max" => c.payload_max = val.parse().map_err(|e: std::num::ParseIntError| e.to_string())?,
            "bound" => c.bound = val.parse().map_err(|e: std::num::ParseIntError| e.to_string())?,
            "window_s" => c.window_s = val.parse().map_err(|e: std::num::ParseFloatError| e.to_string())?,
            "timeout_ms" => c.timeout_ms = val.parse().map_err(|e: std::num::ParseIntError| e.to_string())?,
            "tolerance" => c.tolerance = val.parse().map_err(|e: std::num::ParseFloatError| e.to_string())?,
            "trials" => c.trials = val.parse().map_err(|e: std::num::ParseIntError| e.to_string())?,
            "max_delay_us" => c.max_delay_us = val.parse().map_err(|e: std::num::ParseIntError| e.to_string())?,
            "seed" => {
                let s = val.trim_start_matches("0x");
                c.seed = u32::from_str_radix(s, 16).map_err(|e| e.to_string())?;
            }
            "min_claims" => c.min_claims = val.parse().map_err(|e: std::num::ParseIntError| e.to_string())?,
            _ => return Err(format!("unknown param: {}", key)),
        }
    }
    Ok(c)
}

// ---------------------------------------------------------------------------
// Timing helpers (04-LITMUS §0.3)
// ---------------------------------------------------------------------------

fn now_ns() -> u64 {
    let t = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap();
    t.as_nanos() as u64
}

fn deadline_sleep(next: &mut u64, period_ns: u64) {
    let now = now_ns();
    if now < *next {
        let rem = *next - now;
        std::thread::sleep(Duration::from_nanos(rem));
        while now_ns() < *next { /* spin */ }
    }
    *next += period_ns;
    let now2 = now_ns();
    if now2 > *next + period_ns {
        *next = now2 + period_ns;
    }
}

fn hold_inject_ms(ms: i32) {
    if ms <= 0 { return; }
    let deadline = now_ns() + (ms as u64) * 1_000_000;
    while now_ns() < deadline {
        let rem = deadline - now_ns();
        if rem > 2_000_000 {
            std::thread::sleep(Duration::from_nanos(rem));
        }
        // busy-wait last 2ms
    }
}

// ---------------------------------------------------------------------------
// Frame verification (04-LITMUS §0.4) — read LIVE buffer
// ---------------------------------------------------------------------------

unsafe fn verify_held(w: &Weft, expected_seq: u32, payload_len: u32) -> bool {
    if w.r_magic() != weft_core::WEFT_MAGIC { return false; }
    if w.r_seq() != expected_seq { return false; }
    let payload = w.r_live_ptr(16);
    if payload.is_null() { return false; }
    for i in 0..payload_len {
        if *payload.add(i as usize) != pat(expected_seq, i) { return false; }
    }
    let canary_ptr = w.r_live_ptr(w.buf_size - 8);
    if canary_ptr.is_null() { return false; }
    let cv = std::ptr::read_unaligned(canary_ptr as *const u64).to_le();
    if cv != expected_seq as u64 { return false; }
    true
}

unsafe fn fill_payload(w: &Weft, seq: u32, payload_len: u32) {
    let p = w.r_live_ptr(16);  // Note: r_live_ptr returns the reader's held buffer.
    // For the WRITER, we want w_work's buffer. We need a different accessor.
    // Workaround: use w_write_payload with a stack buffer.
    // Actually, the kernel's w_write_payload takes a src pointer. We need a way to
    // fill the writer's buffer. The cleanest is to construct a stack buffer and call w_write_payload.
    // But that allocates per-call (violates Law 2).
    //
    // For the litmus runner (NOT the hot path), we can allocate a stack buffer of payload_max.
    // The runner is the test, not the kernel — Law 2 applies to the kernel, not the runner.
    let mut buf = vec![0u8; payload_len as usize];
    for i in 0..payload_len {
        buf[i as usize] = pat(seq, i);
    }
    w.w_write_payload(buf.as_ptr(), payload_len as usize).ok();
    // Silence unused variable warning.
    let _ = p;
}

// ---------------------------------------------------------------------------
// L1 — tear
// ---------------------------------------------------------------------------

fn run_l1(c: &Cli) -> i32 {
    let payload_max = if c.payload_max > 0 { c.payload_max } else { 1024 };
    let writer_hz = if c.writer_hz > 0 { c.writer_hz } else { 240 };
    let frames = if c.frames > 0 { c.frames } else { 600 };
    let holds: Vec<i32> = if !c.holds_ms.is_empty() { c.holds_ms.clone() } else { vec![5, 10, 50] };

    let mut total_torn = 0;
    let mut total_claims = 0u64;
    let mut all_drain_ok = true;
    let min_claims = if c.min_claims > 0 { c.min_claims as u64 } else { 600 };  // v1.1 (A4): per-lang floor
    let mut claims_per_s = 0.0;  // v1.1 (A4): falsifiable recalibration telemetry
    let mut holds_count = 0u32;

    for &hold in &holds {
        holds_count += 1;
        let w = match Weft::new(payload_max as usize) {
            Some(w) => w,
            None => { eprintln!("L1: weft_init failed for hold={}", hold); return 1; }
        };
        let w = Arc::new(w);
        let stop = Arc::new(AtomicBool::new(false));
        let published = Arc::new(AtomicU64::new(0));

        let w2 = w.clone();
        let stop2 = stop.clone();
        let pub2 = published.clone();
        let writer = std::thread::spawn(move || {
            let period_ns = 1_000_000_000 / writer_hz as u64;
            let mut next = now_ns() + period_ns;
            let mut seq: u32 = 1;
            while !stop2.load(Ordering::Relaxed) && seq <= frames as u32 {
                unsafe { fill_payload(&w2, seq, payload_max as u32); }
                let r = unsafe { w2.publish(seq, payload_max as u32) };
                if r == PubResult::Ok { pub2.fetch_add(1, Ordering::Relaxed); }
                seq += 1;
                deadline_sleep(&mut next, period_ns);
            }
        });

        let mut last_seq: u32 = 0;
        let mut max_observed_s: u32 = 0;  // v1.1 (A2): track max seq seen for drain_ok
        let mut claims: u64 = 0;
        let mut torn: u32 = 0;
        let mut drain_ok = false;
        let run_start = now_ns();
        let timeout = 30 * 1_000_000_000u64;

        loop {
            if now_ns() - run_start > timeout { break; }
            let writer_done = published.load(Ordering::Relaxed) >= frames as u64;

            let _idx = w.claim();
            let s = w.r_seq();
            claims += 1;
            if s > max_observed_s { max_observed_s = s; }

            if s != last_seq {
                hold_inject_ms(hold);
                if !unsafe { verify_held(&w, s, payload_max as u32) } {
                    torn += 1;
                }
                last_seq = s;
            } else {
                std::thread::sleep(Duration::from_millis(1));
            }

            if writer_done && max_observed_s >= frames as u32 {
                drain_ok = true;
                break;
            }
        }

        stop.store(true, Ordering::Relaxed);
        writer.join().unwrap();

        // v1.1 (A2): bounded post-join drain — up to 4 attempts (1ms apart)
        if !drain_ok {
            for _ in 0..4 {
                let _idx = w.claim();
                let s = w.r_seq();
                if s > max_observed_s { max_observed_s = s; }
                if max_observed_s >= frames as u32 { break; }
                std::thread::sleep(Duration::from_millis(1));
            }
        }
        drain_ok = max_observed_s >= frames as u32;

        total_torn += torn;
        total_claims += claims;
        if !drain_ok { all_drain_ok = false; }

        let elapsed_s = (now_ns() - run_start) as f64 / 1e9;
        let hold_cps = if elapsed_s > 0.0 { claims as f64 / elapsed_s } else { 0.0 };
        claims_per_s += hold_cps;
        eprintln!("L1 hold={}ms claims={} torn={} drain_ok={} claims_per_s={:.1}", hold, claims, torn, drain_ok, hold_cps);
    }

    claims_per_s /= holds_count as f64;
    let pass = total_torn == 0 && all_drain_ok && total_claims >= min_claims;
    print!("{{\"test\":\"L1-tear\",\"lang\":\"rust\",\"pass\":{},\"metrics\":{{\"holds_ms\":[",
           if pass { "true" } else { "false" });
    for (i, h) in holds.iter().enumerate() {
        print!("{}", h);
        if i + 1 < holds.len() { print!(","); }
    }
    print!("],\"claims\":{},\"claims_per_s\":{:.1},\"torn\":{},\"drain_ok\":{}}}}}\n",
           total_claims, claims_per_s, total_torn, all_drain_ok);
    if pass { 0 } else { 1 }
}

// ---------------------------------------------------------------------------
// L2 — writer step bound
// ---------------------------------------------------------------------------

fn run_l2(c: &Cli) -> i32 {
    let payload_max = if c.payload_max > 0 { c.payload_max } else { 256 };
    let publishes = if c.publishes > 0 { c.publishes } else { 2000 };
    let writer_hz = if c.writer_hz > 0 { c.writer_hz } else { 2000 };
    let bound = if c.bound > 0 { c.bound } else { 2 };
    let holds: Vec<i32> = if !c.holds_ms.is_empty() { c.holds_ms.clone() } else { vec![0, 10, 25, 50, 100] };

    let mut total_publishes = 0u64;
    let mut max_wsteps: u64 = 0;
    let mut all_ok = true;

    for &hold in &holds {
        let w = match Weft::new(payload_max as usize) {
            Some(w) => w,
            None => return 1,
        };
        let w = Arc::new(w);
        let stop = Arc::new(AtomicBool::new(false));
        let published = Arc::new(AtomicU64::new(0));
        let max_delta = Arc::new(AtomicU64::new(0));

        let w2 = w.clone();
        let stop2 = stop.clone();
        let pub2 = published.clone();
        let max_d2 = max_delta.clone();
        let writer = std::thread::spawn(move || {
            let period_ns = 1_000_000_000 / writer_hz as u64;
            let mut next = now_ns() + period_ns;
            let mut seq: u32 = 1;
            while !stop2.load(Ordering::Relaxed) && seq <= publishes as u32 {
                unsafe { fill_payload(&w2, seq, payload_max as u32); }
                let before = w2.t_wsteps();
                let r = unsafe { w2.publish(seq, payload_max as u32) };
                let after = w2.t_wsteps();
                if r == PubResult::Ok {
                    pub2.fetch_add(1, Ordering::Relaxed);
                    let d = after - before;
                    let mut cur = max_d2.load(Ordering::Relaxed);
                    while d > cur {
                        if max_d2.compare_exchange_weak(cur, d, Ordering::Relaxed, Ordering::Relaxed).is_ok() { break; }
                        cur = max_d2.load(Ordering::Relaxed);
                    }
                }
                seq += 1;
                deadline_sleep(&mut next, period_ns);
            }
        });

        let w3 = w.clone();
        let stop3 = stop.clone();
        let reader = std::thread::spawn(move || {
            while !stop3.load(Ordering::Relaxed) {
                w3.claim();
                hold_inject_ms(hold);
            }
        });

        writer.join().unwrap();
        stop.store(true, Ordering::Relaxed);
        reader.join().unwrap();

        let published_n = published.load(Ordering::Relaxed);
        let max_d = max_delta.load(Ordering::Relaxed);
        total_publishes += published_n;
        if max_d > max_wsteps { max_wsteps = max_d; }
        let ok = max_d <= bound as u64 && published_n == publishes as u64;
        if !ok { all_ok = false; }
        eprintln!("L2 hold={}ms published={} max_wsteps={} ok={}", hold, published_n, max_d, ok);
    }

    let pass = all_ok && max_wsteps <= bound as u64 && total_publishes == (publishes * holds.len() as i32) as u64;
    print!("{{\"test\":\"L2-writer-steps\",\"lang\":\"rust\",\"pass\":{},\"metrics\":{{\"holds_ms\":[",
           if pass { "true" } else { "false" });
    for (i, h) in holds.iter().enumerate() {
        print!("{}", h);
        if i + 1 < holds.len() { print!(","); }
    }
    print!("],\"publishes\":{},\"max_wsteps\":{},\"bound\":{}}}}}\n",
           total_publishes, max_wsteps, bound);
    if pass { 0 } else { 1 }
}

// ---------------------------------------------------------------------------
// L3 — reader step bound
// ---------------------------------------------------------------------------

fn run_l3(c: &Cli) -> i32 {
    let payload_max = if c.payload_max > 0 { c.payload_max } else { 256 };
    let writer_hz = if c.writer_hz > 0 { c.writer_hz } else { 960 };
    let claims = if c.claims > 0 { c.claims } else { 2000 };
    let bound = if c.bound > 0 { c.bound } else { 2 };
    let frames = claims * 2;

    let w = match Weft::new(payload_max as usize) { Some(w) => w, None => return 1 };
    let w = Arc::new(w);
    let stop = Arc::new(AtomicBool::new(false));
    let published = Arc::new(AtomicU64::new(0));
    let claimed = Arc::new(AtomicU64::new(0));
    let max_delta = Arc::new(AtomicU64::new(0));

    let w2 = w.clone();
    let stop2 = stop.clone();
    let pub2 = published.clone();
    let writer = std::thread::spawn(move || {
        let period_ns = 1_000_000_000 / writer_hz as u64;
        let mut next = now_ns() + period_ns;
        let mut seq: u32 = 1;
        while !stop2.load(Ordering::Relaxed) && seq <= frames as u32 {
            unsafe { fill_payload(&w2, seq, payload_max as u32); }
            let r = unsafe { w2.publish(seq, payload_max as u32) };
            if r == PubResult::Ok { pub2.fetch_add(1, Ordering::Relaxed); }
            seq += 1;
            deadline_sleep(&mut next, period_ns);
        }
    });

    let w3 = w.clone();
    let stop3 = stop.clone();
    let cl2 = claimed.clone();
    let max_d2 = max_delta.clone();
    let reader = std::thread::spawn(move || {
        while !stop3.load(Ordering::Relaxed) && cl2.load(Ordering::Relaxed) < claims as u64 {
            let before = w3.t_rsteps();
            w3.claim();
            let after = w3.t_rsteps();
            let d = after - before;
            let mut cur = max_d2.load(Ordering::Relaxed);
            while d > cur {
                if max_d2.compare_exchange_weak(cur, d, Ordering::Relaxed, Ordering::Relaxed).is_ok() { break; }
                cur = max_d2.load(Ordering::Relaxed);
            }
            cl2.fetch_add(1, Ordering::Relaxed);
        }
    });

    reader.join().unwrap();
    stop.store(true, Ordering::Relaxed);
    writer.join().unwrap();

    let claimed_n = claimed.load(Ordering::Relaxed);
    let max_rsteps = max_delta.load(Ordering::Relaxed);
    let pass = max_rsteps <= bound as u64 && claimed_n == claims as u64;
    eprintln!("L3 claims={} max_rsteps={} bound={} pass={}", claimed_n, max_rsteps, bound, pass);

    print!("{{\"test\":\"L3-reader-steps\",\"lang\":\"rust\",\"pass\":{},\"metrics\":{{\"claims\":{},\"max_rsteps\":{},\"bound\":{}}}}}\n",
           if pass { "true" } else { "false" }, claimed_n, max_rsteps, bound);
    if pass { 0 } else { 1 }
}

// ---------------------------------------------------------------------------
// L4 — freshness
// ---------------------------------------------------------------------------

fn run_l4(c: &Cli) -> i32 {
    let payload_max = if c.payload_max > 0 { c.payload_max } else { 256 };
    let writer_hz = if c.writer_hz > 0 { c.writer_hz } else { 960 };
    let reader_hz = if c.reader_hz > 0 { c.reader_hz } else { 240 };
    let frames = if c.frames > 0 { c.frames } else { 2000 };

    let w = match Weft::new(payload_max as usize) { Some(w) => w, None => return 1 };
    let w = Arc::new(w);
    let stop = Arc::new(AtomicBool::new(false));
    let published = Arc::new(AtomicU64::new(0));

    let w2 = w.clone();
    let stop2 = stop.clone();
    let pub2 = published.clone();
    let writer = std::thread::spawn(move || {
        let period_ns = 1_000_000_000 / writer_hz as u64;
        let mut next = now_ns() + period_ns;
        let mut seq: u32 = 1;
        while !stop2.load(Ordering::Relaxed) && seq <= frames as u32 {
            unsafe { fill_payload(&w2, seq, payload_max as u32); }
            let r = unsafe { w2.publish(seq, payload_max as u32) };
            if r == PubResult::Ok { pub2.fetch_add(1, Ordering::Relaxed); }
            seq += 1;
            deadline_sleep(&mut next, period_ns);
        }
    });

    let reader_period = 1_000_000_000 / reader_hz as u64;
    let mut next = now_ns() + reader_period;
    let mut freshness_violations = 0;
    let mut future_violations = 0;
    let mut stale_returns = 0;  // v1.1: stale returns (not a violation, per 04-LITMUS §0.6)
    let mut last: u32 = 0;     // v1.1: highest seq observed
    let run_start = now_ns();
    let timeout = 60 * 1_000_000_000u64;

    // Phase 1: concurrent. v1.1 (WO-P0A §1.2): track `last`; new-frame vs stale-return.
    loop {
        if now_ns() - run_start > timeout { break; }
        let tpub = w.t_publish();
        let writer_done = tpub >= frames as u64;
        if writer_done { break; }

        let p0 = tpub;
        let _idx = w.claim();
        let s = w.r_seq();
        let p1 = w.t_publish();

        if s > last {
            // NEW FRAME — freshness violation check applies
            if (s as u64) < p0 { freshness_violations += 1; }
            last = s;
        } else {
            // STALE RETURN (§0.6) — exchange lawfully handed back reader's own buffer.
            stale_returns += 1;
        }

        if s as u64 > p1 {
            let spin_deadline = now_ns() + 2_000_000;
            while now_ns() < spin_deadline {
                let p1_new = w.t_publish();
                if s as u64 <= p1_new { break; }
            }
            if s as u64 > w.t_publish() { future_violations += 1; }
        }

        deadline_sleep(&mut next, reader_period);
    }

    stop.store(true, Ordering::Relaxed);
    writer.join().unwrap();

    // v1.1 drain: after writer join, claim up to 4 attempts (1ms apart);
    // drain_exact = (last == frames).
    for _ in 0..4 {
        let _idx = w.claim();
        let s = w.r_seq();
        if s > last { last = s; }
        if last == frames as u32 { break; }
        std::thread::sleep(Duration::from_millis(1));
    }
    let drain_exact = last == frames as u32;

    let pass = freshness_violations == 0 && future_violations == 0 && drain_exact;
    eprintln!("L4 freshness_violations={} future_violations={} stale_returns={} drain_exact={} (last={}) pass={}",
              freshness_violations, future_violations, stale_returns, drain_exact, last, pass);

    print!("{{\"test\":\"L4-freshness\",\"lang\":\"rust\",\"pass\":{},\"metrics\":{{\"frames\":{},\"freshness_violations\":{},\"future_violations\":{},\"drain_exact\":{},\"stale_returns\":{}}}}}\n",
           if pass { "true" } else { "false" }, frames, freshness_violations, future_violations, drain_exact, stale_returns);
    if pass { 0 } else { 1 }
}

// ---------------------------------------------------------------------------
// L5 — progress
// ---------------------------------------------------------------------------

fn run_l5(c: &Cli) -> i32 {
    let payload_max = if c.payload_max > 0 { c.payload_max } else { 256 };
    let writer_hz = if c.writer_hz > 0 { c.writer_hz } else { 2000 };
    let window_s = if c.window_s > 0.0 { c.window_s } else { 1.0 };
    let tolerance = if c.tolerance > 0.0 { c.tolerance } else { 1.5 };
    let holds: Vec<i32> = if !c.holds_ms.is_empty() { c.holds_ms.clone() } else { vec![0, 10, 50, 100] };

    let mut rates: Vec<f64> = Vec::new();

    for &hold in &holds {
        let w = match Weft::new(payload_max as usize) { Some(w) => w, None => return 1 };
        let w = Arc::new(w);
        let stop = Arc::new(AtomicBool::new(false));
        let published_in_window = Arc::new(AtomicU64::new(0));
        let window_ns = (window_s * 1e9) as u64;

        let w2 = w.clone();
        let stop2 = stop.clone();
        let pub2 = published_in_window.clone();
        let writer = std::thread::spawn(move || {
            let period_ns = 1_000_000_000 / writer_hz as u64;
            let mut next = now_ns() + period_ns;
            let window_start = now_ns();
            let window_end = window_start + window_ns;
            let mut seq: u32 = 1;
            while !stop2.load(Ordering::Relaxed) && now_ns() < window_end {
                unsafe { fill_payload(&w2, seq, payload_max as u32); }
                let r = unsafe { w2.publish(seq, payload_max as u32) };
                if r == PubResult::Ok { pub2.fetch_add(1, Ordering::Relaxed); }
                seq += 1;
                deadline_sleep(&mut next, period_ns);
            }
        });

        let w3 = w.clone();
        let stop3 = stop.clone();
        let reader = std::thread::spawn(move || {
            while !stop3.load(Ordering::Relaxed) {
                w3.claim();
                hold_inject_ms(hold);
            }
        });

        writer.join().unwrap();
        stop.store(true, Ordering::Relaxed);
        reader.join().unwrap();

        let pub_n = published_in_window.load(Ordering::Relaxed);
        let rate = pub_n as f64 / window_s;
        rates.push(rate);
        eprintln!("L5 hold={}ms rate={:.1} Hz", hold, rate);
    }

    // Suspended reader config
    {
        let w = match Weft::new(payload_max as usize) { Some(w) => w, None => return 1 };
        let w = Arc::new(w);
        let stop = Arc::new(AtomicBool::new(false));
        let published_in_window = Arc::new(AtomicU64::new(0));
        let window_ns = (window_s * 1e9) as u64;

        let w2 = w.clone();
        let stop2 = stop.clone();
        let pub2 = published_in_window.clone();
        let writer = std::thread::spawn(move || {
            let period_ns = 1_000_000_000 / writer_hz as u64;
            let mut next = now_ns() + period_ns;
            let window_start = now_ns();
            let window_end = window_start + window_ns;
            let mut seq: u32 = 1;
            while !stop2.load(Ordering::Relaxed) && now_ns() < window_end {
                unsafe { fill_payload(&w2, seq, payload_max as u32); }
                let r = unsafe { w2.publish(seq, payload_max as u32) };
                if r == PubResult::Ok { pub2.fetch_add(1, Ordering::Relaxed); }
                seq += 1;
                deadline_sleep(&mut next, period_ns);
            }
        });
        writer.join().unwrap();
        let pub_n = published_in_window.load(Ordering::Relaxed);
        let rate = pub_n as f64 / window_s;
        rates.push(rate);
        eprintln!("L5 suspended-reader rate={:.1} Hz", rate);
    }

    let max_r = rates.iter().cloned().fold(0.0f64, f64::max);
    let min_r = rates.iter().cloned().fold(1e18f64, f64::min);
    let ratio = if min_r > 0.0 { max_r / min_r } else { 999.0 };
    let pass = ratio <= tolerance;
    eprintln!("L5 ratio={:.3} tolerance={:.2} pass={}", ratio, tolerance, pass);

    print!("{{\"test\":\"L5-progress\",\"lang\":\"rust\",\"pass\":{},\"metrics\":{{\"rates_hz\":[",
           if pass { "true" } else { "false" });
    for (i, r) in rates.iter().enumerate() {
        print!("{:.1}", r);
        if i + 1 < rates.len() { print!(","); }
    }
    print!("],\"ratio\":{:.3},\"tolerance\":{:.2},\"configs\":{}}}}}\n",
           ratio, tolerance, holds.len() + 1);
    if pass { 0 } else { 1 }
}

// ---------------------------------------------------------------------------
// L6 — ownership
// ---------------------------------------------------------------------------

fn run_l6(c: &Cli) -> i32 {
    let payload_max = if c.payload_max > 0 { c.payload_max } else { 256 };
    let trials = if c.trials > 0 { c.trials } else { 200 };
    let frames = if c.frames > 0 { c.frames } else { 64 };
    let max_delay_us = if c.max_delay_us > 0 { c.max_delay_us } else { 500 };
    let seed = if c.seed != 0 { c.seed } else { 0x00C0FFEE };

    let mut total_violations = 0;
    for _ in 0..trials {
        let w = match Weft::new(payload_max as usize) { Some(w) => w, None => return 1 };
        let w = Arc::new(w);
        let stop = Arc::new(AtomicBool::new(false));
        let violations = Arc::new(AtomicU64::new(0));

        let w2 = w.clone();
        let stop2 = stop.clone();
        let _viol2 = violations.clone();  // unused in writer; reader uses violations via Arc
        let writer = std::thread::spawn(move || {
            let mut s = seed;
            for seq in 1..=frames as u32 {
                let d = xorshift32(&mut s) % max_delay_us as u32;
                if d > 0 { std::thread::sleep(Duration::from_micros(d as u64)); }
                unsafe { fill_payload(&w2, seq, payload_max as u32); }
                unsafe { w2.publish(seq, payload_max as u32); }
            }
            stop2.store(true, Ordering::Relaxed);
        });

        let w3 = w.clone();
        let stop3 = stop.clone();
        let viol3 = violations.clone();
        let reader = std::thread::spawn(move || {
            let mut s = seed ^ 0x9E3779B9;
            let mut last_seq: u32 = 0;
            while !stop3.load(Ordering::Relaxed) {
                let d = xorshift32(&mut s) % max_delay_us as u32;
                if d > 0 { std::thread::sleep(Duration::from_micros(d as u64)); }
                let _idx = w3.claim();
                let s_now = w3.r_seq();
                if s_now != last_seq {
                    if !unsafe { verify_held(&w3, s_now, payload_max as u32) } {
                        viol3.fetch_add(1, Ordering::Relaxed);
                    }
                    last_seq = s_now;
                }
            }
        });

        writer.join().unwrap();
        let deadline = now_ns() + 1_000_000_000;
        while !stop.load(Ordering::Relaxed) && now_ns() < deadline {
            std::thread::sleep(Duration::from_millis(1));
        }
        // Force-stop reader if still running
        stop.store(true, Ordering::Relaxed);
        reader.join().unwrap();

        let v = violations.load(Ordering::Relaxed);
        total_violations += v;
        if v > 0 { eprintln!("L6 trial: violations={}", v); }
    }

    let pass = total_violations == 0;
    eprintln!("L6 trials={} violations={} seed=0x{:08X} pass={}", trials, total_violations, seed, pass);
    print!("{{\"test\":\"L6-ownership\",\"lang\":\"rust\",\"pass\":{},\"metrics\":{{\"trials\":{},\"frames_per_trial\":{},\"violations\":{},\"seed\":\"0x{:08X}\"}}}}\n",
           if pass { "true" } else { "false" }, trials, frames, total_violations, seed);
    if pass { 0 } else { 1 }
}

// ---------------------------------------------------------------------------
// L7 — revocation
// ---------------------------------------------------------------------------

fn run_l7(c: &Cli) -> i32 {
    let payload_max = if c.payload_max > 0 { c.payload_max } else { 256 };
    let timeout_ms = if c.timeout_ms > 0 { c.timeout_ms } else { 2000 };

    let w = match Weft::new(payload_max as usize) { Some(w) => w, None => return 1 };
    let w = Arc::new(w);
    let stop = Arc::new(AtomicBool::new(false));
    let publish_count = Arc::new(AtomicU64::new(0));
    let first_revoked_at = Arc::new(AtomicU64::new(0));
    let post_revoke_revoked = Arc::new(AtomicU64::new(0));

    let w2 = w.clone();
    let stop2 = stop.clone();
    let pc2 = publish_count.clone();
    let fr2 = first_revoked_at.clone();
    let pr2 = post_revoke_revoked.clone();
    let writer = std::thread::spawn(move || {
        let mut seq: u32 = 1;
        let mut seen_revoked = false;
        while !stop2.load(Ordering::Relaxed) {
            // Per 02 §6: after the ACK, writer must NEVER touch buffer bytes again.
            if !seen_revoked {
                unsafe { fill_payload(&w2, seq, payload_max as u32); }
            }
            let r = unsafe { w2.publish(seq, payload_max as u32) };
            match r {
                PubResult::Ok => { pc2.fetch_add(1, Ordering::Relaxed); }
                PubResult::DroppedRevoked => {
                    if !seen_revoked {
                        fr2.store(pc2.load(Ordering::Relaxed), Ordering::Relaxed);
                        seen_revoked = true;
                    }
                    let n = pr2.load(Ordering::Relaxed);
                    if n < 100 { pr2.fetch_add(1, Ordering::Relaxed); }
                    if n + 1 >= 100 { stop2.store(true, Ordering::Relaxed); break; }
                }
            }
            seq += 1;
        }
    });

    hold_inject_ms(5);
    let e0 = w.epoch();
    w.revoke();
    let reclaim_ok = w.reclaim(e0, timeout_ms as u32).is_ok();

    // Poison all 3 buffers with 0xDE
    for i in 0..3 {
        unsafe {
            let p = w.buffer_ptr(i);
            std::ptr::write_bytes(p, 0xDE, w.buf_size);
        }
    }

    let join_deadline = now_ns() + 5 * 1_000_000_000;
    while !stop.load(Ordering::Relaxed) && now_ns() < join_deadline {
        std::thread::sleep(Duration::from_millis(1));
    }
    stop.store(true, Ordering::Relaxed);
    writer.join().unwrap();

    // Scan all 3 buffers
    let mut poison_intact = true;
    for i in 0..3 {
        unsafe {
            let p = w.buffer_ptr(i);
            for j in 0..w.buf_size {
                if *p.add(j) != 0xDE {
                    poison_intact = false;
                    eprintln!("L7: buffer {} offset {} = 0x{:02X} (expected 0xDE)", i, j, *p.add(j));
                    break;
                }
            }
            if !poison_intact { break; }
        }
    }

    let post_revoke = post_revoke_revoked.load(Ordering::Relaxed);
    let first_revoked = first_revoked_at.load(Ordering::Relaxed);
    let pass = reclaim_ok && post_revoke == 100 && poison_intact;
    eprintln!("L7 reclaim_ok={} post_revoke_revoked={} poison_intact={} pass={}",
              reclaim_ok, post_revoke, poison_intact, pass);

    print!("{{\"test\":\"L7-revocation\",\"lang\":\"rust\",\"pass\":{},\"metrics\":{{\"reclaim_ok\":{},\"post_revoke_revoked\":{},\"poison_intact\":{},\"first_revoked_at\":{}}}}}\n",
           if pass { "true" } else { "false" },
           reclaim_ok, post_revoke, poison_intact, first_revoked);
    if pass { 0 } else { 1 }
}

// ---------------------------------------------------------------------------
// L8 — envelope (pure functions; no threads)
// ---------------------------------------------------------------------------

fn run_l8() -> i32 {
    // a. round-trip
    let a = {
        let mut enc = [0u8; 256];
        envelope_encode_v1(enc.as_mut_ptr(), 7, 100);
        match envelope_decode(enc.as_ptr(), 256) {
            Ok((v, hs, s, pl)) => v == 1 && hs == 16 && s == 7 && pl == 100 && {
                let mut enc2 = [0u8; 256];
                envelope_encode_v1(enc2.as_mut_ptr(), 7, 100);
                enc[..16] == enc2[..16]
            },
            Err(_) => false,
        }
    };

    // b. unknown trailing fields
    let b = {
        let mut enc = [0u8; 256];
        envelope_encode(enc.as_mut_ptr(), 1, 24, 7, 100);
        match envelope_decode(enc.as_ptr(), 256) {
            Ok((v, hs, s, pl)) => v == 1 && hs == 24 && s == 7 && pl == 100
                && enc[16] == 0xAA && enc[23] == 0xAA,
            Err(_) => false,
        }
    };

    // c. negotiation (per §3 formula, see REPORT.md note about §5 table inconsistency)
    let c = {
        let s1 = [1u16];
        if negotiate(1, &s1) != 1 { false }
        else {
            let s12 = [1u16, 2];
            if negotiate(2, &s12) != 2 { false }
            else {
                let s1_only = [1u16];
                if negotiate(2, &s1_only) != 1 { false }
                else {
                    let s12_only = [1u16, 2];
                    if negotiate(3, &s12_only) != 2 { false }
                    else {
                        let s23 = [2u16, 3];
                        negotiate(1, &s23) == 0
                    }
                }
            }
        }
    };

    // d. coexistence
    let d = {
        let mut enc_v1 = [0u8; 256];
        let mut enc_v2 = [0u8; 256];
        envelope_encode(enc_v1.as_mut_ptr(), 1, 16, 7, 100);
        envelope_encode(enc_v2.as_mut_ptr(), 2, 16, 8, 100);
        match (envelope_decode(enc_v1.as_ptr(), 256), envelope_decode(enc_v2.as_ptr(), 256)) {
            (Ok((v1, _, s1, _)), Ok((v2, _, s2, _))) => v1 == 1 && v2 == 2 && s1 == 7 && s2 == 8,
            _ => false,
        }
    };

    let pass = a && b && c && d;
    eprintln!("L8 roundtrip={} unknown={} negotiation={} coexist={} pass={}", a, b, c, d, pass);
    print!("{{\"test\":\"L8-envelope\",\"lang\":\"rust\",\"pass\":{},\"metrics\":{{\"roundtrip\":{},\"unknown_fields\":{},\"negotiation\":{},\"coexist\":{}}}}}\n",
           if pass { "true" } else { "false" }, a, b, c, d);
    if pass { 0 } else { 1 }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let c = match parse_args(&args) {
        Ok(c) => c,
        Err(e) => { eprintln!("{}", e); std::process::exit(2); }
    };

    let rc = match c.test_id.as_str() {
        "L1-tear" => run_l1(&c),
        "L2-writer-steps" => run_l2(&c),
        "L3-reader-steps" => run_l3(&c),
        "L4-freshness" => run_l4(&c),
        "L5-progress" => run_l5(&c),
        "L6-ownership" => run_l6(&c),
        "L7-revocation" => run_l7(&c),
        "L8-envelope" => run_l8(),
        _ => { eprintln!("unknown test id: {}", c.test_id); std::process::exit(2); }
    };
    std::process::exit(rc);
}
