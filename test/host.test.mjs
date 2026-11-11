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

// tick(frame, cycle, cps, epoch, playing, external, sr, lookahead, rendered, reset)
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

test('RESET: from cycle 0, what can no longer sound on time moves to the first frame both can make', async () => {
  globalThis.__times = [];
  await host.evaluate('note("c*8").onSchedule((hap, t) => __times.push(t), false).midi()');
  host.tick(0, 0, 0.5, 50, true, false, 48000, 0.2);
  host.midiEvents();
  globalThis.__times = [];
  // Cycle 0 on frame 100000; the JS sees the reset at frame 104800 (cycle 0.05), the render thread at 105728: the
  // first frame both can still make is 106208 (+ 10 ms). Cycle 0 goes there, sound and .midi() event (duration kept);
  // cycle 0.125 is still on time, on its frame.
  host.tick(104800, 0.05, 0.5, 51, true, false, 48000, 0.2, 105728, true);
  const late = 0.05 + (1408 / 48000) * 0.5;
  host.tick(105760, 0.06, 0.5, 51, true, false, 48000, 0.5, 106688, true); // [0.15, 0.31): as usual
  assert.deepEqual(__times, [106208 / 48000, 112000 / 48000, 124000 / 48000]);
  assert.deepEqual(events(), [
    [0, late, 0.125 + late, 48, '', 0],
    [0, 0.125, 0.25, 48, '', 0],
    [0, 0.25, 0.375, 48, '', 0],
  ]);
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

// tick() -> rackHighlight(id, from, to, start, end): what the editor highlights while it plays.
test('tick() publishes each scheduled hap location with its frames and the code id', async () => {
  const code = 'note("c e").midi()';
  assert.deepEqual(await host.evaluate(code, 7), { ok: true });
  host.highlights();
  host.tick(0, 0, 0.5, 60, true, false, 48000, 2); // [0, 1): 1 cycle = 96000 frames
  const got = host.highlights();
  assert.deepEqual(got, [
    { id: 7, from: 0, to: 48000, start: 6, end: 7 },
    { id: 7, from: 48000, to: 96000, start: 8, end: 9 },
  ]);
  assert.deepEqual(got.map((h) => code.slice(h.start, h.end)), ['c', 'e']);
  host.tick(96000, 1, 0.5, 60, false, false, 48000, 2); // stopped: none
  assert.deepEqual(host.highlights(), []);
});

test('highlights keep the id of the playing code: a failed evaluate keeps it, revert() goes back', async () => {
  await host.evaluate('s("bd")', 8);
  assert.equal((await host.evaluate('s("bd"', 9)).ok, false);
  host.highlights();
  host.tick(0, 0, 0.5, 61, true, false, 48000, 1);
  assert.deepEqual(host.highlights().map((h) => h.id), [8]);
  await host.evaluate('s("hh")', 10);
  host.revert();
  host.tick(0, 0, 0.5, 62, true, false, 48000, 1);
  assert.deepEqual(host.highlights().map((h) => [h.id, h.start]), [[8, 3]]);
});

test('a RESET late onset is highlighted from the frame it plays on, for its whole duration', async () => {
  await host.evaluate('note("c*8")', 11);
  host.tick(0, 0, 0.5, 70, true, false, 48000, 0.2);
  host.highlights();
  // As in the RESET test above: cycle 0 plays at frame 106208, 0.125 cycles (12000 frames) long.
  host.tick(104800, 0.05, 0.5, 71, true, false, 48000, 0.2, 105728, true);
  const c = host.highlights().filter((h) => h.start === 6); // "c"; "8" (8, 9) is highlighted with it
  assert.deepEqual(c.slice(0, 2).map((h) => [h.from, h.to]), [
    [106208, 118208],
    [112000, 124000],
  ]);
});

// @strudel/midi's midin() on Filo's MIDI input: host.midiCC and host.midiDevice stand in for the engine's natives.
test("midin(): cc(n, chan?) is a pattern of the input's latest CC value when queried; no device: warns once", async () => {
  const { midiCC, midiDevice } = host;
  const warn = console.warn;
  const lines = [];
  const values = new Map(); // `${chan} ${cc}` -> value (chan 0: any)
  console.warn = (...a) => lines.push(a.map(String).join(' '));
  host.midiCC = (cc, chan) => values.get(`${chan} ${cc}`) ?? 0;
  try {
    const code =
      "let cc = await midin('My controller')\nglobalThis.__cc = await midin('Other')\n" +
      "globalThis.__promise = midin('Third')\n" +
      '$: note("c").lpf(cc(74).range(0, 1000)).lpq(cc(71, 2))';
    assert.deepEqual(await host.evaluate(code), { ok: true });
    assert.deepEqual(lines, ["[midi] midin('My controller'): choose a MIDI device in Filo's menu"]);
    assert.ok(globalThis.__promise instanceof Promise);
    assert.equal(typeof globalThis.__cc, 'function');
    assert.ok(globalThis.__cc(74) instanceof host.pattern.constructor);
    const controls = (b, e) => host.query(b, e).map((h) => [h.value.cutoff, h.value.resonance]);
    assert.deepEqual(controls(0, 1), [[0, 0]]); // before any CC
    values.set('0 74', 0.5).set('2 71', 1).set('0 71', 0.25);
    assert.deepEqual(controls(1, 2), [[500, 1]]); // read at query time, no evaluate needed
    host.midiDevice = () => true;
    assert.deepEqual(await host.evaluate(code), { ok: true });
    assert.equal(lines.length, 1);
    const pattern = await host.evaluate('let cc = await midin("IAC")');
    assert.equal(pattern.ok, false);
    assert.match(pattern.error, /Midi input cannot be a pattern/);
  } finally {
    console.warn = warn;
    Object.assign(host, { midiCC, midiDevice });
  }
});

// @strudel/midi's midikeys() on the same input. keys(notes, frame, cycle, cps, playing, sr, delay, rendered) stands in
// for the engine's poll: 5 numbers per MIDI message (its frame, status nibble, channel 0-15, note, velocity).
test('midikeys(): kb(noteLength) plays each note-on once, at once, `delay` frames after it', async () => {
  const { midiDevice } = host;
  const warn = console.warn;
  const lines = [];
  console.warn = (...a) => lines.push(a.map(String).join(' '));
  globalThis.__sched = [];
  try {
    const code =
      "globalThis.__promise = midikeys('Keys')\nconst kb = await __promise\nglobalThis.__kb = kb\n" +
      "let cc = await midin('Keys')\n$: kb().midi()\n" +
      '$: kb("0.25 1").onSchedule((hap, t) => __sched.push([t * 48000, hap.duration.valueOf(), hap.value]), true)';
    assert.deepEqual(await host.evaluate(code), { ok: true });
    assert.deepEqual(lines, ["[midi] midikeys('Keys'): choose a MIDI device in Filo's menu"]); // midin()'s too
    assert.ok(globalThis.__promise instanceof Promise);
    assert.ok(globalThis.__kb() instanceof host.pattern.constructor);
    host.tick(0, 0, 0.5, 90, true, false, 48000, 0.1); // [0, 0.05)
    host.midiEvents();
    // At frame 1256, the render thread at 2280: frame 1000's note-on plays at 2856 (cycle 0.02975); its note-off and a
    // note-on with velocity 0 change nothing.
    const notes = [1000, 9, 2, 60, 127, 1000, 8, 2, 60, 0, 1010, 9, 0, 62, 0];
    host.keys(notes, 1256, 1256 / 96000, 0.5, true, 48000, 1856, 2280);
    assert.deepEqual(events(), [[0, 0.02975, 0.52975, 60, '', 0]]); // kb(): 0.5 cycles
    assert.deepEqual(__sched.splice(0), [[2856, 0.25, { midikey: 'Keys_60', note: 60, velocity: 1, midichan: 3 }]]);
    // The JS too late for frame 2000 + 1856: on the first frame both can still make, the render thread's + 10 ms.
    host.keys([2000, 9, 0, 64, 64], 3000, 3000 / 96000, 0.5, true, 48000, 1856, 4000);
    assert.deepEqual(events().map((e) => [e[1] * 96000, e[3]]), [[4480, 64]]);
    assert.deepEqual(__sched.splice(0).map((s) => s[0]), [4480]);
    // kb("0.25 1"): the second half of the cycle is 1 cycle long.
    host.keys([48144, 9, 0, 65, 100], 48200, 48200 / 96000, 0.5, true, 48000, 1856, 49000);
    assert.deepEqual(__sched.splice(0).map((s) => [s[0], s[1]]), [[50000, 1]]);
    events();
    // Played, then forgotten: the query loop's windows over them get nothing; stopped, notes are ignored.
    host.tick(2400, 0.025, 0.5, 90, true, false, 48000, 1); // [0.05, 0.525)
    assert.deepEqual(host.query(0, 2), []);
    host.keys([60000, 9, 0, 60, 100], 60000, 0.625, 0.5, false, 48000, 1856, 61024);
    assert.deepEqual([events(), __sched], [[], []]);
  } finally {
    console.warn = warn;
    host.midiDevice = midiDevice;
  }
});

test("strudel.cc's widgets, outputs and inputs Rack doesn't have are ignored: the code plays, each warns once", async () => {
  const warn = console.warn;
  const lines = [];
  console.warn = (...a) => lines.push(a.map(String).join(' '));
  const code = `await loadCsound\`instr Organ
endin\`
const pad = gamepad(0)
$: note("c e g").osc().serial().mqtt()._pianoroll()
$: note("c3").csound('Organ').lpf(slider(800, 100, 2000)).gain(pad.a.add(1))._scope()
$: n(accX.segment(4).range(0, 7)).scale("C:minor").markcss('color: red')`;
  let haps;
  try {
    for (let i = 0; i < 2; i++) {
      assert.deepEqual(await host.evaluate(code), { ok: true });
      haps = host.query(0, 1);
    }
  } finally {
    console.warn = warn;
  }
  assert.deepEqual(
    haps.map((h) => h.value.note),
    ['c', 'e', 'g', 'c3', 'C3', 'C3', 'C3', 'C3'],
  );
  assert.deepEqual([haps[3].value.cutoff, haps[3].value.gain], [800, 1]); // slider()'s value; gamepad's a: 0
  assert.deepEqual(lines.sort(), [
    "[csound] .csound(): Csound is not available in Rack, the pattern plays through superdough",
    '[csound] Csound is not available in Rack: loadCsound() does nothing',
    '[draw] Strudel canvas visuals and editor widgets (scope, pianoroll, ...) are not shown in Rack',
    '[gamepad] gamepad() is not available in Rack: its axes and buttons stay 0',
    '[motion] device motion is not available in Rack: its signals stay 0',
    '[mqtt] .mqtt(): MQTT is not available in Rack, the pattern plays through superdough',
    '[osc] .osc(): OSC (SuperDirt) is not available in Rack, the pattern plays through superdough',
    '[serial] .serial(): serial is not available in Rack, the pattern plays through superdough',
    "[slider] slider() is strudel.cc's editor widget: in Rack it stays at its value",
  ]);
});
