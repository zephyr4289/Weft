"""
Weft Sandbox Roadmap — PDF generator
Output: /home/z/my-project/download/Weft-Sandbox-Roadmap.pdf
"""
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.lib.colors import HexColor, white
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.enums import TA_LEFT, TA_CENTER
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfbase.pdfmetrics import registerFontFamily
from reportlab.platypus import (
    BaseDocTemplate, PageTemplate, Frame, Paragraph, Spacer, PageBreak,
    Table, TableStyle, KeepTogether, HRFlowable, Preformatted,
)
from pathlib import Path

FONT_DIR = "/usr/share/fonts"
pdfmetrics.registerFont(TTFont('LibSans',   f'{FONT_DIR}/truetype/liberation/LiberationSans-Regular.ttf'))
pdfmetrics.registerFont(TTFont('LibSans-B', f'{FONT_DIR}/truetype/liberation/LiberationSans-Bold.ttf'))
pdfmetrics.registerFont(TTFont('LibSans-I', f'{FONT_DIR}/truetype/liberation/LiberationSans-Italic.ttf'))
pdfmetrics.registerFont(TTFont('LibSerif',  f'{FONT_DIR}/truetype/liberation/LiberationSerif-Regular.ttf'))
pdfmetrics.registerFont(TTFont('LibSerif-B',f'{FONT_DIR}/truetype/liberation/LiberationSerif-Bold.ttf'))
pdfmetrics.registerFont(TTFont('LibMono',   f'{FONT_DIR}/truetype/liberation/LiberationMono-Regular.ttf'))
registerFontFamily('LibSans', normal='LibSans', bold='LibSans-B', italic='LibSans-I')
registerFontFamily('LibSerif', normal='LibSerif', bold='LibSerif-B')

C_TEXT    = HexColor('#0F172A')
C_TEXT_2  = HexColor('#475569')
C_TEXT_3  = HexColor('#64748B')
C_ACCENT  = HexColor('#0369A1')
C_OK      = HexColor('#047857')
C_WARN    = HexColor('#C2410C')
C_BG_CODE = HexColor('#F1F5F9')
C_BORDER  = HexColor('#CBD5E1')
C_DIV     = HexColor('#E2E8F0')
C_OK_BG   = HexColor('#ECFDF5')
C_OK_BD   = C_OK

PAGE_W, PAGE_H = A4
ML = MR = 22*mm
MT = 22*mm
MB = 22*mm

style_h1 = ParagraphStyle('h1', fontName='LibSans-B', fontSize=18, leading=22,
    textColor=C_TEXT, spaceBefore=10, spaceAfter=8, keepWithNext=1)
style_h2 = ParagraphStyle('h2', fontName='LibSans-B', fontSize=13, leading=17,
    textColor=C_TEXT, spaceBefore=12, spaceAfter=4, keepWithNext=1)
style_h3 = ParagraphStyle('h3', fontName='LibSans-B', fontSize=10.5, leading=14,
    textColor=C_ACCENT, spaceBefore=8, spaceAfter=2, keepWithNext=1)
style_body = ParagraphStyle('body', fontName='LibSerif', fontSize=10.5, leading=14.5,
    textColor=C_TEXT, spaceAfter=6, alignment=TA_LEFT)
style_bullet = ParagraphStyle('bullet', fontName='LibSerif', fontSize=10.5, leading=14,
    textColor=C_TEXT, leftIndent=14, spaceAfter=3, alignment=TA_LEFT)
style_code = ParagraphStyle('code', fontName='LibMono', fontSize=8, leading=10,
    textColor=C_TEXT, alignment=TA_LEFT)
style_cap = ParagraphStyle('cap', fontName='LibSans-I', fontSize=9, leading=11,
    textColor=C_TEXT_2, spaceBefore=2, spaceAfter=10, alignment=TA_CENTER)
style_meta = ParagraphStyle('meta', fontName='LibSans', fontSize=9, leading=12,
    textColor=C_TEXT_2)

def _nb(t): return t.replace(' — ', '\u00a0— ')
def p(t): return Paragraph(_nb(t), style_body)
def bp(t): return Paragraph(f'• {_nb(t)}', style_bullet)
def cap(t): return Paragraph(t, style_cap)
def h1(t): return Paragraph(t, style_h1)
def h2(t): return Paragraph(t, style_h2)
def h3(t): return Paragraph(t, style_h3)
def hr(): return HRFlowable(width='100%', thickness=0.5, color=C_DIV, spaceBefore=8, spaceAfter=8)

def code_block(text):
    text = text.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
    pre = Preformatted(text, style_code)
    tbl = Table([[pre]], colWidths=[PAGE_W - ML - MR])
    tbl.setStyle(TableStyle([
        ('BACKGROUND', (0,0), (-1,-1), C_BG_CODE),
        ('BOX', (0,0), (-1,-1), 0.5, C_BORDER),
        ('LEFTPADDING', (0,0), (-1,-1), 8),
        ('RIGHTPADDING', (0,0), (-1,-1), 8),
        ('TOPPADDING', (0,0), (-1,-1), 6),
        ('BOTTOMPADDING', (0,0), (-1,-1), 6),
    ]))
    return KeepTogether([tbl, Spacer(1, 6)])

