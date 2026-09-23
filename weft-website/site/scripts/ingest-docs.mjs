/**
 * ingest-docs.mjs — copies the normative repo docs into src/docs-repo/ (verbatim)
 * and generates src/data/rfcs-index.json. Run before `astro build`.
 */
import { cpSync, readFileSync, writeFileSync, readdirSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';

const REPO = new URL('../../weft-repo/', import.meta.url).pathname;
const OUT = new URL('../src/docs-repo/', import.meta.url).pathname;
mkdirSync(OUT, { recursive: true });

const FILES = [
  ['docs/WHITEPAPER.md', 'whitepaper.md'],
  ['docs/PHILOSOPHY.md', 'philosophy.md'],
  ['docs/GLOSSARY.md', 'glossary.md'],
  ['docs/PORTS.md', 'ports.md'],
  ['docs/ERRATA.md', 'errata.md'],
  ['docs/studio/STUDIO-SEAMS-V1.md', 'studio-seams-v1.md'],
  ['rfcs/0001-triad-exchange-protocol.md', 'rfc-0001.md'],
];
for (const [src, dst] of FILES) {
  try { cpSync(join(REPO, src), join(OUT, dst)); console.log('[docs]', dst); }
  catch (e) { console.warn('[docs] missing:', src, e.message); }
}

// RFC index ---------------------------------------------------------------
const rfcs = [];
for (const f of readdirSync(join(REPO, 'rfcs')).sort()) {
  if (!/^0\d{3}-.*\.md$/.test(f)) continue;
  const text = readFileSync(join(REPO, 'rfcs', f), 'utf8');
  const num = (text.match(/^RFC:\s*(\d{4})/m) ?? [])[1] ?? f.slice(0, 4);
  const title = (text.match(/^Title:\s*(.+)$/m) ?? [])[1]
    ?? (text.match(/^#\s+RFC\s*[-–]?[\dN]+\s*[—:-]?\s*(.+)$/im) ?? [])[1] ?? f;
  const status = ((text.match(/^Status:\s*(.+)$/m) ?? [])[1] ?? '').split(/[.(]/)[0].trim();
  rfcs.push({ num, title: title.trim(), status, file: f });
}
writeFileSync(new URL('../src/data/rfcs-index.json', import.meta.url), JSON.stringify(rfcs, null, 2));
console.log('[docs] rfcs-index.json:', rfcs.length, 'entries');
