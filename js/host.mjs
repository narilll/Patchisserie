// Patchisserie host: Strudel's own repl() driven by the plugin instead of a browser clock.
// Bundled by scripts/build-engine.mjs into res/engine.js; runs unmodified in QuickJS and Node.
import './polyfills.mjs';
import * as core from '@strudel/core';
import * as mini from '@strudel/mini';
import * as tonal from '@strudel/tonal';
import * as xen from '@strudel/xen';
import * as edo from '@strudel/edo';
import * as webaudio from '@strudel/webaudio';
import { registerTranspilerPlugin, transpiler } from '@strudel/transpiler';
import { H } from '@strudel/hydra';

// Same scope as strudel.cc (website/src/repl/util.mjs loadModules) minus visual/IO packages; soundfonts join in
// initAudio(). Of @strudel/hydra: its H() and our initHydra(); hydra.ojack.xyz's loadScript(); @strudel/midi's midin()
// and midikeys() (below).
const ready = core.evalScope(core, edo, tonal, mini, xen, webaudio, { H, initHydra, loadScript, midin, midikeys });

// Strudel's canvas visuals need a canvas, which Rack doesn't have (out of v1). As upstream without a `window` (ours is
// for superdough): .draw() returns the pattern, so .scope(), .spiral() etc. play on and draw nothing; the few that
// take a canvas before drawing do the same. They warn once.
let drawSaid = false;
const noCanvas = (result) =>
  function () {
    if (!drawSaid) (drawSaid = true), console.warn('[draw] Strudel canvas visuals (scope, pianoroll, ...) are not shown in Rack');
    return result ?? this;
  };
for (const name of ['draw', 'pianoroll', 'pitchwheel']) core.Pattern.prototype[name] = noCanvas();
core.Pattern.prototype.animate = noCanvas(core.silence);

let evalError;
let seconds = 0; // playback position of the last tick()
const { evaluate, scheduler, setCps } = core.repl({
  transpiler,
  getTime: () => seconds,
  // The host queries the pattern itself, so the Cyclist must never tick.
  setInterval: () => 0,
  clearInterval: () => {},
  onEvalError: (err) => (evalError = err),
});

const time = (t) => t.valueOf();
// Strudel's dispatch: webaudio unless a dominant onSchedule (e.g. .midi()) takes the hap; errors go to the log.
const onSchedule = core.getSchedule({ onSchedule: webaudio.webaudioOutput });
let previous; // the pattern before the last evaluate: back to it when the engine interrupts a query of the new one
// The engine's id of the code the pattern (and `previous`) came from: highlights say which code their offsets are in.
let [patternId, previousId] = [0, 0];
const highlights = []; // where highlights go without an engine (tests): see rackHighlight below

// .midi() sends the pattern to Rack instead of superdough, through Strudel's own mechanism (portable to strudel.cc).
// Events (src/dsp.hpp filo::Event) carry cycles: the Sequencer fires them when its cycle gets there. Values as in
// Strudel's @strudel/midi: note (name or number), velocity * gain, midichan; a hap without note but with s is a TRIG,
// ccn/ccv a CC, both mapped to channels on the C++ side.
const [Note, Trig, CC, Tempo] = [0, 1, 2, 3];
const midiEvents = []; // where events go without an engine (tests): see rackEvent below
// Cycles dispatch() shifts the hap it schedules by (late onsets). toRack runs within that call: .midi() is dominant, so
// getSchedule calls it before its first await.
let rackShift = 0;
const toRack = (hap) => {
  const v = hap.value;
  const [begin, end, chan] = [time(hap.whole.begin) + rackShift, time(hap.whole.end) + rackShift, v.midichan ?? -1];
  if (v.note !== undefined) {
    const note = typeof v.note === 'number' ? v.note : core.noteToMidi(v.note);
    api.rackEvent(Note, begin, end, note, (v.gain ?? 1) * (v.velocity ?? 0.9), chan, '', 0);
  } else if (v.s !== undefined) api.rackEvent(Trig, begin, end, 0, 0, chan, String(v.s), 0);
  if (v.ccn !== undefined && v.ccv !== undefined) api.rackEvent(CC, begin, end, 0, 0, chan, String(v.ccn), v.ccv);
};
core.register('midi', (pat) => pat.onSchedule(toRack, true));

