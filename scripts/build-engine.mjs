// Bundles js/host.mjs + the Strudel checkout from scripts/update-engine (with hydra-synth at Strudel's lockfile
// version) into res/engine.js, and superdough's AudioWorklet module into res/worklets.js.
import { build } from 'esbuild';
import { existsSync, readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';

const root = new URL('..', import.meta.url).pathname;
const packages = `${root}build/engine-src/strudel/packages/`;
// hydra-synth's sources, as @strudel/hydra depends on it (its package exports hide the files host.mjs imports).
const hydraSynth = `${packages}hydra/node_modules/hydra-synth`;

// What superdough hands to audioWorklet.addModule(); the native binding maps it to res/worklets.js (M1c-3).
const WORKLETS_URL = 'patchisserie:worklets.js';

// Virtual modules for the two imports that only work under Vite / in a browser. Strudel sources stay untouched.
const stubs = {
  name: 'patchisserie-stubs',
  setup(b) {
    b.onResolve({ filter: /\?audioworklet$/ }, () => ({ path: 'worklets', namespace: 'stub' }));
    // superdough's initAudio() awaits this import outside its try; kabelsalat is out of v1.
    b.onResolve({ filter: /^@kabelsalat\/web$/ }, () => ({ path: 'kabelsalat', namespace: 'stub' }));
    b.onLoad({ filter: /^worklets$/, namespace: 'stub' }, () => ({
      contents: `export default ${JSON.stringify(WORKLETS_URL)};`,
    }));
    b.onLoad({ filter: /^kabelsalat$/, namespace: 'stub' }, () => ({
      contents: "export class SalatRepl { evaluate() { throw new Error('kabelsalat is not available'); } }",
    }));
    // hydra-synth's WebGL, animation loop and microphone analysis: only its constructor would call them, and host.mjs
    // overrides the methods that do.
    b.onResolve({ filter: /^(regl|raf-loop|meyda)$/ }, (args) => ({ path: args.path, namespace: 'stub' }));
    b.onLoad({ filter: /^(regl|raf-loop|meyda)$/, namespace: 'stub' }, (args) => ({
      contents: `export default () => { throw new Error('${args.path} is not available'); };`,
    }));
  },
};

const common = {
  absWorkingDir: root,
  bundle: true,
  format: 'iife',
  platform: 'neutral',
  target: 'es2022',
  mainFields: ['module', 'main'],
  define: {
    'import.meta.url': '"file:///engine.js"', // only NeoCyclist's SharedWorker uses it, never constructed here
    'process.env.NODE_ENV': '"production"',
  },
  legalComments: 'eof',
  metafile: true, // inputs -> res/ENGINE_LICENSES below
  logLevel: 'warning',
  // superdough.mjs initAudio(): `(!audioCtx) instanceof OfflineAudioContext` (always false) is upstream's.
  logOverride: { 'suspicious-boolean-not': 'silent' },
};

const engine = await build({
  ...common,
  entryPoints: ['js/host.mjs'],
  outfile: 'res/engine.js',
  // Resolve our imports straight into the checkout; Strudel's own imports go through its pnpm links to the same files.
  alias: {
    ...Object.fromEntries(
      ['core', 'mini', 'tonal', 'transpiler', 'xen', 'edo', 'webaudio', 'soundfonts', 'hydra'].map((p) => [
        `@strudel/${p}`,
        packages + p,
      ]),
    ),
    'hydra-synth': hydraSynth,
    // mondo`...`: @strudel/mondo lives in packages/mondough, its parser mondolang in packages/mondo (no npm deps).
    '@strudel/mondo': packages + 'mondough',
    mondolang: packages + 'mondo',
  },
  plugins: [stubs],
});

// The AudioWorkletGlobalScope side (worklets.mjs + its imports), loaded by the worklet host in M1c-3.
const worklets = await build({
  ...common,
  entryPoints: [`${packages}superdough/worklets.mjs`],
  outfile: 'res/worklets.js',
});

// res/ENGINE_LICENSES: every npm package that ended up in the two bundles, with its license text, so the notices
// BSD/MIT/ISC require ship with res/ and stay right when the engine is updated.
const readJson = (f) => JSON.parse(readFileSync(f, 'utf8'));
const pkgs = new Map(); // name@version -> { json, dir, workspace }
for (const file of new Set([...Object.keys(engine.metafile.inputs), ...Object.keys(worklets.metafile.inputs)])) {
  if (file.includes(':') || !/(^|\/)(node_modules|build\/engine-src\/strudel\/packages)\//.test(file)) continue; // stubs, js/
  let dir = join(root, file);
  do dir = dirname(dir);
  while (!(existsSync(join(dir, 'package.json')) && readJson(join(dir, 'package.json')).name));
  const json = readJson(join(dir, 'package.json'));
  pkgs.set(`${json.name}@${json.version}`, { json, dir, workspace: !dir.includes('/node_modules/') });
}

const sep = `\n${'='.repeat(72)}\n\n`;
const licenseField = (j) => (typeof j.license === 'string' ? j.license : (j.license?.type ?? 'UNKNOWN'));
const entries = [...pkgs.entries()]
  .sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0))
  .map(([id, { json, dir, workspace }]) => {
    const head = `${id} \u2014 ${licenseField(json)}\n\n`;
    if (workspace) return `${head}Part of Strudel (https://codeberg.org/uzu/strudel), see LICENSE at its repository root.\n`;
    const texts = readdirSync(dir)
      .filter((f) => /^(licen[sc]e|copying)([.-][\w.-]+)?$/i.test(f)) // LICENSE, LICENSE.md, LICENSE.BSD, LICENSE-MIT
      .sort()
      .map((f) => readFileSync(join(dir, f), 'utf8').trim());
    if (texts.length) return head + texts.join('\n\n') + '\n';
    const author = typeof json.author === 'string' ? json.author : json.author?.name;
    const repo = typeof json.repository === 'string' ? json.repository : json.repository?.url;
    return `${head}${[author && `Author: ${author}`, repo && `Repository: ${repo}`].filter(Boolean).join('\n')}\n(no license file in the package)\n`;
  });

const vendor = (f) => readFileSync(join(root, f), 'utf8').trim() + '\n';
writeFileSync(
  `${root}res/ENGINE_LICENSES`,
  `Third-party components bundled into res/engine.js and res/worklets.js (generated by scripts/build-engine.mjs;
do not edit). Patchisserie itself and the AGPL components (Strudel, hydra-synth) are under the GNU Affero General
Public License v3 or later (see LICENSE); the other packages are under the licenses given below.
${sep}${entries.join(sep)}${sep}Compiled into the plugin binary

quickjs-ng (vendor/quickjs-ng) \u2014 MIT

${vendor('vendor/quickjs-ng/LICENSE')}${sep}dr_libs (vendor/dr_libs: dr_wav, dr_mp3, dr_flac) \u2014 public domain or MIT-0

${vendor('vendor/dr_libs/LICENSE')}${sep}orca-c (vendor/orca-c: the Orca VM, sim.c and vmio.c) \u2014 MIT

${vendor('vendor/orca-c/LICENSE.md')}`,
);
