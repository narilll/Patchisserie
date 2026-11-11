// res/engine.js under QuickJS: build/qjs test/qjs_smoke.js
import * as std from 'qjs:std';

std.loadScript(import.meta.url.replace(/^file:\/\//, '').replace(/[^/]*$/, '') + '../res/engine.js');
const host = globalThis.__patchisserie;
const code = 'note("c e g")';
const res = await host.evaluate(code);
const got = JSON.stringify(host.query(0, 1).map((h) => [h.begin, h.end, h.value.note, h.locations.map((l) => code.slice(l.start, l.end))]));
const want = JSON.stringify([[0, 1 / 3, 'c', ['c']], [1 / 3, 2 / 3, 'e', ['e']], [2 / 3, 1, 'g', ['g']]]);
if (!res.ok || got !== want) {
  print(`qjs smoke FAILED: ${JSON.stringify(res)}\n  got:  ${got}\n  want: ${want}`);
  std.exit(1);
}
print('qjs smoke: note("c e g") -> 3 haps with locations OK');

// Hydra's generator under QuickJS too. window.addEventListener: what src/audio/webaudio.js gives the engine.
globalThis.window = globalThis;
globalThis.addEventListener = () => {};
const hydra = await host.evaluate('await initHydra()\nsrc(o0).modulate(noise(3), 0.005).blend(shape(4), 0.01).out(o0)');
const [program, ...more] = host.hydraPrograms();
const frame = host.hydraFrame(0, 0, 0.5, Array(24).fill(1), 24);
if (!hydra.ok || more.length || !/gl_FragColor = blend\(src\(modulate\(st, noise/.test(program?.frag) || frame?.[4] !== program.id) {
  print(`qjs smoke FAILED: Hydra ${JSON.stringify(hydra)} ${JSON.stringify(frame)}\n${program?.frag}`);
  std.exit(1);
}
print('qjs smoke: initHydra() + src(o0).modulate(...).blend(...).out(o0) -> 1 shader and its frame OK');