// @strudel/midi's midin(input) on Filo's MIDI input: the device is the one chosen in Filo's menu, `input` only names
// it. As upstream, cc(n, chan?) is a pattern of CC n's latest value / 127 (on MIDI channel chan 1-16, or any), 0 until
// one arrives, read whenever Strudel queries it. Only CC messages count (upstream takes any message's data bytes).
// ponytail: values aren't persisted (upstream: localStorage per device, sent back to it on connect): 0 after a reload.
let midiSaid = false; // the no-device warning (midin() and midikeys() alike), once per evaluate
function midiInput(name, input) {
  if (core.isPattern(input)) {
    const hint = "Make sure to pass device name with single quotes. Example: midin('IAC Driver Bus 1')";
    throw new Error(`[midi] Midi input cannot be a pattern. ${hint}`);
  }
  if (!api.midiDevice() && !midiSaid) {
    midiSaid = true;
    console.warn(`[midi] ${name}('${input}'): choose a MIDI device in Filo's menu`);
  }
}
async function midin(input) {
  midiInput('midin', input);
  return (cc, chan) => core.ref(() => api.midiCC(cc, chan ?? 0));
}

// @strudel/midi's midikeys(input) on the same input: kb(noteLength = 0.5) is a pattern of the notes just played,
// { midikey: `${input}_${note}`, note, velocity: v / 127, midichan: 1-16 }, noteLength cycles long (a pattern too).
// Its query is upstream's: every note held, whatever the span. As upstream's trigger, a note-on doesn't wait for the
// query loop: keys() below holds it here only while it queries and dispatches it, so tick() never gets it.
const keyHaps = {}; // input -> the notes keys() is dispatching
async function midikeys(input) {
  midiInput('midikeys', input);
  keyHaps[input] ??= [];
  return (noteLength = 0.5) => {
    const lengths = core.reify(noteLength);
    return new core.Pattern((state) =>
      keyHaps[input].flatMap((hap) =>
        lengths.query(state.setSpan(hap.wholeOrPart())).map((length) => {
          const nl = length.value ?? 0.5;
          const whole = new core.TimeSpan(hap.whole.begin, hap.whole.begin.add(nl));
          const part = new core.TimeSpan(hap.part.begin, hap.part.begin.add(nl));
          return new core.Hap(whole, part, hap.value, hap.combineContext(length));
        }),
      ),
    );
  };
}

// strudel.cc's Dirt-Samples subset (website/src/repl/prebake.mjs), served from its CDN.
const dirtSamples = {
  casio: ['casio/high.wav', 'casio/low.wav', 'casio/noise.wav'],
  crow: ['crow/000_crow.wav', 'crow/001_crow2.wav', 'crow/002_crow3.wav', 'crow/003_crow4.wav'],
  insect: [
    'insect/000_everglades_conehead.wav',
    'insect/001_robust_shieldback.wav',
    'insect/002_seashore_meadow_katydid.wav',
  ],
  wind: [
    'wind/000_wind1.wav',
    'wind/001_wind10.wav',
    'wind/002_wind2.wav',
    'wind/003_wind3.wav',
    'wind/004_wind4.wav',
    'wind/005_wind5.wav',
    'wind/006_wind6.wav',
    'wind/007_wind7.wav',
    'wind/008_wind8.wav',
    'wind/009_wind9.wav',
  ],
  jazz: [
    'jazz/000_BD.wav',
    'jazz/001_CB.wav',
    'jazz/002_FX.wav',
    'jazz/003_HH.wav',
    'jazz/004_OH.wav',
    'jazz/005_P1.wav',
    'jazz/006_P2.wav',
    'jazz/007_SN.wav',
  ],
  metal: [
    'metal/000_0.wav',
    'metal/001_1.wav',
    'metal/002_2.wav',
    'metal/003_3.wav',
    'metal/004_4.wav',
    'metal/005_5.wav',
    'metal/006_6.wav',
    'metal/007_7.wav',
    'metal/008_8.wav',
    'metal/009_9.wav',
  ],
  east: [
    'east/000_nipon_wood_block.wav',
    'east/001_ohkawa_mute.wav',
    'east/002_ohkawa_open.wav',
    'east/003_shime_hi.wav',
    'east/004_shime_hi_2.wav',
    'east/005_shime_mute.wav',
    'east/006_taiko_1.wav',
    'east/007_taiko_2.wav',
    'east/008_taiko_3.wav',
  ],
  space: [
    'space/000_0.wav',
    'space/001_1.wav',
    'space/002_11.wav',
    'space/003_12.wav',
    'space/004_13.wav',
    'space/005_14.wav',
    'space/006_15.wav',
    'space/007_16.wav',
    'space/008_17.wav',
    'space/009_18.wav',
    'space/010_2.wav',
    'space/011_3.wav',
    'space/012_4.wav',
    'space/013_5.wav',
    'space/014_6.wav',
    'space/015_7.wav',
    'space/016_8.wav',
    'space/017_9.wav',
  ],
  numbers: [
    'numbers/0.wav',
    'numbers/1.wav',
    'numbers/2.wav',
    'numbers/3.wav',
    'numbers/4.wav',
    'numbers/5.wav',
    'numbers/6.wav',
    'numbers/7.wav',
    'numbers/8.wav',
  ],
  num: [
    'num/00.wav',
    'num/01.wav',
    'num/02.wav',
    'num/03.wav',
    'num/04.wav',
    'num/05.wav',
    'num/06.wav',
    'num/07.wav',
    'num/08.wav',
    'num/09.wav',
    'num/10.wav',
    'num/11.wav',
    'num/12.wav',
    'num/13.wav',
    'num/14.wav',
    'num/15.wav',
    'num/16.wav',
    'num/17.wav',
    'num/18.wav',
    'num/19.wav',
    'num/20.wav',
  ],
};