def verdict_box(passed, body):
    bg = C_OK_BG; bd = C_OK_BD; label = 'CONSTRAINT ACKNOWLEDGED'
    lp = Paragraph(label, ParagraphStyle('vb', fontName='LibSans-B', fontSize=9, textColor=bd))
    bp_ = Paragraph(_nb(body), ParagraphStyle('vb2', fontName='LibSans', fontSize=10, leading=14, textColor=C_TEXT))
    tbl = Table([[lp],[bp_]], colWidths=[PAGE_W - ML - MR - 16])
    tbl.setStyle(TableStyle([
        ('BACKGROUND', (0,0), (-1,-1), bg),
        ('LINEBEFORE', (0,0), (0,-1), 2.5, bd),
        ('LEFTPADDING', (0,0), (-1,-1), 10),
        ('RIGHTPADDING', (0,0), (-1,-1), 10),
        ('TOPPADDING', (0,0), (0,0), 8),
        ('TOPPADDING', (0,1), (0,1), 2),
        ('BOTTOMPADDING', (0,-1), (-1,-1), 8),
    ]))
    return KeepTogether([tbl, Spacer(1, 8)])

def table(headers, rows, col_widths):
    avail = PAGE_W - ML - MR
    total = sum(col_widths)
    widths = [w / total * avail for w in col_widths]
    th = ParagraphStyle('th', fontName='LibSans-B', fontSize=9, leading=12, textColor=white)
    td = ParagraphStyle('td', fontName='LibSerif', fontSize=9, leading=11.5, textColor=C_TEXT)
    data = [[Paragraph(h, th) for h in headers]]
    for row in rows:
        data.append([Paragraph(_nb(str(c)), td) for c in row])
    tbl = Table(data, colWidths=widths, hAlign='CENTER', repeatRows=1)
    tbl.setStyle(TableStyle([
        ('BACKGROUND', (0,0), (-1,0), C_ACCENT),
        ('TEXTCOLOR', (0,0), (-1,0), white),
        ('GRID', (0,0), (-1,-1), 0.25, C_BORDER),
        ('ROWBACKGROUNDS', (0,1), (-1,-1), [white, HexColor('#F8FAFC')]),
        ('VALIGN', (0,0), (-1,-1), 'TOP'),
        ('LEFTPADDING', (0,0), (-1,-1), 5),
        ('RIGHTPADDING', (0,0), (-1,-1), 5),
        ('TOPPADDING', (0,0), (-1,-1), 4),
        ('BOTTOMPADDING', (0,0), (-1,-1), 4),
    ]))
    return KeepTogether([tbl, Spacer(1, 8)])

def draw_header_footer(canvas, doc):
    canvas.saveState()
    canvas.setStrokeColor(C_DIV)
    canvas.setLineWidth(0.5)
    canvas.line(ML, PAGE_H - MT + 8*mm, PAGE_W - MR, PAGE_H - MT + 8*mm)
    canvas.setFont('LibSans', 8.5); canvas.setFillColor(C_TEXT_2)
    canvas.drawString(ML, PAGE_H - MT + 11*mm, 'WEFT SANDBOX ROADMAP  ·  v0.1')
    canvas.drawRightString(PAGE_W - MR, PAGE_H - MT + 11*mm, 'Zephyr  ·  2025')
    canvas.line(ML, MB - 6*mm, PAGE_W - MR, MB - 6*mm)
    canvas.setFont('LibMono', 8); canvas.setFillColor(C_TEXT_3)
    canvas.drawString(ML, MB - 10*mm, '// weft-sandbox-roadmap/v0.1')
    canvas.drawRightString(PAGE_W - MR, MB - 10*mm, f'page {doc.page}')
    canvas.restoreState()

# Story
story = []
story.append(h1('Weft Sandbox-Constrained Roadmap'))
story.append(Paragraph('Reframed plan: full development, testing, and reporting inside the Linux sandbox. Real-device verification deferred.', style_meta))
story.append(Spacer(1, 4))
story.append(hr())

story.append(verdict_box(True,
    'The entire project is developed, tested, and reported in-sandbox. Real-device verification is Phase 6+, '
    'gated on the constraint lifting. The canonical artifact (the litmus suite) is fully runnable here in three '
    'languages (C, Rust, TypeScript). The platform ports (Kotlin, Swift, Dart) are source-only until real devices are available.'))

# === 0. The constraint ===
story.append(h1('0. The constraint'))
story.append(h2('0.1 What the sandbox can compile and run'))
story.append(table(
    ['Toolchain', 'Version', 'Available', 'Role in project'],
    [
        ['GCC (C/C++)', '14.2.0', 'YES', 'Kernel + litmus runner + tools'],
        ['Rust (rustc + cargo)', '1.98.1', 'YES (installed via rustup)', 'Kernel + litmus runner + tools'],
        ['Node.js + npm', '24.19.0 / 11.17.0', 'YES', 'TypeScript kernel + litmus runner + web Heddles'],
        ['Python', '3.12.14', 'YES', 'Benchmark harness + result pipeline + structural validators'],
        ['Bash + Unix', '(n/a)', 'YES', 'CI glue, Makefiles'],
        ['OpenJDK (with javac)', '(n/a)', 'NO (JRE only)', 'Kotlin/Android source-only'],
        ['Swift', '(n/a)', 'NO', 'Swift/iOS source-only'],
        ['Dart / Flutter', '(n/a)', 'NO', 'Dart/Flutter source-only'],
    ],
    col_widths=[0.22, 0.22, 0.20, 0.36],
))

