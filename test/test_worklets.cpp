// Worklet host (worklets.hpp): AudioWorkletGlobalScope semantics, failure isolation, lifetime and messages through
// the webaudio_js binding, the native supersaw/wavetable ports against their JS processors, and the runtime on a
// render thread next to the JS thread (make tsan).
#include <atomic>
#include <chrono>
#include <thread>

#include "js_rig.hpp"

namespace {

// Test processors, loaded as module 'test:module'.
const char* kProbes = R"js(
const loadRate = sampleRate;
registerProcessor('probe', class extends AudioWorkletProcessor {
  static get parameterDescriptors() {
    return [
      { name: 'k', defaultValue: 0.5, automationRate: 'k-rate' },
      { name: 'a', defaultValue: 1, minValue: 0, maxValue: 2 },
      { name: 'c', defaultValue: 3 },
    ];
  }
  constructor(options) {
    super();
    this.options = options;
    this.calls = 0;
    this.port.onmessage = (e) => this.port.postMessage({ echo: e.data, frame: currentFrame });
  }
  process(inputs, outputs, parameters) {
    if (this.calls++ < 3) {
      this.port.postMessage({
        loadRate, rate: sampleRate, frame: currentFrame, time: currentTime, options: this.options,
        inputs: inputs.map((i) => i.length), outputs: outputs.map((o) => o.length),
        zeroed: outputs.every((o) => o.every((ch) => ch.every((v) => v === 0))),
        k: parameters.k.length, a: parameters.a.length, c: parameters.c.length, a0: parameters.a[0],
        in1: inputs[1][0]?.[5],
      });
    }
    outputs[0][0].fill(0.25); // dirty: the next call must see zeros again
    return true;
  }
});
registerProcessor('thrower', class extends AudioWorkletProcessor {
  constructor() { super(); this.n = 0; }
  process(inputs, outputs) { outputs[0][0].fill(1); if (++this.n === 3) throw new Error('boom'); return true; }
});
registerProcessor('ctor-thrower', class extends AudioWorkletProcessor {
  constructor() { super(); throw new RangeError('bad options'); }
  process() { return true; }
});
registerProcessor('spinner', class extends AudioWorkletProcessor {
  process() { for (;;) {} }
});
registerProcessor('finisher', class extends AudioWorkletProcessor {
  constructor() { super(); this.n = 0; }
  process(inputs, outputs) { outputs[0][0].fill(1); return ++this.n < 2; }
});
registerProcessor('keeper', class extends AudioWorkletProcessor {
  process(inputs, outputs) { outputs[0][0].fill(1); return true; }
});
registerProcessor('echo', class extends AudioWorkletProcessor {
  constructor() { super(); this.port.onmessage = (e) => this.port.postMessage(e.data); }
  process() { return true; }
});
)js";

// Math.random() of the scope as the native ports' xorshift32 (WorkletScope::random), seeded like randomState.
std::string seededRandom(uint32_t seed) {
	return "{ let s = " + std::to_string(seed) +
		   "; Math.random = () => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; "
		   "return s / 4294967296; }; }";
}

