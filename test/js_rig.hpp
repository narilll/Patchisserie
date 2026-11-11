#pragma once
// JS rig for test_webaudio_js.cpp / test_worklets.cpp: one QuickJS runtime with the webaudio_js binding over a real
// graph and its worklet scope, driven single-threaded (JS thread == render thread).
// Each test is a JS snippet (an async function body) returning 'ok' or throwing; helpers: assert(cond, msg),
// render(frames) -> left channel (renders the realtime graph, then pollWebAudio()), right() -> its right channel,
// pump() (pollWebAudio() only), nodeCount(), gc() (both runtimes, single-threaded tests only), wav44k.
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#include <dr_wav.h>

#include "audio/webaudio_js.hpp"
#include "audio_test.hpp"
#include "check.hpp"

using namespace audiotest;

namespace {

// 0.25 s of a 1 kHz sine at 44.1 kHz, 16-bit mono WAV.
std::vector<uint8_t> sineWav() {
	drwav_data_format format{};
	format.container = drwav_container_riff;
	format.format = DR_WAVE_FORMAT_PCM;
	format.channels = 1;
	format.sampleRate = 44100;
	format.bitsPerSample = 16;
	std::vector<int16_t> pcm(11025);
	for (size_t i = 0; i < pcm.size(); i++)
		pcm[i] = (int16_t)std::lround(16000 * std::sin(2 * M_PI * 1000 * i / 44100.0));
	void* data = nullptr;
	size_t size = 0;
	drwav wav;
	drwav_init_memory_write(&wav, &data, &size, &format, nullptr);
	drwav_write_pcm_frames(&wav, pcm.size(), pcm.data());
	drwav_uninit(&wav);
	std::vector<uint8_t> out((uint8_t*)data, (uint8_t*)data + size);
	drwav_free(data, nullptr);
	return out;
}

struct JsRig {
	std::unique_ptr<Graph> graph{new Graph(kSr)}; // destroyed after the runtime: finalizers still post to it
	WorkletScope* scope = new WorkletScope(kSr);  // load test modules before JS creates the first worklet node
	WebAudioHost host;
	std::vector<std::string> logs;
	std::vector<float> lastRight;
	JSRuntime* rt = JS_NewRuntime();
	JSContext* ctx = JS_NewContext(rt);

