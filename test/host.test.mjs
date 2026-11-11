// js/host.mjs API, via the built res/engine.js loaded as a classic script (as QuickJS will load it).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

vm.runInThisContext(readFileSync(new URL('../res/engine.js', import.meta.url), 'utf8'), { filename: 'res/engine.js' });
const host = globalThis.__patchisserie;
const source = (code, hap) => hap.locations.map(({ start, end }) => code.slice(start, end));

test('note("c e g") gives 3 haps with mini-notation locations', async () => {
  const code = 'note("c e g")';
  assert.deepEqual(await host.evaluate(code), { ok: true });
  const haps = host.query(0, 1);
  assert.deepEqual(
    haps.map((h) => [h.begin, h.end, h.whole.begin, h.whole.end, h.value.note]),
    [
      [0, 1 / 3, 0, 1 / 3, 'c'],
      [1 / 3, 2 / 3, 1 / 3, 2 / 3, 'e'],
      [2 / 3, 1, 2 / 3, 1, 'g'],
    ],
  );
  assert.deepEqual(haps.map((h) => h.locations), [[{ start: 6, end: 7 }], [{ start: 8, end: 9 }], [{ start: 10, end: 11 }]]);
  assert.deepEqual(haps.map((h) => source(code, h)), [['c'], ['e'], ['g']]);
  assert.deepEqual(JSON.parse(JSON.stringify(haps)), haps, 'query result is plain JSON');
});

test('$: blocks are stacked', async () => {
  const code = '$: note("c d")\n$: s("bd")';
  assert.deepEqual(await host.evaluate(code), { ok: true });
  const haps = host.query(0, 1);
  assert.deepEqual(haps.map((h) => [h.begin, h.value]), [
    [0, { note: 'c' }],
    [0.5, { note: 'd' }],
    [0, { s: 'bd' }],
  ]);
  assert.deepEqual(haps.map((h) => source(code, h)), [['c'], ['d'], ['bd']]);
});

test('syntax error returns ok:false with a line and keeps the previous pattern', async () => {
  assert.deepEqual(await host.evaluate('note("c")'), { ok: true });
  const js = await host.evaluate('note("c")\n  .fast(2\nnote("e")');
  assert.equal(js.ok, false);
  assert.equal(js.line, 3);
  assert.match(js.error, /Unexpected token/);
  const mini = await host.evaluate('stack(\n  note("c [e")\n)');
  assert.equal(mini.ok, false);
  assert.equal(mini.line, 2);
  assert.match(mini.error, /\[mini\] parse error/);
  assert.deepEqual(host.query(0, 1).map((h) => h.value), [{ note: 'c' }]);
});

test('runtime error returns ok:false without a line', async () => {
  const res = await host.evaluate('nosuchfunction("c")');
  assert.equal(res.ok, false);
  assert.equal(res.line, undefined);
  assert.match(res.error, /nosuchfunction/);
});

test('evaluate after evaluate replaces the pattern', async () => {
  await host.evaluate('note("c e g")');
  await host.evaluate('note("d")');
  assert.deepEqual(host.query(0, 2).map((h) => [h.begin, h.value]), [
    [0, { note: 'd' }],
    [1, { note: 'd' }],
  ]);
});

test('.midi() haps go to the Rack event list on the frame timeline, not to superdough', async () => {
  assert.deepEqual(await host.evaluate('note("c e").midi()'), { ok: true });
  host.setCps(0.5);
  host.setClock(10); // cycle 0 sounds at context time 10 s
  await host.schedule(10, 13); // cycles [0, 1.5)
  await host.schedule(13, 14); // cycles [1.5, 2): windows tile without gaps or repeats
  const events = host.midiEvents();
  assert.deepEqual(
    events.map((e) => [e.t, e.duration, e.cps, e.value.note]),
    [
      [10, 1, 0.5, 'c'],
      [11, 1, 0.5, 'e'],
      [12, 1, 0.5, 'c'],
      [13, 1, 0.5, 'e'],
    ],
  );
  assert.deepEqual(host.midiEvents(), [], 'drained');
});
