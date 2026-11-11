#pragma once
// QuickJS bindings for superdough: the Web Audio classes (src/audio/webaudio.js, embedded) over audio::Graph, plus
// the browser globals Strudel touches (window, document, CustomEvent, navigator, fetch).
// Translation only: nodes, DSP and decoding live in nodes.cpp / decode.cpp. Everything here runs on the JS thread,
// which owns its runtime and is the graph's producer (graph.hpp contracts a-c are kept here).
#include <functional>
#include <string>

#include <quickjs.h>

#include "audio/graph.hpp"
#include "audio/worklets.hpp"

namespace audio {

struct WebAudioHost {
	/** Graph behind `new AudioContext()`; the embedder renders it (render thread, or an offline loop) and must keep
	it alive until the JS context is freed. */
	Graph* graph = nullptr;
	/** AudioWorkletGlobalScope of `graph`: created by the embedder before rendering and released after the graph.
	Its runtime runs where the graph renders, and that thread calls collectGarbage(). Unset: no worklets there. */
	WorkletScope* worklets = nullptr;
	/** False: supersaw and wavetable run their JS processors too (oracle, benchmarks). */
	bool nativeWorklets = true;
	/** fetch() and audioWorklet.addModule(): URL -> local file path, "" if unavailable. Unset: plain paths, file://
	URLs and patchisserie:<file> = res/<file>. */
	std::function<std::string(const std::string& url)> resolve;
	/** strudel.log events (Strudel's logger) and binding errors. */
	std::function<void(const std::string& message, const std::string& type)> log;
};

/** Installs the classes and globals into ctx. `host` must outlive ctx. False with a pending JS exception on failure. */
bool installWebAudio(JSContext* ctx, WebAudioHost* host);

/** Delivers the realtime context's graph events ('ended') and frees retired graph objects. Call every JS tick.
False with a pending JS exception on failure. */
bool pollWebAudio(JSContext* ctx);

} // namespace audio