	explicit JsRig(bool nativeWorklets = true) {
		host.graph = graph.get();
		host.worklets = scope;
		host.nativeWorklets = nativeWorklets;
		host.log = [this](const std::string& m, const std::string& type) { logs.push_back(type + ": " + m); };
		CHECK(installWebAudio(ctx, &host));
		JSValue global = JS_GetGlobalObject(ctx);
		auto fn = [&](const char* name, JSCFunctionData* f) { // the rig's address travels as function data
			JSValue data = JS_NewFloat64(ctx, (double)(uintptr_t)this);
			JS_SetPropertyStr(ctx, global, name, JS_NewCFunctionData(ctx, f, 1, 0, 1, &data));
		};
		fn("render", [](JSContext* ctx, JSValueConst, int, JSValueConst* argv, int, JSValueConst* data) {
			JsRig* r = rig(ctx, data);
			int32_t frames = 0;
			JS_ToInt32(ctx, &frames, argv[0]);
			std::vector<float> L(frames), R(frames);
			r->graph->render(L.data(), R.data(), frames);
			r->lastRight = R;
			if (!pollWebAudio(ctx))
				return JS_EXCEPTION;
			JSValue buffer = JS_NewArrayBufferCopy(ctx, (const uint8_t*)L.data(), L.size() * sizeof(float));
			JSValue out = JS_NewTypedArray(ctx, 1, &buffer, JS_TYPED_ARRAY_FLOAT32);
			JS_FreeValue(ctx, buffer);
			return out;
		});
		// pollWebAudio() only, for tests whose graph renders on another thread.
		fn("pump", [](JSContext* ctx, JSValueConst, int, JSValueConst*, int, JSValueConst*) {
			std::this_thread::sleep_for(std::chrono::microseconds(200));
			return pollWebAudio(ctx) ? JS_UNDEFINED : JS_EXCEPTION;
		});
		fn("nodeCount", [](JSContext* ctx, JSValueConst, int, JSValueConst*, int, JSValueConst* data) {
			return JS_NewInt32(ctx, rig(ctx, data)->graph->nodeCount());
		});
		fn("right", [](JSContext* ctx, JSValueConst, int, JSValueConst*, int, JSValueConst* data) {
			const std::vector<float>& R = rig(ctx, data)->lastRight;
			JSValue buffer = JS_NewArrayBufferCopy(ctx, (const uint8_t*)R.data(), R.size() * sizeof(float));
			JSValue out = JS_NewTypedArray(ctx, 1, &buffer, JS_TYPED_ARRAY_FLOAT32);
			JS_FreeValue(ctx, buffer);
			return out;
		});
		fn("gc", [](JSContext* ctx, JSValueConst, int, JSValueConst*, int, JSValueConst* data) {
			JS_RunGC(JS_GetRuntime(ctx));
			rig(ctx, data)->scope->collectGarbage();
			return JS_UNDEFINED;
		});
		std::vector<uint8_t> wav = sineWav();
		JS_SetPropertyStr(ctx, global, "wav44k", JS_NewArrayBufferCopy(ctx, wav.data(), wav.size()));
		JS_FreeValue(ctx, global);
		JS_FreeValue(ctx, eval("globalThis.assert = (c, m) => { if (!c) throw new Error(m); };"));
	}
	~JsRig() {
		JS_FreeContext(ctx);
		JS_FreeRuntime(rt); // asserts that no JS object leaked
		graph.reset();
		release(scope);
	}
	/** Loads a module into the realtime scope (before any worklet node exists): addModule(url) then resolves. */
	void load(const std::string& source, const std::string& url = "test:module") {
		std::string error;
		CHECK(scope->load(source, url, error));
		if (!error.empty())
			std::fprintf(stderr, "  module: %s\n", error.c_str());
	}
	static JsRig* rig(JSContext* ctx, JSValueConst* data) {
		double p = 0;
		JS_ToFloat64(ctx, &p, data[0]);
		return (JsRig*)(uintptr_t)p;
	}
	std::string string(JSValueConst v) {
		const char* s = JS_ToCString(ctx, v);
		std::string out = s ? s : "(not a string)";
		JS_FreeCString(ctx, s);
		return out;
	}
	JSValue eval(const std::string& code) {
		return JS_Eval(ctx, code.c_str(), code.size(), "<test>", JS_EVAL_TYPE_GLOBAL);
	}
	/** Runs an async function body to completion (all jobs) and returns its result, or "error: <stack>". */
	std::string run(const std::string& body) {
		JSValue p = eval("(async () => {\n" + body + "\n})()");
		std::string out;
		if (JS_IsException(p)) {
			JSValue e = JS_GetException(ctx);
			out = "error: " + string(e);
			JS_FreeValue(ctx, e);
			return out;
		}
		JSContext* c;
		while (JS_ExecutePendingJob(rt, &c) > 0) {
		}
		JSValue v = JS_PromiseResult(ctx, p);
		if (JS_PromiseState(ctx, p) == JS_PROMISE_FULFILLED) {
			out = string(v);
		} else {
			JSValue stack = JS_GetPropertyStr(ctx, v, "stack");
			out = "error: " + string(v) + "\n" + string(stack);
			JS_FreeValue(ctx, stack);
		}
		JS_FreeValue(ctx, v);
		JS_FreeValue(ctx, p);
		return out;
	}
};

bool ok(const std::string& result) {
	if (result != "ok")
		std::fprintf(stderr, "  JS: %s\n", result.c_str());
	return result == "ok";
}

} // namespace
