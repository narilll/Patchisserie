// Rough CPU benchmark (make bench): 64 voices of saw -> biquad -> gain -> panner plus one 2 s stereo convolver reverb,
// then one voice of every res/worklets.js processor (QuickJS, and the native ports), each in an OfflineAudioContext
// like superdough would set it up. Prints microseconds per 128-frame quantum; asserts nothing. Then Lattice: one orca
// frame of a dense grid, microseconds.
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>

#include "audio/nodes.hpp"
#include "js_rig.hpp"
#include "lattice.hpp"

using namespace audio;

// One voice per processor, minus the sawtooth that feeds the effects. `native` = the native ports' turn.
const char* kWorkletBench = R"js(
const sr = 48000, frames = 2 * sr, quanta = frames / 128, budget = (1e6 * 128) / sr;
async function time(build) {
  const ac = new OfflineAudioContext(2, frames, sr);
  await ac.audioWorklet.addModule('patchisserie:worklets.js');
  build(ac);
  const t0 = performance.now();
  await ac.startRendering();
  return ((performance.now() - t0) * 1000) / quanta;
}
const saw = (ac) => {
  const o = new OscillatorNode(ac, { type: 'sawtooth', frequency: 110 });
  o.start();
  return o;
};
const live = { begin: 0, end: 1e9 };
const effect = (name, parameterData = {}, processorOptions) =>
  Object.assign((ac) => saw(ac).connect(new AudioWorkletNode(ac, name, { parameterData, processorOptions })).connect(ac.destination), { effect: true });
const source = (name, parameterData, init) => (ac) => {
  const n = new AudioWorkletNode(ac, name, { outputChannelCount: [2], parameterData: { ...live, ...parameterData } });
  init?.(n);
  n.connect(ac.destination);
};
const table = Float32Array.from({ length: 64 * 2048 }, (_, i) => Math.sin((i * (1 + (i >> 11))) / 326));
const frames64 = Array.from({ length: 64 }, (_, f) => table.subarray(f * 2048, (f + 1) * 2048));
const wavetable = (voices) => source('wavetable-oscillator-processor', { voices, frequency: 220, position: 0.5 }, (n) =>
  n.port.postMessage({ type: 'initialize', payload: { key: `bench${voices}`, frameLen: 2048, frames: frames64 } }));
const cases = native ? [
  ['supersaw-oscillator (5 voices) native', source('supersaw-oscillator', { frequency: 110 })],
  ['envelope-processor native', source('envelope-processor', {})],
  ['wavetable (1 voice) native', wavetable(1)],
  ['wavetable (5 voices) native', wavetable(5)],
] : [
  ['crush-processor', effect('crush-processor', { crush: 4 })],
  ['shape-processor', effect('shape-processor', { shape: 0.5 })],
  ['coarse-processor', effect('coarse-processor', { coarse: 4 })],
  ['distort-processor', effect('distort-processor', { distort: 0.5 }, { algorithm: 'scurve' })],
  ['ladder-processor', effect('ladder-processor', { frequency: 800, q: 0.5 })],
  ['djf-processor (lowpass)', effect('djf-processor', { value: 0.3 })],
  ['transient-processor', effect('transient-processor', {}, { ...live, attack: 0.5, sustain: -0.5 })],
  ['lfo-processor', source('lfo-processor', { frequency: 2, depth: 1 })],
  ['envelope-processor', source('envelope-processor', {})],
  ['pulse-oscillator', source('pulse-oscillator', { frequency: 110 })],
  ['byte-beat-processor', source('byte-beat-processor', { frequency: 110 }, (n) =>
    n.port.postMessage({ codeText: 't*(t>>10^t)', byteBeatStartTime: 0, frequency: 110 }))],
  ['supersaw-oscillator (5 voices)', source('supersaw-oscillator', { frequency: 110 })],
  ['wavetable (1 voice)', wavetable(1)],
  ['wavetable (5 voices)', wavetable(5)],
];
const base = await time((ac) => saw(ac).connect(ac.destination));
const lines = [];
for (const [label, build] of cases) {
  const us = Math.max(0, (await time(build)) - (build.effect ? base : 0));
  lines.push(`${label.padEnd(40)} ${us.toFixed(1).padStart(7)} us/quantum ${((100 * us) / budget).toFixed(2).padStart(6)} % of one core`);
}
return lines.join('\n');
)js";

