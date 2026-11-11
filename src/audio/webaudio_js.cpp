#include "audio/webaudio_js.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>

#include "audio/decode.hpp"
#include "audio/nodes.hpp"

// src/audio/webaudio.js, embedded by `xxd -i` (Rack's BINARIES rule; the test Makefile does the same).
extern "C" unsigned char src_audio_webaudio_js[];
extern "C" unsigned int src_audio_webaudio_js_len;

namespace audio {

namespace {

/** One graph as seen from the JS thread. Refcounted by every handle that may still post to it. */
struct Ctx {
	Graph* graph;
	bool owned; // an OfflineAudioContext's private graph; the realtime one belongs to the embedder
	int refs = 1;
	std::deque<Command> pending; // contract (c): commands the full queue refused, retried in order

	Ctx(Graph* g, bool owned) : graph(g), owned(owned) {}
	~Ctx() {
		flush();
		for (const Command& c : pending) { // never reached the graph
			if (c.type == Command::Create)
				delete c.node;
			release(c.res);
		}
		if (owned)
			delete graph;
	}
	void flush() {
		while (!pending.empty() && graph->post(pending.front()))
			pending.pop_front();
	}
	void post(const Command& c) {
		flush();
		if (!pending.empty() || !graph->post(c))
			pending.push_back(c);
	}
};

void unref(Ctx* c) {
	if (--c->refs == 0)
		delete c;
}

Command command(Command::Type type, Node* n) {
	Command c;
	c.type = type;
	c.node = n;
	return c;
}

// Every native object JS holds is one class whose opaque is a Handle; finalizers release what it owns.
struct Handle {
	virtual ~Handle() = default;
};

struct CtxHandle : Handle {
	Ctx* c;
	explicit CtxHandle(Ctx* c) : c(c) { c->refs++; }
	~CtxHandle() override { unref(c); }
};

// Shared by a JS node and its AudioParams (contract a): the native node is disposed once all of them are gone.
// Started sources are pinned in JS until 'ended' (contract b, webaudio.js `playing`).
struct NodeHandle : Handle {
	Ctx* c;
	Node* n;
	bool owned; // false for the destination
	NodeHandle(Ctx* c, Node* n, bool owned) : c(c), n(n), owned(owned) { c->refs++; }
	~NodeHandle() override {
		if (owned)
			c->post(command(Command::Dispose, n));
		unref(c);
	}
};

struct ResHandle : Handle {
	Resource* r;
	explicit ResHandle(Resource* r) : r(r) {}
	~ResHandle() override { release(r); }
};

struct Binding : Handle {
	WebAudioHost* host = nullptr;
	JSClassID cls = 0;
	Ctx* realtime = nullptr;
	~Binding() override {
		if (realtime)
			unref(realtime);
	}
	void log(const std::string& message, const std::string& type) const {
		if (host->log)
			host->log(message, type);
		else
			std::fprintf(stderr, "%s\n", message.c_str());
	}
};

void finalize(JSRuntime*, JSValueConst v) {
	JSClassID id;
	delete static_cast<Handle*>(JS_GetAnyOpaque(v, &id));
}

Binding* binding(JSValueConst* data) {
	JSClassID id;
	return static_cast<Binding*>(static_cast<Handle*>(JS_GetAnyOpaque(data[0], &id)));
}

JSValue wrap(JSContext* ctx, const Binding* b, Handle* h) {
	JSValue o = JS_NewObjectClass(ctx, (int)b->cls);
	if (JS_IsException(o))
		delete h;
	else
		JS_SetOpaque(o, h);
	return o;
}

/** The handle of type T in v, or nullptr with a TypeError pending. */
template <class T> T* get(JSContext* ctx, const Binding* b, JSValueConst v) {
	auto* h = dynamic_cast<T*>(static_cast<Handle*>(JS_GetOpaque(v, b->cls)));
	if (!h)
		JS_ThrowTypeError(ctx, "webaudio: not a valid handle");
	return h;
}

double num(JSContext* ctx, JSValueConst v) {
	double d = NAN;
	JS_ToFloat64(ctx, &d, v);
	return d;
}

std::string str(JSContext* ctx, JSValueConst v) {
	size_t n = 0;
	const char* s = JS_ToCStringLen(ctx, &n, v);
	std::string out = s ? std::string(s, n) : std::string();
	JS_FreeCString(ctx, s);
	return out;
}

bool isNull(JSValueConst v) {
	return JS_IsNull(v) || JS_IsUndefined(v);
}

/** Float32Array contents, or nullptr with a TypeError pending. */
const float* floats(JSContext* ctx, JSValueConst v, size_t& n) {
	if (JS_GetTypedArrayType(v) != JS_TYPED_ARRAY_FLOAT32) {
		JS_ThrowTypeError(ctx, "webaudio: expected a Float32Array");
		return nullptr;
	}
	size_t offset = 0, bytes = 0, size = 0;
	JSValue buffer = JS_GetTypedArrayBuffer(ctx, v, &offset, &bytes, nullptr);
	uint8_t* p = JS_GetArrayBuffer(ctx, &size, buffer);
	JS_FreeValue(ctx, buffer); // the typed array keeps it alive
	n = bytes / sizeof(float);
	return p ? reinterpret_cast<const float*>(p + offset) : nullptr;
}

JSValue newFloats(JSContext* ctx, const float* p, size_t n) {
	JSValue buffer = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(p), n * sizeof(float));
	if (JS_IsException(buffer))
		return buffer;
	JSValue a = JS_NewTypedArray(ctx, 1, &buffer, JS_TYPED_ARRAY_FLOAT32);
	JS_FreeValue(ctx, buffer);
	return a;
}

/** [values...], or [<slot 0>, values...] with `skipFirst`. */
JSValue array(JSContext* ctx, std::initializer_list<double> values, bool skipFirst = false) {
	JSValue a = JS_NewArray(ctx);
	uint32_t i = skipFirst;
	for (double v : values)
		JS_SetPropertyUint32(ctx, a, i++, JS_NewFloat64(ctx, v));
	return a;
}

// --- Natives: argv is padded with undefined up to the declared length ---

#define NATIVE(name)                                                                                                   \
	JSValue name(JSContext* ctx, JSValueConst, int, [[maybe_unused]] JSValueConst* argv, int,                          \
				 [[maybe_unused]] JSValueConst* data)
#define B binding(data)
#define GET(T, var, v)                                                                                                 \
	T* var = get<T>(ctx, B, v);                                                                                        \
	if (!var)                                                                                                          \
	return JS_EXCEPTION

NATIVE(ctxRealtime) {
	Binding* b = B;
	if (!b->host->graph)
		return JS_ThrowTypeError(ctx, "AudioContext: this engine has no audio output");
	if (!b->realtime)
		b->realtime = new Ctx(b->host->graph, false);
	return wrap(ctx, b, new CtxHandle(b->realtime));
}

NATIVE(ctxOffline) {
	// ponytail: a few thousand edges cover reverb IR filtering and decode contexts; raise for big offline graphs.
	auto* c = new Ctx(new Graph(num(ctx, argv[0]), 0, 4096), true);
	JSValue h = wrap(ctx, B, new CtxHandle(c));
	unref(c);
	return h;
}

NATIVE(sampleRate) {
	GET(CtxHandle, h, argv[0]);
	return JS_NewFloat64(ctx, h->c->graph->sampleRate());
}

NATIVE(currentTime) {
	GET(CtxHandle, h, argv[0]);
	return JS_NewFloat64(ctx, h->c->graph->currentTime());
}

NATIVE(nodeCount) {
	GET(CtxHandle, h, argv[0]);
	return JS_NewInt32(ctx, h->c->graph->nodeCount());
}

NATIVE(destination) {
	GET(CtxHandle, h, argv[0]);
	return wrap(ctx, B, new NodeHandle(h->c, h->c->graph->destination(), false));
}

/** create(ctx, kind, id, arg) -> [handle, inputs, outputs, channelCount, mode, interpretation,
(default, min, max, aRate) per param]. Read before the Create is posted: afterwards the render thread owns the node. */
NATIVE(create) {
	GET(CtxHandle, h, argv[0]);
	std::string kind = str(ctx, argv[1]);
	double sr = h->c->graph->sampleRate(), arg = num(ctx, argv[3]);
	Node* n = kind == "gain"           ? (Node*)new GainNode(sr)
			  : kind == "biquad"       ? (Node*)new BiquadFilterNode(sr)
			  : kind == "oscillator"   ? (Node*)new OscillatorNode(sr)
			  : kind == "constant"     ? (Node*)new ConstantSourceNode(sr)
			  : kind == "bufferSource" ? (Node*)new AudioBufferSourceNode(sr)
			  : kind == "panner"       ? (Node*)new StereoPannerNode(sr)
			  : kind == "shaper"       ? (Node*)new WaveShaperNode(sr)
			  : kind == "delay"        ? (Node*)new DelayNode(sr, arg)
			  : kind == "convolver"    ? (Node*)new ConvolverNode(sr)
			  : kind == "merger"       ? (Node*)new ChannelMergerNode(sr, (int)arg)
			  : kind == "splitter"     ? (Node*)new ChannelSplitterNode(sr, (int)arg)
			  : kind == "compressor"   ? (Node*)new DynamicsCompressorNode(sr)
			  : kind == "analyser"     ? (Node*)new AnalyserNode(sr)
									   : nullptr;
	if (!n)
		return JS_ThrowTypeError(ctx, "webaudio: unknown node kind '%s'", kind.c_str());
	n->id = (uint64_t)num(ctx, argv[2]);
	std::vector<double> params;
	for (const AudioParam* p : n->params)
		params.insert(params.end(), {p->defaultValue, p->minValue, p->maxValue, p->aRate ? 1.0 : 0.0});
	JSValue info = array(ctx,
						 {(double)n->inputs.size(), (double)n->outputs.size(), (double)n->channelCount,
						  (double)(int)n->countMode, (double)(int)n->interpretation},
						 true);
	h->c->post(command(Command::Create, n)); // before the handle exists: its finalizer posts the Dispose
	JS_SetPropertyUint32(ctx, info, 0, wrap(ctx, B, new NodeHandle(h->c, n, true)));
	for (size_t i = 0; i < params.size(); i++)
		JS_SetPropertyUint32(ctx, info, (uint32_t)(6 + i), JS_NewFloat64(ctx, params[i]));
	return info;
}

/** The node's param argv[i] (an index), or nullptr with a RangeError pending. */
AudioParam* param(JSContext* ctx, NodeHandle* h, JSValueConst i) {
	double k = num(ctx, i);
	if (!(k >= 0 && k < (double)h->n->params.size())) {
		JS_ThrowRangeError(ctx, "webaudio: no such AudioParam");
		return nullptr;
	}
	return h->n->params[(size_t)k];
}

/** Both ends of a connection live in the same graph, or a TypeError is pending. */
bool sameGraph(JSContext* ctx, NodeHandle* a, NodeHandle* b) {
	if (a->c->graph == b->c->graph)
		return true;
	JS_ThrowTypeError(ctx, "webaudio: nodes of different contexts");
	return false;
}

NATIVE(connect) { // (src, dst, output, input)
	GET(NodeHandle, src, argv[0]);
	GET(NodeHandle, dst, argv[1]);
	if (!sameGraph(ctx, src, dst))
		return JS_EXCEPTION;
	Command c = command(Command::Connect, src->n);
	c.dst = dst->n;
	c.out = (int)num(ctx, argv[2]);
	c.in = (int)num(ctx, argv[3]);
	src->c->post(c);
	return JS_UNDEFINED;
}

NATIVE(connectParam) { // (src, owner, paramIndex, output)
	GET(NodeHandle, src, argv[0]);
	GET(NodeHandle, dst, argv[1]);
	AudioParam* p = param(ctx, dst, argv[2]);
	if (!p || !sameGraph(ctx, src, dst))
		return JS_EXCEPTION;
	Command c = command(Command::Connect, src->n);
	c.param = p;
	c.out = (int)num(ctx, argv[3]);
	c.in = -1;
	src->c->post(c);
	return JS_UNDEFINED;
}

NATIVE(disconnect) { // (src, dst | null, output | -1, input | -1)
	GET(NodeHandle, src, argv[0]);
	Command c = command(Command::Disconnect, src->n);
	if (!isNull(argv[1])) {
		GET(NodeHandle, dst, argv[1]);
		if (!sameGraph(ctx, src, dst))
			return JS_EXCEPTION;
		c.dst = dst->n;
	}
	c.out = (int)num(ctx, argv[2]);
	c.in = (int)num(ctx, argv[3]);
	src->c->post(c);
	return JS_UNDEFINED;
}

NATIVE(disconnectParam) { // (src, owner, paramIndex, output | -1)
	GET(NodeHandle, src, argv[0]);
	GET(NodeHandle, dst, argv[1]);
	AudioParam* p = param(ctx, dst, argv[2]);
	if (!p || !sameGraph(ctx, src, dst))
		return JS_EXCEPTION;
	Command c = command(Command::Disconnect, src->n);
	c.param = p;
	c.out = (int)num(ctx, argv[3]);
	src->c->post(c);
	return JS_UNDEFINED;
}

NATIVE(event) { // (node, paramIndex, kind 0 set | 1 linear | 2 exponential, value, time)
	GET(NodeHandle, h, argv[0]);
	AudioParam* p = param(ctx, h, argv[1]);
	if (!p)
		return JS_EXCEPTION;
	int kind = (int)num(ctx, argv[2]);
	Command c = command(Command::ParamEvent, nullptr);
	c.kind = kind == 1 ? AudioParam::Linear : kind == 2 ? AudioParam::Exp : AudioParam::Set;
	c.param = p;
	c.a = num(ctx, argv[3]);
	c.b = num(ctx, argv[4]);
	h->c->post(c);
	return JS_UNDEFINED;
}

NATIVE(cancel) { // (node, paramIndex, time)
	GET(NodeHandle, h, argv[0]);
	AudioParam* p = param(ctx, h, argv[1]);
	if (!p)
		return JS_EXCEPTION;
	Command c = command(Command::ParamCancel, nullptr);
	c.param = p;
	c.b = num(ctx, argv[2]);
	h->c->post(c);
	return JS_UNDEFINED;
}

void setAttr(NodeHandle* h, int attr, double v, Resource* r = nullptr) {
	Command c = command(Command::SetAttr, h->n);
	c.attr = attr;
	c.c = v;
	c.res = r; // one reference travels with the command
	h->c->post(c);
}

NATIVE(attr) { // (node, name, value): enums arrive as their audio:: values
	GET(NodeHandle, h, argv[0]);
	std::string name = str(ctx, argv[1]);
	int attr = name == "channelCount"            ? AttrChannelCount
			   : name == "channelCountMode"      ? AttrChannelCountMode
			   : name == "channelInterpretation" ? AttrChannelInterpretation
			   : name == "type"                  ? AttrType
			   : name == "loop"                  ? AttrLoop
			   : name == "loopStart"             ? AttrLoopStart
			   : name == "loopEnd"               ? AttrLoopEnd
			   : name == "oversample"            ? AttrOversample
			   : name == "fftSize"               ? AttrFftSize
												 : -1;
	if (attr < 0)
		return JS_ThrowTypeError(ctx, "webaudio: unknown attribute '%s'", name.c_str());
	setAttr(h, attr, num(ctx, argv[2]));
	return JS_UNDEFINED;
}

NATIVE(buffer) { // (node, buffer | null, normalize): a source plays the data, a convolver gets a prepared kernel
	GET(NodeHandle, h, argv[0]);
	Resource* r = nullptr;
	if (!isNull(argv[1])) {
		GET(ResHandle, res, argv[1]);
		auto* data = dynamic_cast<AudioBufferData*>(res->r);
		if (!data)
			return JS_ThrowTypeError(ctx, "webaudio: not an AudioBuffer");
		if (dynamic_cast<ConvolverNode*>(h->n))
			r = new ConvolverKernel(*data, JS_ToBool(ctx, argv[2]));
		else
			r = Resource::retain(data);
	}
	setAttr(h, AttrBuffer, 0, r);
	return JS_UNDEFINED;
}

NATIVE(curve) { // (node, Float32Array | null): copied
	GET(NodeHandle, h, argv[0]);
	CurveData* curve = nullptr;
	if (!isNull(argv[1])) {
		size_t n = 0;
		const float* p = floats(ctx, argv[1], n);
		if (!p)
			return JS_EXCEPTION;
		curve = new CurveData;
		curve->data.assign(p, p + n);
	}
	setAttr(h, AttrCurve, 0, curve);
	return JS_UNDEFINED;
}

NATIVE(wave) { // (oscillator, periodicWave)
	GET(NodeHandle, h, argv[0]);
	GET(ResHandle, res, argv[1]);
	auto* table = dynamic_cast<WaveTable*>(res->r);
	if (!table)
		return JS_ThrowTypeError(ctx, "webaudio: not a PeriodicWave");
	setAttr(h, AttrPeriodicWave, 0, Resource::retain(table));
	return JS_UNDEFINED;
}

NATIVE(start) { // (node, when, offset, duration | -1)
	GET(NodeHandle, h, argv[0]);
	Command c = command(Command::Start, h->n);
	c.a = num(ctx, argv[1]);
	c.b = num(ctx, argv[2]);
	c.c = num(ctx, argv[3]);
	h->c->post(c);
	return JS_UNDEFINED;
}

NATIVE(stop) { // (node, when)
	GET(NodeHandle, h, argv[0]);
	Command c = command(Command::Stop, h->n);
	c.a = num(ctx, argv[1]);
	h->c->post(c);
	return JS_UNDEFINED;
}

NATIVE(poll) { // (ctx) -> ids of the sources that ended
	GET(CtxHandle, h, argv[0]);
	Ctx* c = h->c;
	c->flush();
	c->graph->collect();
	JSValue ended = JS_NewArray(ctx);
	uint32_t i = 0;
	Event e;
	while (c->graph->pollEvent(e)) {
		if (e.type == Event::Ended)
			JS_SetPropertyUint32(ctx, ended, i++, JS_NewFloat64(ctx, (double)e.id));
		else
			B->log(e.code == EdgePoolFull ? "[webaudio] error: too many connections, one was dropped"
										  : "[webaudio] error: too many automation events on one AudioParam",
				   "error");
	}
	return ended;
}

NATIVE(render) { // (offlineCtx, length, channels) -> the rendered buffer
	GET(CtxHandle, h, argv[0]);
	Ctx* c = h->c;
	if (!c->owned)
		return JS_ThrowTypeError(ctx, "webaudio: only an OfflineAudioContext renders on the JS thread");
	int64_t length = (int64_t)num(ctx, argv[1]);
	int channels = (int)num(ctx, argv[2]);
	auto* out = new AudioBufferData(channels, length, c->graph->sampleRate());
	{
		DenormalsOff ftz; // contract (d): this thread renders now
		float L[kQuantum], R[kQuantum];
		for (int64_t done = 0; done < length; done += kQuantum) {
			c->flush();
			int n = (int)std::min<int64_t>(kQuantum, length - done);
			c->graph->render(L, R, n);
			std::copy(L, L + n, out->data[0].begin() + done);
			if (channels > 1)
				std::copy(R, R + n, out->data[1].begin() + done);
		}
	}
	return wrap(ctx, B, new ResHandle(out));
}

NATIVE(bufferNew) { // (Float32Array[], sampleRate) -> an immutable snapshot
	uint32_t channels = 0;
	JSValue len = JS_GetPropertyStr(ctx, argv[0], "length");
	JS_ToUint32(ctx, &channels, len);
	JS_FreeValue(ctx, len);
	auto* b = new AudioBufferData((int)channels, 0, num(ctx, argv[1]));
	for (uint32_t i = 0; i < channels; i++) {
		JSValue ch = JS_GetPropertyUint32(ctx, argv[0], i);
		size_t n = 0;
		const float* p = floats(ctx, ch, n);
		if (p)
			b->data[i].assign(p, p + n);
		JS_FreeValue(ctx, ch);
		if (!p) {
			delete b;
			return JS_EXCEPTION;
		}
	}
	return wrap(ctx, B, new ResHandle(b));
}

NATIVE(bufferInfo) { // (buffer) -> [channels, length, sampleRate]
	GET(ResHandle, res, argv[0]);
	auto* b = dynamic_cast<AudioBufferData*>(res->r);
	if (!b)
		return JS_ThrowTypeError(ctx, "webaudio: not an AudioBuffer");
	return array(ctx, {(double)b->data.size(), (double)b->length(), b->sampleRate});
}

NATIVE(bufferRead) { // (buffer, channel) -> a copy
	GET(ResHandle, res, argv[0]);
	auto* b = dynamic_cast<AudioBufferData*>(res->r);
	double c = num(ctx, argv[1]);
	if (!b || !(c >= 0 && c < (double)b->data.size()))
		return JS_ThrowRangeError(ctx, "webaudio: no such channel");
	const std::vector<float>& d = b->data[(size_t)c];
	return newFloats(ctx, d.data(), d.size());
}

NATIVE(periodicWave) { // (real, imag, normalize): equal lengths, checked in JS
	size_t n = 0, m = 0;
	const float* re = floats(ctx, argv[0], n);
	const float* im = re ? floats(ctx, argv[1], m) : nullptr;
	if (!im)
		return JS_EXCEPTION;
	return wrap(ctx, B, new ResHandle(new WaveTable(re, im, (int)std::min(n, m), JS_ToBool(ctx, argv[2]))));
}

NATIVE(decode) { // (ArrayBuffer, sampleRate) -> buffer
	size_t size = 0;
	uint8_t* p = JS_GetArrayBuffer(ctx, &size, argv[0]);
	if (!p)
		return JS_ThrowTypeError(ctx, "decodeAudioData: argument 1 is not an ArrayBuffer");
	std::string error;
	AudioBufferData* b = decodeAudio(p, size, num(ctx, argv[1]), error);
	if (!b)
		return JS_ThrowPlainError(ctx, "%s", error.c_str());
	return wrap(ctx, B, new ResHandle(b));
}

std::string localPath(const std::string& url) {
	if (url.compare(0, 7, "file://") == 0)
		return url.substr(7);
	return url.find("://") == std::string::npos ? url : "";
}

NATIVE(fetch) { // (url) -> ArrayBuffer
	Binding* b = B;
	std::string url = str(ctx, argv[0]);
	std::string path = b->host->resolve ? b->host->resolve(url) : localPath(url);
	if (path.empty())
		return JS_ThrowPlainError(ctx, "fetch %s: not available offline", url.c_str());
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return JS_ThrowPlainError(ctx, "fetch %s: cannot read %s", url.c_str(), path.c_str());
	std::string bytes;
	char chunk[1 << 16];
	for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;)
		bytes.append(chunk, n);
	std::fclose(f);
	return JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

NATIVE(utf8) { // (ArrayBuffer) -> string
	size_t size = 0;
	uint8_t* p = JS_GetArrayBuffer(ctx, &size, argv[0]);
	if (!p)
		return JS_ThrowTypeError(ctx, "webaudio: not an ArrayBuffer");
	return JS_NewStringLen(ctx, reinterpret_cast<const char*>(p), size);
}

NATIVE(log) { // (message, type)
	B->log(str(ctx, argv[0]), str(ctx, argv[1]));
	return JS_UNDEFINED;
}

#undef GET
#undef B
#undef NATIVE

struct NativeFn {
	const char* name;
	JSCFunctionData* fn;
	int length;
};

const NativeFn kNatives[] = {
	{"ctxRealtime", ctxRealtime, 0},
	{"ctxOffline", ctxOffline, 1},
	{"sampleRate", sampleRate, 1},
	{"time", currentTime, 1},
	{"nodeCount", nodeCount, 1},
	{"destination", destination, 1},
	{"create", create, 4},
	{"connect", connect, 4},
	{"connectParam", connectParam, 4},
	{"disconnect", disconnect, 4},
	{"disconnectParam", disconnectParam, 4},
	{"event", event, 5},
	{"cancel", cancel, 3},
	{"attr", attr, 3},
	{"buffer", buffer, 3},
	{"curve", curve, 2},
	{"wave", wave, 2},
	{"start", start, 4},
	{"stop", stop, 2},
	{"poll", poll, 1},
	{"render", render, 3},
	{"bufferNew", bufferNew, 2},
	{"bufferInfo", bufferInfo, 1},
	{"bufferRead", bufferRead, 2},
	{"periodicWave", periodicWave, 3},
	{"decode", decode, 2},
	{"fetch", fetch, 1},
	{"utf8", utf8, 1},
	{"log", log, 2},
};

} // namespace

