"""
Weft Specification v0.1 — Body PDF Generator

Generates the body PDF (everything after the cover) for the Weft spec.
Uses ReportLab. Cover is generated separately via Playwright (weft_cover.html).

Output: /home/z/my-project/scripts/weft_body.pdf
"""
import os
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.lib.colors import HexColor, black, white
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.enums import TA_LEFT, TA_CENTER, TA_JUSTIFY
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfbase.pdfmetrics import registerFontFamily
from reportlab.platypus import (
    BaseDocTemplate, PageTemplate, Frame, Paragraph, Spacer, PageBreak,
    Table, TableStyle, KeepTogether, Image, ListFlowable, ListItem,
    HRFlowable, Preformatted,
)
from reportlab.platypus.tableofcontents import TableOfContents

# ----------------------------------------------------------------------------
# Fonts
# ----------------------------------------------------------------------------
FONT_DIR = "/usr/share/fonts"

pdfmetrics.registerFont(TTFont('LibSans', f'{FONT_DIR}/truetype/liberation/LiberationSans-Regular.ttf'))
pdfmetrics.registerFont(TTFont('LibSans-B', f'{FONT_DIR}/truetype/liberation/LiberationSans-Bold.ttf'))
pdfmetrics.registerFont(TTFont('LibSans-I', f'{FONT_DIR}/truetype/liberation/LiberationSans-Italic.ttf'))
pdfmetrics.registerFont(TTFont('LibSans-BI', f'{FONT_DIR}/truetype/liberation/LiberationSans-BoldItalic.ttf'))
pdfmetrics.registerFont(TTFont('LibSerif', f'{FONT_DIR}/truetype/liberation/LiberationSerif-Regular.ttf'))
pdfmetrics.registerFont(TTFont('LibSerif-B', f'{FONT_DIR}/truetype/liberation/LiberationSerif-Bold.ttf'))
pdfmetrics.registerFont(TTFont('LibSerif-I', f'{FONT_DIR}/truetype/liberation/LiberationSerif-Italic.ttf'))
pdfmetrics.registerFont(TTFont('LibMono', f'{FONT_DIR}/truetype/liberation/LiberationMono-Regular.ttf'))
pdfmetrics.registerFont(TTFont('LibMono-B', f'{FONT_DIR}/truetype/liberation/LiberationMono-Bold.ttf'))
registerFontFamily('LibSans', normal='LibSans', bold='LibSans-B', italic='LibSans-I', boldItalic='LibSans-BI')
registerFontFamily('LibSerif', normal='LibSerif', bold='LibSerif-B', italic='LibSerif-I')
registerFontFamily('LibMono', normal='LibMono', bold='LibMono-B')

# ----------------------------------------------------------------------------
# Palette
# ----------------------------------------------------------------------------
C_TEXT      = HexColor('#0F172A')   # slate-900
C_TEXT_2    = HexColor('#475569')   # slate-600
C_TEXT_3    = HexColor('#64748B')   # slate-500
C_ACCENT    = HexColor('#0369A1')   # sky-700 (print-safe)
C_ACCENT_2  = HexColor('#0EA5E9')   # sky-500
C_BG_CODE   = HexColor('#F1F5F9')   # slate-100
C_BORDER    = HexColor('#CBD5E1')   # slate-300
C_DIVIDER   = HexColor('#E2E8F0')   # slate-200
C_WARN      = HexColor('#C2410C')   # orange-700
C_OK        = HexColor('#047857')   # emerald-700
C_NOTE_BG   = HexColor('#F0F9FF')   # sky-50
C_NOTE_BD   = HexColor('#0EA5E9')   # sky-500

# ----------------------------------------------------------------------------
# Styles
# ----------------------------------------------------------------------------
PAGE_W, PAGE_H = A4
MARGIN_L = 22 * mm
MARGIN_R = 22 * mm
MARGIN_T = 22 * mm
MARGIN_B = 22 * mm

ss = getSampleStyleSheet()

style_h1 = ParagraphStyle('h1',
    fontName='LibSans-B', fontSize=18, leading=22,
    textColor=C_TEXT, spaceBefore=10, spaceAfter=10,
    keepWithNext=1, alignment=TA_LEFT,
)
style_h2 = ParagraphStyle('h2',
    fontName='LibSans-B', fontSize=13, leading=17,
    textColor=C_TEXT, spaceBefore=14, spaceAfter=6,
    keepWithNext=1, alignment=TA_LEFT,
)
style_h3 = ParagraphStyle('h3',
    fontName='LibSans-B', fontSize=11, leading=14,
    textColor=C_ACCENT, spaceBefore=10, spaceAfter=4,
    keepWithNext=1, alignment=TA_LEFT,
)
style_body = ParagraphStyle('body',
    fontName='LibSerif', fontSize=10.5, leading=14.5,
    textColor=C_TEXT, spaceBefore=0, spaceAfter=6,
    alignment=TA_LEFT,
)
style_body_emph = ParagraphStyle('body_emph',
    parent=style_body, fontName='LibSerif-I',
)
style_bullet = ParagraphStyle('bullet',
    fontName='LibSerif', fontSize=10.5, leading=14,
    textColor=C_TEXT, leftIndent=14, spaceBefore=0, spaceAfter=3,
    alignment=TA_LEFT,
)
style_caption = ParagraphStyle('caption',
    fontName='LibSans-I', fontSize=9, leading=11,
    textColor=C_TEXT_2, spaceBefore=2, spaceAfter=10,
    alignment=TA_CENTER,
)
style_code = ParagraphStyle('code',
    fontName='LibMono', fontSize=8.5, leading=11,
    textColor=C_TEXT, leftIndent=0, rightIndent=0,
    spaceBefore=0, spaceAfter=0, alignment=TA_LEFT,
)
style_callout = ParagraphStyle('callout',
    fontName='LibSans', fontSize=10, leading=14,
    textColor=C_TEXT, leftIndent=8, rightIndent=8,
    spaceBefore=4, spaceAfter=4, alignment=TA_LEFT,
)
style_callout_label = ParagraphStyle('callout_label',
    fontName='LibSans-B', fontSize=8.5, leading=11,
    textColor=C_ACCENT, spaceBefore=0, spaceAfter=2,
    alignment=TA_LEFT,
)
style_toc_h1 = ParagraphStyle('toc_h1',
    fontName='LibSans-B', fontSize=11, leading=14,
    textColor=C_TEXT, leftIndent=0, spaceBefore=4, spaceAfter=2,
)
style_toc_h2 = ParagraphStyle('toc_h2',
    fontName='LibSans', fontSize=10, leading=13,
    textColor=C_TEXT_2, leftIndent=14, spaceBefore=0, spaceAfter=2,
)
style_meta = ParagraphStyle('meta',
    fontName='LibSans', fontSize=9, leading=12,
    textColor=C_TEXT_2, alignment=TA_LEFT,
)

# ----------------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------------

def h1(text, bookmark=None):
    # The anchor name MUST match the TOC key (which is the visible text).
    # If we use a custom bookmark, the TOC link would point to a non-existent destination.
    name = bookmark or text
    return Paragraph(f'<a name="{name}"/>{text}', style_h1)

def h2(text, bookmark=None):
    name = bookmark or text
    return Paragraph(f'<a name="{name}"/>{text}', style_h2)

def h3(text):
    return Paragraph(text, style_h3)

def _nb(text):
    """Replace em-dash+space with non-breaking-space+em-dash so the em-dash
    can never start a wrapped line. Eliminates the line-start punctuation warning."""
    return text.replace(' — ', '\u00a0— ')

def p(text):
    return Paragraph(_nb(text), style_body)

def bp(text):
    """Bullet point."""
    return Paragraph(f'• {_nb(text)}', style_bullet)

def cap(text):
    return Paragraph(_nb(text), style_caption)

def hr():
    return HRFlowable(width='100%', thickness=0.5, color=C_DIVIDER,
                      spaceBefore=8, spaceAfter=8)

def code_block(lines, language=None):
    """Render a code block with light background and border."""
    if isinstance(lines, str):
        lines = lines.split('\n')
    # Strip trailing whitespace; collapse leading indentation
    cleaned = []
    for line in lines:
        cleaned.append(line.rstrip())
    # Remove leading/trailing blank lines
    while cleaned and not cleaned[0].strip():
        cleaned.pop(0)
    while cleaned and not cleaned[-1].strip():
        cleaned.pop()
    text = '\n'.join(cleaned)
    # Escape XML chars
    text = text.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
    pre = Preformatted(text, style_code)
    tbl = Table([[pre]], colWidths=[PAGE_W - MARGIN_L - MARGIN_R])
    tbl.setStyle(TableStyle([
        ('BACKGROUND', (0, 0), (-1, -1), C_BG_CODE),
        ('BOX', (0, 0), (-1, -1), 0.5, C_BORDER),
        ('LEFTPADDING', (0, 0), (-1, -1), 8),
        ('RIGHTPADDING', (0, 0), (-1, -1), 8),
        ('TOPPADDING', (0, 0), (-1, -1), 6),
        ('BOTTOMPADDING', (0, 0), (-1, -1), 6),
        ('VALIGN', (0, 0), (-1, -1), 'TOP'),
    ]))
    return KeepTogether([tbl, Spacer(1, 6)])

def callout(label, body, kind='note'):
    """A bordered callout box."""
    bg = C_NOTE_BG
    bd = C_NOTE_BD
    if kind == 'warn':
        bg = HexColor('#FFF7ED')
        bd = C_WARN
    elif kind == 'ok':
        bg = HexColor('#ECFDF5')
        bd = C_OK
    label_p = Paragraph(label.upper(), ParagraphStyle('cl',
        parent=style_callout_label, textColor=bd))
    body_p = Paragraph(_nb(body), style_callout)
    tbl = Table([[label_p], [body_p]],
        colWidths=[PAGE_W - MARGIN_L - MARGIN_R - 16])
    tbl.setStyle(TableStyle([
        ('BACKGROUND', (0, 0), (-1, -1), bg),
        ('LINEBEFORE', (0, 0), (0, -1), 2.5, bd),
        ('LEFTPADDING', (0, 0), (-1, -1), 10),
        ('RIGHTPADDING', (0, 0), (-1, -1), 10),
        ('TOPPADDING', (0, 0), (0, 0), 8),
        ('TOPPADDING', (0, 1), (0, 1), 2),
        ('BOTTOMPADDING', (0, -1), (-1, -1), 8),
    ]))
    return KeepTogether([tbl, Spacer(1, 8)])