static double run(bool sweep, bool reverb) {
	const double sr = 48000;
	std::unique_ptr<Graph> g(new Graph(sr));
	ConvolverNode* rev = nullptr;
	if (reverb) {
		std::mt19937 rng(1);
		std::uniform_real_distribution<float> u(-1, 1);
		AudioBufferData ir(2, (int64_t)(2 * sr), sr);
		for (auto& ch : ir.data)
			for (size_t i = 0; i < ch.size(); i++)
				ch[i] = u(rng) * std::exp(-3.0f * i / ch.size());
		rev = new ConvolverNode(sr);
		g->create(rev);
		g->setAttr(rev, AttrBuffer, 0, new ConvolverKernel(ir, true));
		g->connect(rev, g->destination());
	}
	const double seconds = 4;
	for (int v = 0; v < 64; v++) {
		auto* o = new OscillatorNode(sr);
		o->frequency.value = 55.f * (1 + v % 24);
		auto* f = new BiquadFilterNode(sr);
		auto* gain = new GainNode(sr);
		gain->gain.value = 0.01f;
		auto* p = new StereoPannerNode(sr);
		p->pan.value = (v % 9) / 4.f - 1;
		for (Node* n : std::initializer_list<Node*>{o, f, gain, p})
			g->create(n);
		g->setAttr(o, AttrType, OscillatorNode::Sawtooth);
		if (sweep) { // filter envelope running the whole time: per-frame coefficients
			g->setValueAtTime(&f->frequency, 200, 0);
			g->exponentialRampToValueAtTime(&f->frequency, 8000, seconds);
		} else {
			f->frequency.value = 2000;
		}
		g->connect(o, f);
		g->connect(f, gain);
		g->connect(gain, p);
		g->connect(p, g->destination());
		if (rev)
			g->connect(p, rev);
		g->start(o, 0);
	}
	const int quanta = (int)(seconds * sr / kQuantum);
	float l[kQuantum], r[kQuantum];
	g->render(l, r, kQuantum); // apply commands outside the timing
	auto t0 = std::chrono::steady_clock::now();
	for (int i = 1; i < quanta; i++)
		g->render(l, r, kQuantum);
	double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
	return us / (quanta - 1);
}

// Lattice: an external clock edge on every sample is one frame per sample (4 pulses per beat), so the time per
// process() call is the frame (orca_run + event mapping) plus a sample's few ns.
static double benchLattice(int w, int h, int frames) {
	// Busy tile of 14x6, every operator class but the movers (they pile up and go quiet).
	const char* tile[6] = {"2C8.1D4.R4Z.A3", "..3M2.0I7.U3.9", ":03C.%25..*.F1",
						   "A5.L4.O00.K3.Q", "T4abcd.V3x.Y.H", "J.....B2.*.:15"};
	std::string g;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			g += tile[y % 6][x % 14];
	lattice::Machine m;
	m.resize(w, h);
	m.write({0, 0, w, h}, g);
	m.setPulsesPerBeat(4);
	lattice::Machine::Inputs in;
	in.clockConnected = in.clockEdge = true;
	lattice::Machine::Outputs out;
	m.request(lattice::Machine::Transport::Play);
	for (int i = 0; i < 200; i++) // the reset guard, then the grid settles
		m.process(in, out);
	int64_t f0 = m.frame();
	auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < frames; i++)
		m.process(in, out);
	double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
	return us / (double)(m.frame() - f0);
}

int main() {
	const double budget = 1e6 * kQuantum / 48000;
	std::printf("budget per quantum at 48 kHz: %.1f us\n", budget);
	for (bool sweep : {false, true})
		for (bool reverb : {false, true}) {
			double us = run(sweep, reverb);
			std::printf("64 voices%s%s: %.1f us/quantum (%.1f%% of one core)\n", sweep ? " + filter sweep" : "",
						reverb ? " + 2 s reverb" : "", us, 100 * us / budget);
		}
	std::printf("Lattice 64x32: %.1f us per orca frame\n", benchLattice(64, 32, 4000));
	std::printf("Lattice 256x256: %.1f us per orca frame\n", benchLattice(256, 256, 300));
	std::printf("one voice per worklet processor at 48 kHz (effects: minus their sawtooth input):\n");
	for (bool native : {false, true}) {
		JsRig r(native);
		std::string out = r.run(std::string("const native = ") + (native ? "true" : "false") + ";\n" + kWorkletBench);
		std::printf("%s\n", out.c_str());
	}
	return 0;
}
