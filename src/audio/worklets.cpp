#include "audio/worklets.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>

// The native ports repeat the JS processors operation for operation (test_worklets.cpp compares samples): no fused
// multiply-adds that JS arithmetic would not do. (Rack's -funsafe-math-optimizations builds trade that exactness away.)
#pragma STDC FP_CONTRACT OFF

namespace audio {

namespace {

constexpr size_t kMemoryLimit = 256 << 20;
// ponytail: QuickJS collects cycles when the heap outgrows the threshold, which can land inside a process() call.
// Raised so that it practically never does while collectGarbage() runs between blocks; measure GC pauses if it does.
constexpr size_t kGcThreshold = 32 << 20;
// Processors don't recurse: 256 KiB of JS stack, so the render thread needs >= 512 KiB of real stack (M2).
constexpr size_t kStackSize = 256 << 10;
constexpr double kLoadDeadline = 2;
constexpr int kMaxVoices = 1024; // a JS processor with more would blow its deadline: the native ports refuse too

// Every processor res/worklets.js registers, one per line: scripts/update-engine stops on a name that is not here.
// note: Native = its parameters in declaration order, Disabled = what the user calls it. Unlisted names run as scripts.
struct Entry {
	const char* name;
	ProcessorDef::Policy policy;
	const char* note;
};
const Entry kProcessors[] = {
	{"supersaw-oscillator", ProcessorDef::Native, "begin end frequency panspread freqspread detune voices"},
	{"wavetable-oscillator-processor", ProcessorDef::Native,
	 "begin end frequency detune freqspread position warp warpMode voices panspread phaserand"},
	{"lfo-processor", ProcessorDef::Script, nullptr},
	{"coarse-processor", ProcessorDef::Script, nullptr},
	{"crush-processor", ProcessorDef::Script, nullptr},
	{"shape-processor", ProcessorDef::Script, nullptr},
	{"djf-processor", ProcessorDef::Script, nullptr},
	{"ladder-processor", ProcessorDef::Script, nullptr},
	{"distort-processor", ProcessorDef::Script, nullptr},
	{"pulse-oscillator", ProcessorDef::Script, nullptr},
	{"byte-beat-processor", ProcessorDef::Script, nullptr},
	{"envelope-processor", ProcessorDef::Script, nullptr},
	{"transient-processor", ProcessorDef::Script, nullptr},
	{"phase-vocoder-processor", ProcessorDef::Disabled, "stretch"},
	{"generic-processor", ProcessorDef::Disabled, "kabelsalat"},
};

// AudioWorkletGlobalScope members beyond sampleRate/currentTime/currentFrame (set natively). Called once with the
// natives; returns the entry points the nodes use.
const char* kGlue = R"js(
(function (define, post, log) {
  'use strict';
  let pendingPort = null;
  class AudioWorkletProcessor {
    constructor() {
      if (!pendingPort) throw new TypeError('AudioWorkletProcessor: illegal constructor');
      this.port = pendingPort; // ponytail: a plain member, not the spec's getter
      pendingPort = null;
    }
  }
  const FLT_MAX = 3.4028234663852886e38;
  const registered = new Set();
  function registerProcessor(name, ctor) {
    name = String(name);
    const fail = (what, type) => {
      throw new DOMException(`registerProcessor('${name}'): ${what}`, type);
    };
    if (!name) fail('empty name', 'NotSupportedError');
    if (registered.has(name)) fail('already registered', 'NotSupportedError');
    if (typeof ctor !== 'function' || typeof ctor.prototype !== 'object') fail('not a constructor', 'TypeError');
    const flat = [];
    const seen = new Set();
    // Spec AudioParamDescriptor keys only: worklets.mjs' `min`/`max` are ignored, as in browsers.
    for (const d of ctor.parameterDescriptors ?? []) {
      const p = String(d.name);
      const { defaultValue = 0, minValue = -FLT_MAX, maxValue = FLT_MAX, automationRate = 'a-rate' } = d;
      if (seen.has(p)) fail(`parameter '${p}' declared twice`, 'NotSupportedError');
      if (!(defaultValue >= minValue && defaultValue <= maxValue)) fail(`'${p}' default out of range`, 'InvalidStateError');
      if (automationRate !== 'a-rate' && automationRate !== 'k-rate') fail(`'${p}': ${automationRate}`, 'TypeError');
      seen.add(p);
      flat.push(p, +defaultValue, +minValue, +maxValue, automationRate === 'a-rate');
    }
    define(name, ctor, flat);
    registered.add(name);
  }
  const console = {};
  for (const k of ['log', 'info', 'warn', 'error', 'debug']) console[k] = (...a) => log(a.join(' '), k === 'error');
  console.assert = (ok, ...a) => ok || log(`Assertion failed: ${a.join(' ')}`, true);
  Object.assign(globalThis, { AudioWorkletProcessor, registerProcessor, console });
  return {
    // The processor and its port: the AudioWorkletProcessor constructor picks the port up (spec "pending port").
    construct(ctor, id, options) {
      const port = { onmessage: null, postMessage: (message) => post(id, message), start() {}, close() {} };
      pendingPort = port;
      try {
        return [Reflect.construct(ctor, [options]), port];
      } finally {
        pendingPort = null;
      }
    },
    deliver(port, data) {
      if (typeof port.onmessage === 'function') port.onmessage({ type: 'message', data, target: port });
    },
  };
})
)js";

int64_t nowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

std::string str(JSContext* ctx, JSValueConst v) {
	size_t n = 0;
	const char* s = JS_ToCStringLen(ctx, &n, v);
	std::string out = s ? std::string(s, n) : std::string("(no text)");
	JS_FreeCString(ctx, s);
	return out;
}

/** The pending exception as "message\nstack"; clears it. */
std::string exceptionText(JSContext* ctx) {
	JSValue e = JS_GetException(ctx);
	std::string out = str(ctx, e);
	if (JS_IsError(e)) {
		JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
		if (JS_IsString(stack))
			out += "\n" + str(ctx, stack);
		JS_FreeValue(ctx, stack);
	}
	JS_FreeValue(ctx, e);
	return out;
}

/** ECMAScript ToInt32, i.e. `x | 0`. */
int32_t toInt32(double x) {
	if (std::fabs(x) < 2147483648.0)
		return (int32_t)x;
	if (!std::isfinite(x))
		return 0;
	double m = std::fmod(std::trunc(x), 4294967296.0);
	return (int32_t)(uint32_t)(m < 0 ? m + 4294967296.0 : m);
}

// worklets.mjs helpers, same names and operation order.
double clamp(double x, double lo, double hi) {
	return std::min(std::max(x, lo), hi); // Math.min(Math.max(...)): NaN stays NaN
}
double frac(double x) {
	return x - std::floor(x);
}
double ffloor(double x) {
	return toInt32(x);
}
double fround(double x) {
	return toInt32(x + 0.5);
}
double ffrac(double x) {
	return x - toInt32(x);
}

double polyBlep(double phase, double dt) {
	dt = std::min(dt, 1 - dt);
	double invdt = 1 / dt;
	if (phase < dt) {
		phase *= invdt;
		return 2 * phase - std::pow(phase, 2) - 1;
	}
	if (phase > 1 - dt) {
		phase = (phase - 1) * invdt;
		return std::pow(phase, 2) + 2 * phase + 1;
	}
	return 0;
}

double sawblep(double phase, double dt) {
	double v = 2 * phase - 1;
	return v - polyBlep(phase, dt);
}

/** getDetuner(unison, detune)(n). */
double detuner(double unison, double detune, int n) {
	if (unison < 2)
		return 0;
	double scale = detune / (unison - 1);
	double center = detune * 0.5;
	return n * scale - center;
}

double toFrequency(double frequency, double semitones) {
	return frequency * std::pow(2, semitones / 12); // applySemitoneDetuneToFrequency
}

// Wavetable warps (WarpMode): hash32/noise/brownian keep JS number semantics (doubles, ToInt32/ToUint32 on bit ops).
uint32_t toUint32(double x) {
	return (uint32_t)toInt32(x);
}
double shl(double x, int n) {
	return (int32_t)((uint32_t)toInt32(x) << n);
}
double hash32(double u) {
	u = u + 0x7ed55d16 + shl(u, 12);
	u = toInt32(u) ^ toInt32(0xc761c23c) ^ (int32_t)(toUint32(u) >> 19);
	u = u + 0x165667b1 + shl(u, 5);
	u = toInt32(u + 0xd3a2646c) ^ toInt32(shl(u, 9));
	u = u + 0xfd7046c5 + shl(u, 3);
	u = toInt32(u) ^ toInt32(0xb55a4f09) ^ (int32_t)(toUint32(u) >> 16);
	return toUint32(u);
}
double hash01(double i) {
	return (toUint32(hash32(i)) >> 8) / (double)0x01000000;
}
double noise(double x) {
	double i = std::floor(x), f = x - i;
	double a = hash01(i), b = hash01(i + 1);
	return a + (b - a) * f;
}
double brownian(double x, int oct) {
	double amp = 0.5, sum = 0, norm = 0, freq = 1;
	for (int o = 0; o < oct; o++) {
		sum += amp * noise(x * freq);
		norm += amp;
		amp *= 0.5;
		freq *= 2;
	}
	return (sum / norm) * 2 - 1;
}
bool isPrime(int n) {
	if (n < 2)
		return false;
	if (n % 2 == 0)
		return n == 2;
	for (int d = 3; d * d <= n; d += 2)
		if (n % d == 0)
			return false;
	return true;
}
int bitReverse(int i, int n) {
	int r = 0;
	for (int b = 0; b < n; b++) {
		r = (r << 1) | (i & 1);
		i = (int)((unsigned)i >> 1);
	}
	return r;
}
/** _toBits(amt, min, max): {b, n}. */
void toBits(double amt, double min, double max, double& b, double& n) {
	b = max + (min - max) * amt;
	n = fround(std::pow(2, b));
}
double mirror(double x) {
	return 1 - std::fabs(2 * x - 1);
}

double warpPhase(double phase, double amt, double mode) {
	constexpr double kTwoPi = 2 * M_PI;
	double b, n;
	int m = mode >= 0 && mode <= 21 && mode == std::floor(mode) ? (int)mode : -1; // switch: strict equality
	switch (m) {
	case 1: { // ASYM
		double a = 0.01 + 0.99 * amt;
		return phase < a ? (0.5 * phase) / a : 0.5 + (0.5 * (phase - a)) / (1 - a);
	}
	case 2: // MIRROR
		return mirror(warpPhase(phase, amt, 1));
	case 3: // BENDP
		return std::pow(phase, 1 + 3 * amt);
	case 4: // BENDM
		return std::pow(phase, 1 / (1 + 3 * amt));
	case 5: // BENDMP
		return amt < 0.5 ? warpPhase(phase, 1 - 2 * amt, 3) : warpPhase(phase, 2 * amt - 1, 2);
	case 6: // SYNC
		return std::fmod(phase * std::pow(16, std::pow(amt, 2)), 1);
	case 7: // QUANT
		toBits(amt, 2, 12, b, n);
		return ffloor(phase * n) / n;
	case 8: { // FOLD
		double k = 1 + std::max(1.0, fround(7 * amt));
		return std::fabs(ffrac(k * phase) - 0.5) * 2;
	}
	case 9: { // PWM
		double w = clamp(0.5 + 0.49 * (2 * amt - 1), 0, 1);
		if (phase < w)
			return (phase / w) * 0.5;
		return 0.5 + ((phase - w) / (1 - w)) * 0.5;
	}
	case 10: // ORBIT
		return frac(phase + 0.5 * amt * std::sin(kTwoPi * 3 * phase));
	case 11: // SPIN
		toBits(amt, 1, 6, b, n);
		return frac(phase + 0.5 * amt * std::sin(kTwoPi * n * phase));
	case 12: { // CHAOS
		double r = 3.7 + 0.3 * amt;
		double logistic = r * phase * (1 - phase);
		return clamp((1 - amt) * phase + amt * logistic, 0, 1);
	}
	case 13: { // PRIMES
		toBits(amt, 3, 12, b, n);
		int k = (int)n;
		while (!isPrime(k))
			k++;
		return ffloor(phase * k) / k;
	}
	case 14: { // BINARY
		toBits(amt, 3, 12, b, n);
		int bits = (int)fround(b);
		int count = 1 << bits;
		int idx = (int)ffloor(phase * count);
		return bitReverse(idx, bits) / (double)count;
	}
	case 15: // BROWNIAN
		return frac(phase + 0.25 * amt * brownian(64 * phase, 4));
	case 16: { // RECIPROCAL
		double g = 2 + 4 * amt;
		double num = phase * g;
		double den = phase + (1 - phase) * g;
		return clamp(den > 1e-12 ? num / den : 0, 0, 1);
	}
	case 17: { // WORMHOLE
		double gap = clamp(0.8 * amt, 0, 1);
		double a = 0.5 * (1 - gap), c = 0.5 * (1 + gap);
		if (phase < a)
			return (phase / a) * 0.5;
		if (phase > c)
			return 0.5 * (1 + (phase - c) / (1 - c));
		return 0.5;
	}
	case 18: { // LOGISTIC
		double x = phase, r = 3.6 + 0.4 * amt;
		double iters = 1 + fround(2 * amt);
		for (int i = 0; i < iters; i++)
			x = r * x * (1 - x);
		return clamp(x, 0, 1);
	}
	case 19: { // SIGMOID
		double k = 1 + 10 * amt;
		double x = phase - 0.5;
		double y = 1 / (1 + std::exp(-k * x));
		double y0 = 1 / (1 + std::exp(0.5 * k));
		double y1 = 1 / (1 + std::exp(-0.5 * k));
		return (y - y0) / (y1 - y0);
	}
	case 20: // FRACTAL
		return frac(phase + 0.5 * std::sin(kTwoPi * phase) * amt);
	default: // NONE, FLIP, anything else
		return phase;
	}
}

double sampleFrame(const float* frame, int len, double phase) {
	double pos = phase * len;
	int i = toInt32(pos);
	if (i >= len)
		i = 0;
	if (i < 0)
		return NAN; // frame[-1] is undefined in JS
	double f = pos - i;
	double a = frame[i];
	int i1 = i + 1 >= len ? 0 : i + 1;
	double b = frame[i1];
	return a + (b - a) * f;
}

} // namespace

