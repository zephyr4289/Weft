# WEFT — Premium Product Page: Master Delivery Plan

**Goal:** Design and ship a state-of-the-art, single-repository product site for `zephyr4289/Weft`, hosted directly on GitHub Pages. The site must feel like a top-tier systems product (Linear × Vercel × Apple Silicon page × Stripe Docs) while being *honest* to Weft's own culture: every claim backed by evidence, every number labeled with its boundary.

**Constraint:** Pure static hosting (GitHub Pages, no server). Everything must build in CI and deploy from a branch or `docs/` folder.

---

## 1. Positioning & Narrative Arc

The page tells one story in five beats (matching the scroll journey):

1. **The Problem — "Hot State Breaks Reactive UI."** Open with a visceral demo: a 60–120 Hz data stream (audio waveform / order book ticks) rendered two ways side-by-side — a simulated janky GC-bound reactive path vs. the smooth Weft path. Emotionally: *your framework was never designed for this.*
2. **The Thesis — "Two planes, one surface."** Cold state stays reactive; continuous state flows through an off-heap, zero-copy, draw-phase-read channel. Introduce the Four Laws here as a manifesto strip.
3. **The Mechanism — "The Triad Protocol."** Interactive explainer of 3 buffers + 1 atomic (`latest.exchange(AcqRel)`), wait-free both directions, latest-wins, no torn reads. This is the technical centerpiece.
4. **The Proof — "Honesty as a feature."** Litmus catalog, TLA+ formal models, extreme CI (~40 shards), alloc==0 assertions, perf-regression gate, public withdrawals (RFC-0001/F1). Weft's differentiator isn't just speed — it's *verifiable* speed.
5. **The Reach — "One kernel, eight targets."** C/Rust/TS/Kotlin/Swift/Dart/WASM/Python ports + heddle bindings for React/Vue/Svelte/Compose/SwiftUI/Flutter. Install snippets per platform.
6. **The Studio — "See the machine."** Weft Studio (Pillar 7): the Android-Studio-grade visual suite and live profiler — the tool that makes the invisible protocol visible. This beat closes the narrative: *the product even ships the X-ray machine.* (Detail in §4A.)

**Narrative placement note:** Studio is not a footnote — it is the emotional payoff of the "Proof" act. Sections 3–4 prove the mechanism is correct; Studio proves developers can *see and feel* it running at 240 FPS.

**Voice:** Confident, precise, engineering-literate. No hype without a citation. Copy pattern: *claim → mechanism → evidence link*.

---

## 2. Information Architecture (Sitemap)

```
/                       Landing / product page (long-scroll, sections below)
/docs                   Docs hub (card grid → deep pages)
  /docs/getting-started Install per platform (npm, SPM, Gradle, pub, pip, cargo)
  /docs/concepts        Two-plane model, hot vs cold state, Steward lifecycle
  /docs/triad           Triad Protocol deep-dive (the whitepaper §3, web-native)
  /docs/four-laws       Laws with boundary conditions
  /docs/api/*           Generated API reference (core + each heddle package)
  /docs/ports           Port matrix + parity status (from docs/PORTS.md)
  /docs/formal          TLA+ models, litmus catalog L1–L16, memory-ordering matrix
  /docs/ci              Extreme-testing CI explained, shard map, baselines
  /docs/benchmarks      Honest benchmark tables w/ environment labels
  /docs/studio          Weft Studio hub: overview, panels guide, .weft schema language,
                        weft-lsp editor setup, .weftrec flight-record format, Tauri desktop
  /docs/errata          ERRATA + RFC history incl. withdrawn designs
/studio                   Dedicated Weft Studio product-within-product page (see §4A)
/blog                     Changelog-style posts (release notes, phase reports)
/playground             In-browser WASM triad demo (stretch goal, Phase 4)
/evidence               D-numbered evidence index (links into repo files via permalinks)
```

Landing-page sections: Hero → Live Demo Strip → Problem → Four Laws → Triad Explainer → Proof/Lab (litmus+formal+CI counters) → **Studio Showcase (interstitial band linking to /studio)** → Ports Matrix → Packages/Heddles → Benchmarks teaser → Testimonial-style quote wall (whitepaper excerpts) → Docs teaser → Security/honesty note → Footer (governance, SHA256SUMS, minisign key mention).

---

## 3. Visual Design System ("Loom Noir")