story.append(h2('0.2 What this means for the project'))
story.append(bp('<b>The kernel can be implemented, litmus-tested, and benchmarked in three languages: C, Rust, TypeScript.</b> All three toolchains are live. The protocol can be proven sound across all three on x86_64 Linux.'))
story.append(bp('<b>The platform ports (Kotlin/Android, Swift/iOS, Dart/Flutter) are source-only.</b> Source files are written and structurally validated by a Python parser, but cannot be compiled or run in-sandbox. Marked "pending real-device verification" and delivered as Phase 6+ artifacts.'))
story.append(bp('<b>The canonical artifact (the litmus suite) does not require a real device.</b> It is a YAML catalog plus per-language runners. Runners run on whatever hardware the language supports.'))
story.append(bp('<b>The moat (the benchmark suite) does not require a real device for the protocol-proof numbers.</b> It does require real devices for the platform numbers (Pixel 7a vs Galaxy S24, etc.). Sandbox numbers are labeled <code>x86_64-sandbox</code>.'))
story.append(bp('<b>Real-device numbers replace sandbox numbers when the constraint lifts.</b> Until then, the spec, docs, and whitepaper carry sandbox numbers with explicit labels.'))

story.append(h2('0.3 What changes vs the canonical ROADMAP'))
story.append(table(
    ['Canonical phase', 'Reframed phase', 'What changes'],
    [
        ['Phase 0 (Kotlin spike)', 'Phase 0 (C + Rust + TS kernels + litmus)', 'Reference language is C, not Kotlin; three implementations instead of one'],
        ['Phase 1 (Android v0.1)', 'Phase 4 (source-only) + Phase 6+ (real device)', 'Split: source written and validated in-sandbox; compilation and on-device tests deferred'],
        ['Phase 2 (Web v0.1)', 'Phase 0e/0f (TS kernel + litmus) + Phase 6+ (browser publish)', 'Kernel and litmus in-sandbox; browser deployment deferred'],
        ['Phase 3 (Benchmark launch)', 'Phase 1 (Python harness) + Phase 5 (sandbox release)', 'Harness and site mockup in-sandbox; public weft.dev launch deferred'],
        ['Phase 4 (Whitepaper)', 'Phase 3', 'In-sandbox, uses sandbox numbers (clearly labeled)'],
        ['Phase 5 (SwiftUI)', 'Phase 4 (source-only) + Phase 6+', 'Source written; compilation deferred'],
        ['Phase 6 (Flutter)', 'Phase 4 (conditional, source-only) + Phase 6+', 'Same'],
        ['Phase 7 (React Native)', 'Phase 4 (source-only, last) + Phase 6+', 'Same'],
        ['Phase 8 (Pro tier)', 'Out of scope', 'Separate repo per charter clause 4'],
    ],
    col_widths=[0.24, 0.34, 0.42],
))

story.append(h2('0.4 What does not change'))
story.append(bp('<b>The Four Laws.</b> Reader never blocked, zero is a contract, mechanism not policy, honesty is a feature. The honesty law now includes "sandbox numbers are labeled sandbox numbers."'))
story.append(bp('<b>The litmus suite is the canonical core.</b> Three languages run it; the project\'s claim to "Weft" is passing it, not lineage.'))
story.append(bp('<b>The frame envelope is Tier 0 frozen.</b> The 16-byte header is implemented once per language and never changes shape.'))
story.append(bp('<b>Writer revocation (I6) is implemented from day one.</b> The use-after-free guard across FFI is non-negotiable; it costs one relaxed load per publish.'))
story.append(bp('<b>Sequential, benchmark-gated phases.</b> A phase ends when its success criterion is met, not when its calendar slot does. Slippage is published.'))

story.append(PageBreak())

# === Phase 0 ===
story.append(h1('Phase 0 — Corrected kernel + L1–L8 litmus suite, in three languages'))
story.append(p(
    'The single most important phase. This is the only thing that can kill the project, '
    'and the founding spec\'s version of it (the two-variable protocol) was formally unsound. '
    'De-risk it first, adversarially, in three languages, or nothing after this matters.'
))

story.append(h2('0a — C kernel (corrected Triad Protocol)'))
story.append(p('Re-implement the protocol from RFC 0001 §4, not the withdrawn two-variable design from the founding spec §5.'))
story.append(bp('<b>Single shared atomic</b> (<code>latest</code>), writer-private <code>w_work</code>, reader-private <code>r_work</code>.'))
story.append(bp('<b>Publish via <code>latest.swap(w_work, AcqRelease)</code></b> — one RMW, one ownership transfer.'))
story.append(bp('<b>Claim via <code>latest.swap(r_work, AcqRelease)</code></b> — same primitive, same semantics.'))
story.append(bp('<b>Frame envelope</b> (Tier 0 frozen): 16-byte header before payload, every buffer, every language.'))
story.append(bp('<b>Writer revocation token</b> (<code>revoked: AtomicBool</code>, <code>epoch: u32</code>): checked once per publish; release sets revoked (Release) before free; free deferred until writer quiescence.'))
story.append(bp('<b>Zero dependencies</b> — pure C11, pthread, stdatomic, posix_memalign.'))
story.append(p('<b>LOC budget:</b> ~600 (kernel + envelope + I6).'))