// --- AudioWorkletNode processing, shared by the QuickJS host and the native ports ---

struct WorkletNode : Node {
	WorkletNode(double sr, WorkletScope* scope, const ProcessorDef& def, int numInputs,
				const std::vector<int>& outputChannels)
		: Node(sr, numInputs, (int)outputChannels.size()), scope(Resource::retain(scope)), def(def),
		  channels(outputChannels) {
		for (const ProcessorDef::Param& p : def.params)
			owned.emplace_back(new AudioParam(this, p.defaultValue, p.minValue, p.maxValue, p.aRate));
	}
	~WorkletNode() override { release(scope); }

	// Spec: processed while [[callable process]] and (active source flag or an actively processing input).
	void process(int64_t f0) final {
		bool inputActive = false;
		for (const Input& in : inputs)
			inputActive |= !in.bus.silent;
		if (!callable || !(activeSource || inputActive)) {
			for (Bus& b : outputs)
				b.silence();
			return;
		}
		for (size_t o = 0; o < outputs.size(); o++) {
			outputs[o].silence(); // zeroed
			outputs[o].begin(channels[o] ? channels[o] : inputs[0].bus.channels);
		}
		activeSource = run(f0);
		if (!callable)
			for (Bus& b : outputs)
				b.silence();
	}

