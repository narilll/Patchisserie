// Upstream tune regression, shared by Node (tunes.test.mjs) and QuickJS (qjs_tunes.mjs).
// Replicates Strudel's test/tunes.test.mjs + test/runtime.mjs at the pinned commit; scripts/update-engine
// copies those files and the snapshots into test/upstream/. Only the engine differs: tunes are evaluated
// through our host (Strudel's repl() + transpiler) instead of upstream's bare transpiler evaluate().
import * as tunes from './upstream/tunes.mjs';

// Tunes we cannot compare, with the reason. Everything else must match its snapshot exactly.
export const notComparable = {
  outroMusic: 'upstream has no snapshot for it (test/__snapshots__/tunes.test.mjs.snap at the pinned commit)',
};

// Copied verbatim from upstream test/tunes.test.mjs.
const roundFloatStrings = (input, precision = 12) => {
  const regex = /-?\d+\.\d+/g;
  return input.replace(regex, (match) => {
    const rounded = Number(match).toFixed(precision);
    return rounded.replace(/\.?0+$/, '').replace(/\.$/, '');
  });
};

// vitest snapshot format for an array of strings (escapeString: false, printBasicPrototype: false).
const printSnapshot = (strings) => (strings.length ? `\n[\n${strings.map((s) => `  "${s}",\n`).join('')}]\n` : '\n[]\n');

// read(pathRelativeToTestDir) -> string; Node and QuickJS read files differently.
export async function setup(read) {
  const h = globalThis.__patchisserie;
  await h.evaluate('silence'); // engine scope is loaded; mocks below must override it like upstream's evalScope order

  // upstream runtime.mjs mocks (audio/visual/IO not under test); 'p' is re-injected by repl() on every evaluate.
  const self = function () {
    return this;
  };
  const protoMocks = ['osc', 'csound', 'tone', 'webdirt', 'pianoroll', 'speak', 'wave', 'filter', 'adsr', 'webaudio',
    'soundfont', 'tune', 'midi', '_scope', '_spiral', '_pitchwheel', '_pianoroll', '_spectrum', 'markcss', 'p', 'dough'];
  for (const name of protoMocks) Pattern.prototype[name] = self;
  class MockedNode {}
  for (const name of ['chain', 'connect', 'toDestination', 'set', 'start']) MockedNode.prototype[name] = self;
  const mockNode = () => new MockedNode();
  const id = (x) => x;
  const toneHelpersMocked = {
    FeedbackDelay: MockedNode, MembraneSynth: MockedNode, NoiseSynth: MockedNode, MetalSynth: MockedNode,
    Synth: MockedNode, PolySynth: MockedNode, Chorus: MockedNode, Freeverb: MockedNode, Gain: MockedNode,
    Reverb: MockedNode, vol: mockNode, out: id, osc: id, samples: id, adsr: id, getDestination: id,
    players: mockNode, sampler: mockNode, synth: mockNode, piano: mockNode, polysynth: mockNode, fmsynth: mockNode,
    membrane: mockNode, noise: mockNode, metal: mockNode, lowpass: mockNode, highpass: mockNode,
  };
  const uiHelpersMocked = { backgroundImage: id };
  // The real @strudel/webaudio is in the engine's scope like upstream's; only samples() is mocked, since it would
  // fetch sample maps over the network (Node) or find no fetch() at all (qjs). Tunes call it for side effects only.
  const webaudioMocked = { samples: () => Promise.resolve() };
  const canvasCtx = { clearRect: () => {}, fillText: () => {}, fillRect: () => {}, canvas: { width: 100, height: 100 } };
  const audioCtx = { currentTime: 1 };
  evalScope(toneHelpersMocked, uiHelpersMocked, webaudioMocked, {
    midin: () => () => ref(() => 0),
    midikeys: async () => () => silence,
    sysex: () => {},
    csound: id,
    loadOrc: id,
    getDrawContext: () => canvasCtx,
    getAudioContext: () => audioCtx,
    loadSoundfont: () => {},
    loadCSound: () => {},
    loadCsound: () => {},
    loadcsound: () => {},
    getDuration: () => {},
    setcps: id,
    setcpm: id,
    initDough: id,
    Clock: {},
  });
  // upstream runtime.mjs imports website/src/repl/piano.mjs for .piano(); run that exact file against our globals.
  const piano = read('upstream/piano.mjs').replace(/^import \{([^}]*)\} from '@strudel\/core';/m, 'const {$1} = globalThis;');
  new Function(piano)();

  const testCycles = new Function(`return ${/export const testCycles = (\{[^}]*\});/.exec(read('upstream/runtime.mjs'))[1]}`)();
  const snapshots = {};
  new Function('exports', read('upstream/tunes.test.mjs.snap'))(snapshots);

  return {
    names: Object.keys(tunes).filter((name) => !(name in notComparable)),
    expected: (name) => snapshots[`renders tunes > tune: ${name} 1`],
    // Evaluates through the host, then queries and stringifies exactly like upstream's queryCode().
    async actual(name) {
      const res = await h.evaluate(tunes[name]);
      if (!res.ok) return `evaluate failed: ${res.error}`;
      const haps = h.pattern.sortHapsByPart().queryArc(0, testCycles[name] || 1);
      return printSnapshot(haps.map((hap) => roundFloatStrings(hap.show(true))));
    },
  };
}
