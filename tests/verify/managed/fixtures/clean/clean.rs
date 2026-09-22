// clean.rs — clean Rust fixture: ZERO findings expected.
// with_capacity-style preallocation at init is the compliant pattern.

#[weft_hot]
fn on_quote(q: &Quote, out: &mut Out) {
    let mut acc: f64 = 0.0;
    for i in 0..64 {
        acc += q.window[i];
    }
    out.acc = acc;
    let scalar = q.px as f64;
    out.last = scalar * 0.5;
}

fn cold() -> String {
    format!("{:?}", 42) // NOT hot: legal
}
