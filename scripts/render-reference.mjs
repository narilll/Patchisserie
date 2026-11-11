// Audio regression references: renders test/render/patterns.json with the pinned Strudel commit (res/ENGINE_VERSIONS)
// in headless Chromium, through upstream renderPatternAudio (strudel.cc's WAV export), into build/render-reference/<id>.wav,
// and writes their features to test/render/reference.json.
// Usage: node scripts/render-reference.mjs           every https request is served from build/render-cache/<host>/<path>
//        node scripts/render-reference.mjs --record  real network; writes every https URL fetched to test/render/urls.txt
import { execSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { build } from 'esbuild';
import { chromium } from 'playwright';
import { features, readWav } from '../test/render/features.mjs';

const root = new URL('..', import.meta.url).pathname;
const record = process.argv.includes('--record');
const sha = /^strudel (\S+)/m.exec(readFileSync(`${root}res/ENGINE_VERSIONS`, 'utf8'))[1];
const set = JSON.parse(readFileSync(`${root}test/render/patterns.json`, 'utf8'));
const out = `${root}build/render-reference/`;
const src = `${out}strudel/`;

// The pinned commit's tracked files (not the working tree) with the audio packages' runtime deps at Strudel's lockfile
// versions. A private copy, so the shared checkout's node_modules (scripts/update-engine) stays untouched.
if (!existsSync(`${src}.sha`) || readFileSync(`${src}.sha`, 'utf8') !== sha) {
  execSync(
    `rm -rf '${src}' && mkdir -p '${src}' && git -C '${root}build/engine-src/strudel' archive ${sha} | tar -x -C '${src}'`,
  );
  execSync(
    "CI=true pnpm install --prod --frozen-lockfile --ignore-scripts --filter '@strudel/soundfonts...' --filter '@strudel/transpiler...' --filter '@strudel/tonal...'",
    { cwd: src, stdio: 'inherit' },
  );
  writeFileSync(`${src}.sha`, sha);
}

// Same as Strudel's vite-plugin-bundle-audioworklet: 'x.mjs?audioworklet' imports an IIFE bundle of x.mjs as a data: URL.
const audioworklet = {
  name: 'audioworklet',
  setup(b) {
    b.onResolve({ filter: /\?audioworklet$/ }, (a) => ({
      path: `${a.resolveDir}/${a.path.slice(0, -13)}`,
      namespace: 'audioworklet',
    }));
    b.onLoad({ filter: /.*/, namespace: 'audioworklet' }, async (a) => {
      const { outputFiles } = await build({ entryPoints: [a.path], bundle: true, format: 'iife', write: false });
      return {
        contents: `export default "data:text/javascript;base64,${Buffer.from(outputFiles[0].contents).toString('base64')}"`,
      };
    });
  },
};
const pkgs = ['core', 'mini', 'tonal', 'transpiler', 'webaudio', 'soundfonts'];
const { outputFiles } = await build({
  stdin: {
    contents: `${pkgs.map((p) => `import * as ${p} from '@strudel/${p}';`).join('\n')}\nwindow.strudel = { ${pkgs} };`,
    resolveDir: src,
  },
  alias: Object.fromEntries(pkgs.map((p) => [`@strudel/${p}`, `${src}packages/${p}`])),
  bundle: true,
  format: 'esm',
  write: false,
  plugins: [audioworklet],
  logLevel: 'error', // upstream superdough.mjs:304 trips esbuild's suspicious-boolean-not warning
});

const origin = 'https://patchisserie.invalid/'; // served below, never resolved; https = secure context (AudioWorklet)
const browser = await chromium.launch();
const page = await browser.newPage();
const urls = new Set();
const problems = [];
page.on('console', (m) => /error/i.test(m.text()) && problems.push(m.text()));
page.on('pageerror', (e) => problems.push(e.message));
await page.route('**/*', (route) => {
  const url = new URL(route.request().url());
  if (url.href === origin)
    return route.fulfill({ contentType: 'text/html', body: '<script type="module" src="strudel.js"></script>' });
  if (url.href === `${origin}strudel.js`)
    return route.fulfill({ contentType: 'text/javascript', body: outputFiles[0].text });
  if (record) return urls.add(url.href), route.continue();
  const file = `${root}build/render-cache/${url.host}${url.pathname}`;
  if (url.protocol === 'https:' && existsSync(file)) {
    return route.fulfill({ body: readFileSync(file), headers: { 'access-control-allow-origin': '*' } });
  }
  problems.push(`not in build/render-cache (scripts/fetch-test-samples): ${url.href}`);
  return route.abort();
});
const check = (what) => {
  if (problems.length) throw new Error(`${what}:\n${problems.join('\n')}`);
};

await page.goto(origin);
await page.waitForFunction(() => window.strudel);
await page.evaluate(async () => {
  const { core, mini, tonal, webaudio, soundfonts } = window.strudel;
  await core.evalScope(core, mini, tonal, webaudio, soundfonts);
  // Test preload contract, identical in the native harness.
  webaudio.registerSynthSounds();
  webaudio.registerZZFXSounds();
  soundfonts.registerSoundfonts();
  await webaudio.samples('github:tidalcycles/dirt-samples');
});
check('preload');

mkdirSync(out, { recursive: true });
const playwright = JSON.parse(readFileSync(`${root}node_modules/playwright/package.json`, 'utf8')).version;
const reference = { strudel: sha, chrome: browser.version(), playwright, patterns: {} };
for (const { id, code } of set.patterns) {
  const [download] = await Promise.all([
    page.waitForEvent('download'),
    page.evaluate(
      async ({ id, code, cps, cycles, sampleRate }) => {
        const {
          core,
          transpiler: { transpiler },
          webaudio,
        } = window.strudel;
        // As strudel.cc's handleExport: repl.evaluate(code, false), then renderPatternAudio(repl.state.pattern, ...).
        // core.repl like the website's StrudelMirror (webaudioRepl() would close the shared AudioContext it re-sets).
        let error;
        const repl = core.repl({ transpiler, getTime: () => 0, onEvalError: (e) => (error = e) });
        await repl.evaluate(code, false);
        if (error) throw error;
        // maxPolyphony 1024, multiChannelOrbits true: strudel.cc's export tab defaults.
        await webaudio.renderPatternAudio(repl.state.pattern, cps, 0, cycles, sampleRate, 1024, true, id);
      },
      { id, code, cps: set.cps, cycles: set.cycles, sampleRate: set.sampleRate },
    ),
  ]);
  check(id);
  const file = `${out}${id}.wav`;
  await download.saveAs(file);
  reference.patterns[id] = features(readWav(readFileSync(file)), set.cps);
  console.log(`${id}: ${reference.patterns[id].onsets.length} onsets, rms ${reference.patterns[id].rms_db} dB`);
}
await browser.close();

const json = JSON.stringify(reference, null, 1).replace(/\[[^[\]{}]*\]/g, (a) => a.replace(/\s+/g, '')); // number arrays on one line
writeFileSync(`${root}test/render/reference.json`, `${json}\n`);
if (record) {
  const list = [...urls].filter((u) => u.startsWith('https://')).sort();
  writeFileSync(`${root}test/render/urls.txt`, `${list.join('\n')}\n`);
  console.log(`test/render/urls.txt: ${list.length} URLs`);
}