// Onset order like the Cyclist: by begin, cps changes first among equals.
const byOnset = (a, b) =>
  time(a.whole.begin) - time(b.whole.begin) || (b.value.cps !== undefined) - (a.value.cps !== undefined);
// Realtime query state, see tick(); `tl`: the timeline snapshot of the last tick() or keys().
const rt = { epoch: -1, from: 0, cps: 0, tempos: [], late: false };
const tl = { frame: 0, cycle: 0, cps: 0, sr: 48000, external: false };
const tempo = (at, value) => {
  rt.cps = Math.fround(value); // what the Sequencer stores
  api.rackEvent(Tempo, at, at, 0, 0, -1, '', rt.cps);
  if (!tl.external) rt.tempos.push({ cycle: at, cps: rt.cps });
};
// Frame of cycle b: through the tempo changes still ahead, each taking effect on the first sample at or after its
// cycle (with the Sequencer's kEps); snapped to that sample, the one the Sequencer fires on, never before the sample
// of the last change (that sample's cycle may be past b: back from it at a slower cps is a frame early).
const kEps = 1e-9;
const sampleAt = (f, c, k, b) => Math.max(f, Math.ceil(f + ((b - kEps - c) / k) * tl.sr));
const timeAt = (b) => {
  let [f, c, k] = [tl.frame, tl.cycle, tl.cps];
  for (const t of rt.tempos) {
    if (t.cycle > b) break;
    const p = sampleAt(f, c, k, t.cycle);
    [c, f, k] = [c + ((p - f) * k) / tl.sr, p, t.cps];
  }
  return sampleAt(f, c, k, b) / tl.sr;
};
const cpsAt = (b) => rt.tempos.reduce((k, t) => (t.cycle <= b ? t.cps : k), tl.cps);
// Haps (by onset) to Strudel's dispatch: sounds at their onset's frame, .midi() events at its cycle, an onset before
// `earliest` (a cycle) moved there, duration kept; their mini-notation locations to rackHighlight() with those frames.
function dispatch(haps, earliest) {
  for (const hap of haps) {
    const onset = time(hap.whole.begin);
    const b = Math.max(onset, earliest);
    const cpsHap = hap.value.cps;
    if (cpsHap !== undefined && Math.fround(cpsHap) !== rt.cps) {
      tempo(b, cpsHap); // .cps(): like the Cyclist, the global tempo from this hap on
      scheduler.setCps(cpsHap);
    }
    rackShift = b - onset;
    const t = timeAt(b);
    onSchedule(hap, t, cpsHap ?? cpsAt(b));
    rackShift = 0;
    const locations = hap.context.locations ?? [];
    if (!locations.length) continue;
    const [from, to] = [Math.round(t * tl.sr), Math.round(timeAt(time(hap.whole.end) + b - onset) * tl.sr)];
    for (const { start, end } of locations) api.rackHighlight(patternId, from, to, start, end);
  }
}