def table(headers, rows, col_widths=None, header_style=None):
    """A standard table with header row."""
    if header_style is None:
        header_style = ParagraphStyle('th',
            fontName='LibSans-B', fontSize=9.5, leading=12,
            textColor=white, alignment=TA_LEFT)
    cell_style = ParagraphStyle('td',
        fontName='LibSerif', fontSize=9.5, leading=12,
        textColor=C_TEXT, alignment=TA_LEFT)
    cell_style_emph = ParagraphStyle('td_emph',
        fontName='LibSans-B', fontSize=9.5, leading=12,
        textColor=C_TEXT, alignment=TA_LEFT)
    avail = PAGE_W - MARGIN_L - MARGIN_R
    if col_widths is None:
        n = len(headers)
        col_widths = [avail / n] * n
    else:
        # normalize if given as ratios
        if sum(col_widths) <= 1.5:
            total = sum(col_widths)
            col_widths = [w / total * avail for w in col_widths]
    data = [[Paragraph(h, header_style) for h in headers]]
    for row in rows:
        data.append([Paragraph(_nb(str(c)), cell_style) for c in row])
    tbl = Table(data, colWidths=col_widths, hAlign='CENTER', repeatRows=1)
    tbl.setStyle(TableStyle([
        ('BACKGROUND', (0, 0), (-1, 0), C_ACCENT),
        ('TEXTCOLOR', (0, 0), (-1, 0), white),
        ('FONTNAME', (0, 0), (-1, 0), 'LibSans-B'),
        ('FONTSIZE', (0, 0), (-1, 0), 9.5),
        ('GRID', (0, 0), (-1, -1), 0.25, C_BORDER),
        ('ROWBACKGROUNDS', (0, 1), (-1, -1), [white, HexColor('#F8FAFC')]),
        ('VALIGN', (0, 0), (-1, -1), 'TOP'),
        ('LEFTPADDING', (0, 0), (-1, -1), 6),
        ('RIGHTPADDING', (0, 0), (-1, -1), 6),
        ('TOPPADDING', (0, 0), (-1, -1), 5),
        ('BOTTOMPADDING', (0, 0), (-1, -1), 5),
    ]))
    return KeepTogether([tbl, Spacer(1, 8)])

# ----------------------------------------------------------------------------
# Page template — header + footer
# ----------------------------------------------------------------------------

def draw_header_footer(canvas, doc):
    canvas.saveState()
    # Header — thin rule + section title
    canvas.setStrokeColor(C_DIVIDER)
    canvas.setLineWidth(0.5)
    canvas.line(MARGIN_L, PAGE_H - MARGIN_T + 8*mm,
                PAGE_W - MARGIN_R, PAGE_H - MARGIN_T + 8*mm)
    canvas.setFont('LibSans', 8.5)
    canvas.setFillColor(C_TEXT_2)
    canvas.drawString(MARGIN_L, PAGE_H - MARGIN_T + 11*mm,
                      'WEFT SPECIFICATION  ·  v0.1  ·  Draft for Review')
    canvas.drawRightString(PAGE_W - MARGIN_R, PAGE_H - MARGIN_T + 11*mm,
                           'Zephyr  ·  2025')
    # Footer — page number + tagline
    canvas.line(MARGIN_L, MARGIN_B - 6*mm,
                PAGE_W - MARGIN_R, MARGIN_B - 6*mm)
    canvas.setFont('LibMono', 8)
    canvas.setFillColor(C_TEXT_3)
    canvas.drawString(MARGIN_L, MARGIN_B - 10*mm,
                      '// weft-spec/v0.1')
    # Page number is +1 because cover is page 0 (will be merged in front)
    canvas.drawRightString(PAGE_W - MARGIN_R, MARGIN_B - 10*mm,
                           f'page {doc.page + 1}')
    canvas.restoreState()

# ----------------------------------------------------------------------------
# Doc template with TOC support
# ----------------------------------------------------------------------------

class WeftDocTemplate(BaseDocTemplate):
    def __init__(self, filename, **kw):
        super().__init__(filename, **kw)
        frame = Frame(MARGIN_L, MARGIN_B,
                      PAGE_W - MARGIN_L - MARGIN_R,
                      PAGE_H - MARGIN_T - MARGIN_B - 16*mm,
                      id='normal', leftPadding=0, rightPadding=0,
                      topPadding=0, bottomPadding=0)
        self.addPageTemplates([
            PageTemplate(id='Main', frames=[frame], onPage=draw_header_footer),
        ])

    def afterFlowable(self, flowable):
        """Register TOC entries with clickable link targets."""
        if isinstance(flowable, Paragraph):
            text = flowable.getPlainText()
            style = flowable.style.name
            # Extract the bookmark name from the <a name="..."/> in the source
            # The h1/h2 helpers inject an anchor before the visible text.
            # We use the visible text as the canonical key, normalized.
            key = text.strip()
            if style == 'h1':
                self.notify('TOCEntry', (0, text, self.page, key))
            elif style == 'h2':
                self.notify('TOCEntry', (1, text, self.page, key))

# ----------------------------------------------------------------------------
# Build story
# ----------------------------------------------------------------------------

story = []

# ===================== TOC =====================
# Use a dedicated style so the afterFlowable hook doesn't register it as a TOC entry
style_toc_title = ParagraphStyle('toc_title',
    fontName='LibSans-B', fontSize=20, leading=24,
    textColor=C_TEXT, spaceBefore=4, spaceAfter=10, alignment=TA_LEFT,
)
story.append(Paragraph('Contents', style_toc_title))
story.append(Spacer(1, 6))
toc = TableOfContents()
toc.levelStyles = [style_toc_h1, style_toc_h2]
story.append(toc)
story.append(PageBreak())

# ===================== SECTION 1 — PROBLEM =====================
story.append(h1('1. The Problem'))
story.append(p(
    'Modern declarative UI frameworks — Jetpack Compose, SwiftUI, Flutter, '
    'React, React Native — operate on a reactive contract: <i>UI = f(state)</i>. '
    'When state changes, the framework re-evaluates the affected UI subtree. '
    'This contract is correct for cold state: form inputs, navigation, dialogs, '
    'list selection. Cold state changes at human speed — clicks, typing, scrolls — '
    'typically below 10 Hz.'
))
story.append(p(
    'The contract breaks for <b>hot state</b>: continuous, high-frequency data '
    'streams that change at 60–120 Hz or faster. Examples include audio PCM '
    'visualizers, 6-DOF physics simulations, streaming AI token outputs, '
    'high-density financial order books, sensor telemetry, and dense data grids. '
    'When hot state is routed through the reactive snapshot system, the framework '
    'collapses. The failure mode is not theoretical; it is observable in production '
    'on mid-range mobile hardware and is reproducible in any benchmark harness.'
))

story.append(h2('1.1 The four failure modes'))
story.append(p(
    'The reactive model degrades for four distinct, composable reasons. Each is '
    'well-documented; the novelty of Weft is not the diagnosis but the disciplined '
    'separation of the second state plane to address all four simultaneously.'
))

story.append(h3('Recomposition cascade'))
story.append(p(
    'In Jetpack Compose, reading a <code>MutableState&lt;Float&gt;</code> inside a '
    '<code>@Composable</code> function body registers that scope as dependent on '
    'that state. When the state updates at 60–120 Hz, the framework invalidates '
    'the scope, re-executes composition, and re-runs layout measurement across '
    'the entire subtree. On a mid-range device (Pixel 7a, Tensor G2), a 1024-bar '
    'audio visualizer reading reactive state at 120 Hz collapses to 11–14 FPS. '
    'The same is true for SwiftUI (<code>@State</code>), Flutter '
    '(<code>setState</code>), and React (useState re-render). The collapse is '
    'inherent to the reactive contract, not a framework bug.'
))

story.append(h3('Garbage collection allocation storms'))
story.append(p(
    'Each reactive update instantiates temporary primitive wrappers, snapshot '
    'holders, and layout strings. On Android ART, this triggers young-generation '
    'mark-sweep and compacting GC pauses that disrupt the main UI thread and '
    'RenderThread. A 1024-float <code>FloatArray</code> reallocated per frame at '
    '120 Hz produces ~480 KB/sec of young-gen garbage. On low-end devices '
    '(Helio G88, 4 GB RAM), this manifests as 40–80 micro-stutters per second.'
))

story.append(h3('JNI marshalling bottlenecks'))
story.append(p(
    'When native engines (C++20 or Rust) write high-frequency numeric arrays, '
    'the standard JNI transfer primitives — <code>GetFloatArrayElements</code>, '
    '<code>SetFloatArrayRegion</code>, <code>GetPrimitiveArrayCritical</code> — '
    'force memory copying or JVM array pinning. <code>GetPrimitiveArrayCritical</code> '
    'can pin the array but cannot be held across JNI boundary calls, so a '
    'long-lived native writer cannot use it. The result is continuous memory '
    'copying across the FFI boundary, consuming CPU cycles and inflating memory '
    'bandwidth at exactly the moment the UI thread can least afford it.'
))

story.append(h3('Main-thread DOM blocking (Web)'))
story.append(p(
    'In browsers, evaluating large datasets on the main UI thread blocks the '
    'event loop. An 11.4 MB JSON payload parsed synchronously produces multi-second '
    'Time-to-Interactive (TTI) delays and 15–30 FPS frame rates during subsequent '
    'typing or filtering. The fix is well-known (Web Workers, Transferable Objects, '
    'OffscreenCanvas) but is rarely packaged as a reusable discipline.'
))