  protected:
	/** One quantum into the zeroed outputs; returns the active source flag. */
	virtual bool run(int64_t f0) = 0;
	/** The processor threw (or would have): silent from now on, one ProcessorError event. */
	void fail(const std::string& what) {
		callable = false;
		scope->emitText(Event::ProcessorError, id, 0, def.name + ": " + what);
	}
	const AudioParam& param(int i) const { return *owned[i]; }

	WorkletScope* scope;
	const ProcessorDef& def; // lives in the scope
	std::vector<int> channels;
	std::vector<std::unique_ptr<AudioParam>> owned; // def.params order = params
	bool callable = true, activeSource = true;
};

/** The module's processor, run in the scope's QuickJS runtime. Its JS state is built on the first entry and freed
on retire(), both on the render thread. */
struct ScriptWorkletNode : WorkletNode {
	ScriptWorkletNode(double sr, WorkletScope* scope, const ProcessorDef& def, int numInputs,
					  const std::vector<int>& outputChannels, Blob* options)
		: WorkletNode(sr, scope, def, numInputs, outputChannels), options(options) {}
	~ScriptWorkletNode() override {
		freeScript(); // graph destroyed with the node still in it (no render thread any more)
		release(options);
	}
	void retire() override { freeScript(); }

	void setAttr(int attr, double v, Resource* r) override {
		if (attr != AttrMessage)
			return WorkletNode::setAttr(attr, v, r);
		// Delivered at the quantum boundary, before the quantum that starts at currentTime.
		scope->enter(graph, std::llround(graph->currentTime() * sampleRate));
		if (callable && (built || build())) {
			auto* blob = static_cast<Blob*>(r);
			JSContext* ctx = scope->ctx_;
			JSValue data = JS_ReadObject(ctx, blob->bytes.data(), blob->bytes.size(), JS_READ_OBJ_REFERENCE);
			JSValue args[2] = {port, data};
			JSValue res = JS_IsException(data) ? data : scope->call(scope->deliver_, JS_UNDEFINED, 2, args);
			if (JS_IsException(res)) // like an exception in a browser's onmessage: reported, the node runs on
				scope->emitText(Event::Log, 0, 1, def.name + " onmessage: " + exceptionText(ctx));
			JS_FreeValue(ctx, res);
			JS_FreeValue(ctx, data);
		}
		graph->drop(r);
	}