const api = (globalThis.__patchisserie = {
  async evaluate(code, id = 0) {
    await ready;
    evalError = undefined;
    const before = scheduler.pattern;
    const hydraBefore = strudelGlobals !== undefined;
    restoreStrudel(); // Strudel's names again, unless this code calls initHydra()
    hydraCalled = false;
    said.clear();
    midiSaid = false;
    try {
      await evaluate(code, false); // false: don't start the Cyclist; errors land in onEvalError
    } catch (err) {
      evalError = err; // repl throws (outside its try) only for empty code
    }
    if (!evalError) {
      if (!hydraCalled) api.hydraHush();
      [previous, previousId, patternId] = [before, patternId, id];
      return { ok: true };
    }
    if (hydraBefore && !strudelGlobals) installHydra(); // the previous code plays on, with its Hydra globals
    // acorn syntax errors carry loc; mini-notation errors only say "at line N" in the message.
    const line = evalError.loc?.line ?? Number(/^\[mini\] parse error at line (\d+)/.exec(evalError.message)?.[1]);
    return line ? { ok: false, error: evalError.message, line } : { ok: false, error: evalError.message };
  },

  query(begin, end) {
    if (!scheduler.pattern) return [];
    return scheduler.pattern.queryArc(begin, end, { _cps: scheduler.cps }).map((hap) => ({
      begin: time(hap.part.begin),
      end: time(hap.part.end),
      ...(hap.whole && { whole: { begin: time(hap.whole.begin), end: time(hap.whole.end) } }),
      value: hap.value,
      locations: hap.context.locations ?? [],
    }));
  },

  // superdough on the engine's AudioContext, with strudel.cc's built-in sounds (its prebake minus the online banks).
  async initAudio() {
    await ready;
    await webaudio.initAudio();
    webaudio.registerSynthSounds();
    webaudio.registerZZFXSounds();
    // Dynamic, as in strudel.cc's prebake: soundfont2 reads `window` at import time.
    const soundfonts = await import('@strudel/soundfonts');
    await core.evalScope(soundfonts);
    soundfonts.registerSoundfonts();
  },

  // strudel.cc's prebake (website/src/repl/prebake.mjs) on top of initAudio(): the same CDN sample maps and bank
  // aliases. Failed maps are logged, the others still load.
  async prebake() {
    const cdn = 'https://strudel.b-cdn.net';
    const drums = { prebake: true, tag: 'drum-machines' };
    const maps = [
      [`${cdn}/piano.json`, `${cdn}/piano/`, { prebake: true }],
      [`${cdn}/vcsl.json`, `${cdn}/VCSL/`, { prebake: true }],
      [`${cdn}/tidal-drum-machines.json`, `${cdn}/tidal-drum-machines/machines/`, drums],
      [`${cdn}/uzu-drumkit.json`, `${cdn}/uzu-drumkit/`, drums],
      [`${cdn}/uzu-wavetables.json`, `${cdn}/uzu-wavetables/`, { prebake: true }],
      [`${cdn}/mridangam.json`, `${cdn}/mrid/`, drums],
      [dirtSamples, `${cdn}/Dirt-Samples/`, { prebake: true }],
    ];
    const results = await Promise.allSettled(maps.map((args) => webaudio.samples(...args)));
    for (const r of results) if (r.status === 'rejected') core.logger(`[prebake] ${r.reason?.message}`, 'warning');
    await webaudio.aliasBank(`${cdn}/tidal-drum-machines-alias.json`).catch((e) => core.logger(`[prebake] ${e}`, 'warning'));
  },

  setCps,

  /**
   * The realtime query loop (src/engine.cpp calls it every ~20 ms) on the audio thread's timeline: at playback frame
   * `frame` the Sequencer is at `cycle` with `cps`. Sounds go to superdough at context time = playback frame / sr,
   * .midi() events to the Sequencer in cycles, so both land on the same sample. Queries up to `lookahead` s ahead,
   * each window starting where the last one ended: like the Cyclist, a new pattern takes over after what is already
   * scheduled, for sounds and .midi() events alike. A new epoch (play, stop) starts over at `cycle`, after a RESET
   * (`reset`) at cycle 0, a few frames back.
   * external: the CLOCK input drives the cycle, cps is the PLL's estimate. rendered: the render thread's frame.
   * Every scheduled hap's mini-notation locations go to rackHighlight() with the frames it plays.
   */
  tick(frame, cycle, cps, epoch, playing, external, sr, lookahead, rendered = -Infinity, reset = false) {
    seconds = frame / sr;
    if (epoch !== rt.epoch) {
      Object.assign(rt, { epoch, from: reset ? 0 : cycle, cps: Math.fround(cps), tempos: [], late: reset });
    }
    if (!playing || !scheduler.pattern || !(cps > 0)) return;
    // What would start before the render thread's frame + 10 ms can no longer sound on time. Behind (a stall), it is
    // skipped, sounds and .midi() events alike, not played late. In a RESET's first query (from cycle 0) it plays late
    // instead, all on the first frame left, sounds and .midi() events together (`earliest`: no tempo change ahead yet).
    // ponytail: a RESET the JS sees only after a stall piles everything since cycle 0 onto that frame.
    const soon = Math.max(frame, rendered + 0.01 * sr);
    const from = rt.late ? rt.from : Math.max(rt.from, cycle + ((soon - frame) / sr) * cps);
    const earliest = rt.late ? cycle + ((Math.ceil(soon) - frame) / sr) * cps : -Infinity;
    const end = cycle + lookahead * cps;
    if (end <= from) return;
    // Applied by now (fired before the snapshot's sample, part of its cps): a change at `cycle` itself fires on it.
    rt.tempos = rt.tempos.filter((t) => t.cycle > cycle - cps / sr + 1e-9);
    Object.assign(tl, { frame, cycle, cps, sr, external });
    if (Math.fround(scheduler.cps) !== rt.cps) tempo(Math.max(from, earliest), scheduler.cps); // setcps(), setcpm()
    const haps = scheduler.pattern
      .queryArc(from, end, { _cps: scheduler.cps })
      .filter((hap) => hap.hasOnset())
      .sort(byOnset);
    dispatch(haps, earliest);
    rt.from = end;
    rt.late = false;
  },

  /**
   * midikeys()' notes from Filo's MIDI input as soon as the engine sees them (src/engine.cpp polls every 1 ms between
   * ticks): `notes` has 5 numbers per message (its playback frame, status nibble, channel 0-15, note, velocity), the
   * rest is the timeline as tick() gets it. As upstream's trigger, each note-on becomes a hap of every midikeys()
   * input, `delay` frames after its own (or on the first frame both sound and .midi() can still make, as tick()'s
   * RESET onsets); the playing pattern is queried around them and what comes from midikeys() is dispatched as tick()
   * does, then they are forgotten. Note-offs (and note-ons with velocity 0) are ignored, as upstream: fixed lengths.
   */
  keys(notes, frame, cycle, cps, playing, sr, delay, rendered = -Infinity) {
    if (!playing || !scheduler.pattern || !(cps > 0) || !Object.keys(keyHaps).length) return;
    const soon = Math.ceil(Math.max(frame, rendered + 0.01 * sr)); // tick()'s
    // ponytail: at the snapshot's cps; a tempo change within the delay moves a note's frame (sound and gate still
    // together). Through rt.tempos (timeAt()'s inverse) if that ever matters.
    const cycleOf = (f) => cycle + ((f - frame) / sr) * cps;
    let [from, to] = [Infinity, -Infinity];
    for (let i = 0; i + 5 <= notes.length; i += 5) {
      const [arrival, status, channel, note, velocity] = notes.slice(i, i + 5);
      if (status !== 9 || velocity === 0) continue;
      const c = cycleOf(Math.max(arrival + delay, soon));
      const span = new core.TimeSpan(c, c);
      for (const input in keyHaps) {
        const midikey = `${input}_${note}`;
        const value = { midikey, note: Math.round(note), velocity: velocity / 127, midichan: channel + 1 };
        keyHaps[input].push(new core.Hap(span, span, value, {}));
      }
      [from, to] = [Math.min(from, c), Math.max(to, c)];
    }
    if (from > to) return;
    Object.assign(tl, { frame, cycle, cps, sr });
    let haps;
    try {
      haps = scheduler.pattern.queryArc(from - 1e-6, to + 1e-6, { _cps: scheduler.cps });
    } finally {
      for (const input in keyHaps) keyHaps[input] = [];
    }
    dispatch(haps.filter((hap) => hap.value?.midikey !== undefined && hap.hasOnset()).sort(byOnset), cycleOf(soon));
  },

  /** Where .midi() events go (kind, begin, end, note, velocity, midichan, key, value); the engine replaces it. */
  rackEvent: (kind, begin, end, note, velocity, midichan, key, value) =>
    midiEvents.push({ kind, begin, end, note, velocity, midichan, key, value }),
  /** Drains the events collected by the default rackEvent. */
  midiEvents: () => midiEvents.splice(0),
  /** Where highlights go (code id, frames [from, to), UTF-16 offsets [start, end)); the engine replaces it. */
  rackHighlight: (id, from, to, start, end) => highlights.push({ id, from, to, start, end }),
  /** Drains the highlights collected by the default rackHighlight. */
  highlights: () => highlights.splice(0),
  /** midin()'s source: CC cc's latest value (0..1) on MIDI channel chan (1-16; 0: any), 0 before one arrives, and
   * whether a MIDI device is chosen in Filo's menu. The engine replaces them. */
  midiCC: (cc, chan) => 0,
  midiDevice: () => false,

  /** A query of the playing pattern was interrupted (endless loop): back to the one before the last evaluate, then
   * silence. (Query errors are Strudel's own: logged, no haps.) */
  revert() {
    scheduler.pattern = previous ?? core.silence;
    [previous, patternId, previousId] = [undefined, previous ? previousId : 0, 0];
  },

  /** superdough's orbits and buses back to their initial state; their nodes become garbage. */
  resetAudio: () => webaudio.resetGlobalEffects(),

  /**
   * Icing's frame (src/engine.cpp, once per UI frame): Hydra's tick() at `time` (s, Icing's clock) while Filo plays
   * `cycle`, with Meyda's loudness of the AUDIO input (24 Bark bands, total). Returns null (no Hydra, or its fps
   * skipped this frame) or [display (-1: render() all four, 0-3: render(oN)), time, width, height, then per output:
   * its program id (0: none), n, its n uniform values]. Hydra's bpm stays its own (30, or what the code sets), as on
   * strudel.cc: its arrays (fast(), smooth()) run at that pace.
   */
  hydraFrame(time, cycle, specific, total) {
    if (!hydra) return null;
    frameCycle = cycle;
    features = { loudness: { specific, total } };
    const dt = frameTime === undefined ? 0 : Math.max(0, time - frameTime) * 1000;
    frameTime = time;
    ticked = false;
    once(() => hydra.tick(dt));
    if (!ticked) return null;
    const out = [hydra.isRenderingAll ? -1 : (hydra.output.id ?? 0), hydra.synth.time, hydra.canvas.width];
    out.push(hydra.canvas.height);
    for (const o of hydra.o) out.push(o.program, o.values.length, ...o.values);
    return out;
  },
  /** Hydra's hush(): every output black, update() a no-op (Filo's STOP, an evaluate without initHydra()). */
  hydraHush() {
    if (hydra && visible) once(() => hydra.hush());
    visible = false;
  },
  /** Where Output's shaders go (output, GLSL ES source, uniform names, which of them are samplers); returns the
   * program's id. The engine replaces it. */
  hydraProgram(output, frag, uniforms, samplers) {
    hydraPrograms.push({ id: ++programs, output, frag, uniforms, samplers });
    return programs;
  },
  /** Drains the programs collected by the default hydraProgram. */
  hydraPrograms: () => hydraPrograms.splice(0),

  // Test seam: the upstream tune regression stringifies haps with hap.show() like Strudel's own test.
  get pattern() {
    return scheduler.pattern;
  },
});