### 3.1 Theme concept
Weft = weaving. The visual language is a **dark "loom at night"** aesthetic: near-black indigo base, thin luminous warp/weft thread lines animating subtly across section boundaries, amber/cyan accent pair (warp=warm, weft=cool). One light theme variant for docs reading mode. Signature motif: an animated SVG thread interlacing that reacts to scroll position.

### 3.2 Typography (premium, web-safe licensing)
| Role | Face | Fallback stack | Notes |
|---|---|---|---|
| Display / H1-H2 | **Clash Display** or **Instrument Serif** (OFL/free tier) | system-ui | Tight tracking, optical sizing |
| Body | **General Sans** or **Inter var** | -apple-system | variable weight axis |
| Mono / code | **JetBrains Mono var** or **Berkeley Mono-like: Fragment Mono** | ui-monospace | ligatures on for code only |
| Numbers/stats | tabular-nums everywhere; display serif for big stats | | |

Self-host all fonts (woff2 subset, `unicode-range`, `font-display: swap`, preload the 3 critical faces). Total font budget ≤ ~250 KB.

### 3.3 Type scale & rhythm
Fluid `clamp()` scale (1.25 major third for marketing, 1.2 for docs), 8-pt spacing grid, max body measure 68ch, generous leading (1.7 marketing / 1.6 docs). Section headers use a numbered "§" system echoing the whitepaper (§2.2 Four Laws etc.) — reinforces the document-as-product identity.

### 3.4 Color tokens
```
--ink-0 #07080F   base bg     --ink-1 #0E1120   panels    --ink-2 #171B30   cards
--thread-warp #F5A623 (amber) --thread-weft #4CC2FF (cyan) --ok #3DDC97 --warn #FFB84C --bad #FF6B6B
--text-hi #EDF0F7  --text-mid #9AA3B8  --text-low #5C6478
Light docs theme: paper #FAFAF7, ink #14161F, same accents darkened for AA contrast.
```
All text/accent pairs validated ≥ WCAG AA (4.5:1 body, 3:1 large/UI).

### 3.5 Motion language
- Scroll-reveal: 20px rise + fade, staggered 60ms, `prefers-reduced-motion` kills everything non-essential.
- Signature animation: the loom threads draw-on with `stroke-dashoffset` tied to IntersectionObserver progress.
- Micro-interactions: copy-buttons, tab underline spring (200ms cubic-bezier(0.2,0,0,1)), hover lift on cards (translateY(-2px)+shadow).
- Budget: total JS < 120 KB gzipped on landing; CSS-only animations wherever possible; canvas/WebGL used *only* for the hero demo.

---

## 4. Key Interactive Components (the "wow" inventory)

1. **Hero live demo — "Two planes, one frame":** real-time canvas rendering an audio-style waveform + order-book ticker at rAF rate. Toggle button switches the same data through (a) a simulated setState-per-frame path with visible dropped frames/GC spikes vs (b) a triad-backed path staying pinned at 60fps. FPS counter + allocation counter overlay. Built with the actual TS triad implementation from `core/ts` compiled into the page — dogfooding the product.
2. **Triad Protocol interactive diagram:** step-through animation of publish/claim ownership exchange across 3 buffer slots and the `latest` atomic; user can scrub timeline, trigger "reader mid-write" and see why no torn read occurs; toggle relaxed vs acquire-release ordering to visualize the F1 finding (why two-atomic design was unsound).
3. **Litmus lab widget:** list of L1–L16 tests; clicking shows the test intent, the invariant checked, and a replayable pass trace; "exposure retry" explained visually.
4. **Ports matrix:** filterable table/grid (language × platform × parity status × package name) generated from `docs/PORTS.md`.
5. **Install snippet tabs:** npm / pnpm / yarn / SPM / Gradle / pub / cargo / pip with syntax highlighting + copy.
6. **Code examples with run-in-place output panes** (pre-rendered outputs; playground extends later with WASM).
7. **Benchmark honesty cards:** each number carries an environment chip (`x86_64-sandbox · protocol-proof`) exactly matching the repo's honest-label convention.
8. **Evidence links:** every claim footnoted → deep-links to permalink'd files in the GitHub repo (`/blob/<sha>/path#L10-L42`) so claims are auditable forever.

---

## 4A. Weft Studio — The Product-Within-a-Product (`/studio`)

