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
const { evaluate, scheduler, setCps } = core.repl({
  transpiler,
  // ponytail: clock frozen at 0, so scheduler.now()/getTime() users see t=0; M2 injects rendered-frame time.
  getTime: () => 0,
  // The host queries the pattern itself, so the Cyclist must never tick.
  setInterval: () => 0,
  clearInterval: () => {},
  onEvalError: (err) => (evalError = err),
});

const time = (t) => t.valueOf();

// The host's query loop on the frame timeline (context time): cycle c sounds at seconds + (c - cycle) / cps.
let clock = { cps: 0.5, cycle: 0, seconds: 0 };
const cycleAt = (t) => clock.cycle + (t - clock.seconds) * clock.cps;
// Strudel's dispatch: webaudio unless a dominant onSchedule (e.g. .midi()) takes the hap; errors go to the log.
const onSchedule = core.getSchedule({ onSchedule: webaudio.webaudioOutput });

// .midi() sends the pattern to Rack instead of superdough, through Strudel's own mechanism (portable to strudel.cc).
// ponytail: events are only collected here; M2 turns them into V/OCT, GATE, VEL, TRIG and CC.
const midiEvents = [];
const toRack = (hap, t, cps) => midiEvents.push({ t, cps, duration: time(hap.duration) / cps, value: hap.value });
core.register('midi', (pat) => pat.onSchedule(toRack, true));

globalThis.__patchisserie = {
  async evaluate(code) {
    await ready;
    evalError = undefined;
    try {
      await evaluate(code, false); // false: don't start the Cyclist; errors land in onEvalError
    } catch (err) {
      evalError = err; // repl throws (outside its try) only for empty code
    }
    if (!evalError) return { ok: true };
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

  setCps,

  /** Restarts the timeline: `cycle` sounds at context time `seconds`. */
  setClock(seconds, cycle = 0) {
    clock = { cps: scheduler.cps, cycle, seconds };
  },

  /** Schedules every onset in context time [t0, t1). Resolves once superdough has built the voices. */
  schedule(t0, t1) {
    if (!scheduler.pattern) return Promise.resolve();
    // ponytail: a cps change re-anchors at t0; M2's native cycle clock with tempo events replaces this.
    if (scheduler.cps !== clock.cps) clock = { cps: scheduler.cps, cycle: cycleAt(t0), seconds: t0 };
    const haps = scheduler.pattern
      .queryArc(cycleAt(t0), cycleAt(t1), { _cps: clock.cps })
      .filter((hap) => hap.hasOnset())
      .sort((a, b) => time(a.whole.begin) - time(b.whole.begin)); // onset order like the Cyclist (cut groups)
    const at = (hap) => clock.seconds + (time(hap.whole.begin) - clock.cycle) / clock.cps;
    return Promise.all(haps.map((hap) => onSchedule(hap, at(hap), hap.value.cps ?? clock.cps)));
  },

  /** Drains the .midi() events collected since the last call. */
  midiEvents: () => midiEvents.splice(0),

  /** superdough's orbits and buses back to their initial state; their nodes become garbage. */
  resetAudio: () => webaudio.resetGlobalEffects(),

  // Test seam: the upstream tune regression stringifies haps with hap.show() like Strudel's own test.
  get pattern() {
    return scheduler.pattern;
  },
};