// strudel.cc's Pattern.piano() (website/src/repl/prebake.mjs).
const maxPan = core.noteToMidi('C8');
const panwidth = (pan, width) => pan * width + (1 - width) / 2;
core.Pattern.prototype.piano = function () {
  return this.fmap((v) => ({ ...v, clip: v.clip ?? 1 }))
    .s('piano')
    .release(0.1)
    .fmap((value) => {
      const midi = core.valueToMidi(value);
      const pan = panwidth(Math.min(Math.round(midi) / maxPan, 1), 0.5);
      return { ...value, pan: (value.pan || 1) * pan };
    });
};

// --- Icing: Hydra (plan item 8) ---
// hydra-synth's own renderer - its generator, eval sandbox, tick(), hush() and Audio's a.fft binning - minus WebGL:
// Output keeps each output's shader and evaluates its uniforms per frame, Icing compiles and draws them on Rack's UI
// thread (src/hydra.cpp). initHydra() installs Hydra's globals (hydra-synth's makeGlobal, as on strudel.cc), the next
// evaluate() restores Strudel's. The visuals belong to code that calls initHydra(): an evaluate without it hushes
// them (their functions would see Strudel's globals), and so does Filo's STOP (strudel.cc's clearHydra()).
let hydraReady, hydra; // the renderer, created by the first initHydra()
let strudelGlobals; // while Hydra's are installed: the globals they replaced, name -> property descriptor or undefined
let hydraCalled = false; // initHydra() ran in this evaluate
let visible = false; // something drew since the last hush
let features = null; // this frame's AUDIO analysis, as Meyda's features (hydra-synth's Audio reads it)
let [frameTime, frameCycle, ticked] = [undefined, 0, false];
const hydraPrograms = []; // where programs go without an engine (tests): see hydraProgram
let programs = 0;
scheduler.now = () => frameCycle; // Strudel's getTime() (H()): the cycle Filo plays at the frame

