#pragma once
// AudioWorklet host: one AudioWorkletGlobalScope (QuickJS runtime #2) per graph, where superdough's processors
// (res/worklets.js) run unchanged, plus native ports of the costly ones (supersaw, wavetable) as plain graph nodes
// selected by processor name. Pure logic: no Rack headers.
//
// Threads: a scope is created and its modules are loaded on the producer (JS) thread. The first AudioWorkletNode seals
// it; from then on only the thread that renders the graph enters the runtime (every entry updates the stack top, so
// the scope may be created on another thread). The producer only reads the processor definitions, immutable by then.
// Port messages are Blobs (QuickJS JS_WriteObject: JSON-like values plus typed arrays) that travel through the graph's
// command queue (AttrMessage) and event queue (Event::Message); processor errors and console lines are text events.
#include <string>
#include <vector>

#include <quickjs.h>

#include "audio/graph.hpp"

namespace audio {

/** Bytes handed between threads: a serialized message or UTF-8 text. */
struct Blob : Resource {
	std::vector<uint8_t> bytes;
};

/** One wavetable (wavetable.mjs payload.frames), shared by every native wavetable node that plays it. */
struct WavetableData : Resource {
	int frames = 0, frameLen = 0;
	std::vector<float> data; // frames x frameLen
};

/** What registerProcessor() recorded. */
struct ProcessorDef {
	enum Policy { Script, Native, Disabled };
	struct Param {
		std::string name;
		float defaultValue, minValue, maxValue;
		bool aRate;
	};
	std::string name;
	std::vector<Param> params;
	Policy policy = Script;        // worklets.cpp processor table; names not in it run as scripts
	bool listed = false;           // in that table
	const char* feature = nullptr; // Disabled: what a Strudel user calls it
	bool nativeMatches = false;    // Native: the module declares exactly the native node's parameters
	JSValue ctor = JS_UNDEFINED;   // the scope's runtime: render thread only
};

struct WorkletNode;
struct ScriptWorkletNode;

class WorkletScope : public Resource {
  public:
	explicit WorkletScope(double sampleRate);
	~WorkletScope() override;
	WorkletScope(const WorkletScope&) = delete;
	WorkletScope& operator=(const WorkletScope&) = delete;

	// --- Producer thread ---
	/** Evaluates a classic script (registerProcessor() calls) as module `url`, until sealed. False + `error`. */
	bool load(const std::string& source, const std::string& url, std::string& error);
	bool loaded(const std::string& url) const;
	const std::vector<ProcessorDef>& processors() const { return defs_; }
	const ProcessorDef* find(const std::string& name) const;
	/** Before the first node is posted: the runtime belongs to the render thread from here on. */
	void seal() { sealed_.store(true, std::memory_order_release); }
	bool sealed() const { return sealed_.load(std::memory_order_acquire); }

	// --- Render thread (once sealed) ---
	/** Cycle collection between blocks (M2: when the output ring is full). No-op before sealing. */
	void collectGarbage();
	/** Budget of one call into a processor (constructor, onmessage, process): past it the call is interrupted. */
	double deadline = 0.05;
	/** Math.random() of the native ports (xorshift32): one stream per scope, like the JS processors' Math.random. */
	uint32_t randomState;
	double random();

  private:
	friend struct WorkletNode;
	friend struct ScriptWorkletNode;
	static int interrupt(JSContext*, void* opaque);
	static JSValue define(JSContext* ctx, JSValueConst, int, JSValueConst* argv);
	static JSValue post(JSContext* ctx, JSValueConst, int, JSValueConst* argv);
	static JSValue log(JSContext* ctx, JSValueConst, int, JSValueConst* argv);
	/** Each entry from the render thread: stack top, the graph to report to and (f0 >= 0) currentTime/currentFrame. */
	void enter(Graph* graph, int64_t f0);
	JSValue call(JSValueConst fn, JSValueConst self, int argc, JSValueConst* argv);
	void emitText(Event::Type type, uint64_t id, int code, const std::string& text);

	double sampleRate_;
	JSRuntime* rt_;
	JSContext* ctx_;
	JSValue global_, construct_ = JS_UNDEFINED, deliver_ = JS_UNDEFINED;
	JSAtom timeAtom_, frameAtom_;
	int64_t frame_ = -1, deadlineNs_ = 0;
	bool interrupted_ = false;
	Graph* graph_ = nullptr;
	std::vector<ProcessorDef> defs_;
	std::vector<std::string> modules_;
	std::atomic<bool> sealed_{false};
};

/** The node behind `new AudioWorkletNode(ctx, def.name, options)`: a native port when `allowNative` and the
definition matches one, else the module's processor in QuickJS. outputChannels[o] = 0: the input's channel count
(1 input, 1 output, no outputChannelCount). `options` (serialized AudioWorkletNodeOptions, the processor constructor's
argument) is taken over. Producer thread; seals the scope. */
Node* createWorkletNode(double sampleRate, WorkletScope* scope, const ProcessorDef& def, int numInputs,
						const std::vector<int>& outputChannels, Blob* options, bool allowNative);

} // namespace audio
