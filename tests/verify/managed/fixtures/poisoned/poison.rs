// poison.rs — poisoned Rust fixture. Every commented rule id MUST fire.

#[weft_hot]
fn on_quote(q: &Quote, out: &mut Out) {
    let boxed = Box::new(q.px);              // expect: WV-RS-001
    let v: Vec<u8> = vec![0u8; 64];          // expect: WV-RS-003
    let s = format!("{:?}", q.px);           // expect: WV-RS-009
    let owned = q.symbol.to_string();        // expect: WV-RS-006
    let cloned = out.scratch.clone();        // expect: WV-RS-008
    let acc: Vec<f64> = it.collect();        // expect: WV-RS-010
    let _ = (boxed, v, s, owned, cloned, acc);
}
