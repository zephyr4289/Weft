#!/usr/bin/env node
// @weft/verify bin launcher.
//
// Sources are pure TypeScript with erasable-only syntax. Node >= 23.6 runs
// them natively (type stripping on by default); Node 22.6..22.x needs
// --experimental-strip-types. This launcher probes once and re-execs the
// CLI with the right flags. Zero third-party dependencies.
import { spawnSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const cli = path.join(here, '..', 'src', 'cli.ts');

function stripFlagSupported() {
  const probe = spawnSync(
    process.execPath,
    ['--experimental-strip-types', '--no-warnings', '-e', '0'],
    { encoding: 'utf8' },
  );
  return probe.status === 0;
}

const nativeStrip = Boolean(process.features.typescript);
let flags;
if (nativeStrip) {
  flags = ['--no-warnings'];
} else if (stripFlagSupported()) {
  flags = ['--no-warnings', '--experimental-strip-types'];
} else {
  process.stderr.write(
    `weft-verify: Node >= 22.6 is required (found ${process.version}).\n` +
      `Type-stripping support is unavailable; install Node 22 LTS or newer.\n`,
  );
  process.exit(2);
}

const child = spawnSync(process.execPath, [...flags, cli, ...process.argv.slice(2)], {
  stdio: 'inherit',
});
if (child.error) {
  process.stderr.write(`weft-verify: failed to launch: ${child.error.message}\n`);
  process.exit(1);
}
process.exit(child.status ?? 1);