  protected:
	bool run(int64_t f0) override {
		scope->enter(graph, f0);
		if (!built && !build())
			return false;
		JSContext* ctx = scope->ctx_;
		for (size_t i = 0; i < inputs.size(); i++) {
			const Bus& b = inputs[i].bus;
			JSValue set = b.silent ? ins[i].none : b.channels == 1 ? ins[i].mono : ins[i].stereo;
			JS_SetPropertyUint32(ctx, inputsJs, (uint32_t)i, JS_DupValue(ctx, set));
		}
		for (size_t o = 0; o < outputs.size(); o++)
			JS_SetPropertyUint32(ctx, outputsJs, (uint32_t)o,
								 JS_DupValue(ctx, outputs[o].channels == 1 ? outs[o].mono : outs[o].stereo));
		for (size_t p = 0; p < views.size(); p++)
			JS_SetProperty(ctx, paramsJs, views[p].name,
						   JS_DupValue(ctx, owned[p]->constant ? views[p].one : views[p].all));
		JSValue args[3] = {inputsJs, outputsJs, paramsJs};
		JSValue r = scope->call(processFn, processor, 3, args);
		if (JS_IsException(r)) {
			fail("process(): " + exceptionText(ctx));
			return false;
		}
		bool active = JS_ToBool(ctx, r);
		JS_FreeValue(ctx, r);
		return active;
	}