class Output {
  constructor(id) {
    Object.assign(this, { id, label: `o${id}`, precision: 'mediump', uniforms: {}, program: 0, fns: [], values: [] });
  }
  /** GlslSource.out(): this output's new shader. Its uniforms are functions, evaluated by tick() per frame. */
  render([pass]) {
    const names = Object.keys(pass.uniforms);
    this.fns = Object.values(pass.uniforms);
    this.samplers = names.map((n) => pass.frag.includes(`uniform sampler2D ${n};`));
    this.program = api.hydraProgram(this.id, pass.frag, names, this.samplers);
    visible = true;
  }
  tick(props) {
    // A sampler's value is its output's index (src/hydra.cpp samples that output's other buffer); -1: a source. A
    // float's is what WebGL's uniform1f() makes of it, ToNumber(): an array index out of range (undefined) is NaN, as
    // hydra-synth's smooth() before its first step; NaN reaches glUniform1f() as it does WebGL's. An RGBA8 buffer
    // can't keep it (Apple's GL stores 0, as Chrome's WebGL does): feedback recovers with the next finite frame.
    this.values = this.fns.map((f, i) => {
      const v = f({}, props);
      return v instanceof Output ? v.id : this.samplers[i] ? -1 : Number(v);
    });
    ticked = true;
  }
  resize() {}
  getCurrent() {
    return this;
  }
  getTexture() {
    return this;
  }
}