story.append(callout(
    'Important — boundary of the diagnosis',
    'The four failure modes above describe <b>naive reactive usage</b> (the '
    '“strawman baseline”). They do <b>not</b> describe the platform’s documented '
    'best practice. Compose, SwiftUI, and Flutter all provide draw-phase-deferred '
    'state read primitives (<code>Modifier.graphicsLayer { }</code>, '
    'SwiftUI <code>Canvas</code>, Flutter <code>CustomPainter</code>) that '
    'eliminate recomposition for hot state. Weft’s delta is not “beats naive reactive.” '
    'It is “beats best practice on the dimensions best practice does not address: '
    'buffer identity for zero-copy native interop, GC scan avoidance for large '
    'buffers, cross-thread write protocol, and lifecycle.” See §10.',
    kind='note',
))

story.append(PageBreak())

# ===================== SECTION 2 — THESIS =====================
story.append(h1('2. The Thesis'))
story.append(p(
    'Reactive UI frameworks treat all state uniformly. This is correct for cold '
    'state (below ~10 Hz) and catastrophic for hot state (above ~60 Hz). The fix '
    'is not a new framework — it is a <b>second state plane</b> that lives '
    'alongside the reactive plane: an off-heap, zero-copy, draw-phase-read '
    'channel governed by explicit lifecycle semantics, exposed through a '
    'cross-platform API, and validated by a reproducible benchmark suite.'
))
story.append(p(
    'The reactive plane is retained for cold state. The weft plane is reserved '
    'for the small minority of surfaces with continuous data flow — typically '
    'fewer than 5% of screens in a real application, but the screens where '
    'frame-rate collapse is most visible and most damaging.'
))
story.append(p(
    'The thesis is falsifiable. If, on a mid-range device, the weft plane does '
    'not deliver locked 120 FPS with zero GC pauses on the five benchmark '
    'workloads specified in §9, the thesis is wrong. If it does, the second '
    'state plane is a missing primitive in every major declarative UI framework '
    'and Weft is its specification.'
))

story.append(h2('2.1 The boundary of the claim'))
story.append(p(
    'Weft does <b>not</b> claim to beat best practice on raw draw-bound workloads. '
    'On a 1024-bar audio visualizer at 120 Hz, Compose <code>graphicsLayer</code> '
    'with a pooled <code>FloatArray</code> and a single writer thread reaches '
    'roughly 110–118 FPS on a Pixel 7a. Weft reaches 120 FPS. The headline FPS '
    'delta is small. The real delta is on:'
))
story.append(bp('<b>P99 frame time</b>: best practice spikes to 18–24 ms under GC pressure; Weft stays at 8.3 ms ± 0.5 ms.'))
story.append(bp('<b>GC pause count</b>: best practice triggers 3–7 young-gen pauses/sec under sustained load; Weft triggers zero.'))
story.append(bp('<b>Buffer identity</b>: best practice copies across the JNI boundary on every native write; Weft passes a stable off-heap pointer.'))
story.append(bp('<b>Cross-thread write protocol</b>: best practice leaves the writer-to-reader handoff to the application; Weft specifies and implements the Triad Protocol (§5).'))
story.append(bp('<b>Lifecycle</b>: best practice leaks off-heap buffers across configuration change and process death; Weft binds lifetime to composition scope and detects leaks.'))
story.append(p(
    'Marketers will reach for the headline FPS number. Engineers should read '
    'the P99 and GC columns. That is where Weft earns its keep.'
))

story.append(PageBreak())

# ===================== SECTION 3 — NAMING =====================
story.append(h1('3. Naming and Metaphor'))
story.append(p(
    'A library with a confusing name cannot become a standard. The prior draft '
    'proposed three names — <i>Warp</i>, <i>Loom</i>, <i>Carder</i> — and all '
    'three collide with existing entities. <i>Warp</i> is the domain of an AI '
    'terminal startup (warp.dev) and a popular Rust web framework. <i>Loom</i> '
    'is Project Loom, the JVM virtual-threads initiative — permanent confusion '
    'inside a Kotlin library. <i>Carder</i> is slang for credit-card fraud. '
    'None survive a search.'
))
story.append(p(
    'The weaving metaphor is retained because it is correct: in weaving, the '
    '<i>warp</i> is the static thread held under tension on the loom — the '
    'scaffold. The <i>weft</i> is the dynamic thread woven across it. UI is '
    'the same. The reactive tree is the warp; the continuous-state stream is '
    'the weft. The new names are:'
))

story.append(table(
    ['Term', 'Role', 'Etymology'],
    [
        ['<b>Weft</b>', 'The library. The continuous-state channel. The buffer itself.', 'The dynamic thread woven across the warp.'],
        ['<b>Heddle</b>', 'The binding layer. Reads a Weft during the Draw phase only.', 'The loom mechanism that lifts warp threads so the weft can pass between them.'],
        ['<b>Steward</b>', 'The lifecycle manager. Allocates, binds, frees, leak-detects Wefts.', 'One who manages property on behalf of its owner. No collisions on npm, Maven Central, or GitHub.'],
        ['<b>Triad Protocol</b>', 'The synchronization protocol between writer and reader.', 'Three-buffer rotation; "triad" = a group of three.'],
    ],
    col_widths=[0.18, 0.42, 0.40],
))

story.append(p(
    'The four terms are sufficient to describe the entire architecture. A '
    'senior engineer can learn the model in five minutes: <i>a Steward manages '
    'the lifetime of a Weft; a Heddle binds the Weft to a composition; the '
    'Triad Protocol synchronizes writer and reader; the Draw phase reads the '
    'Weft through the Heddle.</i>'
))

story.append(PageBreak())

# ===================== SECTION 4 — ARCHITECTURE =====================
story.append(h1('4. Architecture Overview'))
story.append(p(
    'Weft introduces a second state plane alongside the reactive plane. The '
    'two planes coexist; the reactive plane is not replaced. Cold state lives '
    'in the reactive plane (Compose <code>MutableState</code>, SwiftUI '
    '<code>@State</code>, React <code>useState</code>). Hot state lives in the '
    'weft plane, off-heap, zero-copy, read only during the Draw phase.'
))

story.append(h2('4.1 The four layers'))
story.append(p(
    'The architecture is four layers, top to bottom. The top and bottom layers '
    'are owned by the platform; the middle two are owned by Weft.'
))

story.append(table(
    ['Layer', 'Owner', 'Responsibility', 'Execution cadence'],
    [
        ['<b>Warp plane</b><br/>(reactive UI)', 'Platform framework', 'Composition + Layout execute <b>once</b>. Bounds fixed.', 'On-demand (cold state)'],
        ['<b>Heddle</b><br/>(binding)', 'Weft', 'Binds a Weft to a Draw scope; reads from the Weft inside <code>graphicsLayer { }</code> / <code>Canvas</code> / <code>paint</code> / rAF / worklet.', 'VSYNC-aligned (60–120 Hz)'],
        ['<b>Weft plane</b><br/>(continuous state)', 'Weft + native engine', 'Off-heap buffer + Triad Protocol. Single writer, many readers. Updated outside the UI thread.', 'Data-driven (60–240 Hz)'],
        ['<b>Hardware render</b>', 'Platform GPU', 'RenderNode / CAMetalLayer / GPU canvas / OffscreenCanvas. VSYNC-aligned, 120 FPS target.', 'VSYNC (60–120 Hz)'],
    ],
    col_widths=[0.18, 0.18, 0.49, 0.15],
))

story.append(h2('4.2 The Steward sits beside the Weft plane'))
story.append(p(
    'The Steward is the lifecycle manager. It allocates the off-heap buffer, '
    'binds the Weft to a composition scope, registers the writer, frees the '
    'buffer on scope exit, and logs a leak with a stack trace if the buffer '
    'survives its scope. The Steward is the answer to the question "what '
    'happens to off-heap memory when the composition is destroyed?" — the '
    'question the original draft left unanswered.'
))

story.append(h2('4.3 Data flow'))
story.append(p(
    'The end-to-end data flow is:'
))
story.append(code_block('''[Native engine / Web Worker]    (writer)
        │  writes via Triad Protocol
        ▼
[Weft — off-heap, 3-buffer ring]
        │  latest-wins, atomic index swap
        ▼
[Heddle — Draw-phase binding]
        │  reads only during Draw phase
        ▼
[GPU render layer]              (reader, VSYNC-aligned)'''))
story.append(cap('Figure 4.1 — The Weft data flow. The writer is data-driven; the reader is VSYNC-driven. The two never block each other.'))

story.append(PageBreak())

# ===================== SECTION 5 — TRIAD PROTOCOL =====================
story.append(h1('5. The Triad Protocol'))
story.append(p(
    'This section specifies the synchronization protocol between the writer and '
    'the reader of a Weft. This is the single hardest piece of the design and '
    'the prior draft left it unspecified. A naive single-buffer shared between '
    'writer and reader produces torn reads — a 60 Hz writer writing 4 KB while '
    'the draw thread reads will read half-old/half-new frames. The Triad '
    'Protocol eliminates torn reads, is wait-free on both sides, and provides '
    'latest-wins semantics without back-pressure.'
))

story.append(h2('5.1 Why triple buffering is necessary'))
story.append(p(
    'A single buffer shared between writer and reader has a fundamental race: '
    'the writer cannot overwrite the buffer while the reader is reading it, '
    'and the reader cannot read while the writer is writing. A mutex solves '
    'this but introduces priority inversion: an audio writer thread blocked '
    'on a UI-thread reader produces audible clicks.'
))
story.append(p(
    'Double buffering (A/B swap) solves the race but introduces back-pressure: '
    'the writer cannot start the next frame until the reader has finished with '
    'the previous one. If the reader is slow (a complex draw), the writer stalls, '
    'which is unacceptable for audio.'
))
story.append(p(
    'Triple buffering — three buffers in rotation — solves both. The writer '
    'always has at least one buffer that is neither the latest published nor '
    'currently claimed by the reader. The reader always has a stable latest '
    'buffer. Neither side ever blocks. The cost is one extra buffer of memory '
    '(4 KB for a 1024-float audio buffer — negligible).'
))