  private:
	struct InputSet {
		JSValue none, mono, stereo;
	};
	struct OutputSet {
		JSValue mono, stereo;
	};
	struct ParamViews {
		JSAtom name;
		JSValue one, all; // the param's values[] as length 1 (constant quantum) and 128
	};

	JSValue keep(JSValue v) {
		values.push_back(v);
		return v;
	}
	/** A Float32Array of `length` floats from float `offset` of `buffer` (this node's memory, detached on retire). */
	JSValue view(JSValue buffer, size_t offset, size_t length) {
		JSContext* ctx = scope->ctx_;
		JSValue args[3] = {buffer, JS_NewInt64(ctx, (int64_t)(offset * sizeof(float))),
						   JS_NewInt64(ctx, (int64_t)length)};
		return keep(JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_FLOAT32));
	}
	JSValue external(void* p, size_t bytes) {
		JSValue b = keep(JS_NewArrayBuffer(scope->ctx_, static_cast<uint8_t*>(p), bytes, 0, nullptr, nullptr, false));
		buffers.push_back(b);
		return b;
	}
	JSValue list(std::initializer_list<JSValue> items) {
		JSContext* ctx = scope->ctx_;
		JSValue a = keep(JS_NewArray(ctx));
		uint32_t i = 0;
		for (JSValue v : items)
			JS_SetPropertyUint32(ctx, a, i++, JS_DupValue(ctx, v));
		return a;
	}

	/** Preallocated process() arguments over the node's own buses and params, then the processor itself. */
	bool build() {
		built = true;
		JSContext* ctx = scope->ctx_;
		for (Input& in : inputs) {
			JSValue b = external(in.bus.data, sizeof in.bus.data);
			JSValue ch0 = view(b, 0, kQuantum), ch1 = view(b, kQuantum, kQuantum);
			ins.push_back({list({}), list({ch0}), list({ch0, ch1})});
		}
		for (Bus& out : outputs) {
			JSValue b = external(out.data, sizeof out.data);
			JSValue ch0 = view(b, 0, kQuantum), ch1 = view(b, kQuantum, kQuantum);
			outs.push_back({list({ch0}), list({ch0, ch1})});
		}
		inputsJs = keep(JS_NewArray(ctx));
		outputsJs = keep(JS_NewArray(ctx));
		paramsJs = keep(JS_NewObject(ctx));
		for (size_t p = 0; p < owned.size(); p++) {
			JSValue b = external(owned[p]->values, sizeof owned[p]->values);
			views.push_back({JS_NewAtom(ctx, def.params[p].name.c_str()), view(b, 0, 1), view(b, 0, kQuantum)});
		}
		JSValue opts = options ? JS_ReadObject(ctx, options->bytes.data(), options->bytes.size(), JS_READ_OBJ_REFERENCE)
							   : JS_NewObject(ctx);
		JSValue args[3] = {def.ctor, JS_NewFloat64(ctx, (double)id), opts};
		JSValue pair = JS_IsException(opts) ? opts : scope->call(scope->construct_, JS_UNDEFINED, 3, args);
		JS_FreeValue(ctx, opts);
		if (JS_IsException(pair)) {
			fail("constructor: " + exceptionText(ctx));
			return false;
		}
		processor = keep(JS_GetPropertyUint32(ctx, pair, 0));
		port = keep(JS_GetPropertyUint32(ctx, pair, 1));
		JS_FreeValue(ctx, pair);
		processFn = keep(JS_GetPropertyStr(ctx, processor, "process"));
		if (!JS_IsFunction(ctx, processFn)) {
			fail("process is not a function");
			return false;
		}
		return true;
	}

	void freeScript() {
		if (!built)
			return;
		built = false;
		JSContext* ctx = scope->ctx_;
		JS_UpdateStackTop(scope->rt_);
		for (JSValue b : buffers)
			JS_DetachArrayBuffer(ctx, b); // a processor that kept an array can't reach our freed memory
		if (JS_IsObject(port))
			JS_SetPropertyStr(ctx, port, "onmessage", JS_NULL); // breaks processor <-> port <-> handler cycles
		for (JSValue v : values)
			JS_FreeValue(ctx, v);
		for (const ParamViews& p : views)
			JS_FreeAtom(ctx, p.name);
		values.clear();
		buffers.clear();
		views.clear();
		ins.clear();
		outs.clear();
	}

	Blob* options;
	bool built = false;
	JSValue processor = JS_UNDEFINED, port = JS_UNDEFINED, processFn = JS_UNDEFINED;
	JSValue inputsJs = JS_UNDEFINED, outputsJs = JS_UNDEFINED, paramsJs = JS_UNDEFINED;
	std::vector<JSValue> values, buffers; // every value we hold; the external ArrayBuffers among them
	std::vector<InputSet> ins;
	std::vector<OutputSet> outs;
	std::vector<ParamViews> views;
};