story.append(h2('0b — C litmus runner'))
story.append(p('The runner executes the L1–L8 catalog against the kernel. The catalog itself is language-agnostic (Phase 0g); the runner is the only platform-specific code.'))
story.append(table(
    ['Test', 'Adversarial condition', 'Verdict'],
    [
        ['L1-tear', 'Reader hold stretched 5/10/50 ms between swap and read; writer at 2× display rate', 'Every claimed payload consistent with envelope seq; zero partial frames'],
        ['L2-writer-steps', 'Instrumented publish under reader holds swept 0–100 ms', 'Publish step count ≤ hard bound regardless of reader'],
        ['L3-reader-steps', 'Instrumented claim under writer storms (4× rate)', 'Claim step count ≤ hard bound; never retries, never fails'],
        ['L4-freshness', 'Writer at 4× reader rate', 'Claimed seq ≥ newest published at claim time, every frame'],
        ['L5-progress', 'Reader hold swept 0–100 ms; reader suspended entirely', 'Writer throughput flat (±noise) in every config'],
        ['L6-ownership', 'Canary word per buffer; randomized interleavings', 'Non-owner mutation of canary fails'],
        ['L7-revocation', 'release() under spinning native writer; freed pages poisoned', 'Zero writes to poisoned pages; DROPPED_REVOKED within one publish'],
        ['L8-envelope', 'Round-trip; unknown trailing fields; version negotiation', 'Byte-identical round-trip; unknown fields ignored; triad-1 + triad-2 coexist'],
    ],
    col_widths=[0.14, 0.46, 0.40],
))
story.append(p('<b>LOC budget:</b> ~1,200 (8 tests × harness code).'))

story.append(h2('0c — Rust kernel'))
story.append(p(
    'Same protocol, same envelope, same I6 contract. Rust\'s <code>std::sync::atomic</code> '
    'maps cleanly to C11 <code>&lt;stdatomic.h&gt;</code>; <code>alloc::alloc</code> with '
    '<code>Layout</code> provides the off-heap buffer. No <code>unsafe</code> in the public '
    'API; every <code>unsafe</code> block has a <code>SAFETY:</code> comment citing the RFC section.'
))
story.append(p('<b>LOC budget:</b> ~600.'))

story.append(h2('0d — Rust litmus runner'))
story.append(p('Same catalog, Rust runner. Validates that the protocol\'s soundness is not a C-specific artifact.'))
story.append(p('<b>LOC budget:</b> ~1,200.'))

story.append(h2('0e — TypeScript kernel'))
story.append(p(
    'The web port. Uses <code>SharedArrayBuffer</code> (Node 24 supports it without COOP/COEP '
    'for in-process Workers) + <code>Atomics</code> for the single atomic. Frame envelope '
    'serialized as a <code>DataView</code> over the SAB.'
))
story.append(p('<b>LOC budget:</b> ~700 (slightly larger due to SAB setup and Worker message protocol).'))

story.append(h2('0f — TypeScript litmus runner'))
story.append(p(
    'Same catalog, TypeScript runner. Uses <code>worker_threads</code> for the writer and reader threads. '
    'Validates the protocol on the web memory model (sequentially consistent for Atomics — stronger than C\'s relaxed ordering).'
))
story.append(p('<b>LOC budget:</b> ~1,400 (Worker setup + message passing overhead).'))

story.append(h2('0g — Litmus catalog (language-agnostic)'))
story.append(p(
    'The catalog is the project\'s canonical specification. YAML file listing each test\'s scenario, '
    'adversarial parameters, and verdict. Runners in C, Rust, and TS all parse the same catalog and execute it.'
))
story.append(p('<b>LOC budget:</b> ~400 (YAML spec + Python catalog validator).'))

story.append(h2('Phase 0 success criterion'))
story.append(bp('<b>L1–L8 green in C, Rust, and TypeScript</b>, each in debug and release builds.'))
story.append(bp('L1 tears = 0 under all adversarial holds (5/10/50 ms).'))
story.append(bp('L2/L3 step bounds hold on release builds.'))
story.append(bp('L7 zero writes to poisoned pages under concurrent release.'))
story.append(bp('Cross-language consistency: each test passes in all three languages, or the protocol is wrong.'))
story.append(bp('RFC 0001 flips to <code>Status: Accepted</code>.'))

story.append(h3('Deliverables'))
story.append(bp('<code>core/c/</code> — C kernel + litmus runner'))
story.append(bp('<code>core/rust/</code> — Rust kernel + litmus runner'))
story.append(bp('<code>core/ts/</code> — TypeScript kernel + litmus runner'))
story.append(bp('<code>litmus/catalog.yaml</code> — language-agnostic test catalog'))
story.append(bp('<code>litmus/REPORT.md</code> — Phase 0 results, 24 green-cell combinations (8 tests × 3 languages)'))
story.append(bp('PDF: <code>Weft-Phase0-Litmus-Report.pdf</code>'))

story.append(PageBreak())

# === Phase 1 ===
story.append(h1('Phase 1 — Python benchmark harness + result pipeline'))
story.append(p('The benchmark suite is the moat. The harness is the engine that produces the moat\'s numbers.'))

story.append(h2('1a — Python harness wrapping the C and Rust kernels'))
story.append(p(
    'Wraps the C kernel (via <code>ctypes</code>) and the Rust kernel (via <code>cdylib</code> + <code>ctypes</code>) '
    'as interchangeable backends. Harness selects the backend at runtime; workload code is identical regardless of backend.'
))

