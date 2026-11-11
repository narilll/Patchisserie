// Patchisserie host: Strudel's own repl() driven by the plugin instead of a browser clock.
// Bundled by scripts/build-engine.mjs into res/engine.js; runs unmodified in QuickJS and Node.
import './polyfills.mjs';
import * as core from '@strudel/core';
import * as mini from '@strudel/mini';
import * as tonal from '@strudel/tonal';
import * as xen from '@strudel/xen';
import * as edo from '@strudel/edo';
import { transpiler } from '@strudel/transpiler';

// Same scope as strudel.cc (website/src/repl/util.mjs loadModules) minus audio/visual/IO packages.
const ready = core.evalScope(core, edo, tonal, mini, xen);

let evalError;
const { evaluate, scheduler } = core.repl({
  transpiler,
  // ponytail: clock frozen at 0, so scheduler.now()/getTime() users see t=0; M2 injects rendered-frame time.
  getTime: () => 0,
  // The host queries the pattern itself, so the Cyclist must never tick.
  setInterval: () => 0,
  clearInterval: () => {},
  onEvalError: (err) => (evalError = err),
});

const time = (t) => t.valueOf();

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

  // Test seam: the upstream tune regression stringifies haps with hap.show() like Strudel's own test.
  get pattern() {
    return scheduler.pattern;
  },
};