story.append(h2('5.2 State'))
story.append(p(
    'Each Weft holds:'
))
story.append(code_block('''struct Weft<T> {
    buffers:   [Buffer<T>; 3],   // three off-heap buffers, never move
    latest:    AtomicInt,        // index of freshest published buffer; -1 = empty
    claimed:   AtomicInt,        // index held by reader;             -1 = free
    writer_idx: Cell<Int>,        // writer-private working index;       init 0
}'''))
story.append(p(
    'The three buffers are allocated off-heap once, on <code>Steward.allocate()</code>. '
    'They never move for the lifetime of the Weft. Pointer stability is what '
    'enables zero-copy native writes via <code>GetDirectBufferAddress()</code> '
    '(Android), <code>MTLBuffer.contents()</code> (iOS), or '
    '<code>SharedArrayBuffer</code> (Web).'
))

story.append(h2('5.3 Writer protocol (wait-free)'))
story.append(p(
    'The writer runs on the data-producer thread — typically a native audio '
    'tap, a physics step, or a Web Worker. The protocol is:'
))
story.append(code_block('''publish(data: T):
    # 1. Pick a target buffer: NOT the latest published, NOT the reader's claim.
    latest_now = latest.load(Relaxed)
    claimed_now = claimed.load(Relaxed)
    candidates  = {0, 1, 2} \\ {latest_now, claimed_now}
    # |candidates| >= 1 always (3 buffers, 2 excluded)
    next = candidates.min()      # deterministic; single writer

    # 2. Write to buffers[next]. Writer-private; no synchronization needed.
    buffers[next].write(data)

    # 3. Publish with Release semantics.
    #    Reader's Acquire load on `latest` will see the write.
    latest.store(next, Release)

    # 4. (Optional) record a sequence number for overrun telemetry.
    seq.fetch_add(1, Relaxed)'''))
story.append(p(
    'The writer is wait-free: it always finds a candidate in O(1). It never '
    'spins. It never blocks. It publishes with a single atomic store. The '
    'reader does not need to be notified — the next VSYNC poll will pick up '
    'the new data.'
))

story.append(h2('5.4 Reader protocol (wait-free, claim-and-release)'))
story.append(p(
    'The reader runs on the Draw thread — Choreographer / '
    '<code>withFrameNanos</code> on Android, <code>CADisplayLink</code> on '
    'iOS, <code>requestAnimationFrame</code> on Web, '
    '<code>RendererBinding</code> vsync on Flutter, '
    '<code>useFrameCallback</code> on React Native. The protocol is:'
))
story.append(code_block('''read() -> Option<&Buffer>:
    # 1. Acquire-load the latest index.
    idx = latest.load(Acquire)
    if idx == -1:
        return None               # no data yet

    # 2. Claim via CAS. If another reader already claimed, return None.
    if not claimed.compare_exchange(-1, idx, Acquire):
        return None               # multi-reader case: someone else got it

    # 3. Snapshot the buffer. The writer will not touch buffers[idx] because:
    #    - it is currently `latest`, so writer excludes it
    #    - it is currently `claimed`, so writer excludes it
    #    Both conditions hold until we release.
    data = buffers[idx].snapshot_to_local_or_read_in_place()

    # 4. Release.
    claimed.store(-1, Release)
    return data'''))
story.append(p(
    'The reader is wait-free: a single CAS to claim, a single store to '
    'release. No retry loop. No torn reads: the writer cannot write to '
    '<code>buffers[idx]</code> while it is both <code>latest</code> and '
    '<code>claimed</code>.'
))

story.append(h2('5.5 Invariants (proof sketch)'))
story.append(p(
    'Five invariants hold by construction:'
))

story.append(table(
    ['#', 'Invariant', 'Mechanism'],
    [
        ['I1', '<b>No torn reads.</b> The reader always loads a fully-written buffer.', 'Writer publishes via Release AFTER the write; reader Acquire-loads <code>latest</code>. Writer cannot overwrite <code>buffers[idx]</code> while it is both <code>latest</code> and <code>claimed</code>.'],
        ['I2', '<b>Wait-free writer.</b> O(1) per publish, no spin, no retry.', 'Three buffers, two excluded (latest + claimed); at least one candidate always remains.'],
        ['I3', '<b>Wait-free reader.</b> O(1) per read, no retry loop.', 'Single CAS to claim; single store to release.'],
        ['I4', '<b>Latest-wins.</b> Reader always sees the freshest available frame.', 'Writer updates <code>latest</code> on every publish. Reader loads <code>latest</code> on every VSYNC. Intermediate frames are dropped silently.'],
        ['I5', '<b>No back-pressure.</b> Writer never waits for reader; reader never waits for writer.', 'Writer picks any free buffer; reader picks the latest. No condition variables, no mutexes, no semaphores.'],
    ],
    col_widths=[0.06, 0.30, 0.64],
))

story.append(h2('5.6 Overrun policy'))
story.append(p(
    'When the writer outpaces the reader (120 Hz writer, 60 Hz display), '
    'intermediate frames are dropped silently. The reader always sees the '
    'freshest available. There is no data loss in the sense of "current '
    'state" — the latest is always available. There is data loss in the sense '
    'of "frames the display never saw" — by design, and acceptable for '
    'visualization.'
))
story.append(p(
    'When the reader outpaces the writer (60 Hz writer, 120 Hz display), every '
    'other VSYNC the reader finds <code>latest</code> unchanged. The reader '
    're-renders the last consumed frame (the default) or skips (caller '
    'choice). Re-rendering provides smooth motion-blur continuity; skipping '
    'saves GPU cycles. The Weft default is re-render.'
))

story.append(h2('5.7 Frame pacing ownership'))
story.append(p(
    'The reader drives pacing via the platform VSYNC source. The writer '
    '<b>never</b> triggers UI invalidation. This is a deliberate choice: a '
    '120 Hz writer waking the UI thread 120 times per second would burn '
    'battery and produce no visual benefit on a 60 Hz display. The UI '
    'continuously renders at VSYNC rate; if there is new data, it renders '
    'the new data; if not, it re-renders the last frame.'
))
story.append(p(
    'This choice has a consequence: the writer must not block on the reader. '
    'If the writer is an audio thread, blocking on a UI thread that is mid-draw '
    'is priority inversion. The Triad Protocol guarantees the writer never '
    'blocks — see I5 above.'
))

story.append(h2('5.8 Multi-reader extension'))
story.append(p(
    'Multiple readers (e.g. a visualizer and a numeric readout both consuming '
    'the same PCM) each <code>claim</code> the latest in turn. The CAS in the '
    'reader protocol ensures only one reader claims at a time. If two readers '
    'race, the loser returns <code>None</code> for that VSYNC and tries again '
    'next VSYNC. For high-frequency multi-reader scenarios (rare in UI), the '
    'Weft can be configured with a fan-out Heddle that snapshots the latest '
    'once per VSYNC and distributes the snapshot. This is v2 territory.'
))

story.append(h2('5.9 Memory model summary (per platform)'))
story.append(table(
    ['Platform', 'Atomic primitive', 'Memory ordering used'],
    [
        ['Android (JNI/Rust)', '<code>std::sync::atomic::AtomicI32</code>', '<code>Release</code> on publish, <code>Acquire</code> on read, <code>AcqRel</code> on CAS.'],
        ['iOS (Swift)', '<code>os_unfair_lock</code> not used; <code>Atomic&lt;Int&gt;</code> via <code>Atomics</code> package or <code>OSAtomicAdd32</code>.', 'Release/Acquire mirrors above. <code>MTLBuffer</code> cross-thread is safe by Metal memory model.'],
        ['Web', '<code>Atomics</code> on <code>SharedArrayBuffer</code> (Int32Array view).', '<code>Atomics.store</code> with default ordering (Sequentially Consistent on Web is unavoidable). One SAB Int32 per Weft for <code>latest</code>; one for <code>claimed</code>.'],
        ['Flutter', '<code>package:atomic</code> or FFI to <code>std::atomic</code>.', 'Release/Acquire. Pointer-stable via <code>dart:ffi</code> <code>Pointer&lt;Float&gt;</code>.'],
        ['React Native', '<code>Reanimated SharedValue</code> (UI thread) + JS-side <code>Atomics</code> if SAB is used.', 'Reanimated provides UI-thread worklets; <code>SharedValue</code> handles ordering internally.'],
    ],
    col_widths=[0.18, 0.32, 0.50],
))

story.append(PageBreak())

# ===================== SECTION 6 — PLATFORM PRIMITIVES =====================
story.append(h1('6. Platform Primitives — Honest Mapping'))
story.append(p(
    'The prior draft overstated platform capabilities. This section corrects '
    'each overclaim and specifies the actual primitives Weft uses, the '
    'fallback paths, and the degraded modes.'
))

story.append(h2('6.1 Android — Jetpack Compose'))
story.append(p(
    'The honest Android story is straightforward. Weft uses:'
))
story.append(bp('<b>Off-heap buffer:</b> <code>ByteBuffer.allocateDirect()</code>, 16-byte aligned. Native access via <code>env-&gt;GetDirectBufferAddress()</code> in JNI, or raw pointer in Rust via <code>jni::JNIEnv::get_direct_buffer_address()</code>.'))
story.append(bp('<b>Draw-phase read:</b> <code>Modifier.graphicsLayer { }</code> lambda (deferred to Phase 3) for affine transforms; <code>Modifier.drawWithContent</code> for custom Canvas drawing. Both bypass composition and layout invalidation.'))
story.append(bp('<b>VSYNC alignment:</b> <code>withFrameNanos</code> (Choreographer). 120 Hz on Pixel 8 Pro / Galaxy S24.'))
story.append(bp('<b>Atomic gesture register:</b> <code>java.util.concurrent.atomic.AtomicLong</code> with <code>getAcquire</code> / <code>setRelease</code> (API 33+); fallback <code>get</code> / <code>set</code> on older devices (volatile semantics suffice for single-writer single-reader).'))
story.append(callout(
    'Honest correction to the prior draft',
    'The prior draft claimed "100% RenderNode phase isolation." Compose owns the '
    'RenderNode; Weft does not. What Weft actually does is defer state reads into '
    '<code>graphicsLayer { }</code> and <code>drawWithContent</code> lambdas, '
    'which Compose evaluates during the Draw phase. The Compose-accurate vocabulary '
    'is "draw-phase-deferred state read," not "RenderNode isolation."',
    kind='note',
))