story.append(h2('1b — Five workloads (W1–W5)'))
story.append(p('Each workload is a config + a draw routine. The draw routine is the same shared <code>drawBar</code>-class function across all four implementations (A/B/C/D) — the fairness pin.'))
story.append(table(
    ['Workload', 'Hot state', 'Bounds'],
    [
        ['W1 Audio visualizer', '1024-float PCM @ 60/120 Hz', 'Fixed'],
        ['W2 Particle field', '500 particles × 6-DOF RK4 @ 120 Hz', 'Fixed'],
        ['W3 Spectrogram / heatmap', '256×64 float matrix @ 60 Hz', 'Fixed'],
        ['W4 Data grid', '10k rows × 20 cols, live updates', 'Fixed-cell subset'],
        ['W5 Order book', '1000 levels × 10 fields, 60 Hz L2 feed', 'Fixed'],
    ],
    col_widths=[0.22, 0.50, 0.28],
))

story.append(h2('1c — Four implementations (A/B/C/D)'))
story.append(bp('<b>A — Reactive naive:</b> hot state in a Python <code>list</code> (closest analog to MutableState), redrawn every frame.'))
story.append(bp('<b>B — Best practice:</b> pooled <code>array.array</code>, no per-frame allocation, deferred draw (memcpy into pre-allocated render buffer).'))
story.append(bp('<b>C — Weft:</b> the C kernel via ctypes.'))
story.append(bp('<b>D — Hand-rolled:</b> a hand-coded triple-buffer in Python with the same envelope and I6 contract.'))

story.append(h2('1d — Metrics'))
story.append(bp('P50 / P99 / P100 FPS (<b>P99 is the headline</b>)'))
story.append(bp('Frame allocation rate (bytes/frame, asserted at 0 for C and D)'))
story.append(bp('GC pause count + total ms (via <code>gc.get_stats()</code>)'))
story.append(bp('CPU% (via <code>psutil</code>)'))
story.append(bp('Cold start overhead'))
story.append(bp('Thermal sustained (30-minute run, FPS decay curve)'))

story.append(h2('1e — Fairness pin verification'))
story.append(p(
    'The harness verifies mechanically that A/B/C/D call the same <code>drawBar</code> function with the same '
    'values per frame. Draw-call count and element count are recorded per frame and asserted equal across '
    'implementations. A result bundle without fairness-pin verification is not a result.'
))

story.append(h2('1f — Result bundle format (JSON)'))
story.append(p(
    'Every run emits a signed result bundle: raw frame timestamps, alloc counters, gc logs, device state '
    '(sandbox: x86_64-linux, OS version, CPU model, RAM), harness version, fairness-pin verification.'
))

story.append(h2('1g — Static site generator'))
story.append(p(
    'Python script that reads result bundles from <code>bench/results/</code> and emits a static HTML+CSS '
    'site (no JS framework, no backend) at <code>bench/site/</code>. The site is the future '
    '<code>weft.dev/benchmarks</code> mockup, fully rendered in-sandbox.'
))

story.append(h2('Phase 1 success criterion'))
story.append(bp('All five workloads × four implementations × two backends (C and Rust) produce result bundles.'))
story.append(bp('Fairness pin verified in every bundle.'))
story.append(bp('C and D measure 0 B/frame (asserted; fails the run otherwise).'))
story.append(bp('Static site renders all bundles.'))
story.append(bp('P99 FPS for C and D within 5% of each other (Weft = hand-rolled; if not, the library costs something).'))

story.append(h3('Deliverables'))
story.append(bp('<code>bench/harness/</code> — Python harness'))
story.append(bp('<code>bench/workloads/</code> — W1–W5 configs + draw routines'))
story.append(bp('<code>bench/results/</code> — result bundles (JSON)'))
story.append(bp('<code>bench/site/</code> — static HTML site'))
story.append(bp('PDF: <code>Weft-Phase1-Benchmark-Report.pdf</code>'))

story.append(PageBreak())

# === Phase 2 ===
story.append(h1('Phase 2 — Tools (weft-probe, weft-record)'))
story.append(p('Debug tooling that lives outside the hot path. Both are pure C and Rust; both run in-sandbox.'))

story.append(h2('2a — weft-probe'))
story.append(p(
    'Inspects a running Weft\'s state: dumps the envelope, the <code>latest</code> index, the '
    '<code>w_work</code> and <code>r_work</code> indices, telemetry counters, the canary words. '
    'Connects to a Weft via a shared-memory name (no FFI; reads the buffer header directly).'
))
story.append(p('<b>LOC budget:</b> ~400 (C) + ~400 (Rust).'))

story.append(h2('2b — weft-record'))
story.append(p(
    'Captures frames for debug replay. Writes a <code>.weftrec</code> file (envelope + seq + payload per frame, '
    'binary). The file format is the project\'s first non-Tier-0 artifact; it lives in <code>tools/</code> and '
    'is versioned with the same semver discipline as the kernel public API.'
))
story.append(p('<b>LOC budget:</b> ~600 (C) + ~600 (Rust).'))

story.append(h2('Phase 2 success criterion'))
story.append(bp('<code>weft-probe</code> correctly dumps state from a running C-kernel Weft and a Rust-kernel Weft.'))
story.append(bp('<code>weft-record</code> captures a 30-second run; replay byte-identical.'))
story.append(bp('Both tools\' file formats documented with round-trip tests.'))

story.append(h3('Deliverables'))
story.append(bp('<code>tools/weft-probe/</code> — C and Rust implementations'))
story.append(bp('<code>tools/weft-record/</code> — C and Rust implementations'))
story.append(bp('<code>tools/FORMATS.md</code> — file format specs'))
story.append(bp('PDF: <code>Weft-Phase2-Tools-Report.pdf</code>'))