namespace {

/** Phases of the native ports: NaN = not drawn yet (`this.phase[n] ?? Math.random()`). */
struct Phases {
	std::vector<double> v = std::vector<double>(kMaxVoices, NAN);
	void reset() { std::fill(v.begin(), v.end(), NAN); }
};

// worklets.mjs SuperSawOscillatorProcessor. Params: begin end frequency panspread freqspread detune voices.
struct SuperSawNode : WorkletNode {
	using WorkletNode::WorkletNode;
	void setAttr(int attr, double v, Resource* r) override {
		if (attr == AttrInitialize) // {type: 'initialize'}: this.phase = []
			phase.reset();
		WorkletNode::setAttr(attr, v, r);
	}

  protected:
	bool run(int64_t f0) override {
		double now = (double)f0 / sampleRate;
		double begin = param(0).at(0), end = param(1).at(0);
		if (end >= 0 && now >= end + 0.5)
			return false;
		if ((end >= 0 && now >= end) || now <= begin || !(begin >= 0))
			return true;
		double voices = param(6).at(0); // k-rate
		if (outputs[0].channels < 2 || voices > kMaxVoices) {
			fail(voices > kMaxVoices ? "too many voices" : "TypeError: output[1] is undefined");
			return false;
		}
		float* L = outputs[0].data[0];
		float* R = outputs[0].data[1];
		const double invSr = 1 / sampleRate;
		for (int i = 0; i < kQuantum; i++) {
			double detune = param(5).at(i);
			double freqspread = param(4).at(i);
			double panspread = param(3).at(i) * 0.5 + 0.5;
			double gainL = std::sqrt(1 - panspread);
			double gainR = std::sqrt(panspread);
			double freq = toFrequency(param(2).at(i), detune / 100);
			for (int n = 0; n < voices; n++) {
				double freqVoice = toFrequency(freq, detuner(voices, freqspread, n));
				double dt = frac(freqVoice * invSr);
				double& ph = phase.v[n];
				if (std::isnan(ph))
					ph = scope->random();
				double v = sawblep(ph, dt);
				L[i] = (float)(L[i] + v * gainL);
				R[i] = (float)(R[i] + v * gainR);
				double pn = ph + dt;
				if (pn >= 1.0)
					pn -= 1.0;
				ph = pn;
				std::swap(gainL, gainR);
			}
		}
		return true;
	}

  private:
	Phases phase;
};

// worklets.mjs WavetableOscillatorProcessor. Params: begin end frequency detune freqspread position warp warpMode
// voices panspread phaserand. The table comes with 'initialize' (AttrInitialize), shared, never copied per note.
struct WavetableNode : WorkletNode {
	using WorkletNode::WorkletNode;
	~WavetableNode() override { release(table); }
	void setAttr(int attr, double v, Resource* r) override {
		if (attr != AttrInitialize)
			return WorkletNode::setAttr(attr, v, r);
		graph->drop(table);
		table = static_cast<WavetableData*>(r);
		phase.reset();
	}

  protected:
	bool run(int64_t f0) override {
		double now = (double)f0 / sampleRate;
		double begin = param(0).at(0), end = param(1).at(0);
		if (end >= 0 && now >= end + 0.5)
			return false;
		if ((end >= 0 && now >= end) || now <= begin || !(begin >= 0))
			return true;
		if (!table)
			return true;                // zeros
		double voices = param(8).at(0); // k-rate
		if (voices > kMaxVoices) {
			fail("too many voices");
			return false;
		}
		float* L = outputs[0].data[0];
		float* R = outputs[0].channels > 1 ? outputs[0].data[1] : L; // outputs[0][1] || outputs[0][0]
		const int frames = table->frames, len = table->frameLen;
		const double invSr = 1 / sampleRate;
		for (int i = 0; i < kQuantum; i++) {
			double detune = param(3).at(i);
			double freqspread = param(4).at(i);
			double tablePos = clamp(param(5).at(i), 0, 1);
			double idx = tablePos * (frames - 1);
			int fIdx = toInt32(idx);
			double interpT = idx - fIdx;
			double warpAmount = clamp(param(6).at(i), 0, 1);
			double warpMode = param(7).at(i);
			double phaseRand = clamp(param(10).at(i), 0, 1);
			double panspread = voices > 1 ? clamp(param(9).at(i), 0, 1) : 0;
			double gain1 = std::sqrt(0.5 - 0.5 * panspread);
			double gain2 = std::sqrt(0.5 + 0.5 * panspread);
			double f = toFrequency(param(2).at(i), detune / 100);
			double normalizer = 1 / std::sqrt(voices);
			const float* frame0 = &table->data[(size_t)fIdx * len];
			const float* frame1 = &table->data[(size_t)std::min(frames - 1, fIdx + 1) * len];
			for (int n = 0; n < voices; n++) {
				bool odd = (n & 1) == 1;
				double gainL = odd ? gain2 : gain1, gainR = odd ? gain1 : gain2;
				double fVoice = toFrequency(f, detuner(voices, freqspread, n));
				double dPhase = fVoice * invSr;
				double& ph = phase.v[n];
				if (std::isnan(ph))
					ph = scope->random() * phaseRand;
				double w = warpPhase(ph, warpAmount, warpMode);
				double s0 = sampleFrame(frame0, len, w);
				double s1 = sampleFrame(frame1, len, w);
				double s = interpT * (s1 - s0) + s0;
				if (warpMode == 21 && ph < warpAmount) // FLIP
					s = -s;
				L[i] = (float)(L[i] + s * gainL * normalizer);
				R[i] = (float)(R[i] + s * gainR * normalizer);
				ph = frac(ph + dPhase);
			}
		}
		return true;
	}