const regl = { texture: () => ({}), _refresh: () => {} }; // what hydra-synth's sources and setResolution() still call
const external = ['init', 'initCam', 'initImage', 'initVideo', 'initScreen', 'initStream'];

async function createHydra() {
  // Loaded here, not at startup: hydra-synth's module body needs window.addEventListener (src/audio/webaudio.js).
  const [{ default: HydraRenderer }, { default: Audio }] = await Promise.all([
    import('hydra-synth/src/hydra-synth.js'),
    import('hydra-synth/src/lib/audio.js'),
  ]);
  class Hydra extends HydraRenderer {
    _initRegl() {
      this.regl = regl;
      this.renderAll = this.renderFbo = () => {};
    }
    _initOutputs(n) {
      this.o = Array.from({ length: n }, (_, i) => (this.synth[`o${i}`] = new Output(i)));
      this.output = this.o[0];
    }
    // ponytail: Audio without its constructor (a canvas, the microphone): its defaults copied, as of hydra-synth 1.3.29.
    _initAudio() {
      this.synth.a = Object.assign(Object.create(Audio.prototype), {
        vol: 0,
        scale: 10,
        max: 15,
        cutoff: 2,
        smooth: 0.4,
        isDrawing: false,
        beat: { holdFrames: 20, threshold: 40, _cutoff: 0, decay: 0.98, _framesSinceBeat: 0 },
        onBeat: () => {},
        meyda: { get: () => features },
        show() {}, // no analyser overlay in Icing
        hide() {},
        draw() {},
      });
      this.synth.a.setBins(4); // also sets the globals a0-a3
    }
  }
  const h = new Hydra({
    canvas: { width: 1280, height: 720 },
    precision: 'mediump', // blanked for GLSL 1.20 anyway; given, hydra-synth doesn't probe navigator.platform
    makeGlobal: false, // installHydra()
    autoLoop: false, // Icing's frames tick it
    enableStreamCapture: false,
  });
  h.s.forEach((s, i) => {
    for (const m of external)
      s[m] = () => {
        throw new Error(`s${i}.${m}(): Hydra's external sources are not available in Icing`);
      };
  });
  return h;
}