story.append(PageBreak())

# === Phase 3 ===
story.append(h1('Phase 3 — Whitepaper (synthesized from real measurements)'))
story.append(p(
    'The citable artifact. Every number in it is measured in-sandbox, labeled as <code>x86_64-sandbox</code>. '
    'The whitepaper supersedes the founding spec PDF; the founding spec is explicitly marked '
    '<code>STATUS: SUPERSEDED</code> with a pointer to the whitepaper and the litmus report.'
))

story.append(h2('3a — Structure'))
story.append(bp('1. The problem (recomposition cascade, GC storms, FFI marshalling, main-thread blocking)'))
story.append(bp('2. The thesis — sharpened, with the canonical ROADMAP\'s corrected boundary of the claim'))
story.append(bp('3. The Triad Protocol — corrected single-atomic-exchange design, with the worked trace from RFC 0001 §4.4'))
story.append(bp('4. The frame envelope — Tier 0 frozen'))
story.append(bp('5. Writer revocation (I6) — the use-after-free guard'))
story.append(bp('6. Measured results — Phase 0 litmus + Phase 1 benchmark, all numbers labeled'))
story.append(bp('7. Cross-language consistency — C, Rust, TypeScript all pass the same suite'))
story.append(bp('8. Platform honesty — Safari 60 Hz cap (WebKit bug 173434), SAB COOP/COEP, the iOS Canvas vs MTKView split, RN as weakest differentiator'))
story.append(bp('9. Non-goals — first-class section, not an appendix'))
story.append(bp('10. Open questions — Q1–Q5 from ARCHITECTURE.md, listed not hidden'))
story.append(bp('11. References — prior art (Compose graphicsLayer, Reanimated SharedValue, LeakCanary, Apache Arrow, graphics triple buffering)'))

story.append(h2('3b — Errata to the founding spec'))
story.append(p('The whitepaper\'s appendix explicitly supersedes:'))
story.append(bp('Founding spec §5 (the two-variable protocol) — withdrawn per RFC 0001 §3'))
story.append(bp('Founding spec §9.5 (predicted headline numbers) — replaced by Phase 1 measured numbers'))
story.append(bp('The Phase 1 implementation report (the Rust Android v0.1 code) — that code used the withdrawn protocol; the corrected code is in Phase 0 of this roadmap'))

story.append(h2('Phase 3 success criterion'))
story.append(bp('Every number labeled <code>MEASURED</code> (with sandbox label) or <code>PREDICTION</code> (with reasoning)'))
story.append(bp('Every platform claim carries a source citation'))
story.append(bp('Cross-language consistency results published'))
story.append(bp('Founding spec PDF marked superseded'))

story.append(h3('Deliverables'))
story.append(bp('<code>docs/WHITEPAPER.md</code> — source'))
story.append(bp('PDF: <code>Weft-Whitepaper-v1.0.pdf</code> (typeset, ACM-style)'))
story.append(bp('PDF: <code>Weft-Spec-Errata-v0.1.1.pdf</code> (the explicit supersession)'))

story.append(PageBreak())

# === Phase 4 ===
story.append(h1('Phase 4 — Source-only platform implementations'))
story.append(p(
    'Platform ports written as production-ready source, structurally validated, but not compiled in-sandbox '
    '(no toolchain). Each port is a Phase 6+ deliverable waiting for the constraint to lift.'
))

story.append(h2('4a — Kotlin/Android kernel + Steward + Heddle'))
story.append(bp('Kernel in pure Kotlin/JVM (no Android dependency; runs on plain JVM if JDK installed)'))
story.append(bp('Steward with <code>ViewModel</code> integration (source-only; cannot test without AGP)'))
story.append(bp('Heddle as <code>Modifier.weftDraw</code> extension (source-only; cannot test without Compose runtime)'))
story.append(bp('JNI bridge to the C kernel (so the Kotlin side calls the litmus-passing C code)'))
story.append(bp('Structural validator (Python) checks the API surface matches the spec'))
story.append(p('<b>LOC budget:</b> ~4,700.'))

story.append(h2('4b — Swift/iOS kernel + Steward + Heddle'))
story.append(bp('Kernel in pure Swift (no Apple framework; would run on Linux via swift-atomics if Swift were installed)'))
story.append(bp('Steward with <code>@StateObject</code> integration'))
story.append(bp('Heddle as <code>WeftCanvas</code> (Canvas + CADisplayLink at 60 Hz, MTKView at 120 Hz)'))
story.append(bp('Structural validator'))
story.append(p('<b>LOC budget:</b> ~3,500.'))

story.append(h2('4c — Dart/Flutter kernel + Steward + Heddle'))
story.append(bp('Kernel in pure Dart (no Flutter dependency; would run on Dart VM if dart were installed)'))
story.append(bp('Steward with <code>StatefulWidget</code> integration'))
story.append(bp('Heddle as <code>CustomPainter</code>'))
story.append(bp('Structural validator'))
story.append(p('<b>LOC budget:</b> ~3,050.'))

story.append(h2('4d — TypeScript Heddles (web framework bindings)'))
story.append(p('Phase 0e delivers the TS kernel. Phase 4d wraps it for React, Svelte, Vue, and React Native (Reanimated).'))
story.append(bp('<code>@weft/react</code> — <code>WeftCanvas</code> component, mounts a Worker, transfers OffscreenCanvas'))
story.append(bp('<code>@weft/svelte</code> — Svelte action equivalent'))
story.append(bp('<code>@weft/vue</code> — Vue 3 composable'))
story.append(bp('<code>@weft/react-native</code> — Reanimated <code>SharedValue</code> integration'))
story.append(p('<b>LOC budget:</b> ~1,500 across four bindings.'))