  private:
	WavetableData* table = nullptr;
	Phases phase;
};

bool nativeMatches(const char* note, const std::vector<ProcessorDef::Param>& params) {
	std::string names;
	for (const ProcessorDef::Param& p : params)
		names += (names.empty() ? "" : " ") + p.name;
	return names == note;
}

} // namespace

// --- WorkletScope ---

WorkletScope::WorkletScope(double sampleRate) : sampleRate_(sampleRate) {
	randomState = (uint32_t)nowNs() ^ (uint32_t)(uintptr_t)this;
	randomState |= 1;
	rt_ = JS_NewRuntime();
	JS_SetMemoryLimit(rt_, kMemoryLimit);
	JS_SetGCThreshold(rt_, kGcThreshold);
	JS_SetMaxStackSize(rt_, kStackSize);
	JS_SetInterruptHandler(rt_, interrupt, this);
	ctx_ = JS_NewContext(rt_);
	JS_SetContextOpaque(ctx_, this);
	global_ = JS_GetGlobalObject(ctx_);
	timeAtom_ = JS_NewAtom(ctx_, "currentTime");
	frameAtom_ = JS_NewAtom(ctx_, "currentFrame");
	JS_SetPropertyStr(ctx_, global_, "sampleRate", JS_NewFloat64(ctx_, sampleRate));
	enter(nullptr, 0);
	JSValue fn = JS_Eval(ctx_, kGlue, std::strlen(kGlue), "<worklet scope>", JS_EVAL_TYPE_GLOBAL);
	JSValue natives[3] = {JS_NewCFunction(ctx_, define, "define", 3), JS_NewCFunction(ctx_, post, "post", 2),
						  JS_NewCFunction(ctx_, log, "log", 2)};
	JSValue api = JS_Call(ctx_, fn, JS_UNDEFINED, 3, natives);
	construct_ = JS_GetPropertyStr(ctx_, api, "construct");
	deliver_ = JS_GetPropertyStr(ctx_, api, "deliver");
	for (JSValue v : {api, fn, natives[0], natives[1], natives[2]})
		JS_FreeValue(ctx_, v);
}

WorkletScope::~WorkletScope() {
	JS_UpdateStackTop(rt_);
	for (ProcessorDef& d : defs_)
		JS_FreeValue(ctx_, d.ctor);
	for (JSValue v : {construct_, deliver_, global_})
		JS_FreeValue(ctx_, v);
	JS_FreeAtom(ctx_, timeAtom_);
	JS_FreeAtom(ctx_, frameAtom_);
	JS_FreeContext(ctx_);
	JS_FreeRuntime(rt_);
}

bool WorkletScope::load(const std::string& source, const std::string& url, std::string& error) {
	if (sealed()) {
		error = "the scope is in use by the render thread";
		return false;
	}
	JS_UpdateStackTop(rt_);
	deadlineNs_ = nowNs() + (int64_t)(kLoadDeadline * 1e9);
	JSValue v = JS_Eval(ctx_, source.c_str(), source.size(), url.c_str(), JS_EVAL_TYPE_GLOBAL);
	deadlineNs_ = 0;
	bool ok = !JS_IsException(v);
	if (ok)
		modules_.push_back(url);
	else
		error = exceptionText(ctx_);
	JS_FreeValue(ctx_, v);
	return ok;
}

bool WorkletScope::loaded(const std::string& url) const {
	return std::find(modules_.begin(), modules_.end(), url) != modules_.end();
}

const ProcessorDef* WorkletScope::find(const std::string& name) const {
	for (const ProcessorDef& d : defs_)
		if (d.name == name)
			return &d;
	return nullptr;
}

void WorkletScope::collectGarbage() {
	if (!sealed())
		return;
	JS_UpdateStackTop(rt_);
	JS_RunGC(rt_);
	JS_SetGCThreshold(rt_, kGcThreshold); // the automatic collection would have lowered it
}

double WorkletScope::random() {
	randomState ^= randomState << 13;
	randomState ^= randomState >> 17;
	randomState ^= randomState << 5;
	return randomState / 4294967296.0;
}

int WorkletScope::interrupt(JSContext*, void* opaque) {
	auto* s = static_cast<WorkletScope*>(opaque);
	s->interrupted_ = s->deadlineNs_ && nowNs() > s->deadlineNs_;
	return s->interrupted_;
}