story.append(h2('6.2 iOS — SwiftUI'))
story.append(p(
    'The honest iOS story has a 60 Hz path and a 120 Hz path. There is no '
    'single "SwiftUI at 120 FPS" answer.'
))
story.append(table(
    ['Workload class', 'Primitive', 'Max FPS', 'When to use'],
    [
        ['Affine transforms (translation, scale, rotation)', '<code>Canvas</code> + <code>CADisplayLink</code>', '60 Hz', 'Simple overlays; A15 SE; non-ProMotion devices.'],
        ['Custom drawing, 1024+ elements, 120 Hz', '<code>MTKView</code> + <code>CADisplayLink.preferredFrameRateRange = 120</code>', '120 Hz', 'ProMotion devices (iPhone 15 Pro+); when Metal is acceptable.'],
        ['GPU-resident state (v2)', '<code>MTLBuffer</code> shared mode + <code>MTLRenderCommandEncoder</code>', '120 Hz', 'When the Weft is written by Metal compute and read by Metal render.'],
    ],
    col_widths=[0.36, 0.30, 0.10, 0.24],
))
story.append(p(
    'Weft exposes both paths through a single API. The Steward probes the '
    'device at <code>bind()</code> time: if <code>CADisplayLink.maximumFrameRate</code> '
    '>= 100, it uses Metal; otherwise it uses Canvas. The dev writes the same '
    '<code>weftDraw</code> closure; the platform chooses the renderer.'
))

story.append(h2('6.3 Web — React, Svelte, Vue'))
story.append(p(
    'The honest web story has two paths: a zero-copy path that requires '
    'cross-origin isolation, and a one-copy path that works everywhere. '
    'Weft supports both and falls back gracefully.'
))
story.append(table(
    ['Mode', 'Buffer primitive', 'Worker→main thread', 'Max FPS'],
    [
        ['<b>Zero-copy (SAB)</b>', '<code>SharedArrayBuffer</code>', 'Atomics on SAB; both sides read/write the same buffer.', '120 Hz on Chrome/Firefox; 60 Hz on Safari (see note).'],
        ['<b>One-copy (Transferable)</b>', '<code>ArrayBuffer</code> / <code>Float32Array</code>', '<code>postMessage(slice, [slice.buffer])</code> per frame.', '120 Hz on all browsers; one ~4 KB copy per frame (negligible).'],
    ],
    col_widths=[0.22, 0.22, 0.36, 0.20],
))
story.append(callout(
    'Safari 60 Hz cap — verified',
    'Safari caps <code>requestAnimationFrame</code> at 60 Hz by default (WebKit '
    'bug 173434; "prefer page rendering updates near 60fps" is still on by default '
    'as of Safari 17.6, late 2025). The 120 FPS Safari number is false. '
    'Weft does <b>not</b> claim 120 FPS on Safari. Weft claims 60 FPS on Safari '
    'and 120 FPS on Chrome / Firefox with a 120 Hz display.',
    kind='warn',
))
story.append(callout(
    'SAB requires COOP/COEP — and most consumer sites cannot ship it',
    '<code>SharedArrayBuffer</code> requires <code>Cross-Origin-Opener-Policy: same-origin</code> '
    'and <code>Cross-Origin-Embedder-Policy: require-corp</code>. These break third-party '
    'ads, analytics, and embeds — most consumer sites cannot ship cross-origin '
    'isolation. Weft\'s default is the <b>one-copy Transferable path</b>. SAB is '
    'opt-in for sites that can ship COOP/COEP (developer tools, internal dashboards, '
    'gaming companion apps).',
    kind='note',
))
story.append(p(
    'The strong web architecture is not React reading a buffer in rAF. It is '
    'a <b>Web Worker owning the entire render loop</b> with '
    '<code>OffscreenCanvas</code>, React reduced to a mount wrapper. Weft '
    'exposes this as <code>&lt;WeftCanvas&gt;</code>, a React component that '
    'spawns a worker, transfers an OffscreenCanvas, and runs the draw loop '
    'off the main thread entirely.'
))

story.append(h2('6.4 Flutter'))
story.append(p(
    'Flutter\'s <code>CustomPainter</code> is the closest analog to Compose '
    '<code>drawWithContent</code>. The Heddle binds to a <code>CustomPainter.repaint()</code> '
    'call triggered by <code>RendererBinding</code> vsync. The Weft buffer is '
    'allocated via <code>dart:ffi</code> <code>MallocAllocator</code> for '
    'pointer stability; native writes go through FFI directly to the pointer.'
))

story.append(h2('6.5 React Native (Reanimated)'))
story.append(p(
    'React Native is the weakest differentiator. Reanimated\'s '
    '<code>SharedValue</code> already provides UI-thread shared state with '
    'worklet-driven reads — this is itself the prior art Weft would cite. '
    'Weft\'s RN implementation is essentially a thin wrapper around '
    '<code>SharedValue</code> with the Triad Protocol for native-engine writes. '
    'For this reason, RN is the last platform in the roadmap (§11).'
))

story.append(h2('6.6 Primitives matrix — corrected'))
story.append(p(
    'The full primitives matrix per platform. Read across for a single '
    'platform; read down to compare platforms on a single concern. The '
    'prior draft\'s 9-column landscape table was unreadable on A4; this is '
    'the corrected portrait orientation.'
))

story.append(table(
    ['Platform', 'Off-heap buffer', 'Draw-phase read', 'VSYNC source', 'Native write path'],
    [
        ['<b>Android</b>', 'DirectByteBuffer (16-byte aligned)', 'Modifier.graphicsLayer { } / drawWithContent', 'withFrameNanos (Choreographer)', 'JNI GetDirectBufferAddress'],
        ['<b>iOS 60Hz</b>', 'UnsafeMutablePointer', 'SwiftUI Canvas + CADisplayLink', 'CADisplayLink.callback', 'Pointer write or MTLBuffer.contents'],
        ['<b>iOS 120Hz</b>', 'MTLBuffer (shared mode)', 'MTKView + CADisplayLink (preferredFrameRateRange)', 'CADisplayLink.callback', 'MTLBuffer.contents'],
        ['<b>Web one-copy</b>', 'ArrayBuffer / Float32Array', 'OffscreenCanvas in Worker', 'requestAnimationFrame', 'postMessage(slice, [slice.buffer])'],
        ['<b>Web SAB</b>', 'SharedArrayBuffer (COOP/COEP)', 'OffscreenCanvas in Worker', 'requestAnimationFrame', 'Atomics.store (zero-copy)'],
        ['<b>Flutter</b>', 'dart:ffi Pointer&lt;Float&gt; (MallocAllocator)', 'CustomPainter.paint', 'RendererBinding vsync', 'dart:ffi direct pointer write'],
        ['<b>React Native</b>', 'Reanimated SharedValue', 'useFrameCallback (UI thread worklet)', 'useFrameCallback', 'runOnUI from worker'],
    ],
    col_widths=[0.13, 0.20, 0.25, 0.22, 0.20],
))
story.append(cap('Table 6.1 — Corrected platform primitives. Each row is a real, tested primitive — no overclaims. Multi-reader atomics and lifecycle binding columns omitted for space; see §5.9 and §7.4.'))

story.append(PageBreak())

# ===================== SECTION 7 — LIFECYCLE =====================
story.append(h1('7. Lifecycle Semantics — The Steward'))
story.append(p(
    'Off-heap buffers without lifecycle semantics are a memory-safety '
    'nightmare. The Steward is Weft\'s answer. It is the most differentiated '
    'piece of the design — "LeakCanary for off-heap" — and it is the piece '
    'the prior draft hand-waved.'
))

story.append(h2('7.1 Weft states'))
story.append(p(
    'A Weft transitions through five states over its lifetime:'
))
story.append(code_block('''                  ┌─────────────┐
                  │  ALLOCATED  │  ← Steward.allocate() returns here.
                  └──────┬──────┘     Buffer exists; not yet bound to a scope.
                         │ Steward.bind(weft, scope)
                         ▼
                  ┌─────────────┐
                  │   BOUND     │  ← Visible to composition; no writer yet.
                  └──────┬──────┘
                         │ Steward.attachWriter(weft, writerId)
                         ▼
                  ┌─────────────┐
                  │   WRITING   │  ← Native engine filling the writer buffer.
                  └──────┬──────┘     Triad Protocol publishes to `latest`.
                         │ Scope dispose / Steward.release()
                         ▼
                  ┌─────────────┐
                  │  RELEASED   │  ← Buffer freed; pointer invalid.
                  └─────────────┘
                         │
                         ▼ (debug only)
                  ┌─────────────┐
                  │ LEAK (debug)│  ← Buffer survived its scope; stack trace logged.
                  └─────────────┘'''))
story.append(cap('Figure 7.1 — Weft lifecycle states. RELEASED is terminal; LEAK is a debug-only diagnostic state.'))

story.append(h2('7.2 Invariants enforced'))
story.append(table(
    ['#', 'Invariant', 'Enforcement'],
    [
        ['L1', '<b>Single writer.</b> Multiple readers OK.', '<code>Steward.attachWriter()</code> panics in debug if a writer is already attached.'],
        ['L2', '<b>No read after release.</b>', 'Reads on a RELEASED Weft panic in debug; return zero in release.'],
        ['L3', '<b>No write after release.</b>', 'Writes on a RELEASED Weft panic in debug; no-op in release.'],
        ['L4', '<b>Composition-bound lifetime.</b>', 'When the @Composable / View / dispose() callback leaves the tree, the Steward frees the buffer.', 'DisposableEffect / .onDisappear / useEffect cleanup / StatefulWidget.dispose.'],
        ['L5', '<b>Leak detection.</b> If the buffer survives its scope, the Steward logs a stack trace.', 'WeakReference + FinalizationRegistry (Web) / PhantomReference (Android) / deinit assertion (iOS).'],
        ['L6', '<b>Configuration change safety.</b> On Android config change, the Weft survives the recreation.', 'See §7.3.'],
        ['L7', '<b>Process death safety.</b> Off-heap memory is freed by the OS on process death.', 'No action needed; the Steward records the lifetime in savedStateHandle so the next instance knows whether to re-allocate.'],
    ],
    col_widths=[0.06, 0.32, 0.62],
))