story.append(h2('Phase 4 success criterion'))
story.append(bp('Every source file passes its structural validator (API surface matches spec, no missing functions, panic shielding on every JNI entry, SAFETY comments on every <code>unsafe</code> block)'))
story.append(bp('Every source file has a one-paragraph "why exists" header citing the spec section that justifies it'))
story.append(bp('Each port is marked <code>STATUS: SOURCE-ONLY, PENDING REAL-DEVICE VERIFICATION</code> in its README'))

story.append(h3('Deliverables'))
story.append(bp('<code>core/kotlin/</code>, <code>core/swift/</code>, <code>core/dart/</code> — kernel source'))
story.append(bp('<code>steward/kotlin/</code>, <code>steward/swift/</code>, <code>steward/dart/</code> — steward source'))
story.append(bp('<code>heddles/android/</code>, <code>heddles/swiftui/</code>, <code>heddles/flutter/</code>, <code>heddles/react/</code>, <code>heddles/svelte/</code>, <code>heddles/vue/</code>, <code>heddles/react-native/</code> — Heddle source'))
story.append(bp('<code>tools/validate-structure.py</code> — Python structural validator'))
story.append(bp('PDF: <code>Weft-Phase4-Platform-Source-Report.pdf</code>'))

story.append(PageBreak())

# === Phase 5 ===
story.append(h1('Phase 5 — Sandbox release artifact'))
story.append(p('The sandbox-buildable project is complete at this phase. The deliverable is a single tarball that a contributor with a real dev machine can unpack, build, and use.'))

story.append(h2('5a — Release tarball contents'))
story.append(code_block('''weft-sandbox-v0.1.tar.gz
├── README.md                    (what's runnable here vs what needs a real device)
├── docs/                        (all docs + Phase 3 whitepaper + errata)
├── core/
│   ├── c/                       (litmus-passing)
│   ├── rust/                    (litmus-passing)
│   ├── ts/                      (litmus-passing)
│   ├── kotlin/                  (source-only)
│   ├── swift/                   (source-only)
│   └── dart/                    (source-only)
├── steward/                     (per-language)
├── heddles/                     (per-framework)
├── litmus/
│   ├── catalog.yaml             (language-agnostic)
│   ├── runners/                 (c, rust, ts)
│   └── REPORT.md                (Phase 0 results)
├── bench/
│   ├── harness/                 (Python)
│   ├── workloads/               (W1-W5 configs)
│   ├── results/                 (result bundles)
│   └── site/                    (static HTML)
├── tools/
│   ├── weft-probe/              (C + Rust)
│   └── weft-record/             (C + Rust)
├── rfcs/                        (RFC 0001 + future)
├── reports/                     (all PDFs)
└── INSTALL.md                   (how to build on a real dev machine)'''))

story.append(h2('5b — Install guide'))
story.append(p('<code>INSTALL.md</code> explains:'))
story.append(bp('What runs in-sandbox (C, Rust, TS, Python, tools, litmus, benchmark)'))
story.append(bp('What needs a real dev machine (Kotlin/Android, Swift/iOS, Dart/Flutter)'))
story.append(bp('How to install Rust + Node + Python (already in-sandbox; for contributors)'))
story.append(bp('How to install JDK + Android SDK (for the Kotlin port; not in-sandbox)'))
story.append(bp('How to install Xcode (for the Swift port; not in-sandbox)'))
story.append(bp('How to install Flutter SDK (for the Dart port; not in-sandbox)'))

story.append(h2('Phase 5 success criterion'))
story.append(bp('Tarball builds cleanly when unpacked on a fresh Linux machine with Rust + Node + Python installed'))
story.append(bp('<code>make litmus</code> runs all 24 test combinations (8 tests × 3 languages) and exits 0'))
story.append(bp('<code>make bench</code> runs the harness against C and Rust kernels and produces result bundles'))
story.append(bp('<code>make site</code> regenerates the static benchmark site from result bundles'))
story.append(bp('<code>make validate</code> runs the structural validator against all source-only platform implementations'))

story.append(h3('Deliverables'))
story.append(bp('<code>weft-sandbox-v0.1.tar.gz</code> — release tarball'))
story.append(bp('<code>INSTALL.md</code> — install guide'))
story.append(bp('<code>SHA256SUMS</code> — checksums'))
story.append(bp('PDF: <code>Weft-Phase5-Release-Report.pdf</code>'))

story.append(PageBreak())

# === Phase 6+ ===
story.append(h1('Phase 6+ — Real-device verification (DEFERRED)'))
story.append(verdict_box(True,
    'This phase does not start until the sandbox constraint lifts. It is the next phase after Phase 5, '
    'but it cannot be executed in-sandbox. The plan is documented here so the transition is mechanical '
    'when the constraint lifts.'))

story.append(h2('6a — Compile platform source against the litmus-passing kernels'))
story.append(bp('Kotlin/Android: build the .aar, link against the C kernel via JNI'))
story.append(bp('Swift/iOS: build the .xcframework, link against the C kernel via C interop'))
story.append(bp('Dart/Flutter: build the pub package, link via <code>dart:ffi</code>'))
story.append(bp('TypeScript/Web: deploy the TS kernel to npm, publish <code>@weft/core</code>'))