std::string readText(const char* path) {
	std::string s;
	FILE* f = std::fopen(path, "rb");
	char buf[1 << 16];
	for (size_t n; f && (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
		s.append(buf, n);
	if (f)
		std::fclose(f);
	return s;
}

/** Renders the same JS setup once with the JS processors and once with the native ports, both scopes seeded alike;
returns the largest sample difference (L and R), or -1 if a setup failed or the native render is silent. */
double nativeVsScript(const std::string& setup, int frames) {
	const uint32_t seed = 0x2545f491;
	std::vector<float> out[2][2];
	for (int native = 0; native < 2; native++) {
		JsRig r(native == 1);
		r.scope->deadline = 2; // sanitizer builds: the JS processors are slow, never stuck
		r.load(seededRandom(seed), "test:seed");
		r.scope->randomState = seed;
		if (!ok(r.run(setup)))
			return -1;
		out[native][0].resize(frames);
		out[native][1].resize(frames);
		r.graph->render(out[native][0].data(), out[native][1].data(), frames);
	}
	double diff = 0, peak = 0;
	for (int c = 0; c < 2; c++)
		for (int i = 0; i < frames; i++) {
			diff = std::max(diff, (double)std::fabs(out[0][c][i] - out[1][c][i]));
			peak = std::max(peak, (double)std::fabs(out[1][c][i]));
		}
	return peak > 1e-3 ? diff : -1;
}

} // namespace

TEST(worklet_module_names_are_all_in_the_processor_table) {
	WorkletScope* s = new WorkletScope(kSr);
	std::string error;
	CHECK(s->load(readText("res/worklets.js"), "patchisserie:worklets.js", error));
	CHECK(s->processors().size() == 15);
	for (const ProcessorDef& d : s->processors()) {
		if (!d.listed)
			std::fprintf(stderr, "  res/worklets.js registers '%s', missing in worklets.cpp kProcessors\n",
						 d.name.c_str());
		CHECK(d.listed);
		CHECK(d.policy != ProcessorDef::Native || d.nativeMatches);
	}
	release(s);
}

TEST(worklet_descriptors_are_on_the_js_thread_after_add_module) {
	JsRig r;
	CHECK(ok(r.run(R"js(
		const ac = new AudioContext();
		const name = (f) => { try { f(); } catch (e) { return `${e.name}: ${e.message}`; } return 'none'; };
		assert(/^InvalidStateError/.test(name(() => new AudioWorkletNode(ac, 'supersaw-oscillator'))), 'before');
		await ac.audioWorklet.addModule('patchisserie:worklets.js');
		await ac.audioWorklet.addModule('patchisserie:worklets.js'); // superdough reloads on every initAudio()
		const o = new AudioWorkletNode(ac, 'supersaw-oscillator', { outputChannelCount: [2] });
		const p = o.parameters;
		assert(p instanceof Map && p.size === 7 && p.get('end').value === -1 && p.get('end').defaultValue === -1, 'end');
		assert(p.get('voices').automationRate === 'k-rate' && p.get('frequency').automationRate === 'a-rate', 'rates');
		// worklets.mjs writes min/max, which the spec (and Chrome) ignore: the full float range.
		assert(p.get('frequency').minValue === -3.4028234663852886e38 && p.get('begin').maxValue === 3.4028234663852886e38, 'range');
		const crush = new AudioWorkletNode(ac, 'crush-processor', { parameterData: { crush: 4 } });
		assert(crush.parameters.get('crush').value === 4 && crush.numberOfInputs === 1 && crush.port.onmessage === null, 'crush');
		for (const [proc, feature] of [['phase-vocoder-processor', 'stretch'], ['generic-processor', 'kabelsalat']]) {
			const e = name(() => new AudioWorkletNode(ac, proc));
			assert(e.startsWith('NotSupportedError') && e.includes(feature), e);
		}
		assert(/^IndexSizeError/.test(name(() => new AudioWorkletNode(ac, 'lfo-processor', { outputChannelCount: [1, 1] }))), 'count');
		assert(/^NotSupportedError/.test(name(() => new AudioWorkletNode(ac, 'lfo-processor', { outputChannelCount: [3] }))), '3 ch');
		const late = await ac.audioWorklet.addModule('src/audio/webaudio.js').then(() => 'resolved', (e) => e.message);
		assert(/render thread/.test(late), `a new module after the first node: ${late}`);
		return 'ok';
	)js")));
}

TEST(worklet_scope_semantics) {
	JsRig r;
	r.load(kProbes);
	CHECK(ok(r.run(R"js(
		const ac = new AudioContext();
		await ac.audioWorklet.addModule('test:module');
		const processorOptions = { list: [1, 'two', { three: 3 }], f: new Float32Array([3, 4]) };
		const n = new AudioWorkletNode(ac, 'probe', { numberOfInputs: 2, outputChannelCount: [2], processorOptions });
		const src = new ConstantSourceNode(ac, { offset: 0.5 });
		src.connect(n, 0, 1); // input 0 stays unconnected
		src.start();
		n.parameters.get('a').setValueAtTime(0.5, 0);
		n.parameters.get('a').linearRampToValueAtTime(1.5, 1); // changes within every quantum: 128 values
		n.connect(ac.destination);
		const got = [];
		n.port.onmessage = (e) => got.push(e.data);
		const out = render(128 * 3);
		assert(got.length === 3, `${got.length} reports`);
		const [first, second] = got;
		assert(first.loadRate === 48000 && first.rate === 48000, 'sampleRate is defined when the module loads');
		assert(first.frame === 0 && second.frame === 128 && second.time === 128 / 48000, 'currentFrame/currentTime');
		assert(JSON.stringify(first.inputs) === '[0,1]' && JSON.stringify(first.outputs) === '[2]', JSON.stringify(first));
		assert(first.in1 === 0.5, `input data ${first.in1}`);
		assert(first.zeroed && second.zeroed, 'outputs are zeroed before every call');
		assert(first.k === 1 && first.c === 1 && first.a === 128 && first.a0 === 0.5, `param lengths ${JSON.stringify(first)}`);
		const o = first.options;
		assert(o.numberOfInputs === 2 && o.outputChannelCount[0] === 2 && o.processorOptions.list[2].three === 3, 'options');
		assert(o.processorOptions.f instanceof Float32Array && o.processorOptions.f[1] === 4, 'typed arrays travel');
		assert(out[0] === 0.25 && out[383] === 0.25, 'output reaches the destination');
		n.port.postMessage({ hello: [1, 2], t: new Float32Array([5]) });
		render(128);
		const echo = got[3];
		assert(echo?.echo.hello[1] === 2 && echo.echo.t instanceof Float32Array && echo.echo.t[0] === 5, 'port both ways');
		assert(echo.frame === 384, `delivered at the quantum boundary: ${echo.frame}`);
		const message = (() => { try { n.port.postMessage(() => 1); } catch (e) { return e.message; } })();
		assert(message, 'a function cannot be sent');
		return 'ok';
	)js")));
}

TEST(worklet_exception_silences_the_node_and_reports_once) {
	JsRig r;
	r.load(kProbes);
	CHECK(ok(r.run(R"js(
		const ac = new AudioContext();
		await ac.audioWorklet.addModule('test:module');
		const t = new AudioWorkletNode(ac, 'thrower');
		t.connect(ac.destination);
		const errors = [];
		t.onprocessorerror = (e) => errors.push(e.message);
		let out = render(256);
		assert(out[0] === 1 && out[255] === 1, 'runs until it throws');
		out = render(128 * 8);
		assert(out.every((v) => v === 0), 'silent after the exception');
		assert(errors.length === 1 && /boom/.test(errors[0]), `errors: ${errors}`);
		const c = new AudioWorkletNode(ac, 'ctor-thrower');
		c.connect(ac.destination);
		let ctor = '';
		c.addEventListener('processorerror', (e) => (ctor += e.message));
		out = render(256);
		assert(out.every((v) => v === 0) && /bad options/.test(ctor), `constructor: ${ctor}`);
		return 'ok';
	)js")));
	int logged = 0;
	for (const std::string& l : r.logs)
		logged += l.find("error") == 0 && l.find("thrower: process(): Error: boom") != std::string::npos &&
				  l.find("silent") != std::string::npos;
	CHECK(logged == 1);
}

TEST(worklet_deadline_interrupts_a_stuck_processor) {
	JsRig r;
	r.load(kProbes);
	r.scope->deadline = 0.005;
	auto t0 = std::chrono::steady_clock::now();
	CHECK(ok(r.run(R"js(
		const ac = new AudioContext();
		await ac.audioWorklet.addModule('test:module');
		const s = new AudioWorkletNode(ac, 'spinner');
		const k = new AudioWorkletNode(ac, 'keeper'); // another node in the same scope keeps running
		s.connect(ac.destination);
		k.connect(ac.destination);
		let err = '';
		s.onprocessorerror = (e) => (err = e.message);
		const out = render(128 * 4);
		assert(out.every((v) => v === 1) && /budget/.test(err), `${out[0]} ${err}`);
		return 'ok';
	)js")));
	CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
}

TEST(worklet_returning_false_is_reclaimed) {
	JsRig r;
	r.load(kProbes);
	CHECK(ok(r.run(R"js(
		const ac = new AudioContext();
		await ac.audioWorklet.addModule('test:module');
		render(128);
		const base = nodeCount();
		(() => {
			new AudioWorkletNode(ac, 'finisher').connect(ac.destination); // dropped by JS at once
			new AudioWorkletNode(ac, 'keeper').connect(ac.destination);   // too, but an active source forever
		})();
		const out = render(128 * 4);
		assert(out[0] === 2 && out[128] === 2 && out[256] === 1, `finisher stops after 2 calls: ${out[0]} ${out[256]}`);
		gc();
		render(128);
		assert(nodeCount() === base + 1, `${nodeCount() - base} nodes over the baseline`);
		return 'ok';
	)js")));
}

TEST(worklet_nodes_like_superdough_notes_return_to_baseline) {
	JsRig r;
	int resources = 0;
	CHECK(ok(r.run(R"js(
		const ac = new AudioContext();
		await ac.audioWorklet.addModule('patchisserie:worklets.js');
		render(128);
		globalThis.base = nodeCount();
		const keys = new Float32Array(64).fill(0.5);
		(() => {
			for (let i = 0; i < 100; i++) { // a crushed note, a supersaw note and a wavetable note each, released
				const t = ac.currentTime + 0.001 * i;
				const o = ac.createOscillator();
				const crush = new AudioWorkletNode(ac, 'crush-processor', { parameterData: { crush: 4 } });
				o.connect(crush).connect(ac.destination);
				o.start(t);
				o.stop(t + 0.005);
				o.onended = () => { o.disconnect(); crush.disconnect(); };
				const s = new AudioWorkletNode(ac, 'supersaw-oscillator', { outputChannelCount: [2] });
				s.parameters.get('begin').value = t;
				s.parameters.get('end').value = t + 0.005;
				s.connect(ac.destination);
				const w = new AudioWorkletNode(ac, 'wavetable-oscillator-processor', { outputChannelCount: [2] });
				w.port.postMessage({ type: 'initialize', payload: { key: 'k', frameLen: 32, frames: i ? undefined : [keys.subarray(0, 32), keys.subarray(32)] } });
				w.parameters.get('begin').value = t;
				w.parameters.get('end').value = t + 0.005;
				w.connect(ac.destination);
			}
		})();
		render(128);
		assert(nodeCount() > base + 300, `created ${nodeCount() - base}`);
		return 'ok';
	)js")));
	resources = Resource::live().load();
	CHECK(ok(r.run(R"js(
		for (let k = 0; k < 40; k++) { render(1024); gc(); }
		assert(nodeCount() === base, `${nodeCount()} nodes, baseline ${base}`);
		return 'ok';
	)js")));
	// The table is cached once for its key; every per-note message and option blob is gone again.
	CHECK(Resource::live().load() < resources);
	int before = Resource::live().load();
	CHECK(ok(r.run("render(1024); gc(); render(1024); return 'ok';")));
	CHECK(Resource::live().load() == before);
}

TEST(native_supersaw_matches_its_js_processor) {
	double diff = nativeVsScript(R"js(
		const ac = new AudioContext();
		await ac.audioWorklet.addModule('patchisserie:worklets.js');
		const o = new AudioWorkletNode(ac, 'supersaw-oscillator', { outputChannelCount: [2] });
		const p = (k) => o.parameters.get(k);
		p('begin').value = 0.01;
		p('end').value = 0.3;
		p('voices').value = 7;
		p('freqspread').value = 0.3;
		p('panspread').value = 0.6;
		p('frequency').setValueAtTime(110, 0);
		p('frequency').exponentialRampToValueAtTime(1760, 0.25);
		p('detune').setValueAtTime(0, 0);
		p('detune').linearRampToValueAtTime(-50, 0.2);
		o.port.postMessage({ type: 'initialize' });
		o.connect(ac.destination);
		globalThis.keep = o;
		const fewer = new AudioWorkletNode(ac, 'supersaw-oscillator', { outputChannelCount: [2] }); // 1.5 voices
		fewer.parameters.get('begin').value = 0.2;
		fewer.parameters.get('end').value = 0.4;
		fewer.parameters.get('voices').value = 1.5;
		fewer.connect(ac.destination);
		globalThis.keep2 = fewer;
		return 'ok';
	)js",
								 48000);
	std::printf("  native supersaw vs JS: max |difference| %.3g\n", diff);
	CHECK(diff >= 0 && diff <= 1e-6);
}

TEST(native_wavetable_matches_its_js_processor_in_every_warp_mode) {
	// 22 nodes, one per WarpMode, each sounding in its own 50 ms window while warp, position and pitch sweep.
	double diff = nativeVsScript(R"js(
		const ac = new AudioContext();
		await ac.audioWorklet.addModule('patchisserie:worklets.js');
		const len = 256, frames = [];
		const data = new Float32Array(4 * len);
		for (let f = 0; f < 4; f++) {
			for (let i = 0; i < len; i++) data[f * len + i] = Math.sin((2 * Math.PI * i * (f + 1)) / len) * (1 - f / 8) + 0.1 * Math.sin((2 * Math.PI * i * 7) / len);
			frames.push(data.subarray(f * len, (f + 1) * len));
		}
		globalThis.keep = [];
		for (let mode = 0; mode < 22; mode++) {
			const w = new AudioWorkletNode(ac, 'wavetable-oscillator-processor', { outputChannelCount: [2] });
			const p = (k) => w.parameters.get(k);
			const t = 0.05 * mode;
			w.port.postMessage({ type: 'initialize', payload: { key: 'test', frameLen: len, frames: mode ? undefined : frames } });
			p('begin').value = t + 0.001;
			p('end').value = t + 0.045;
			p('warpMode').value = mode;
			p('voices').value = mode % 3 + 1;
			p('panspread').value = 0.5;
			p('freqspread').value = 0.4;
			p('phaserand').value = mode % 2;
			p('frequency').setValueAtTime(110 * (1 + (mode % 5)), 0);
			p('frequency').linearRampToValueAtTime(200 * (1 + (mode % 5)), t + 0.045);
			p('warp').setValueAtTime(0, t);
			p('warp').linearRampToValueAtTime(1, t + 0.045);
			p('position').setValueAtTime(0, t);
			p('position').linearRampToValueAtTime(1, t + 0.045);
			p('detune').setValueAtTime(mode * 10, 0);
			w.connect(ac.destination);
			keep.push(w);
		}
		return 'ok';
	)js",
								 (int)(kSr * 1.2));
	std::printf("  native wavetable vs JS (22 warp modes): max |difference| %.3g\n", diff);
	CHECK(diff >= 0 && diff <= 1e-6);
}

TEST(worklets_render_thread_and_js_thread) { // make tsan
	JsRig r;
	r.load(kProbes);
	std::atomic<bool> done{false};
	std::thread render([&] {
		DenormalsOff ftz;
		float L[256], R[256];
		for (int n = 0; !done.load(); n++) {
			r.graph->render(L, R, 256);
			if (n % 64 == 0)
				r.scope->collectGarbage(); // the render thread's GC hook
			std::this_thread::yield();
		}
	});
	CHECK(ok(r.run(R"js(
		const ac = new AudioContext();
		// Loads while the render thread runs (it keeps out of the scope until the first node seals it).
		await ac.audioWorklet.addModule('src/audio/webaudio.js');
		await ac.audioWorklet.addModule('test:module');
		const base = nodeCount();
		let echoes = 0;
		for (let i = 0; i < 60; i++) {
			let n = new AudioWorkletNode(ac, 'echo'); // an active source until disconnected
			n.connect(new AudioWorkletNode(ac, 'finisher')).connect(ac.destination); // lives while its input does
			n.port.onmessage = (e) => { if (e.data.i === i && e.data.f[1] === i) echoes++; };
			n.port.postMessage({ i, f: new Float32Array([0, i]) });
			for (let k = 0; k < 5000 && echoes <= i; k++) pump();
			n.disconnect();
			n = null;
		}
		for (let k = 0; k < 5000 && nodeCount() !== base; k++) pump();
		assert(echoes === 60 && nodeCount() === base, `${echoes} echoes, ${nodeCount() - base} nodes over the baseline`);
		return 'ok';
	)js")));
	done = true;
	render.join();
}