**What it is (verified from repo):** `@weft/studio` (Pillar 7, "Android Studio-grade visual suite for the Weft zero-copy ecosystem"), a zero-dependency engine + panels built with React and dogfooding `@weft/react-heddle` at its own 240 FPS hot plane. Governed by `docs/studio/STUDIO-SEAMS-V1.md` (normative) and audited by D-71/D-72/D-73 reports. Ships as web app + **Tauri desktop shell** (`packages/studio/desktop/tauri`, Webview2). Target gates: sub-15 MB bundle, 120 FPS interactive memory canvas.

**Why it matters to the page:** Studio is the *visual proof* of the whole thesis — live seqlock/slot inspection, flight-recorder time travel, render-spy enforcement of Law 4. It is the most screenshot-able, demo-able asset in the repo and deserves its own premium page, not a paragraph.

### Page structure for `/studio`
1. **Hero:** full-bleed mock of the Studio dark UI (chromeless window frame, tool windows glowing), headline *"See the machine."* CTA chips: Launch web build · Download Tauri desktop (per-OS).
2. **The Two-Plane Law (STUI1) as design story:** an animated split showing COLD panel (setState allowed) vs HOT panel (rAF + refs, setState FORBIDDEN) — the studio itself obeys the laws it profiles. Copy hook: *"The profiler is the best test client."*
3. **Panel-by-panel showcase** (sticky-scroll feature walk, one viewport per tool):
   - **Live Seqlock & Atomic Monitor / Ring Monitor:** the 1,000,000-slot × 32 B memory map rendered as a breathing heatmap; slot states FREE < WRITING < COMMITTED < READ + DROPPED color-coded; hover a slot → decode seq/writer_id/msg_type/ts_ns/payload exactly per the documented layout. On the site this is a **simulated but schema-faithful** recreation (seeded from `packages/studio/src/engine/ring.ts` + `sim.ts`); label it honestly as a visualization, not a live capture.
   - **Flight Recorder Time-Travel Debugger:** scrubber stepping forward/backward through `.weftrec`/SREC frames (O(log n) seek, CRC-32C verified); microsecond timestamps; deterministic replay shown as an animatable timeline strip.
   - **Render Spy:** live counter proving "exactly 1 render per mount under a 10,000-frame burst" — presented as an interactive gauge; ties directly to Law 4.
   - **Live Cache-Line Inspector:** 64 B/128 B line grid with false-sharing and crossing tripwires (diagnostics 2101–2104), fed by the frozen-C-ABI layout engine (`weft_field_layout_t`, 40+ `_Static_assert` pins).
   - **Schema editor + `weft-lsp`:** code pane with real-looking `.weft` schema, hover/completion/diagnostics mock; note the WASM `weftc` compiler enables zero-install in-browser compilation — candidate for a genuine mini-playground on the site.
   - **Codegen:** 7-language struct/code previews from one schema (tabs mirroring the port matrix).
4. **Governed cadence section:** 240 Hz virtual cadence, drop-not-queue semantics visualized as a metronome that skips late ticks; p99 ≤ 4.166 ms budget badge.
5. **Architecture seams diagram:** Studio UI ↔ managed engine ↔ native inspector/WASM `weftc` consumed strictly through documented seams (§5/§6 of STUDIO-SEAMS-V1) — reinforces the engineering-maturity story.
6. **Audit wall:** D-71/D-72/D-73 cards (gates G1–G6 green, dual gcc 14.2/clang 19.1.7 strict, malloc-interposition proof: 0 allocations over 200,000 cycles, ASan/UBSan clean) with evidence deep-links.
7. **Dogfooding callout:** "Built with @weft/react-heddle — the tool that profiles the runtime, runs on the runtime."

### Landing-page integration
- Dedicated **"Studio Showcase" interstitial band** after the Proof section: cinematic auto-playing (reduced-motion-safe) loop of the Ring Monitor heatmap + time-travel scrub, one headline, one CTA → `/studio`.
- Nav gets a top-level **Studio** entry alongside Docs/Benchmarks.
- Hero secondary CTA option: "Open Studio ↗".

### Extra components to build for §4
9. **Studio chrome kit:** reusable fake-window/frame component (title bar, tool-window tabs, status bar) so all Studio visuals share one premium look; keyboard-accessible tab switching.
10. **Slot decoder widget:** clickable 32 B slot grid → field-by-field hex decode (schema-faithful to STUDIO-SEAMS-V1 §2).
11. **Time-travel scrubber demo:** pre-recorded synthetic SREC trace (generated in build step via `engine/sim.ts` logic ported to a Node script) replayed deterministically — no server needed.

