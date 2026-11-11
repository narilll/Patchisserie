// Bundles js/host.mjs + the Strudel checkout from scripts/update-engine into res/engine.js.
import { build } from 'esbuild';

const root = new URL('..', import.meta.url).pathname;
const packages = `${root}build/engine-src/strudel/packages/`;

await build({
  absWorkingDir: root,
  entryPoints: ['js/host.mjs'],
  outfile: 'res/engine.js',
  bundle: true,
  format: 'iife',
  platform: 'neutral',
  target: 'es2022',
  mainFields: ['module', 'main'],
  // Resolve our imports straight into the checkout; Strudel's own imports go through its pnpm links to the same files.
  alias: Object.fromEntries(['core', 'mini', 'tonal', 'transpiler', 'xen', 'edo'].map((p) => [`@strudel/${p}`, packages + p])),
  define: {
    'import.meta.url': '"file:///engine.js"', // only NeoCyclist's SharedWorker uses it, never constructed here
    'process.env.NODE_ENV': '"production"',
  },
  legalComments: 'eof',
  logLevel: 'warning',
});
