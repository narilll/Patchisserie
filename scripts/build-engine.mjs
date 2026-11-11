// Bundles js/host.mjs + the Strudel checkout from scripts/update-engine (with hydra-synth at Strudel's lockfile
// version) into res/engine.js, and superdough's AudioWorklet module into res/worklets.js.
import { build } from 'esbuild';

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
  logLevel: 'warning',
  // superdough.mjs initAudio(): `(!audioCtx) instanceof OfflineAudioContext` (always false) is upstream's.
  logOverride: { 'suspicious-boolean-not': 'silent' },
};

await build({
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
  },
  plugins: [stubs],
});

// The AudioWorkletGlobalScope side (worklets.mjs + its imports), loaded by the worklet host in M1c-3.
await build({ ...common, entryPoints: [`${packages}superdough/worklets.mjs`], outfile: 'res/worklets.js' });