### Content sources (build-time ingestion)
`docs/studio/STUDIO-SEAMS-V1.md`, `docs/reports/D-71-STUDIO-CORE-AUDIT.md`, `D-72-STUDIO-NATIVE-AUDIT.md`, `D-73-STUDIO-MANAGED-AUDIT.md`, `docs/pillars/NEXT-GEN-PILLARS-5-8.md` (Pillar 7 spec), `packages/studio/src/**` (engine types, slot layout constants), `core/c/include/weft_studio.h` (frozen ABI tables).

---

## 5. Content Strategy (repo → web pipeline)

- **Source of truth stays in repo.** A build step ingests: `docs/WHITEPAPER.md`, `PHILOSOPHY.md`, `GLOSSARY.md`, `ERRATA.md`, `PORTS.md`, `ROADMAP.md`, RELEASE-NOTES, pillars docs (incl. Pillar 7 Studio spec), `docs/studio/STUDIO-SEAMS-V1.md`, D-71/72/73 studio audits, litmus JSON catalog, TLA+ specs, CI workflow YAMLs, baseline JSONs.
- Docs pages render MDX with custom components (callouts, law-cards, evidence-footnotes, theorem blocks). Frontmatter added during transformation, not hand-edited upstream.
- **API reference:** TypeDoc for TS packages; rustdoc output linked (or summarized) for Rust; docgen comments parsed from C headers into structured pages; KDoc/SwiftDoc/Dart doc referenced with deep links back to repo where generation is impractical on Pages.
- Glossary terms auto-linked via remark plugin (hover popover with definition).
- Blog/changelog seeded from RELEASE-NOTES.md + phase reports.

---

## 6. Tech Stack Decision

**Recommended: Astro 5 + MDX + Tailwind CSS 4 + View Transitions + minimal islands of vanilla TS/Web Components for demos.**

Why Astro over Next.js Docusaurus / VitePress / pure Eleventy:
- Zero-JS-by-default static output → perfect for GitHub Pages perf budgets (target Lighthouse 100/100/100/100).
- Islands architecture: ship React/lit only for the 8 interactive components above.
- First-class MDX content collections for the docs pipeline; built-in image optimization, sitemap, RSS.
- `astro.config`: `site: https://zephyr4289.github.io`, `base: '/Weft'` — solves Pages subpath routing cleanly (VitePress also fine; Next static-export fights the base path and drags 60KB+ runtime).

Supporting libs: Shiki (dual-theme syntax highlighting, build-time), @unovis or plain canvas for charts (no heavy chart lib), GSAP ScrollTrigger *only if* IntersectionObserver proves insufficient (prefer IO), fusee/subset-fonts tooling.

Search: Pagefind (static, post-build index, ~free) — best fit for Pages.

---

## 7. GitHub Pages Delivery & CI/CD

- New top-level `site/` directory in the repo (keeps monorepo intact).
- Workflow `deploy-pages.yml`: push to `main` touching `site/**` or tracked docs sources → install → `astro build` → upload artifact → **GitHub Pages deployment job** (official `actions/deploy-pages`, `github/pages:write` env). No gh-pages branch juggling.
- Preview builds: PR workflow builds site and attaches dist as artifact (+ optional Cloudflare Pages/Pages-style preview skipped to stay 100% GitHub-native).
- Content-freshness job: nightly check that embedded stats (commit count, package count, shard count) match repo reality; fail loudly rather than show stale claims (aligns with Law 4: honesty).
- Artifact hygiene: `.nojekyll`, cache-control meta tags, deterministic builds (pinned lockfile) so the site-determinism ethos extends to the website itself.

---

## 8. Performance, Accessibility, SEO

- **Perf budget (enforced in CI via `lhci`):** LCP < 1.8s, CLS < 0.02, INP < 50ms, landing JS ≤ 120KB gz, images AVIF/WebP, fonts preloaded subsets, everything above-fold inline-critical.
- **A11y:** semantic landmarks, skip-link, full keyboard nav for diagrams (step buttons on the triad animator), focus-visible rings, reduced-motion fallbacks, AA contrast, aria-live regions on demo FPS counters, alt-text policy for decorative vs informative SVGs.
- **SEO/metadata:** OG image generated at build (satori/resvg — premium branded card), JSON-LD (SoftwareSourceCode, TechArticle for docs), canonical URLs, sitemap, RSS for changelog.

