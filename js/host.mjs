// Patchisserie host: Strudel's own repl() driven by the plugin instead of a browser clock.
// Bundled by scripts/build-engine.mjs into res/engine.js; runs unmodified in QuickJS and Node.
import './polyfills.mjs';
import * as core from '@strudel/core';
import * as mini from '@strudel/mini';
import * as tonal from '@strudel/tonal';
import * as xen from '@strudel/xen';
import * as edo from '@strudel/edo';
import * as webaudio from '@strudel/webaudio';
import { transpiler } from '@strudel/transpiler';

// Same scope as strudel.cc (website/src/repl/util.mjs loadModules) minus visual/IO packages; soundfonts join in
// initAudio().
const ready = core.evalScope(core, edo, tonal, mini, xen, webaudio);

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
// Cycles tick() shifts the hap it schedules by (a RESET's late onsets). toRack runs within that call: .midi() is
// dominant, so getSchedule calls it before its first await.
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
// Realtime query state, see tick().
const rt = { epoch: -1, from: 0, cps: 0, tempos: [], late: false };

const api = (globalThis.__patchisserie = {
  async evaluate(code, id = 0) {
    await ready;
    evalError = undefined;
    const before = scheduler.pattern;
    try {
      await evaluate(code, false); // false: don't start the Cyclist; errors land in onEvalError
    } catch (err) {
      evalError = err; // repl throws (outside its try) only for empty code
    }
    if (!evalError) {
      [previous, previousId, patternId] = [before, patternId, id];
      return { ok: true };
    }
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
    const tempo = (at, value) => {
      rt.cps = Math.fround(value); // what the Sequencer stores
      api.rackEvent(Tempo, at, at, 0, 0, -1, '', rt.cps);
      if (!external) rt.tempos.push({ cycle: at, cps: rt.cps });
    };
    // Frame of cycle b: through the tempo changes still ahead, each taking effect on the first sample at or after
    // its cycle (with the Sequencer's kEps); snapped to that sample, the one the Sequencer fires on, never before the
    // sample of the last change (that sample's cycle may be past b: back from it at a slower cps is a frame early).
    const eps = 1e-9;
    const first = (f, c, k, b) => Math.max(f, Math.ceil(f + ((b - eps - c) / k) * sr));
    const at = (b) => {
      let [f, c, k] = [frame, cycle, cps];
      for (const t of rt.tempos) {
        if (t.cycle > b) break;
        const p = first(f, c, k, t.cycle);
        [c, f, k] = [c + ((p - f) * k) / sr, p, t.cps];
      }
      return first(f, c, k, b) / sr;
    };
    const cpsAt = (b) => rt.tempos.reduce((k, t) => (t.cycle <= b ? t.cps : k), cps);
    if (Math.fround(scheduler.cps) !== rt.cps) tempo(Math.max(from, earliest), scheduler.cps); // setcps(), setcpm()
    const haps = scheduler.pattern
      .queryArc(from, end, { _cps: scheduler.cps })
      .filter((hap) => hap.hasOnset())
      .sort(byOnset);
    for (const hap of haps) {
      const onset = time(hap.whole.begin);
      const b = Math.max(onset, earliest);
      const cpsHap = hap.value.cps;
      if (cpsHap !== undefined && Math.fround(cpsHap) !== rt.cps) {
        tempo(b, cpsHap); // .cps(): like the Cyclist, the global tempo from this hap on
        scheduler.setCps(cpsHap);
      }
      rackShift = b - onset;
      const t = at(b);
      onSchedule(hap, t, cpsHap ?? cpsAt(b));
      rackShift = 0;
      const locations = hap.context.locations ?? [];
      if (!locations.length) continue;
      const [from, to] = [Math.round(t * sr), Math.round(at(time(hap.whole.end) + b - onset) * sr)];
      for (const { start, end } of locations) api.rackHighlight(patternId, from, to, start, end);
    }
    rt.from = end;
    rt.late = false;
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

  /** A query of the playing pattern was interrupted (endless loop): back to the one before the last evaluate, then
   * silence. (Query errors are Strudel's own: logged, no haps.) */
  revert() {
    scheduler.pattern = previous ?? core.silence;
    [previous, patternId, previousId] = [undefined, previous ? previousId : 0, 0];
  },

  /** superdough's orbits and buses back to their initial state; their nodes become garbage. */
  resetAudio: () => webaudio.resetGlobalEffects(),

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