/** strudel.cc's initHydra(): Hydra's functions, outputs and values as globals for the rest of this evaluate. Its
 * options (canvas, DOM, microphone, Strudel canvas) are accepted and ignored. */
async function initHydra(options) {
  if (options?.feedStrudel) console.warn("[hydra] feedStrudel: Strudel's canvas isn't drawn in Rack, s0 stays empty");
  hydraCalled = true;
  hydra = await (hydraReady ??= createHydra());
  installHydra();
  return hydra;
}

/**
 * hydra.ojack.xyz's loadScript(url) (hydra-synth's global there): runs an extension's script in global scope, so its
 * setFunction() calls add to Hydra's functions. It starts Hydra as initHydra() does first: the editor's examples never
 * call initHydra(). The script comes through fetch(): https only, kept in the disk cache like samples. Unlike
 * hydra-synth's (which only logs), a failure rejects: the evaluate fails with the reason.
 */
async function loadScript(url) {
  await initHydra();
  try {
    (0, eval)(await (await fetch(url)).text());
  } catch (e) {
    throw new Error(`loadScript ${url}: ${e}`); // with its kind: SyntaxError, ReferenceError (DOM), ...
  }
}
// hydra.ojack.xyz's loadScript("https://...") as written: its string is not mini-notation (Strudel's double quotes).
registerTranspilerPlugin({
  walk: () => ({
    enter(node) {
      if (node.type !== 'CallExpression' || node.callee.name !== 'loadScript') return;
      this.skip();
      return true;
    },
  }),
});

function installHydra() {
  strudelGlobals ??= new Map(Object.keys(hydra.synth).map((n) => [n, Object.getOwnPropertyDescriptor(globalThis, n)]));
  hydra.sandbox.makeGlobal = true;
  for (const n of strudelGlobals.keys()) globalThis[n] = hydra.synth[n];
}

// ponytail: names Hydra adds later (setFunction(), a.setBins() beyond 4) stay global.
function restoreStrudel() {
  if (!strudelGlobals) return;
  hydra.sandbox.tick(); // what the code assigned (speed, update, fps) stays Hydra's
  hydra.sandbox.makeGlobal = false;
  for (const [n, d] of strudelGlobals) d ? Object.defineProperty(globalThis, n, d) : delete globalThis[n];
  strudelGlobals = undefined;
}

// hydra-synth logs a failing uniform or update() on every frame: each line once per evaluate.
const said = new Set();
function once(fn) {
  // An interrupt skips the finally below: a wrapper left behind unwraps to what it wrapped.
  const plain = Object.fromEntries(['log', 'warn', 'error'].map((k) => [k, console[k].plain ?? console[k]]));
  for (const [k, f] of Object.entries(plain)) {
    const quiet = (...a) => {
      const line = a.map(String).join(' ');
      if (said.size < 100 && !said.has(line)) said.add(line), f(...a);
    };
    console[k] = Object.assign(quiet, { plain: f });
  }
  try {
    fn();
  } finally {
    Object.assign(console, plain);
  }
}