bool installWebAudio(JSContext* ctx, WebAudioHost* host) {
	JSRuntime* rt = JS_GetRuntime(ctx);
	JSClassID cls = 0; // per runtime: never shared through a static
	JS_NewClassID(rt, &cls);
	JSClassDef def{};
	def.class_name = "WebAudioHandle";
	def.finalizer = finalize;
	if (JS_NewClass(rt, cls, &def) < 0)
		return false;
	auto* b = new Binding;
	b->host = host;
	b->cls = cls;
	JSValue data = wrap(ctx, b, b); // natives reach the Binding through their function data
	if (JS_IsException(data))
		return false;
	JSValue natives = JS_NewObject(ctx);
	for (const NativeFn& f : kNatives)
		JS_SetPropertyStr(ctx, natives, f.name, JS_NewCFunctionData(ctx, f.fn, f.length, 0, 1, &data));
	JS_FreeValue(ctx, data);
	std::string source(reinterpret_cast<const char*>(src_audio_webaudio_js), src_audio_webaudio_js_len); // 0-ended
	JSValue fn = JS_Eval(ctx, source.c_str(), source.size(), "webaudio.js", JS_EVAL_TYPE_GLOBAL);
	JSValue r = JS_IsException(fn) ? JS_EXCEPTION : JS_Call(ctx, fn, JS_UNDEFINED, 1, &natives);
	bool ok = !JS_IsException(r);
	JS_FreeValue(ctx, r);
	JS_FreeValue(ctx, fn);
	JS_FreeValue(ctx, natives);
	return ok;
}

bool pollWebAudio(JSContext* ctx) {
	JSValue global = JS_GetGlobalObject(ctx);
	JSValue api = JS_GetPropertyStr(ctx, global, "__webaudio");
	JSValue fn = JS_GetPropertyStr(ctx, api, "poll");
	JSValue r = JS_Call(ctx, fn, api, 0, nullptr);
	bool ok = !JS_IsException(r);
	for (JSValue v : {r, fn, api, global})
		JS_FreeValue(ctx, v);
	return ok;
}

} // namespace audio
