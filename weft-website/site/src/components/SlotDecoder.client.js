/**
 * SlotDecoder — Studio's Ring Monitor, distilled. Click any byte of a live
 * 32-byte seqlock slot to inspect the field; watch parity flip even→odd→even
 * as a simulated writer commits frames. Payload: market ticks (msg_type 1).
 */

const FIELDS = [
  { name: 'seq', off: 0, size: 4, type: 'u32', desc: 'seqlock version · even = stable, odd = write in progress' },
  { name: 'writer_id', off: 4, size: 2, type: 'u16', desc: 'producing lane id' },
  { name: 'msg_type', off: 6, size: 1, type: 'u8', desc: '1=MARKET_TICK · 2=IMU_SAMPLE · 3=FRAME_EVENT' },
  { name: 'flags', off: 7, size: 1, type: 'u8', desc: 'bit0=last_in_burst · bit1=out_of_order · bit2=poisoned' },
  { name: 'ts_ns', off: 8, size: 8, type: 'u64', desc: 'monotonic publish timestamp (ns)' },
  { name: 'price_x1e6', off: 16, size: 8, type: 'f64', desc: 'payload: price × 10⁶ (IEEE-754 LE)' },
  { name: 'size', off: 24, size: 4, type: 'u32', desc: 'payload: quantity' },
  { name: 'side_bid', off: 28, size: 1, type: 'u8', desc: 'payload: 1 = bid, 0 = ask' },
  { name: 'pad', off: 29, size: 3, type: '—', desc: 'padding to 32 B (cache-line friendly ×2)' },
];

document.querySelectorAll('[data-slotdec]').forEach((root) => {
  const bytesEl = root.querySelector('.sd-bytes');
  const detailEl = root.querySelector('.sd-detail');
  const stateEl = root.querySelector('.sd-state');
  const priceEl = root.querySelector('.sd-price');
  const seqEl = root.querySelector('.sd-seq');
  const buf = new ArrayBuffer(32);
  const dv = new DataView(buf);
  const u8 = new Uint8Array(buf);
  let seq = 1000n, hover = -1, writing = false;

  function build() {
    bytesEl.innerHTML = '';
    for (let i = 0; i < 32; i++) {
      const cell = document.createElement('button');
      cell.className = 'sd-cell';
      cell.dataset.i = i;
      const hex = u8[i].toString(16).padStart(2, '0').toUpperCase();
      cell.textContent = hex;
      const f = FIELDS.find(f => i >= f.off && i < f.off + f.size);
      if (f) cell.style.setProperty('--fc', fieldColor(f.name));
      cell.addEventListener('mouseenter', () => { hover = i; showDetail(i); });
      cell.addEventListener('mouseleave', () => { hover = -1; });
      cell.addEventListener('focus', () => showDetail(i));
      cell.addEventListener('click', () => showDetail(i, true));
      bytesEl.appendChild(cell);
    }
  }
  function fieldColor(name) {
    return ({ seq: '#a78bfa', writer_id: '#8b93a7', msg_type: '#53d7fb', flags: '#53d7fb', ts_ns: '#f5a524', price_x1e6: '#ffd58a', size: '#ffd58a', side_bid: '#4ade80', pad: '#2a2f40' })[name] || '#8b93a7';
  }
  function refresh() {
    [...bytesEl.children].forEach((cell, i) => {
      cell.textContent = u8[i].toString(16).padStart(2, '0').toUpperCase();
      if (hover === i) cell.classList.add('hot'); else cell.classList.remove('hot');
    });
    const s = dv.getUint32(0, true);
    seqEl.textContent = String(s);
    seqEl.className = 'num ' + (s % 2 ? 'odd' : 'even');
    stateEl.textContent = s % 2 ? 'WRITING (odd — readers must retry)' : 'STABLE (even — safe to read)';
    stateEl.className = 'badge ' + (s % 2 ? 'warn' : 'ok');
    priceEl.textContent = (dv.getFloat64(16, true)).toFixed(2);
  }
  function showDetail(i, pin) {
    const f = FIELDS.find(f => i >= f.off && i < f.off + f.size);
    if (!f) return;
    let val = '';
    try {
      if (f.type === 'u32') val = dv.getUint32(f.off, true).toLocaleString();
      if (f.type === 'u16') val = String(dv.getUint16(f.off, true));
      if (f.type === 'u8') val = String(dv.getUint8(f.off));
      if (f.type === 'u64') val = dv.getBigUint64(f.off, true).toString() + ' ns';
      if (f.type === 'f64') val = dv.getFloat64(f.off, true).toFixed(6);
    } catch { val = '—'; }
    detailEl.innerHTML =
      `<span class="mono-label" style="color:${fieldColor(f.name)}">offset ${f.off} · ${f.size}B · ${f.type}</span>` +
      `<h4>${f.name}</h4><p>${f.desc}</p><p class="sd-val num">${val}</p>`;
    [...bytesEl.children].forEach((c, k) => c.classList.toggle('sel', k >= f.off && k < f.off + f.size));
  }

  function commitFrame() {
    // odd → write fields → even (textbook seqlock)
    dv.setUint32(0, Number(seq), true); seq += 1n; writing = true; refresh();
    setTimeout(() => {
      dv.setUint16(4, 7, true);            // writer_id
      dv.setUint8(6, 1);                   // MARKET_TICK
      dv.setUint8(7, Math.random() < 0.2 ? 1 : 0);
      dv.setBigUint64(8, BigInt(Date.now()) * 1000000n + (seq % 1000000n), true);
      dv.setFloat64(16, 64000 + Math.sin(Number(seq) / 9) * 450 + Math.random() * 40, true);
      dv.setUint32(24, 1 + Math.floor(Math.random() * 900), true);
      dv.setUint8(28, Number(seq) % 2);
      dv.setUint32(0, Number(seq), true); seq += 1n;   // even again
      writing = false; refresh();
    }, 380);
  }

  build(); commitFrame(); refresh();
  setInterval(() => { if (!writing) commitFrame(); }, 1400);
});