void WorkletScope::enter(Graph* graph, int64_t f0) {
	JS_UpdateStackTop(rt_);
	if (graph)
		graph_ = graph;
	if (f0 >= 0 && f0 != frame_) {
		frame_ = f0;
		JS_SetProperty(ctx_, global_, frameAtom_, JS_NewInt64(ctx_, f0));
		JS_SetProperty(ctx_, global_, timeAtom_, JS_NewFloat64(ctx_, (double)f0 / sampleRate_));
	}
}

JSValue WorkletScope::call(JSValueConst fn, JSValueConst self, int argc, JSValueConst* argv) {
	deadlineNs_ = nowNs() + (int64_t)(deadline * 1e9);
	interrupted_ = false;
	JSValue r = JS_Call(ctx_, fn, self, argc, argv);
	if (interrupted_) {
		JS_FreeValue(ctx_, JS_GetException(ctx_));
		JS_ThrowInternalError(ctx_, "exceeded its %.0f ms budget per call and was interrupted", deadline * 1e3);
	}
	deadlineNs_ = 0;
	return r;
}

void WorkletScope::emitText(Event::Type type, uint64_t id, int code, const std::string& text) {
	if (!graph_) { // console output while a module loads (producer thread)
		std::fprintf(stderr, "[worklet] %s\n", text.c_str());
		return;
	}
	// ponytail: allocates on the render thread, like the QuickJS call that produced the text; rare by nature.
	auto* b = new Blob;
	b->bytes.assign(text.begin(), text.end());
	graph_->emit({type, id, code, b});
}

// define(name, ctor, [name, default, min, max, aRate, ...]): registerProcessor() after its spec checks (kGlue).
JSValue WorkletScope::define(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
	auto* s = static_cast<WorkletScope*>(JS_GetContextOpaque(ctx));
	ProcessorDef d;
	d.name = str(ctx, argv[0]);
	int64_t n = 0;
	JS_GetLength(ctx, argv[2], &n);
	for (int64_t i = 0; i + 5 <= n; i += 5) {
		JSValue v[5];
		for (int k = 0; k < 5; k++)
			v[k] = JS_GetPropertyInt64(ctx, argv[2], i + k);
		double def = 0, min = 0, max = 0;
		JS_ToFloat64(ctx, &def, v[1]);
		JS_ToFloat64(ctx, &min, v[2]);
		JS_ToFloat64(ctx, &max, v[3]);
		d.params.push_back({str(ctx, v[0]), (float)def, (float)min, (float)max, JS_ToBool(ctx, v[4]) != 0});
		for (JSValue x : v)
			JS_FreeValue(ctx, x);
	}
	for (const Entry& e : kProcessors)
		if (d.name == e.name) {
			d.listed = true;
			d.policy = e.policy;
			d.feature = e.policy == ProcessorDef::Disabled ? e.note : nullptr;
			d.nativeMatches = e.policy == ProcessorDef::Native && nativeMatches(e.note, d.params);
		}
	d.ctor = JS_DupValue(ctx, argv[1]);
	s->defs_.push_back(std::move(d));
	return JS_UNDEFINED;
}

// post(id, message): a processor's port.postMessage(), render thread.
JSValue WorkletScope::post(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
	auto* s = static_cast<WorkletScope*>(JS_GetContextOpaque(ctx));
	double id = 0;
	JS_ToFloat64(ctx, &id, argv[0]);
	size_t size = 0;
	uint8_t* bytes = JS_WriteObject(ctx, &size, argv[1], JS_WRITE_OBJ_REFERENCE);
	if (!bytes)
		return JS_EXCEPTION; // not serializable (a function, ...)
	if (s->graph_) {
		auto* b = new Blob; // ponytail: render-thread allocation, as above
		b->bytes.assign(bytes, bytes + size);
		s->graph_->emit({Event::Message, (uint64_t)id, 0, b});
	}
	js_free(ctx, bytes);
	return JS_UNDEFINED;
}

JSValue WorkletScope::log(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
	auto* s = static_cast<WorkletScope*>(JS_GetContextOpaque(ctx));
	s->emitText(Event::Log, 0, JS_ToBool(ctx, argv[1]), str(ctx, argv[0]));
	return JS_UNDEFINED;
}

Node* createWorkletNode(double sampleRate, WorkletScope* scope, const ProcessorDef& def, int numInputs,
						const std::vector<int>& outputChannels, Blob* options, bool allowNative) {
	scope->seal();
	if (allowNative && def.policy == ProcessorDef::Native && def.nativeMatches) {
		release(options);
		if (def.name == "supersaw-oscillator")
			return new SuperSawNode(sampleRate, scope, def, numInputs, outputChannels);
		return new WavetableNode(sampleRate, scope, def, numInputs, outputChannels);
	}
	return new ScriptWorkletNode(sampleRate, scope, def, numInputs, outputChannels, options);
}

} // namespace audio