---

## 9. Phased Execution Plan

| Phase | Deliverable | Est. effort |
|---|---|---|
| **0 — Foundation** | Design tokens, typography kit, Astro scaffold under `site/`, deploy pipeline green on Pages, empty shell live | 1–2 days |
| **1 — Landing v1** | Full scroll narrative sections with static content, hero demo v1 (canvas waveform + fps meter, no fake-jank comparator yet), ports matrix, install tabs, footer | 3–4 days |
| **2 — Signature interactions** | Triad interactive diagram (scrubbable, F1 toggle), two-path jank comparator, litmus lab widget, **Studio chrome kit + slot decoder + time-travel scrubber demos**, motion polish, reduced-motion pass | 4–5 days |
| **3 — Docs hub** | MDX pipeline ingesting WHITEPAPER/PHILOSOPHY/GLOSSARY/ERRATA/PORTS/ROADMAP + STUDIO-SEAMS; concepts pages; API ref for TS + C headers (incl. weft_studio.h ABI tables); Pagefind search; light reading theme | 4–6 days |
| **3.5 — Studio page** | Full `/studio` product-within-product page per §4A: hero mock, STUI1 split animation, six-panel sticky walk, cadence metronome, seams diagram, audit wall, landing-page interstitial band | 3–4 days |
| **4 — Proof & extras** | Benchmarks honesty cards wired to baseline JSONs, evidence deep-link system, changelog/blog, OG image automation, Lighthouse/a11y hardening, cross-browser QA (Safari iOS included) | 3–4 days |
| **5 — Stretch** | WASM playground running the real TS/C→WASM triad in-browser; i18n scaffolding; embeddable "law badge" SVGs for other repos | ongoing |

Total to a complete, polished v1: **~3–4 weeks of focused work** (Studio page adds ~4 days).

---

## 10. Definition of Done (acceptance gates)

1. Site live at `https://zephyr4289.github.io/Weft/`, deployed automatically from `main`.
2. Lighthouse mobile ≥ 95 on all four scores, desktop 100s achievable.
3. All 11 interactive components (§4 + §4A) functional in Chrome/Firefox/Safari; graceful no-JS degradation (content readable, demos replaced by annotated screenshots).
4. Every quantitative claim on the site carries an environment label + deep link to its evidence file at a pinned SHA.
5. Docs cover: getting started (6 platforms), concepts, triad protocol, four laws, API (TS+C minimum), ports, formal methods, CI, benchmarks, studio (seams, panels, .weft/.weftrec formats), errata.
6. `prefers-reduced-motion`, keyboard-only walkthrough, and screen-reader audit passed.
7. Repo untouched outside `site/` (plus one workflow file); no committed build artifacts.

---

## 11. Risks & Mitigations

| Risk | Mitigation |
|---|---|
| Base-path (`/Weft/`) asset breakage | Astro `base` config + relative-canonical test in CI; smoke-check crawler |
| Benchmark numbers misread as device-measured | Mandatory env chips; copy review against ERRATA conventions (this is brand-critical) |
| Docs drift from repo source | Build-time ingestion + nightly freshness check; never fork content by hand |
| Heavy demos hurting perf score | Demos lazy-mount on intersection; budget gate in CI blocks regressions |
| Font licensing | Only OFL/Google-BDFL or explicitly free-commercial faces (Clash/General Sans via Fontshare free tier, or fall back fully to Inter+JetBrains) |

---

## 12. What makes this "state of the art" (differentiators)

1. **Dogfooding:** the hero demo literally runs the TS Triad kernel — visitors watch the product prove itself.
2. **Auditable claims:** footnote-to-pinned-SHA evidence chain — no competitor does this; it weaponizes Weft's own Law 4 as design.
3. **The F1 story told interactively:** showing the *withdrawn* unsound design and why the fix works turns humility into the most premium feature on the page.
4. **Editorial identity:** the §-numbered whitepaper typographic system makes the site feel like a living research artifact, not a template.
5. **Zero-compromise performance:** Astro static + islands + Pagefind keeps beauty cheap — the site itself demonstrates the "mechanism, not policy" ethos.
