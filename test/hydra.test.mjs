// Icing's Hydra in js/host.mjs, via the built res/engine.js loaded as QuickJS loads it: hydra-synth's generator with
// our Output, the global swap, H(), a.fft, and the frames src/engine.cpp parses.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

vm.runInThisContext(readFileSync(new URL('../res/engine.js', import.meta.url), 'utf8'), { filename: 'res/engine.js' });
// What src/audio/webaudio.js gives the engine and hydra-synth's module body needs. After the load: with a window,
// Strudel's modules would think they run in a browser.
globalThis.window = globalThis;
globalThis.addEventListener = () => {};
const host = globalThis.__patchisserie;

const bands = (f) => Array.from({ length: 24 }, (_, i) => f(i));
/** hydraFrame() at Icing time t (s), Filo at `cycle` with `cps`, silent AUDIO; parsed. */
const frame = (t, cycle = 0, cps = 0.5, specific = bands(() => 0)) => parse(host.hydraFrame(t, cycle, cps, specific, 0));
function parse(f) {
  if (!f) return f;
  const [display, time, width, height, ...rest] = f;
  const outputs = [];
  for (let k = 0; k < rest.length; ) {
    const [program, n] = [rest[k], rest[k + 1]];
    outputs.push({ program, values: rest.slice(k + 2, k + 2 + n) });
    k += 2 + n;
  }
  return { display, time, width, height, outputs };
}
const ok = async (code) => assert.deepEqual(await host.evaluate(code), { ok: true }, code);

test('no frames before initHydra()', async () => {
  await ok('note("c")');
  assert.equal(host.hydraFrame(0, 0, 0.5, bands(() => 0), 0), null);
  assert.deepEqual(host.hydraPrograms(), []);
});

test("initHydra() installs Hydra's globals for its evaluate, the next evaluate restores Strudel's", async () => {
  const names = ['noise', 'shape', 'speed', 'hush', 'time', 'src'];
  const strudel = Object.fromEntries(names.map((n) => [n, globalThis[n]]));
  for (const n of names) assert.ok(strudel[n], n);
  // Within the evaluate, after the await: Hydra's.
  await ok(`await initHydra()
if (typeof speed !== 'number' || typeof time !== 'number' || !noise(3).out || !shape(4).out) throw new Error('not Hydra')
osc().out()`);
  for (const n of names) assert.notEqual(globalThis[n], strudel[n], n);
  assert.equal(typeof globalThis.o0.getTexture, 'function');
  await ok('s("bd")');
  for (const n of names) assert.equal(globalThis[n], strudel[n], n);
  assert.equal(globalThis.o0, undefined);
  assert.equal(globalThis.osc, undefined);
  // A failed evaluate leaves the previous code's Hydra globals in place, as its pattern.
  await ok('await initHydra()\nosc().out()');
  assert.equal((await host.evaluate('osc(')).ok, false);
  assert.equal(typeof globalThis.speed, 'number');
});

test("Output collects each output's shader; src(o0) is a sampler of that output", async () => {
  host.hydraPrograms();
  await ok('await initHydra()\nsrc(o0).modulate(noise(3), 0.005).out(o0)\nosc(10).out(o1)');
  const [o0, o1, ...more] = host.hydraPrograms();
  assert.deepEqual(more, []);
  assert.deepEqual([o0.output, o0.uniforms, o0.samplers], [0, ['tex0'], [true]]);
  assert.match(o0.frag, /uniform sampler2D tex0;/);
  assert.match(o0.frag, /gl_FragColor = src\(modulate\(st, noise\(st, 3\., 0\.1\), 0\.005\), tex0\);/);
  assert.deepEqual([o1.output, o1.uniforms], [1, []]);
  const f = frame(1);
  assert.deepEqual(f.outputs[0], { program: o0.id, values: [0] }); // o0's index: src/hydra.cpp reads its other buffer
  assert.deepEqual(f.outputs[1], { program: o1.id, values: [] });
  // Sources (s0-s3) are never initialised: no texture.
  await ok('await initHydra()\nsrc(s1).out(o2)');
  assert.deepEqual(frame(2).outputs[2].values, [-1]);
});