story.append(h2('6b — Run the litmus suite on real hardware'))
story.append(bp('Android: Pixel 7a (mid), Realme C55 (low), Galaxy S24 (high)'))
story.append(bp('iOS: iPhone SE 2022 (low), iPhone 15 Pro (high)'))
story.append(bp('Web: Chrome 131, Firefox 132, Safari 17.6 on M2 MacBook Air'))
story.append(p('Each real-device run produces a result bundle in the same format as the sandbox bundles. The static site renders both side-by-side.'))

story.append(h2('6c — Replace sandbox numbers with real-device numbers'))
story.append(p(
    'The benchmark site\'s headline charts switch from <code>x86_64-sandbox</code> to '
    '<code>pixel-7a-android</code> (or equivalent). Sandbox numbers remain accessible via a toggle (transparency).'
))

story.append(h2('6d — Public launches'))
story.append(bp('Maven Central: <code>dev.weft:android:0.1.0</code>'))
story.append(bp('npm: <code>@weft/core</code>, <code>@weft/react</code>, <code>@weft/svelte</code>, <code>@weft/vue</code>, <code>@weft/react-native</code>'))
story.append(bp('Swift Package: <code>Weft</code>'))
story.append(bp('pub.dev: <code>weft</code>'))
story.append(bp('weft.dev: launch the static site publicly'))
story.append(bp('HN, r/androiddev, r/iOSProgramming, r/flutter, r/webdev: launch posts'))

story.append(h2('6e — Whitepaper v1.1'))
story.append(p(
    'Replace the <code>x86_64-sandbox</code> numbers in the whitepaper with real-device numbers. '
    'Sandbox numbers remain in an appendix (transparency).'
))

story.append(h2('Phase 6+ success criterion'))
story.append(bp('All platform source from Phase 4 compiles cleanly against the litmus-passing kernels'))
story.append(bp('The litmus suite passes on every real device (L1–L8, all green)'))
story.append(bp('A stranger can reproduce a published number on their own hardware (the ROADMAP\'s success criterion for Phase 3)'))

# === Endpoint ===
story.append(PageBreak())
story.append(h1('Endpoint: when is the sandbox-buildable project "done"?'))
story.append(p(
    'At Phase 5, the sandbox-buildable project is complete:'
))
story.append(bp('Litmus-passing kernel in <b>three languages</b> (C, Rust, TypeScript), all eight tests green in all three'))
story.append(bp('Benchmark harness with measured numbers (clearly labeled <code>x86_64-sandbox</code>)'))
story.append(bp('Tools (<code>weft-probe</code>, <code>weft-record</code>) in C and Rust'))
story.append(bp('Whitepaper with real sandbox numbers'))
story.append(bp('Source-only platform implementations (Kotlin, Swift, Dart) — structurally validated, ready for Phase 6+'))
story.append(bp('Release tarball with install guide'))

story.append(verdict_box(True,
    '<b>The next phase (real-device verification, Phase 6+) cannot proceed until the sandbox constraint lifts.</b> '
    'That is the explicit endpoint. Everything after Phase 5 is gated on the constraint.'))

story.append(h1('What this roadmap does NOT change'))
story.append(bp('<b>The Four Laws are still enforced.</b> Law 2 (zero is a contract) is asserted at 0 B/frame for C and D in the benchmark harness; Law 4 (honesty) now includes "sandbox numbers are labeled sandbox numbers."'))
story.append(bp('<b>The litmus suite is still the canonical core.</b> Three languages pass it; that\'s three independent proofs of the protocol\'s soundness.'))
story.append(bp('<b>The frame envelope is still Tier 0 frozen.</b>'))
story.append(bp('<b>Writer revocation (I6) is still implemented from day one.</b>'))
story.append(bp('<b>Sequential, benchmark-gated phases.</b> A phase ends when its success criterion is met.'))
story.append(bp('<b>The Pro tier is still out of scope</b> (separate repo, BSL, charter clause 4).'))

story.append(h1('The roadmap\'s one rule (unchanged)'))
story.append(p(
    '<i>A phase without a green success criterion does not get followed by the next phase. Slippage is published.</i>'
))
story.append(p(
    'The sandbox constraint does not relax this rule; it tightens it. A phase that claims green without producing '
    'the artifacts (kernel source, litmus results, benchmark bundles, PDFs) has not finished, regardless of how '
    'much code was written.'
))

# Build
OUT = '/home/z/my-project/download/Weft-Sandbox-Roadmap.pdf'

class RoadmapDoc(BaseDocTemplate):
    def __init__(self, filename, **kw):
        super().__init__(filename, **kw)
        frame = Frame(ML, MB, PAGE_W - ML - MR, PAGE_H - MT - MB - 16*mm,
                      id='normal', leftPadding=0, rightPadding=0,
                      topPadding=0, bottomPadding=0)
        self.addPageTemplates([PageTemplate(id='Main', frames=[frame], onPage=draw_header_footer)])

doc = RoadmapDoc(OUT, pagesize=A4,
    leftMargin=ML, rightMargin=MR, topMargin=MT, bottomMargin=MB,
    title='Weft Sandbox-Constrained Roadmap',
    author='Zephyr (zephyr4289)',
    subject='Reframed plan for in-sandbox development of the Weft Continuous-State Plane',
    creator='Weft sandbox roadmap generator',
)
doc.build(story)
print(f'Sandbox roadmap: {OUT}')
print(f'Pages: {doc.page}')
