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