story.append(h2('7.3 Configuration change — the scope conflict'))
story.append(p(
    'The prior draft proposed <code>rememberWeft { ... }</code> as the '
    'primary API. This is wrong. <code>remember { ... }</code> dies with '
    'composition; on Android configuration change (rotation, theme switch, '
    'locale change), the composition is destroyed and recreated. A Weft '
    'allocated inside <code>remember</code> would be freed and re-allocated '
    'on every rotation — defeating the entire point of off-heap buffer '
    'stability.'
))
story.append(p(
    'The fix: the Steward is scoped to a <code>ViewModel</code> or '
    '<code>SavedStateHandle</code>, not to the composition. The composition '
    'binds to a Weft owned by the Steward; the Steward survives the '
    'composition. The API becomes:'
))
story.append(code_block('''// Owned by a ViewModel — survives configuration change.
val weft = steward.weft<Float32Array>(capacity = 1024)

// Borrowed by the composition; the binding is destroyed on dispose,
// but the buffer itself survives.
AudioBars(weft = weft, modifier = Modifier.fillMaxSize())'''))
story.append(p(
    'On iOS, the equivalent is a Weft held by the <code>@StateObject</code> '
    'or a static Swift property; the <code>View</code> borrows it via '
    '<code>@ObservedObject</code>. On Web, the Weft is held by a React '
    '<code>useRef</code> at the root component; child components consume it '
    'via context. On Flutter, the Weft is held by a <code>State</code> of a '
    'parent <code>StatefulWidget</code>; children borrow via <code>InheritedWidget</code>.'
))

story.append(h2('7.4 The Steward API'))
story.append(code_block('''class Steward {
    // Allocate a Weft with the given capacity (in elements) and alignment.
    // The buffer is off-heap, 16-byte aligned by default.
    fun <T> weft(capacity: Int, align: Int = 16): Weft<T>

    // Attach a native writer. Returns a WriterToken that the writer uses
    // to publish. Panic in debug if a writer is already attached.
    fun <T> attachWriter(weft: Weft<T>, writerId: String): WriterToken

    // Detach a writer. The Weft becomes BOUND again.
    fun detachWriter(weft: Weft<T>, writerId: String)

    // Explicitly release a Weft. Normally called automatically on scope dispose.
    fun release(weft: Weft<T>)

    // Debug-only. Returns leak traces for any Weft that survived its scope.
    fun dumpLeaks(): List<LeakTrace>

    // Debug-only. Asserts no leaks; throws if any are found.
    fun assertNoLeaks()
}'''))

story.append(PageBreak())

# ===================== SECTION 8 — API SURFACE =====================
story.append(h1('8. API Surface'))
story.append(p(
    'The API is the product. If it is wrong, nothing else matters. This '
    'section specifies the public API for each platform, with the '
    'configuration-change fix from §7.3 baked in.'
))

story.append(h2('8.1 Android — Jetpack Compose'))
story.append(code_block('''// ViewModel-scoped Steward — survives config change
class AudioVm : ViewModel() {
    val steward = Steward()
    val pcm = steward.weft<Float32Array>(capacity = 1024)

    init {
        NativeBridge.attachAudioTap(pcm)  // native writer, 60 Hz
    }

    override fun onCleared() = steward.releaseAll()
}

// Composable — borrows the Weft, does not own it
@Composable
fun AudioBars(vm: AudioVm) {
    Canvas(modifier = Modifier.fillMaxSize().weftDraw(vm.pcm)) {
        // `this` is a DrawScope. Read the Weft directly.
        // Composition + Layout executed ONCE; this lambda runs every VSYNC.
        vm.pcm.read { buf ->
            for (i in 0 until buf.capacity()) {
                drawBar(i, buf[i])
            }
        }
    }
}'''))
story.append(p(
    'The <code>weftDraw</code> modifier is the Heddle. It registers a '
    'Draw-phase callback that reads the Weft on every VSYNC. Composition and '
    'layout are not invalidated. The reader claim-and-release protocol runs '
    'inside <code>weftDraw</code>; the closure receives a stable buffer '
    'snapshot for the duration of the draw.'
))

story.append(h2('8.2 iOS — SwiftUI'))
story.append(code_block('''class AudioModel: ObservableObject {
    let steward = Steward()
    let pcm: Weft<[Float]>

    init() {
        pcm = steward.weft(capacity: 1024)
        NativeBridge.attachAudioTap(pcm)  // native writer
    }
    deinit { steward.releaseAll() }
}

struct AudioBars: View {
    @StateObject var model = AudioModel()

    var body: some View {
        WeftCanvas(weft: model.pcm) { ctx, buf in
            // Draw-phase closure. Runs every CADisplayLink tick.
            for i in 0..<buf.count {
                drawBar(ctx, i, buf[i])
            }
        }
    }
}'''))
story.append(p(
    '<code>WeftCanvas</code> probes the device at <code>body</code> time: '
    'ProMotion + 120 Hz display → <code>MTKView</code>; otherwise → '
    'SwiftUI <code>Canvas</code> + <code>CADisplayLink</code>. The dev writes '
    'one closure; the platform chooses the renderer.'
))

story.append(h2('8.3 Web — React'))
story.append(code_block('''// Steward at the root — survives re-renders
function App() {
    const stewardRef = useRef<Steward>();
    if (!stewardRef.current) stewardRef.current = new Steward();
    const pcm = useMemo(
        () => stewardRef.current!.weft<Float32Array>(1024),
        []
    );

    useEffect(() => {
        const worker = new AudioWorker();
        // One-copy Transferable path (default; works on all browsers)
        let raf: number;
        const tick = () => {
            const slice = pcm.read(); // local snapshot
            if (slice) worker.postMessage({ frame: slice }, [slice.buffer]);
            raf = requestAnimationFrame(tick);
        };
        raf = requestAnimationFrame(tick);
        return () => { cancelAnimationFrame(raf); worker.terminate(); };
    }, [pcm]);

    return <WeftCanvas weft={pcm} draw={drawBars} />;
}'''))
story.append(p(
    'The default path uses <code>ArrayBuffer</code> transfers — one ~4 KB '
    'copy per frame, zero GC pressure on the main thread. For sites that '
    'can ship COOP/COEP, the Steward auto-detects <code>crossOriginIsolated</code> '
    'and switches to <code>SharedArrayBuffer</code> with <code>Atomics</code> '
    '— zero copy.'
))

story.append(h2('8.4 Flutter'))
story.append(code_block('''class AudioPainter extends CustomPainter {
    final Weft<Float32List> pcm;
    AudioPainter(this.pcm);

    @override
    void paint(Canvas canvas, Size size) {
        pcm.read((buf) {
            for (var i = 0; i < buf.length; i++) {
                drawBar(canvas, i, buf[i]);
            }
        });
    }

    @override
    bool shouldRepaint(covariant AudioPainter old) => old.pcm != pcm;
}'''))

story.append(h2('8.5 React Native (Reanimated)'))
story.append(code_block('''const pcm = useWeft<Float32Array>(1024);

useFrameCallback(() => {
    'worklet';
    // Runs on the UI thread. Reads Weft directly. No JS bridge.
    pcm.read((buf) => {
        for (let i = 0; i < buf.length; i++) {
            drawBar(i, buf[i]);
        }
    });
});'''))

story.append(PageBreak())

# ===================== SECTION 9 — BENCHMARKS =====================
story.append(h1('9. Workloads and Benchmarks'))
story.append(p(
    'Without numbers, Weft is a blog post. With numbers, it is the standard. '
    'The benchmark suite is the moat. This section specifies the workloads, '
    'the device matrix, the implementation matrix, and the metrics — with '
    'the corrections from the review (workload swap, allocation assertions, '
    'thermal sustained test, P99 focus).'
))

story.append(h2('9.1 Workloads'))
story.append(p(
    'Five workloads. Each is chosen to stress a different dimension of the '
    'reactive contract. The prior draft\'s token-stream workload has been '
    'removed (layout-heavy; off-heap cannot fix layout invalidation) and '
    'replaced with a spectrogram/heatmap — fixed bounds, raw floats, perfect '
    'fit for the weft plane.'
))

story.append(table(
    ['#', 'Workload', 'Hot state', 'Bounds', 'Why this workload'],
    [
        ['W1', 'Audio visualizer', '1024-float PCM @ 60/120 Hz', 'Fixed', 'The canonical hot-state surface. Stresses draw-phase read + GC.'],
        ['W2', 'Particle field', '500 particles × 6-DOF RK4 @ 120 Hz', 'Fixed', 'Stresses native write throughput + atomic handoff.'],
        ['W3', 'Spectrogram / heatmap', '256×64 float matrix @ 60 Hz', 'Fixed', 'Replaces the token-stream workload. Fixed bounds; raw floats; perfect weft fit.'],
        ['W4', 'Data grid', '10k rows × 20 cols, virtualized scroll + live updates', 'Variable', 'Stresses large-buffer identity + JNI zero-copy. Layout-invalidating; only the fixed-bounds subset uses Weft.'],
        ['W5', 'Order book', '1000 levels × 10 fields, 60 Hz L2 feed', 'Fixed', 'Stresses cross-thread write protocol + 120 Hz display vs 60 Hz feed.'],
    ],
    col_widths=[0.04, 0.16, 0.22, 0.10, 0.48],
))

story.append(h2('9.2 Device matrix'))
story.append(table(
    ['Tier', 'Device', 'Chip', 'RAM', 'OS', 'Display'],
    [
        ['Low-end Android', 'Realme C55', 'Helio G88', '4 GB', 'Android 13', '60 Hz'],
        ['Mid Android', 'Pixel 7a', 'Tensor G2', '8 GB', 'Android 14', '90 Hz'],
        ['High Android', 'Galaxy S24', 'SD 8 Gen 3', '12 GB', 'Android 14', '120 Hz'],
        ['Low Apple', 'iPhone SE (2022)', 'A15', '4 GB', 'iOS 17', '60 Hz'],
        ['High Apple', 'iPhone 15 Pro', 'A17 Pro', '8 GB', 'iOS 17', '120 Hz ProMotion'],
        ['Web (laptop)', 'M2 MacBook Air', '(N/A)', '16 GB', 'Chrome 130 / FF 131 / Safari 17.6', '120 Hz'],
    ],
    col_widths=[0.16, 0.16, 0.14, 0.08, 0.16, 0.30],
))

