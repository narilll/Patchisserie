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

// tick(frame, cycle, cps, epoch, playing, external, sr, lookahead)
const events = () => host.midiEvents().map((e) => [e.kind, e.begin, e.end, e.note, e.key, e.value]);

test('.midi() haps become Rack events in cycles, query windows tile without gaps or repeats', async () => {
  assert.deepEqual(await host.evaluate('note("c e").midi()'), { ok: true });
  host.midiEvents();
  host.tick(0, 0, 0.5, 1, true, false, 48000, 3); // cycles [0, 1.5)
  host.tick(72000, 0.75, 0.5, 1, true, false, 48000, 2.5); // [1.5, 2): from where the last one ended
  assert.deepEqual(events(), [
    [0, 0, 0.5, 48, '', 0],
    [0, 0.5, 1, 52, '', 0],
    [0, 1, 1.5, 48, '', 0],
    [0, 1.5, 2, 52, '', 0],
  ]);
  host.tick(96000, 1, 0.5, 1, false, false, 48000, 3); // stopped: nothing
  assert.deepEqual(events(), []);
});

test('.midi() TRIG from s, CC from ccn/ccv, velocity * gain, midichan', async () => {
  await host.evaluate('stack(s("bd hh"), ccn(74).ccv(0.25), note(60).velocity(0.5).gain(0.5).midichan(3)).midi()');
  host.midiEvents();
  host.tick(0, 0, 0.5, 2, true, false, 48000, 2);
  const all = host.midiEvents();
  assert.deepEqual(all.map((e) => [e.kind, e.begin, e.key, e.value]), [
    [1, 0, 'bd', 0],
    [2, 0, '74', 0.25],
    [0, 0, '', 0],
    [1, 0.5, 'hh', 0],
  ]);
  assert.deepEqual([all[2].note, all[2].velocity, all[2].midichan], [60, 0.25, 3]);
});

test('setcps and .cps() become tempo events', async () => {
  await host.evaluate('setcps(1)\nnote("c").midi()');
  host.midiEvents();
  host.tick(0, 0, 0.5, 3, true, false, 48000, 2); // the Sequencer still runs at 0.5: tempo 1 from cycle 0
  assert.deepEqual(events(), [
    [3, 0, 0, 0, '', 1],
    [0, 0, 1, 48, '', 0],
  ]);
  await host.evaluate('note("c d").cps("<1 2>").midi()');
  host.midiEvents();
  host.tick(0, 0, 1, 4, true, false, 48000, 2); // [0, 2)
  assert.deepEqual(
    events().filter((e) => e[0] === 3),
    [[3, 1, 1, 0, '', 2]],
  ); // cycle 0 asks for 1 (already the tempo), cycle 1 for 2
});

test('an evaluate takes over after what is queried, a new epoch starts over at the cycle', async () => {
  await host.evaluate('note("c*4").midi()');
  host.tick(0, 0, 0.5, 5, true, false, 48000, 1); // [0, 0.5)
  host.midiEvents();
  await host.evaluate('note("e*4").midi()');
  const notes = () => events().filter((e) => e[0] === 0).map((e) => [e[1], e[3]]);
  host.tick(24000, 0.25, 0.5, 5, true, false, 48000, 1); // [0.5, 0.75): the new pattern
  assert.deepEqual(notes(), [[0.5, 52]]);
  host.tick(0, 0, 0.5, 6, true, false, 48000, 0.5); // play again at 0
  assert.deepEqual(notes(), [[0, 52]]);
});

test('onset times follow tempo changes still ahead, also one at the snapshot cycle (PLAY pending)', async () => {
  globalThis.__times = [];
  await host.evaluate('setcps(1)\nnote("c*4").onSchedule((hap, t) => __times.push(t), true)');
  // A new epoch, the clock waiting at cycle 0 (frame 48000) with 0.5 cps: tempo 1 from cycle 0, so onsets 0.25 s apart.
  host.tick(48000, 0, 0.5, 20, true, false, 48000, 0.5); // [0, 0.25)
  host.tick(48000, 0, 0.5, 20, true, false, 48000, 1); // [0.25, 0.5), the tempo event not applied yet
  host.tick(72000, 0.5, 1, 20, true, false, 48000, 0.5); // applied: the snapshot has it, [0.5, 1)
  assert.deepEqual(__times, [1, 1.25, 1.5, 1.75]);
});

test('a slower tempo from a hap on: that hap is still on the sample the Sequencer fires it', async () => {
  globalThis.__times = [];
  await host.evaluate('setcps(1)\nnote("c").cps(0.5).late(0.30001).onSchedule((hap, t) => __times.push(t), true)');
  // Cycle 0.30001 at 1 cps is frame 14400.48: the Sequencer fires it (and slows down) on frame 14401.
  host.tick(0, 0, 1, 40, true, false, 48000, 1);
  assert.deepEqual(__times, [14401 / 48000]);
});

test('behind (a stall): what can no longer sound on time is skipped, not played late', async () => {
  await host.evaluate('note("c*8").midi()');
  host.tick(0, 0, 0.5, 30, true, false, 48000, 0.1); // [0, 0.05)
  host.midiEvents();
  // 1 s later, the render thread at 1.02 s: the window starts at what is still ahead of it (+ 10 ms).
  host.tick(48000, 0.5, 0.5, 30, true, false, 48000, 0.5, 48960);
  assert.deepEqual(events().map((e) => e[1]), [0.625]);
});

// Query errors are Strudel's (logged, no haps); a query the engine interrupts (endless loop) reverts.
test('revert() goes back to the pattern before the last evaluate, then to silence', async () => {
  await host.evaluate('note("c").midi()');
  await host.evaluate('note("e").fmap((v) => { for (;;); }).midi()');
  host.revert();
  host.midiEvents();
  host.tick(0, 0, 0.5, 8, true, false, 48000, 2);
  assert.deepEqual(events().filter((e) => e[0] === 0), [[0, 0, 1, 48, '', 0]]);
  host.revert();
  assert.deepEqual(host.query(0, 1), []);
});
