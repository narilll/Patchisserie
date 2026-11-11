// webaudio_js: the Web Audio classes as superdough uses them, run in QuickJS over a real graph (rig: js_rig.hpp).
#include "audio/decode.hpp"
#include "js_rig.hpp"

TEST(js_subclasses_honour_new_target_and_instanceof) {
	JsRig r;
	CHECK(ok(r.run(R"js(
		const ac = new AudioContext();
		// superdough's feedbackdelay.mjs and vowel.mjs, verbatim in shape
		class FeedbackDelayNode extends DelayNode {
			constructor(ac, wet, time, feedback) {
				super(ac);
				this.delayTime.value = time;
				this.feedbackGain = ac.createGain();
				this.feedbackGain.gain.value = feedback;
				this.connect(this.feedbackGain);
				this.feedbackGain.connect(this);
				this.delayGain = ac.createGain();
				this.delayGain.gain.value = wet;
				this.connect(this.delayGain);
				this.connect = (target) => this.delayGain.connect(target);
			}
		}
		class Vowel extends GainNode {
			constructor(ac) { super(ac); this.filter = ac.createBiquadFilter(); super.connect(this.filter); }
			connect(target) { return this.filter.connect(target); }
		}
		BaseAudioContext.prototype.createFeedbackDelay = function (w, t, f) { return new FeedbackDelayNode(this, w, t, f); };
		const d = ac.createFeedbackDelay(1, 0.25, 0.5);
		assert(d instanceof FeedbackDelayNode && d instanceof DelayNode && d instanceof AudioNode && d instanceof EventTarget, 'delay chain');
		assert(d.delayTime instanceof AudioParam && d.delayTime.value === 0.25 && d.delayTime.maxValue === 1, 'delayTime');
		assert(d.connect(ac.destination) === ac.destination, 'overridden connect returns the destination');
		const v = new Vowel(ac);
		assert(v instanceof Vowel && v instanceof GainNode && v.gain.defaultValue === 1, 'gain subclass');
		assert(v.connect(ac.destination) === ac.destination, 'subclass method');
		const o = new OscillatorNode(ac, { frequency: 220, type: 'square' });
		assert(o instanceof AudioScheduledSourceNode && o.type === 'square' && o.frequency.value === 220, 'oscillator options');
		assert(ac instanceof BaseAudioContext && new OfflineAudioContext(1, 128, 48000) instanceof BaseAudioContext, 'contexts');
		assert(ac.destination instanceof AudioDestinationNode && ac.destination.maxChannelCount === 2, 'destination');
		for (const C of [AudioNode, AudioScheduledSourceNode, BaseAudioContext, AudioParam, AudioDestinationNode]) {
			let threw = false;
			try { new C(); } catch (e) { threw = e instanceof TypeError; }
			assert(threw, `${C.name} is not constructible`);
		}
		return 'ok';
	)js")));
}

TEST(js_getters_never_throw) {
	JsRig r;
	CHECK(ok(r.run(R"js(
		const ac = new AudioContext();
		const nodes = [ac.createGain(), ac.createBiquadFilter(), ac.createOscillator(), ac.createConstantSource(),
			ac.createBufferSource(), ac.createStereoPanner(), ac.createWaveShaper(), ac.createDelay(), ac.createConvolver(),
			ac.createChannelMerger(2), ac.createChannelSplitter(2), ac.createDynamicsCompressor(), ac.createAnalyser(),
			ac.destination];
		let params = 0;
		for (const node of [...nodes, ac, ac.createBuffer(1, 10, 48000)]) {
			// superdough's nodePools.mjs getParams(): every own name up the chain, read through the instance...
			for (let proto = node; proto !== Object.prototype; proto = Object.getPrototypeOf(proto)) {
				for (const key of Object.getOwnPropertyNames(proto)) {
					if (node[key] instanceof AudioParam) params++;
					// ...and every getter with its prototype as receiver
					const get = Object.getOwnPropertyDescriptor(proto, key).get;
					if (get) get.call(proto);
				}
			}
		}
		assert(params === 1 + 4 + 2 + 1 + 2 + 1 + 1 + 5, `found ${params} AudioParams`);
		return 'ok';
	)js")));
}

TEST(js_start_stop_and_spec_exceptions) {
	JsRig r;
	CHECK(ok(r.run(R"js(
		const ac = new AudioContext();
		const name = (f) => { try { f(); } catch (e) { return e.name; } return 'none'; };
		const o = ac.createOscillator();
		assert(name(() => o.stop()) === 'InvalidStateError', 'stop before start');
		assert(name(() => o.start(-1)) === 'RangeError', 'negative start');
		o.start();
		assert(name(() => o.start()) === 'InvalidStateError', 'second start');
		o.stop(); o.stop(1); // stop may repeat
		const g = ac.createGain();
		assert(name(() => g.gain.exponentialRampToValueAtTime(0, 1)) === 'RangeError', 'exp ramp to 0');
		assert(name(() => g.gain.setValueAtTime(NaN, 1)) === 'TypeError', 'non-finite value');
		assert(name(() => (ac.createWaveShaper().curve = new Float32Array(1))) === 'InvalidStateError', 'curve length');
		assert(name(() => (ac.createAnalyser().fftSize = 1000)) === 'IndexSizeError', 'fftSize');
		assert(name(() => g.connect(ac.createGain(), 1)) === 'IndexSizeError', 'output index');
		assert(name(() => new AudioWorkletNode(ac, 'crush-processor')) === 'InvalidStateError', 'before addModule');
		await ac.audioWorklet.addModule('patchisserie:worklets.js');
		assert(name(() => new AudioWorkletNode(ac, 'x')) === 'InvalidStateError', 'unknown processor');
		const other = new OfflineAudioContext(1, 128, 48000);
		assert(name(() => g.connect(other.destination)) === 'InvalidAccessError', 'other context');
		const s = ac.createBufferSource();
		s.buffer = ac.createBuffer(1, 8, 48000);
		assert(name(() => (s.buffer = ac.createBuffer(1, 8, 48000))) === 'InvalidStateError', 'buffer set twice');
		return 'ok';
	)js")));
}

TEST(js_param_value_mirror_and_connect) {
	JsRig r;
	CHECK(ok(r.run(R"js(
		const ac = new AudioContext();
		const c = new ConstantSourceNode(ac, { offset: 0.5 });
		const g = new GainNode(ac, { gain: 3 });
		assert(c.offset.value === 0.5 && g.gain.value === 3 && g.gain.defaultValue === 1, 'options set values');
		assert(c.connect(g) === g && g.connect(ac.destination) === ac.destination, 'connect returns the destination');
		c.start();
		let out = render(128);
		assert(out[5] === 1.5, `constant * gain: ${out[5]}`);
		g.gain.setValueAtTime(0.25, ac.currentTime);
		assert(g.gain.value === 0.25, 'mirror follows setValueAtTime');
		out = render(128);
		assert(out[5] === 0.125, `scheduled value: ${out[5]}`);
		const mod = new ConstantSourceNode(ac, { offset: 1 });
		assert(mod.connect(g.gain) === undefined, 'connect(AudioParam) returns undefined');
		mod.start();
		out = render(128);
		assert(out[5] === 0.625, `param input is summed: ${out[5]}`);
		mod.disconnect(g.gain);
		out = render(128);
		assert(out[5] === 0.125, `disconnect(param): ${out[5]}`);
		const p = ac.createStereoPanner();
		p.pan.value = 2;
		assert(p.pan.value === 1 && p.pan.minValue === -1, 'mirror is clamped to the nominal range');
		c.disconnect();
		out = render(128);
		assert(out[5] === 0, 'disconnect()');
		return 'ok';
	)js")));
}

TEST(js_onended_fires_once_via_polling) {
	JsRig r;
	CHECK(ok(r.run(R"js(
		const ac = new AudioContext();
		let calls = 0, listener = 0, self;
		(() => { // only its own onended references it (superdough's webAudioTimeout)
			const c = new ConstantSourceNode(ac);
			c.connect(ac.destination);
			c.onended = function () { calls++; self = this; this.onended = null; };
			c.addEventListener('ended', (e) => (listener += e.type === 'ended'));
			c.start(0);
			c.stop(256 / 48000);
		})();
		gc();
		render(128);
		assert(calls === 0, 'not yet');
		render(256);
		assert(calls === 1 && listener === 1 && self instanceof ConstantSourceNode, `ended ${calls} ${listener}`);
		render(1024);
		assert(calls === 1 && listener === 1, 'only once');
		const s = ac.createBufferSource(); // released before start (superdough's releaseAudioNode): ends right away
		let ended = 0;
		s.onended = () => ended++;
		s.start(ac.currentTime + 5);
		s.stop();
		render(256);
		assert(ended === 1, 'start in the future, stop now');
		return 'ok';
	)js")));
}

// RESET (engine.cpp): __webaudio.cancel() stops what would start after the quantum the render thread may be rendering
// (stop time = start time: it never sounds, and still ends); what plays or starts before plays out; nothing stays.
TEST(js_cancel_silences_sources_not_rendered_yet) {
	JsRig r;
	CHECK(ok(r.run(R"js(
		const ac = new AudioContext();
		render(128);
		const base = nodeCount();
		const at = (frame) => frame / ac.sampleRate;
		let ended = 0;
		(() => {
			const playing = ac.createConstantSource();
			playing.connect(ac.destination);
			playing.start(at(128));
			playing.stop(at(256 + 2048));
		})();
		render(128); // the render position: frame 256
		(() => {
			const soon = ac.createConstantSource(); // within the next quantum: may be rendering already
			soon.offset.value = 10;
			soon.connect(ac.destination);
			soon.start(at(300));
			soon.stop(at(310));
			const later = [ac.createConstantSource(), ac.createOscillator(), ac.createBufferSource()];
			later[0].offset.value = 2;
			later[2].buffer = ac.createBuffer(1, 128, ac.sampleRate);
			later[2].buffer.getChannelData(0).fill(4);
			later.forEach((s, i) => {
				s.connect(ac.destination);
				s.onended = () => ended++;
				s.start(at(400 + 256 * i));
			});
		})();
		__webaudio.cancel();
		const out = render(2048);
		const want = (i) => (i >= 44 && i < 54 ? 11 : 1);
		assert(out.every((v, i) => v === want(i)), `heard ${out.findIndex((v, i) => v !== want(i))}`);
		assert(ended === 3, `ended ${ended}`);
		for (let k = 0; k < 4; k++) { render(128); gc(); }
		assert(nodeCount() === base, `${nodeCount()} nodes, baseline ${base}`);
		return 'ok';
	)js")));
}

TEST(js_buffers_are_acquired_on_assignment_and_start) {
	JsRig r;
	CHECK(ok(r.run(R"js(
		const ac = new AudioContext();
		const b = ac.createBuffer(1, 4, ac.sampleRate);
		b.getChannelData(0).set([1, 2, 3, 4]);
		const s = ac.createBufferSource();
		s.buffer = b; // snapshot [1 2 3 4]
		b.getChannelData(0)[0] = 9; // touched after assignment: start() takes a new snapshot
		s.connect(ac.destination);
		s.start();
		let out = render(128);
		assert(out[0] === 9 && out[3] === 4 && out[4] === 0, `played ${out.slice(0, 5)}`);
		const reversed = ac.createBuffer(1, 4, ac.sampleRate);
		reversed.copyToChannel(b.getChannelData(0).slice().reverse(), 0);
		const t = ac.createBufferSource();
		t.buffer = reversed;
		t.connect(ac.destination);
		t.start();
		out = render(128);
		assert(out[0] === 4 && out[3] === 9, `copyToChannel ${out.slice(0, 4)}`);
		const into = new Float32Array(2);
		reversed.copyFromChannel(into, 0, 1);
		assert(into[0] === 3 && into[1] === 2, 'copyFromChannel');
		assert(b.duration === 4 / 48000 && b.numberOfChannels === 1 && b.length === 4, 'attributes');
		return 'ok';
	)js")));
}

TEST(js_decode_audio_data_both_forms) {
	JsRig r;
	CHECK(ok(r.run(R"js(
		const ac = new AudioContext();
		const b = await ac.decodeAudioData(wav44k.slice(0));
		assert(b instanceof AudioBuffer && b.sampleRate === 48000 && b.length === 12000, `${b.sampleRate} ${b.length}`);
		const d = b.getChannelData(0);
		let err = 0; // resampled 44.1 -> 48 kHz: same frequency, amplitude and phase
		for (let i = 1000; i < 11000; i++) err = Math.max(err, Math.abs(d[i] - (16000 / 32768) * Math.sin((2 * Math.PI * 1000 * i) / 48000)));
		assert(err < 1e-3, `max error ${err}`);
		const viaCallback = await new Promise((ok, fail) => ac.decodeAudioData(wav44k.slice(0), ok, fail));
		assert(viaCallback.length === 12000, 'callback form');
		let failed;
		const rejected = await ac.decodeAudioData(new Uint8Array([1, 2, 3, 4]).buffer, null, (e) => (failed = e)).catch((e) => e);
		assert(rejected.name === 'EncodingError' && failed === rejected, `error forms: ${rejected}`);
		const s = ac.createBufferSource(); // a decoded buffer plays straight from its native data
		s.buffer = b;
		s.connect(ac.destination);
		s.start();
		const out = render(256);
		assert(out[100] === d[100], 'plays the decoded data');
		return 'ok';
	)js")));
}

TEST(js_offline_context_renders_and_completes) {
	JsRig r;
	CHECK(ok(r.run(R"js(
		const ctx = new OfflineAudioContext(1, 1000, 48000);
		const src = ctx.createBufferSource();
		const buffer = ctx.createBuffer(1, 600, 48000);
		buffer.getChannelData(0).fill(0.5);
		src.buffer = buffer;
		const f = ctx.createBiquadFilter(); // reverbGen.mjs shape: source -> lowpass sweep -> destination
		f.frequency.setValueAtTime(20000, 0);
		f.frequency.linearRampToValueAtTime(20000, 0.01);
		src.connect(f).connect(ctx.destination);
		src.start();
		let completed, ended = 0;
		src.onended = () => ended++;
		ctx.oncomplete = (e) => (completed = e.renderedBuffer);
		const rendered = await ctx.startRendering();
		assert(completed === rendered && rendered.length === 1000 && rendered.numberOfChannels === 1, 'oncomplete');
		const d = rendered.getChannelData(0);
		assert(Math.abs(d[400] - 0.5) < 0.01 && Math.abs(d[999]) < 0.05 && ended === 1, `rendered ${d[400]} ${d[999]} ${ended}`);
		assert(ctx.currentTime >= 1000 / 48000 && ctx.state === 'closed', 'time advanced');
		assert((await ctx.startRendering().catch((e) => e.name)) === 'InvalidStateError', 'only once');
		const conv = new AudioContext().createConvolver(); // the rendered buffer is a valid IR
		conv.buffer = new OfflineAudioContext(2, 128, 48000).createBuffer(2, 128, 48000);
		return 'ok';
	)js")));
}

TEST(js_dropped_nodes_return_to_baseline) {
	JsRig r;
	CHECK(ok(r.run(R"js(
		const ac = new AudioContext();
		render(128);
		const base = nodeCount();
		(() => {
			for (let i = 0; i < 100; i++) {
				const g = ac.createGain();
				g.gain.value = 0.5;
				g.connect(ac.destination);
				ac.createBiquadFilter().frequency.setValueAtTime(100, 0);
			}
		})();
		render(128);
		render(128);
		assert(nodeCount() === base, `refcounting alone frees nodes and their params: ${nodeCount()}`);
		(() => {
			for (let i = 0; i < 200; i++) {
				const o = ac.createOscillator(), g = ac.createGain(), f = ac.createBiquadFilter();
				o.connect(f).connect(g).connect(ac.destination);
				g.gain.setValueAtTime(0, 0);
				o.start(ac.currentTime);
				o.stop(ac.currentTime + 0.001 * i);
				o.onended = () => { o.disconnect(); f.disconnect(); g.disconnect(); }; // a cycle: node <-> closure
			}
		})();
		render(128);
		assert(nodeCount() > base + 500, `created ${nodeCount() - base}`);
		for (let k = 0; k < 20; k++) { render(1024); gc(); }
		assert(nodeCount() === base, `${nodeCount()} nodes, baseline ${base}`);
		return 'ok';
	)js")));
}

TEST(js_full_command_queue_never_drops) {
	JsRig r; // 40k commands before the first render: more than the 32k queue
	CHECK(ok(r.run(R"js(
		const ac = new AudioContext();
		const c = ac.createConstantSource();
		const keep = [];
		for (let i = 0; i < 20000; i++) {
			const g = ac.createGain();
			c.connect(g).connect(ac.destination);
			keep.push(g);
		}
		c.start();
		for (let k = 0; k < 3; k++) render(128); // each render + poll moves another queue-full in
		const out = render(128);
		assert(nodeCount() === 20002 && out[0] === 20000, `${nodeCount()} nodes, out ${out[0]}`);
		return 'ok';
	)js")));
}

TEST(js_fetch_and_log_bridge) {
	JsRig r;
	CHECK(ok(r.run(R"js(
		const json = await fetch('test/render/patterns.json').then((res) => res.json());
		assert(json.sampleRate === 48000, 'file path fetch');
		const err = await fetch('https://example.invalid/x.wav').catch((e) => e.message);
		assert(err.includes('https://example.invalid/x.wav'), err);
		document.dispatchEvent(new CustomEvent('strudel.log', { detail: { message: 'hello', type: 'info' } }));
		assert(window === globalThis && typeof window.addEventListener === 'function' && !navigator.mediaDevices, 'stubs');
		return 'ok';
	)js")));
	CHECK(r.logs.size() == 1 && r.logs[0] == "info: hello");
}

TEST(resampler_keeps_frequency_amplitude_and_phase) {
	std::vector<float> in(44100);
	for (size_t i = 0; i < in.size(); i++)
		in[i] = (float)std::sin(2 * M_PI * 1000 * i / 44100.0);
	std::vector<float> out = resample(in, 44100, 48000);
	CHECK(out.size() == 48000);
	double err = 0;
	for (size_t i = 2000; i < 46000; i++) // away from the edges, where the kernel runs out of input
		err = std::max(err, std::fabs(out[i] - std::sin(2 * M_PI * 1000 * i / 48000.0)));
	CHECK(err < 1e-3);
	std::vector<float> down = resample(std::vector<float>(48000, 1.f), 48000, 22050); // DC passes
	CHECK(down.size() == (size_t)(48000 / (48000 / 22050.0))); // Chrome's length formula: 22049 here
	CHECK(std::fabs(down[10000] - 1) < 1e-3);
	std::string error;
	CHECK(!decodeAudio((const uint8_t*)"OggS....", 8, kSr, error) && error.find("Ogg") != std::string::npos);
}
