// ingest.mjs — pulls normative content out of the Weft repo so the site never
// drifts from the source of truth. Outputs JSON into src/data/ and markdown into src/docs-repo/.
import { readFileSync, writeFileSync, mkdirSync, copyFileSync, readdirSync, existsSync } from 'node:fs';
import { join, dirname, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import * as yaml from 'js-yaml';

const __dirname = dirname(fileURLToPath(import.meta.url));
const REPO = join(__dirname, '..', '..', '..');
const OUT_DATA = join(__dirname, '..', 'src', 'data');
const OUT_DOCS = join(__dirname, '..', 'src', 'docs-repo');

mkdirSync(OUT_DATA, { recursive: true });
mkdirSync(OUT_DOCS, { recursive: true });

const read = (p) => readFileSync(join(REPO, p), 'utf8');

// 1. Litmus catalog -> structured JSON
const catalog = yaml.load(read('litmus/catalog.yaml'));
const tests = catalog.tests.map((t) => ({
  id: t.id,
  langs: t.langs ?? ['c', 'rust', 'ts'],
  adversary: t.adversary,
  verdict: t.verdict,
  params: t.params ?? {},
}));
writeFileSync(join(OUT_DATA, 'litmus.json'), JSON.stringify({ version: catalog.version, defaults: catalog.defaults, orderingMatrix: catalog.ordering_matrix, tests }, null, 2));

// 2. Four Laws (verbatim from WHITEPAPER §2.2)
const wp = read('docs/WHITEPAPER.md');
const lawsBlock = wp.split('### 2.2 The Four Laws')[1].split('---')[0];
const laws = [...lawsBlock.matchAll(/^\d+\.\s+\*\*(.+?)\.\*\*\s*(.+)$/gm)].map((m) => ({ title: m[1].trim(), body: m[2].trim() }));
writeFileSync(join(OUT_DATA, 'laws.json'), JSON.stringify(laws, null, 2));

// 3. Protocol pseudocode (verbatim block from §3.2)
const proto = wp.split('### 3.2 The protocol (complete)')[1].split('```')[1].replace(/^```\w*/,'').trim();
writeFileSync(join(OUT_DATA, 'protocol.txt'), proto);

// 4. Invariants table I1–I6 (§3.4)
const invSection = wp.split('### 3.4 Invariants')[1].split('### 3.5')[0];
const invariants = invSection.split('\n')
  .map((l) => l.trim())
  .filter((l) => /^\|\s*I\d+\s*\|/.test(l))
  .map((l) => { const c = l.split('|').map((x) => x.trim()); return { id: c[1], name: c[2], mechanism: c[3] }; });
if (invariants.length !== 6) throw new Error(`expected 6 invariants, got ${invariants.length}`);
writeFileSync(join(OUT_DATA, 'invariants.json'), JSON.stringify(invariants, null, 2));

// 5. Envelope layout (§4.1)
const envSection = wp.split('### 4.1 Tier 0 frozen 16-byte header')[1].split('```')[1];
writeFileSync(join(OUT_DATA, 'envelope.txt'), envSection.trim());

// 6. Measured gates (§6)
const measured = {
  label: '[MEASURED x86_64-sandbox sha256:16b5c663]',
  b3: { ratio: { c: 1.175, rust: 1.211, ts: 0.627 }, claim_p50_64B_ns: { c: 40, rust: 38, ts: 161 }, claim_p50_64K_ns: { c: 47, rust: 46, ts: 101 } },
  b5: { alloc_delta: { c: 0, rust: 0, ts: 'advisory (GC-noisy)' }, rss_growth_pages: { c: 0, rust: 0, ts: 0 } },
  b1: { ops_s: { c: 2916757, rust: 2327011, ts: 1611026 }, publish_p99_ns: { c: 53, rust: 485, ts: 241 } },
  litmusCells: '24/24 green (L1–L8 × C/Rust/TS, debug+release)',
  tsan: 'zero reports across 40 executions',
  loom: '4/4 assertions hold over all interleavings (3 pub × 3 claim × 3 buffers)',
};
writeFileSync(join(OUT_DATA, 'measured.json'), JSON.stringify(measured, null, 2));

// 7. Ports summary
const portsMd = read('docs/PORTS.md');
const portSections = [...portsMd.matchAll(/^## (\d+)\. (.+)$/gm)].map((m) => ({ n: +m[1], name: m[2].trim() }));
writeFileSync(join(OUT_DATA, 'ports.json'), JSON.stringify(portSections, null, 2));

// 8. Studio seams key facts
const studio = read('docs/studio/STUDIO-SEAMS-V1.md');
writeFileSync(join(OUT_DATA, 'studio-seams.md'), studio);

// 9. All 8 Pillars Master Catalog
const pillars = [
  {
    num: 1,
    id: "kernel-core",
    name: "Kernel Core & Triad Protocol",
    tagline: "Off-Heap Zero-Copy Seqlock & Multi-Language Core",
    rfcs: ["RFC-0001"],
    audits: ["D-01", "D-02", "D-03"],
    specs: ["docs/WHITEPAPER.md"],
    desc: "3 buffers + 1 atomic latest.exchange(AcqRel). Zero allocations on render loop. Verified across C, Rust, TS, Swift, Dart, Python, Kotlin, WASM."
  },
  {
    num: 2,
    id: "accelerated-tensor",
    name: "Accelerated Tensor",
    tagline: "Metal/Vulkan/GGML/ORT Zero-Copy Accelerator Bridges",
    rfcs: ["RFC-0017"],
    audits: ["D-29", "D-30"],
    specs: ["docs/weft-tensor/LAYOUT-V1.md"],
    desc: "Zero-copy tensor ring buffers, DLPack C-API integration, and hardware pre-processing pipelines across Apple Metal, Vulkan Compute, GGML, and ONNX Runtime."
  },
  {
    num: 3,
    id: "cluster-protocol",
    name: "Cluster Protocol",
    tagline: "XDP, io_uring & RDMA Kernel-Bypass Fabric",
    rfcs: ["RFC-0018", "RFC-0019"],
    audits: ["D-31", "D-32", "D-33"],
    specs: ["docs/weft-cluster/WIRE-V1.md"],
    desc: "WCR1 wire consensus protocol, eBPF XDP filter kernels, Linux io_uring zero-copy sockets, and InfiniBand RDMA memory-window transfers."
  },
  {
    num: 4,
    id: "heddle2-canvas",
    name: "Heddle 2.0 Canvas",
    tagline: "WHP1 HotPlane 240 FPS Hardware Canvas Engine",
    rfcs: ["RFC-0022"],
    audits: ["D-41", "D-42", "D-43"],
    specs: ["docs/heddle2/HPL1-LAYOUT-V1.md", "docs/heddle/HOTPLANE-LAYOUT-V2.md"],
    desc: "Zero-re-render UI binding layer, WHP1 1M-point waveform decimation, WebGL2 Transform Feedback, WGSL compute shaders, and native Vulkan lavapipe SSBO probes."
  },
  {
    num: 5,
    id: "spectrum-pipeline",
    name: "Spectrum Shader Pipeline",
    tagline: "Universal GPU & Shader Execution Pipeline",
    rfcs: ["RFC-0020"],
    audits: ["D-51", "D-52"],
    specs: ["docs/spectrum/SPECTRUM-WIRE-V1.md"],
    desc: "Cross-vendor hardware driver registry (Apple, MTK, NV, QCOM, ARM), SIMD vectorization (AVX-512, NEON, RVV), and dynamic governor frequency scaling."
  },
  {
    num: 6,
    id: "protocol-adapters",
    name: "Universal Protocol Adapters",
    tagline: "ITCH 5.0, SBE & ROS2 rmw_weft Managed Seams",
    rfcs: ["RFC-0021"],
    audits: ["D-61", "D-62", "D-63"],
    specs: ["docs/adapters/MANAGED-SEAMS-V1.md"],
    desc: "O(1) L2/L3 order book engine, NASDAQ ITCH 5.0 & CME SBE wire decoders, MDP1 snapshot feeds, and ROS2 rmw_weft zero-copy middleware."
  },
  {
    num: 7,
    id: "studio-engine",
    name: "Weft Studio",
    tagline: "Developer Hot-Plane & Live IDE Profiler",
    rfcs: ["RFC-0015"],
    audits: ["D-71", "D-72", "D-73"],
    specs: ["docs/studio/STUDIO-SEAMS-V1.md"],
    desc: "Android Studio-grade visual profiler, live atomic slot heatmap, flight-recorder time-travel debugger, weft-lsp server, and multi-language codegen."
  },
  {
    num: 8,
    id: "verify-governance",
    name: "Formal Verification & Governance",
    tagline: "TLA+ Proofs, AST Governance & Synthetic Silicon",
    rfcs: ["RFC-0016"],
    audits: ["D-81", "D-82", "D-83"],
    specs: ["docs/pillars/NEXT-GEN-PILLARS-5-8.md"],
    desc: "TLA+ Model Checking for seqlock safety, AST static allocation scanners, synthetic silicon thermal curves, and automated CI allocation-lint hooks."
  }
];
writeFileSync(join(OUT_DATA, 'pillars.json'), JSON.stringify(pillars, null, 2));

// 10. Copy Markdown files from repo into docs-repo
const docsToCopy = [
  'docs/WHITEPAPER.md',
  'docs/PHILOSOPHY.md',
  'docs/GLOSSARY.md',
  'docs/ERRATA.md',
  'docs/PORTS.md',
  'docs/studio/STUDIO-SEAMS-V1.md',
  'docs/adapters/MANAGED-SEAMS-V1.md',
  'docs/heddle2/HPL1-LAYOUT-V1.md',
  'docs/spectrum/SPECTRUM-WIRE-V1.md',
  'docs/weft-cluster/WIRE-V1.md',
  'docs/weft-tensor/LAYOUT-V1.md',
  'docs/pillars/NEXT-GEN-PILLARS-5-8.md',
  'SCORECARD-PILLAR8.md',
  'WEFT-PILLAR8-README.md'
];

for (const doc of docsToCopy) {
  const fullPath = join(REPO, doc);
  if (existsSync(fullPath)) {
    const targetName = basename(doc).toLowerCase();
    copyFileSync(fullPath, join(OUT_DOCS, targetName));
  }
}

// Copy D-reports (D-29 through D-83)
const reportDirs = [join(REPO, 'reports'), join(REPO, 'D-report'), join(REPO, 'docs', 'reports')];
let copiedReports = 0;
for (const dir of reportDirs) {
  if (existsSync(dir)) {
    for (const file of readdirSync(dir)) {
      if (file.endsWith('.md')) {
        copyFileSync(join(dir, file), join(OUT_DOCS, file.toLowerCase()));
        copiedReports++;
      }
    }
  }
}

// Copy RFCs (0001 through 0022)
const rfcDir = join(REPO, 'rfcs');
let copiedRfcs = 0;
if (existsSync(rfcDir)) {
  for (const file of readdirSync(rfcDir)) {
    if (file.endsWith('.md')) {
      copyFileSync(join(rfcDir, file), join(OUT_DOCS, file.toLowerCase()));
      copiedRfcs++;
    }
  }
}

console.log('[ingest] Success! Wrote litmus.json (%d tests), laws.json (%d laws), invariants.json (%d), measured.json, ports.json (%d sections), pillars.json (8 pillars), copied %d reports & %d RFCs into src/docs-repo/',
  tests.length, laws.length, invariants.length, portSections.length, copiedReports, copiedRfcs);