test('examples from the Hydra docs generate shaders', async () => {
  const examples = [
    'osc(60, 0.1, 1.2).rotate(0, 0.1).out()',
    'noise(10, 0.1).color(1, 0.5, 0.2).out()',
    'voronoi(5, 0.3, 0.3).modulate(osc(8), 0.2).out()',
    'osc(20, 0.1, 0.8).blend(shape(4, 0.3), 0.5).out()',
    'src(o0).modulateHue(src(o0).scale(1.01), 1).layer(osc(10, 0.1, 2).mask(shape(3, 0.2))).out(o0)',
    'osc(25, 0.1, 0.5).kaleid(50).out()',
    'shape([3, 4, 5].fast(2), 0.4).repeat(3, 3).out()',
    'osc(10, 0.1, () => a.fft[0] * 4).out()',
    'gradient(1).colorama(0.4).out(o1)\nsolid(1, 0, 0).diff(o1).out(o2)\nrender(o2)',
    'osc().out(o0)\nnoise().out(o1)\nvoronoi().out(o2)\nshape().out(o3)\nrender()',
  ];
  for (const code of examples) {
    host.hydraPrograms();
    await ok(`await initHydra()\nrender(o0)\n${code}`);
    const programs = host.hydraPrograms();
    assert.ok(programs.length > 0, code);
    for (const p of programs) assert.match(p.frag, /void main \(\) \{[\s\S]*gl_FragColor = /, code);
  }
  assert.equal(frame(3).display, -1); // render(): all four
});

test('H(pattern) is the value at the cycle Filo plays', async () => {
  await ok('await initHydra()\nosc(H("10 20")).out()');
  assert.deepEqual(frame(4, 0.25).outputs[0].values, [10]);
  assert.deepEqual(frame(4.1, 0.75).outputs[0].values, [20]);
  assert.deepEqual(frame(4.2, 1.25).outputs[0].values, [10]);
});

test("a.fft: hydra-synth's binning, smoothing, cutoff and scale of Meyda's loudness bands", async () => {
  await ok(`await initHydra()
a.setSmooth(0.4); a.setCutoff(2); a.setScale(10); a.setBins(4); a.show(); a.hide()
osc(() => a.fft[0], 0.1, () => a.fft[3]).out()`);
  const specific = bands((i) => i + 1);
  const fft = (t) => parse(host.hydraFrame(t, 0, 0.5, specific, 300)).outputs[0].values;
  const near = (got, want) => got.forEach((g, i) => assert.ok(Math.abs(g - want[i]) < 1e-12, `${got} vs ${want}`));
  // 4 bins of 6 bands: sums 21 and 129 (bins 0, 3); each bin = 0.6 sum + 0.4 previous; fft = (bin - 2) / 10.
  near(fft(5), [(0.6 * 21 - 2) / 10, (0.6 * 129 - 2) / 10]);
  near(fft(5.1), [(0.6 * 21 + 0.4 * 12.6 - 2) / 10, (0.6 * 129 + 0.4 * 77.4 - 2) / 10]);
  await ok(`await initHydra()
a.setBins(2); a.setSmooth(0); a.setCutoff(0); a.setScale(1)
osc(() => a.fft[0], 0.1, () => a.fft[1]).out()`);
  near(fft(5.2), [78, 222]); // bands 1-12, 13-24
});

test('bpm is Filo\'s tempo (4 beats per cycle), time counts seconds since initHydra() times speed', async () => {
  await ok('await initHydra()\nspeed = 1\nosc(({ bpm }) => bpm, 0.1, ({ time }) => time).out()');
  const [bpm0, t0] = frame(10, 0, 0.5).outputs[0].values;
  const [bpm1, t1] = frame(10.5, 0, 0.75).outputs[0].values;
  assert.deepEqual([bpm0, bpm1], [120, 180]);
  assert.ok(Math.abs(t1 - t0 - 0.5) < 1e-9, `${t1 - t0}`);
  await ok('await initHydra()\nspeed = 2\nosc(({ bpm }) => bpm, 0.1, ({ time }) => time).out()');
  const t2 = frame(11).outputs[0].values[1];
  const t3 = frame(11.5).outputs[0].values[1];
  assert.ok(Math.abs(t3 - t2 - 1) < 1e-9, `${t3 - t2}`);
  await ok('await initHydra()\nspeed = 1');
});

test('setResolution() and render(oN) reach the frame', async () => {
  await ok('await initHydra()\nsetResolution(640, 360)\nosc().out(o2)\nrender(o2)');
  const f = frame(12);
  assert.deepEqual([f.display, f.width, f.height], [2, 640, 360]);
  await ok('await initHydra()\nsetResolution(1280, 720)\nrender(o0)');
});

test("Hydra's external sources fail with a clear error", async () => {
  for (const call of ['s0.initCam()', 's1.initVideo("x.mp4")', 's2.initImage("x.png")', 's3.initScreen()', 's0.init({})']) {
    const r = await host.evaluate(`await initHydra()\n${call}`);
    assert.equal(r.ok, false, call);
    assert.match(r.error, /^s\d\.init\w*\(\): Hydra's external sources are not available in Icing/, call);
  }
});

test("an evaluate without initHydra() and Filo's STOP (hydraHush) hush the visuals", async () => {
  await ok('await initHydra()\nosc().out(o1)\nrender()');
  host.hydraPrograms();
  await ok('note("c")');
  const hushed = host.hydraPrograms();
  assert.deepEqual(hushed.map((p) => p.output), [0, 1, 2, 3]);
  for (const p of hushed) assert.match(p.frag, /solid\(st, 0\., 0\., 0\., 0\.\)/);
  assert.equal(frame(13).display, 0); // hush(): render(o0)
  await ok('note("d")'); // hushed already
  assert.deepEqual(host.hydraPrograms(), []);
  await ok('await initHydra()\nosc().out()');
  host.hydraPrograms();
  host.hydraHush();
  assert.equal(host.hydraPrograms().length, 4);
});

test('a failing uniform function warns once, not on every frame', async () => {
  const warn = console.warn;
  const lines = [];
  console.warn = (...a) => lines.push(a.map(String).join(' '));
  try {
    await ok('await initHydra()\nosc(() => nosuchname).out()');
    for (let i = 0; i < 5; i++) assert.deepEqual(frame(14 + i).outputs[0].values, [60]); // osc's default frequency
  } finally {
    console.warn = warn;
  }
  assert.equal(lines.length, 1, lines.join('\n'));
  assert.match(lines[0], /nosuchname/);
});

test("Strudel's canvas visuals and feedStrudel draw nothing but don't stop the code (they warn once)", async () => {
  const warn = console.warn;
  const lines = [];
  console.warn = (...a) => lines.push(a.map(String).join(' '));
  try {
    for (let i = 0; i < 2; i++)
      await ok('await initHydra({feedStrudel:1})\nsrc(s0).kaleid(4).out()\n$: s("bd*4").scope({pos:0}).pianoroll()');
  } finally {
    console.warn = warn;
  }
  assert.equal(host.query(0, 1).length, 4);
  assert.equal(lines.filter((l) => /\[draw\]/.test(l)).length, 1, lines.join('\n'));
  assert.ok(lines.some((l) => /feedStrudel/.test(l)), lines.join('\n'));
});