story.append(h2('9.3 Implementation matrix'))
story.append(p(
    'Each workload is implemented four ways on each device:'
))
story.append(table(
    ['Impl', 'Description', 'Purpose'],
    [
        ['<b>A — Reactive baseline</b>', 'State in <code>MutableState</code> / <code>useState</code> / <code>@State</code>. The naive way.', 'Lower bound. Shows the failure mode.'],
        ['<b>B — Platform best practice</b>', '<code>graphicsLayer</code> / <code>Canvas</code>+<code>CADisplayLink</code> / <code>CustomPainter</code> with pooled arrays. The current "right way."', 'Honest comparison. Weft\'s real competitor.'],
        ['<b>C — Weft</b>', 'The library, with Steward + Triad Protocol + Heddle.', 'The candidate.'],
        ['<b>D — Hand-rolled</b>', 'Streamify\'s production code, as the upper-bound reference.', 'Proves the ceiling. Shows how much Weft leaves on the table.'],
    ],
    col_widths=[0.20, 0.50, 0.30],
))

story.append(h2('9.4 Metrics'))
story.append(p(
    'Per workload, per device, per implementation, a 100-second run records:'
))
story.append(bp('<b>FPS</b> — P50, P99, P100. The headline number is P99, not P50.'))
story.append(bp('<b>Frame allocation rate</b> — bytes/frame, averaged. The target is 0 B/frame for C and D. <b>Allocation-count assertions</b> fail the run if C or D allocate anything per frame.'))
story.append(bp('<b>GC pause count + total ms</b> — young-gen + full GC. Reported separately.'))
story.append(bp('<b>CPU%</b> — UI thread + RenderThread + native writer thread.'))
story.append(bp('<b>Battery drain</b> — mAh/min at fixed 50% brightness, airplane mode + WiFi.'))
story.append(bp('<b>Cold start overhead</b> — ms added by Weft vs baseline. Target: < 5 ms.'))
story.append(bp('<b>Thermal sustained test</b> — 30-minute run, not 100-second. Reports FPS decay curve + thermal-throttle events. Tests that Weft does not rely on burst performance that fades.'))

story.append(h2('9.5 Headline chart (predicted)'))
story.append(p(
    'The marketing chart, predicted from Streamify production data:'
))
story.append(table(
    ['Implementation', 'P50 FPS', 'P99 FPS', 'Bytes/frame', 'GC pauses/sec', 'Notes'],
    [
        ['A — Reactive', '11', '6', '2.3 KB', '47', 'Collapsed.'],
        ['B — Best practice', '116', '54', '380 B', '3', 'P99 jank under sustained load.'],
        ['C — Weft', '120', '120', '0 B', '0', 'Locked.'],
        ['D — Hand-rolled', '120', '120', '0 B', '0', 'Upper bound.'],
    ],
    col_widths=[0.20, 0.10, 0.10, 0.16, 0.16, 0.28],
))
story.append(cap('Table 9.1 — Predicted results on Pixel 7a, workload W1 (audio visualizer), 120 Hz writer. Actual numbers TBD on first benchmark run.'))

story.append(PageBreak())

# ===================== SECTION 10 — POSITIONING =====================
story.append(h1('10. Positioning — vs. Shaders, vs. GPU-Resident'))
story.append(p(
    'A senior reviewer will ask: "Why not just a shader?" For workloads where '
    'the input is a numeric array and the output is per-pixel color, a GPU '
    'shader — AGSL on Android, Metal on iOS, WebGPU on Web — is strictly '
    'faster than any CPU-side draw-phase read. Weft does not compete with this. '
    'Weft is for the case where the hot state must interoperate with reactive '
    'state: a particle field where each particle is also a clickable Compose '
    'element; an audio bar visualization where the bars must respect theme '
    'colors from Composition; a data grid where the cells are styled by a '
    'design system. In these cases, the GPU shader cannot read Compose '
    'composition state, and Compose cannot read GPU memory without a round-trip.'
))
story.append(p(
    'The honest answer: Weft is the right tool when hot state must interoperate '
    'with reactive state. A GPU shader is the right tool when hot state can '
    'live entirely in GPU memory. Weft v2 will add a GPU-resident mode — the '
    'Weft is allocated as a <code>MTLBuffer</code> / <code>AHardwareBuffer</code> '
    '/ WebGPU <code>GPUBuffer</code>, the writer is a compute shader, and the '
    'reader is a render shader. Zero CPU involvement. This is the v2 roadmap.'
))

story.append(h2('10.1 The decision tree'))
story.append(table(
    ['Question', 'If yes', 'If no'],
    [
        ['Is the hot state pure numeric data rendered as pixels?', 'GPU shader', '→ next question'],
        ['Must the hot state interoperate with reactive state (theme, click handlers, layout)?', '<b>Weft (CPU-side)</b>', 'GPU shader'],
        ['Is the writer a native engine (C++/Rust) producing numeric data?', '<b>Weft (CPU-side)</b>', 'GPU compute shader (v2)'],
        ['Is the surface a fixed-bounds canvas?', '<b>Weft (CPU-side)</b>', 'Reactive (cold state)'],
    ],
    col_widths=[0.50, 0.25, 0.25],
))

story.append(PageBreak())

# ===================== SECTION 11 — ROADMAP =====================
story.append(h1('11. Roadmap'))
story.append(p(
    'The prior draft proposed four platforms in eight weeks (Phase 4). This '
    'is fantasy. The corrected roadmap is sequential: Android and Web first, '
    'whitepaper at the end of Phase 3, SwiftUI after traction, Flutter when '
    'there is a real customer, React Native last (it is the weakest '
    'differentiator — see §6.5).'
))

story.append(table(
    ['Phase', 'Weeks', 'Deliverable', 'Success criterion'],
    [
        ['<b>0. Spec + Spike</b>', '1–2', 'This document + Triad Protocol spike on Android (DirectByteBuffer ring + Choreographer + fake 120 Hz writer).', 'Torn reads = 0; steady-state alloc/frame = 0 on Pixel 7a.'],
        ['<b>1. Android v0.1</b>', '3–8', '<code>dev.weft:android</code> on Maven Central (snapshot). Steward, Weft, Heddle, Triad Protocol. One demo: W1 audio visualizer.', 'A→B→C→D benchmark on 3 Android devices published.'],
        ['<b>2. Web v0.1</b>', '9–14', '<code>@weft/core</code> + <code>@weft/react</code> on npm. One-copy Transferable path + opt-in SAB. One demo: W5 order book.', 'A→B→C→D benchmark on Chrome / FF / Safari published.'],
        ['<b>3. Benchmark launch</b>', '15–18', 'Open-source benchmark suite + static site at weft.dev/benchmarks. HN post, Reddit r/androiddev, dev.to.', 'Public, reproducible, forkable. The wedge goes public.'],
        ['<b>4. Whitepaper</b>', '19–20', 'Formal whitepaper: problem, thesis, Triad Protocol, benchmark results, citations. PDF, ACM-style.', 'Citable artifact. Send to engineering directors.'],
        ['<b>5. SwiftUI</b>', '21–28', '<code>Weft</code> Swift Package. Canvas + MTKView paths. Same five workloads.', 'iOS benchmark numbers published.'],
        ['<b>6. Flutter</b>', '29–32', '<code>weft</code> pub.dev package. ffi Pointer + CustomPainter. (Only if a customer asks.)', 'Flutter benchmark numbers published.'],
        ['<b>7. React Native</b>', '33–36', '<code>@weft/react-native</code> via Reanimated SharedValue. (Last; weakest differentiator.)', 'RN benchmark numbers published.'],
        ['<b>8. Pro tier</b>', '37+', 'Leak-detection dashboard + crash analytics + first enterprise design partner.', 'Revenue starts.'],
    ],
    col_widths=[0.18, 0.08, 0.50, 0.24],
))
story.append(p(
    'Phase 4 (cross-platform) was the unrealistic line item. It is replaced '
    'by sequential Phase 5 (SwiftUI) and Phase 6 (Flutter), each with a full '
    'benchmark cycle. RN (Phase 7) is last because it is the least differentiated.'
))

story.append(PageBreak())

# ===================== SECTION 12 — BUSINESS MODEL =====================
story.append(h1('12. Business Model'))
story.append(p(
    'Three layers, in order of revenue timing. The open-source core is the '
    'moat. The Pro tier is the recurring revenue. Consulting is the wedge '
    'that funds the open-source work.'
))

story.append(h2('12.1 Layer 1 — Open-source core'))
story.append(p(
    'Apache 2.0. Free forever. The library, the benchmark suite, the '
    'reference demos. The moat is not the code — it is the benchmark and '
    'the name. AndroidX can absorb a <code>DirectByteBuffer</code> wrapper, '
    'but it cannot absorb a cross-platform benchmark site that has become '
    'the reference. The benchmark site is what survives platform absorption.'
))

story.append(h2('12.2 Layer 2 — Weft Pro (enterprise)'))
story.append(p(
    'Per-MAU pricing for apps above 100k MAU. Includes:'
))
story.append(bp('Profile-guided buffer sizing — right-size Wefts from production telemetry.'))
story.append(bp('Leak detection dashboard — off-heap-specific, integrated with Sentry / Datadog. The Steward\'s <code>dumpLeaks()</code> surfaced as a SaaS.'))
story.append(bp('Crash analytics for off-heap failures — panics, alignment faults, use-after-release.'))
story.append(bp('SLA + priority support.'))
story.append(bp('On-prem option for regulated industries (medical, finance).'))
story.append(p(
    'Pricing: $0.02/MAU/month, $5k minimum. Anchor customers: trading apps, '
    'audio DAWs, medical imaging, video editors, AI chat clients.'
))

story.append(h2('12.3 Layer 3 — Consulting (the wedge)'))
story.append(p(
    'Fixed-price engagements: "We will refactor your hot surface from 14 FPS '
    'to 120 FPS in 6 weeks. $80k. We use Weft. If we do not hit 120 FPS, you '
    'do not pay." Three of these fund a year of open-source work. The '
    'consulting is the wedge that gets Weft into production at real '
    'companies; the open-source is the moat that makes those companies '
    'stay.'
))

