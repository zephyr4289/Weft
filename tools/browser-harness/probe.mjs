// probe.mjs — Playwright chromium leg: load the harness page in a REAL
// browser, wait for the report to be POSTed, exit 0 iff the server-side
// gate passed. The probe asserts nothing itself — the report's contents
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
let chromium;
try {
  ({ chromium } = await import('playwright-core'));
} catch {
  const p = require.resolve('playwright-core', {
    paths: [
      process.env.NODE_PATH,
      new URL('../../ci/browser-tmp/node_modules', import.meta.url).pathname,
      process.cwd(),
    ].filter(Boolean),
  });
  ({ chromium } = await import(p));
}

const PORT = Number(process.argv[2] || 8123);

const browser = await chromium.launch({
  args: [
    // COOP/COEP do not need flags in a stock browser — isolation comes from
    // the headers. No experimental switches: prove the stock-browser path.
    '--no-sandbox',
  ],
});
try {
  const page = await browser.newPage();
  await page.goto(`http://localhost:${PORT}/`, { waitUntil: 'load', timeout: 60_000 });
  await page.waitForFunction(() => document.title === 'WEFT_SAB_REPORT_SENT',
                             { timeout: 60_000 });
  const title = await page.title();
  console.log(`[probe] harness completed: ${title}`);
  process.exit(0);
} catch (e) {
  console.error(`[probe] FAILED: ${e}`);
  process.exit(1);
} finally {
  await browser.close();
}