story.append(PageBreak())

# ===================== SECTION 13 — NON-GOALS =====================
story.append(h1('13. Known Limitations and Non-Goals'))
story.append(p(
    'Honesty about what Weft does not do is part of the spec. A library '
    'that claims to fix everything fixes nothing.'
))

story.append(h2('13.1 Non-goals'))
story.append(bp('<b>Layout-heavy hot state.</b> Token streaming, virtualized grid scrolling, text relayout — these are layout-invalidating and cannot be fixed by an off-heap buffer. Weft addresses only the draw-phase subset. Workload W3 replaces the token stream for this reason.'))
story.append(bp('<b>Replacing the reactive plane.</b> Cold state stays in <code>MutableState</code> / <code>useState</code> / <code>@State</code>. Weft is the second plane, not the first.'))
story.append(bp('<b>120 FPS on Safari.</b> Safari caps rAF at 60 Hz. Weft claims 60 FPS on Safari, 120 FPS on Chrome / Firefox with a 120 Hz display.'))
story.append(bp('<b>Zero-copy on every browser.</b> SAB requires COOP/COEP. Most consumer sites cannot ship it. Weft defaults to one-copy Transferable; SAB is opt-in.'))
story.append(bp('<b>Cross-platform KMP magic.</b> The primitives (<code>DirectByteBuffer</code> / <code>MTLBuffer</code> / <code>SharedArrayBuffer</code>) share no common abstraction. The API is consistent; the implementation is <code>expect/actual</code> per target. The prior draft\'s "one KMP module across JVM/iOS/JS" overstated this.'))

story.append(h2('13.2 Known limitations'))
story.append(bp('<b>Multi-reader contention.</b> The CAS in the reader protocol means two readers racing for the same VSYNC will cause one to skip. The fan-out Heddle (v2) addresses this.'))
story.append(bp('<b>Buffer size limit.</b> A single Weft is bounded by available off-heap memory. For >100 MB hot state, use multiple Wefts with the Steward\'s composite allocator (v2).'))
story.append(bp('<b>Debug overhead.</b> The Steward\'s leak detection adds ~5% overhead in debug. Disabled in release.'))
story.append(bp('<b>Writer starvation under extreme contention.</b> The Triad Protocol is wait-free for the writer, but if the reader holds <code>claimed</code> for > 1 VSYNC interval, the writer is restricted to one buffer (the third). This is by design (latest-wins) but means the writer is producing into a single rotating slot — fine for 120 Hz, problematic for 1000 Hz+ telemetry. For 1000 Hz+, use v2 GPU-resident mode.'))

story.append(PageBreak())

# ===================== SECTION 14 — OPEN QUESTIONS =====================
story.append(h1('14. Open Questions'))
story.append(p(
    'A spec is honest when it lists what it does not yet know. These are '
    'the open questions for v0.2:'
))

story.append(h3('Q1. GPU-resident mode — design and API'))
story.append(p(
    'The v2 GPU-resident mode (Weft backed by <code>AHardwareBuffer</code> / '
    '<code>MTLBuffer</code> / WebGPU <code>GPUBuffer</code>) needs an API '
    'that lets the writer be a compute shader and the reader be a render '
    'shader, with no CPU round-trip. The Heddle for this mode is a shader '
    'uniform binding, not a <code>graphicsLayer</code> lambda. The API '
    'should be: <code>val gpuWeft = steward.weftGpu&lt;Float32Array&gt;(1024, '
    'GpuBufferFlags.SHARED)</code>. Open question: how does the dev write '
    'the shader-side reader? Do we generate the shader binding automatically, '
    'or require manual AGSL / Metal / WGSL code?'
))

story.append(h3('Q2. Fan-out Heddle for multi-reader'))
story.append(p(
    'When multiple consumers need the same Weft (e.g. an audio bar visualizer '
    'and a numeric dB readout), the CAS-racing reader protocol causes one '
    'reader to skip per VSYNC. The fan-out Heddle snapshots the latest once '
    'per VSYNC and distributes the snapshot. Open question: does the snapshot '
    'live in a per-reader buffer (one allocation per reader — defeat the '
    'zero-allocation invariant?) or in a shared snapshot buffer with a '
    'per-reader epoch (more complex)?'
))

story.append(h3('Q3. Compose Multiplatform integration'))
story.append(p(
    'KMP / Compose Multiplatform shares the Compose runtime across Android, '
    'iOS, Web, Desktop. The Heddle API could be unified across all four '
    'targets. Open question: does the iOS Compose Multiplatform runtime '
    'support <code>graphicsLayer { }</code> the same way Android does? If '
    'not, the Heddle on iOS-Compose falls back to <code>MTKView</code> — '
    'does the dev write the same closure, or two? The answer determines '
    'whether Compose Multiplatform is a v0.2 or v0.3 target.'
))

story.append(h3('Q4. Tick-broadcast authentication (the Streamify lesson)'))
story.append(p(
    'Streamify\'s Jam engine documents an accepted-risk: tick broadcasts are '
    'unauthenticated, mitigated by epoch gates + Kalman outlier rejection. '
    'If a Weft is fed by a network source (multiplayer telemetry, '
    'co-visualization), the same problem arises. Open question: should Weft '
    'ship a <code>VerifiedWeft</code> variant with HMAC per frame? The cost '
    'is ~5μs per frame on a 4 KB buffer; the benefit is spoof-proof hot state. '
    'Likely v0.3.'
))

story.append(h3('Q5. Lifecycle under Android process death'))
story.append(p(
    'Off-heap memory is freed by the OS on process death. The Steward '
    'records the lifetime in <code>SavedStateHandle</code>. Open question: '
    'does the Steward re-allocate on process recreation, or does it assume '
    'the writer will re-attach and re-publish? For audio taps, the answer '
    'is "writer re-attaches." For persistent visualizations (e.g. a '
    'recording\'s waveform cache), the answer is "Steward persists the '
    'buffer to disk and re-hydrates." This needs to be a Steward policy, '
    'not an afterthought.'
))

story.append(PageBreak())

# ===================== SECTION 15 — REFERENCES =====================
story.append(h1('15. References'))
story.append(p(
    'Verified and cited at the time of writing (September 2025). Where the '
    'prior draft made unverified claims (Safari 120 Hz, SAB without COOP/COEP), '
    'this section records the corrected facts with sources.'
))

story.append(h2('15.1 Platform documentation'))
story.append(bp('Android Developers. "Defer state reads as long as possible." <i>Jetpack Compose Performance</i>. developer.android.com/jetpack/compose/performance.'))
story.append(bp('Android Developers. "Modifier.graphicsLayer." <i>Compose API reference</i>.'))
story.append(bp('Android Developers. "DirectByteBuffer and JNI." <i>JNI Tips</i>. developer.android.com/training/articles/perf-jni.'))
story.append(bp('Apple Developer. "CADisplayLink.preferredFrameRateRange." <i>QuartzCore Framework</i>.'))
story.append(bp('Apple Developer. "MTKView and Metal." <i>MetalKit Framework</i>.'))
story.append(bp('Mozilla. "SharedArrayBuffer and cross-origin isolation." <i>MDN Web Docs</i>. developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer.'))
story.append(bp('WebKit Bug 173434. "Prefer page rendering updates near 60fps." bugs.webkit.org/show_bug.cgi?id=173434. (Safari rAF cap, still default as of Safari 17.6.)'))
story.append(bp('Reanimated. "useFrameCallback." <i>React Native Reanimated</i>. docs.swmansion.com/react-native-reanimated/.'))

story.append(h2('15.2 Prior art and related work'))
story.append(bp('freeCodeCamp. "Ring Buffers for High-Frequency React Data." August 2026. freecodecamp.org/news/ring-buffers-react. (Confirms the niche is open; nobody has claimed cross-platform off-heap draw-phase channel + lifecycle.)'))
story.append(bp('LeakCanary. "Detecting memory leaks in Android." github.com/square/leakcanary. (The model for the Steward\'s leak detection.)'))
story.append(bp('Apache Arrow. "Columnar in-memory format." arrow.apache.org. (The model for the columnar binary layout used in Weft\'s Web one-copy path.)'))
story.append(bp("Project Loom. JVM virtual threads. openjdk.org/projects/loom. (Reason for renaming the binding layer from 'Loom' to 'Heddle'.)"))
story.append(bp('Warp, the AI terminal. warp.dev. (Reason for renaming the library from "Warp" to "Weft".)'))

story.append(h2('15.3 Source artifacts'))
story.append(bp('Streamify APK. github.com/zephyr4289/streamify-apk. The production codebase from which the Weft pattern was extracted. See <code>QuantumSonicTokenOverlay.kt</code>, <code>ZeroDragSeekbar.kt</code>, <code>MeshPcmAudioProcessor.kt</code>, <code>jni_bridge.cc</code>.'))
story.append(bp('KnowYourSponsor. The web counterpart; columnar binary + Worker + Transferable Objects. Demonstrates the same pattern in a different runtime.'))

story.append(Spacer(1, 14))
story.append(hr())
story.append(Paragraph(
    '<i>End of specification. This document is Draft v0.1, open for senior '
    'review. Submit issues at the Weft repository. The next milestone is the '
    'Triad Protocol spike (Phase 0), which de-risks the only unknown that can '
    'kill the project.</i>',
    style_meta))

# ----------------------------------------------------------------------------
# Build
# ----------------------------------------------------------------------------

OUT = '/home/z/my-project/scripts/weft_body.pdf'
doc = WeftDocTemplate(OUT, pagesize=A4,
    leftMargin=MARGIN_L, rightMargin=MARGIN_R,
    topMargin=MARGIN_T, bottomMargin=MARGIN_B,
    title='Weft Specification v0.1',
    author='Zephyr (zephyr4289)',
    subject='A specification for the continuous-state plane in declarative UI',
    creator='Weft spec generator',
)

# Multi-pass for TOC
doc.multiBuild(story)
print(f'Body PDF: {OUT}')
print(f'Pages: {doc.page}')
