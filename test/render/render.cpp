// Offline render harness (make render; plan gates G1-G3 via node --test test/render.test.mjs, G2 here).
// res/engine.js + superdough in QuickJS on the webaudio_js binding, with this loop rendering the realtime graph where
// the plugin has its render thread. Scheduling mirrors upstream renderPatternAudio (strudel.cc's WAV export, used for
// the Chrome references): per cycle, onset haps queried and awaited through superdough() one cycle ahead.
// Writes build/render/<id>.wav; asserts non-silence, no logged errors, exact sample onsets (`samples`) and that the
// node count returns to its baseline after every pattern. https URLs come from build/render-cache/<host>/<path>.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>

#include <dr_wav.h>

#include "audio/webaudio_js.hpp"

using namespace audio;

namespace {

const char* kHarness = R"js(
globalThis.__render = {
  set: JSON.parse(__patterns),
  starts: [], // AudioBufferSourceNode.start() times, for the onset check
  async preload() {
    await __patchisserie.initAudio();
    // Test preload contract, identical in scripts/render-reference.mjs (Chrome).
    registerSynthSounds();
    registerZZFXSounds();
    registerSoundfonts();
    await samples('github:tidalcycles/dirt-samples');
  },
  // renderPatternAudio(): repl.evaluate(code, false), a fresh superdough controller, initAudio(1024, true).
  async begin(code) {
    const res = await __patchisserie.evaluate(code);
    if (!res.ok) throw new Error(`evaluate: ${res.error}`);
    resetGlobalEffects();
    await initAudio({ maxPolyphony: 1024, multiChannelOrbits: true });
    this.starts = [];
  },
  // scheduleHapsChunk(pattern, cps, begin = 0, c0, c1), on a timeline that starts at t0.
  async chunk(c0, c1, cps, t0) {
    const haps = __patchisserie.pattern
      .queryArc(c0, c1, { _cps: cps })
      .sort((a, b) => a.whole.begin.valueOf() - b.whole.begin.valueOf());
    for (const hap of haps) {
      if (!hap.hasOnset()) continue;
      try {
        hap.ensureObjectValue();
        const t = hap.whole.begin.valueOf() / cps;
        await superdough(hap.value, t0 + t, hap.duration / cps, cps, t);
      } catch (err) {
        errorLogger(err, 'webaudio');
      }
    }
  },
  end() {
    getSuperdoughAudioController(); // exists from here on: part of the baseline
    resetGlobalEffects();
    // reverbGen.mjs:117 pins the last IR's offline filter, whose oncomplete still references the orbit's convolver.
    delete window.filterNode;
  },
};
const start = AudioBufferSourceNode.prototype.start;
AudioBufferSourceNode.prototype.start = function (when = 0, ...rest) {
  __render.starts.push(when);
  return start.call(this, when, ...rest);
};
)js";

std::string readFile(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	std::stringstream s;
	s << f.rdbuf();
	return s.str();
}

std::string cachePath(const std::string& url) {
	const std::string https = "https://";
	if (url.compare(0, https.size(), https) != 0)
		return url.find("://") == std::string::npos ? url : "";
	return "build/render-cache/" + url.substr(https.size(), url.find('?') - https.size());
}

struct Harness {
	// Frames per JS tick (~5 ms, like the plugin's query loop). Every render is whole quanta, so currentTime (the next
	// quantum) is the output position and t0 below is the WAV's frame 0.
	static constexpr int kStep = 2 * kQuantum;
	double sr = 48000;
	std::unique_ptr<Graph> graph;
	WebAudioHost host;
	JSRuntime* rt = nullptr;
	JSContext* ctx = nullptr;
	std::vector<std::string> errors; // logged by the current pattern
	int peakNodes = 0;

	~Harness() {
		if (ctx)
			JS_FreeContext(ctx);
		if (rt)
			JS_FreeRuntime(rt); // asserts that the binding leaked no JS object
	}

	std::string string(JSValueConst v) {
		const char* s = JS_ToCString(ctx, v);
		std::string out = s ? s : "";
		JS_FreeCString(ctx, s);
		return out;
	}
	std::string exception() {
		JSValue e = JS_GetException(ctx);
		JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
		std::string out = string(e) + "\n" + string(stack);
		JS_FreeValue(ctx, stack);
		JS_FreeValue(ctx, e);
		return out;
	}
	void drain() {
		JSContext* c;
		while (JS_ExecutePendingJob(rt, &c) > 0) {
		}
	}
	/** Evaluates `code`; if it yields a promise, runs jobs until it settles. False (and prints) on error. */
	bool run(const std::string& code, const char* what) {
		JSValue v = JS_Eval(ctx, code.c_str(), code.size(), what, JS_EVAL_TYPE_GLOBAL);
		bool ok = !JS_IsException(v);
		if (!ok) {
			std::printf("%s: %s\n", what, exception().c_str());
			return false;
		}
		if (JS_IsPromise(v)) {
			drain();
			if (JS_PromiseState(ctx, v) == JS_PROMISE_REJECTED) {
				JSValue e = JS_PromiseResult(ctx, v);
				JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
				std::printf("%s: %s\n%s\n", what, string(e).c_str(), string(stack).c_str());
				JS_FreeValue(ctx, stack);
				JS_FreeValue(ctx, e);
				ok = false;
			} else if (JS_PromiseState(ctx, v) != JS_PROMISE_FULFILLED) {
				std::printf("%s: still pending after all jobs ran\n", what);
				ok = false;
			}
		}
		JS_FreeValue(ctx, v);
		return ok;
	}
	double number(const std::string& expr) {
		JSValue v = JS_Eval(ctx, expr.c_str(), expr.size(), "<expr>", JS_EVAL_TYPE_GLOBAL);
		double d = NAN;
		JS_ToFloat64(ctx, &d, v);
		JS_FreeValue(ctx, v);
		return d;
	}
	std::string text(const std::string& expr) {
		JSValue v = JS_Eval(ctx, expr.c_str(), expr.size(), "<expr>", JS_EVAL_TYPE_GLOBAL);
		std::string s = JS_IsUndefined(v) ? "" : string(v);
		JS_FreeValue(ctx, v);
		return s;
	}

	bool init() {
		graph.reset(new Graph(sr));
		host.graph = graph.get();
		host.resolve = cachePath;
		host.log = [this](const std::string& message, const std::string& type) {
			std::printf("  [log] %s\n", message.c_str());
			if (type == "error" || message.find("error") != std::string::npos)
				errors.push_back(message);
		};
		rt = JS_NewRuntime();
		ctx = JS_NewContext(rt);
		if (!installWebAudio(ctx, &host)) {
			std::printf("installWebAudio: %s\n", exception().c_str());
			return false;
		}
		JSValue global = JS_GetGlobalObject(ctx);
		// console.* -> stdout; Strudel's logger lines ('%c' + style) also arrive via strudel.log, so they are dropped.
		JS_SetPropertyStr(ctx, global, "__print",
						  JS_NewCFunction(
							  ctx,
							  [](JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
								  const char* s = JS_ToCString(ctx, argv[0]);
								  std::printf("  [console] %s\n", s ? s : "");
								  JS_FreeCString(ctx, s);
								  return JS_UNDEFINED;
							  },
							  "__print", 1));
		std::string patterns = readFile("test/render/patterns.json");
		JS_SetPropertyStr(ctx, global, "__patterns", JS_NewStringLen(ctx, patterns.data(), patterns.size()));
		JS_FreeValue(ctx, global);
		std::string engine = readFile("res/engine.js");
		return run("globalThis.console = Object.fromEntries(['log', 'info', 'warn', 'error', 'debug'].map((k) => [k, "
				   "(...a) => typeof a[0] === 'string' && a[0].startsWith('%c') ? undefined : __print(a.join(' '))]));",
				   "<console>") &&
			   !engine.empty() && run(engine, "res/engine.js") && run(kHarness, "<harness>") &&
			   run("__render.preload()", "<preload>");
	}

	/** Renders `frames` frames (into `out` if given, interleaved stereo), one JS tick per kStep frames. */
	bool render(int64_t frames, std::vector<float>* out = nullptr) {
		float L[kStep], R[kStep];
		for (int64_t done = 0; done < frames;) {
			int n = (int)std::min<int64_t>(kStep, frames - done);
			{
				DenormalsOff ftz; // contract (d)
				graph->render(L, R, n);
			}
			if (out)
				for (int i = 0; i < n; i++)
					out->insert(out->end(), {L[i], R[i]});
			done += n;
			if (!pollWebAudio(ctx)) {
				std::printf("pollWebAudio: %s\n", exception().c_str());
				return false;
			}
			drain();
			peakNodes = std::max(peakNodes, graph->nodeCount());
		}
		return true;
	}

	/** Drops superdough's global effects and renders until the graph is back to `baseline` nodes (or `steps` x 85 ms).
	 */
	int settle(int baseline, int steps = 600) {
		run("__render.end()", "<end>");
		for (int k = 0; k < steps && graph->nodeCount() != baseline; k++) {
			render(4096);
			JS_RunGC(rt);
		}
		render(kStep); // apply the last disposals
		return graph->nodeCount();
	}
};

bool writeWav(const std::string& path, const std::vector<float>& interleaved, double sr) {
	drwav_data_format format{};
	format.container = drwav_container_riff;
	format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
	format.channels = 2;
	format.sampleRate = (drwav_uint32)sr;
	format.bitsPerSample = 32;
	drwav wav;
	if (!drwav_init_file_write(&wav, path.c_str(), &format, nullptr))
		return false;
	drwav_uint64 n = drwav_write_pcm_frames(&wav, interleaved.size() / 2, interleaved.data());
	drwav_uninit(&wav);
	return n == interleaved.size() / 2;
}

} // namespace

int main() {
	auto clock = [] { return std::chrono::steady_clock::now(); };
	auto seconds = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
	Harness h;
	auto t = clock();
	if (!h.init())
		return 1;
	h.sr = h.number("__render.set.sampleRate");
	double cps = h.number("__render.set.cps");
	int cycles = (int)h.number("__render.set.cycles");
	int patterns = (int)h.number("__render.set.patterns.length");
	int baseline = h.settle(-1, 20);
	std::printf("engine + preload: %.2f s, baseline %d nodes\n", seconds(t, clock()), baseline);
	std::system("mkdir -p build/render");

	int failures = 0;
	for (int p = 0; p < patterns; p++) {
		std::string at = "__render.set.patterns[" + std::to_string(p) + "]";
		std::string id = h.text(at + ".id");
		if (h.text(at + ".worklet") == "true") {
			std::printf("skip %s: needs AudioWorklet (M1c-3)\n", id.c_str());
			continue;
		}
		std::printf("%s: %s\n", id.c_str(), h.text(at + ".code").c_str());
		h.errors.clear();
		h.peakNodes = 0;
		t = clock();
		int64_t frames = (int64_t)std::llround(cycles / cps * h.sr);
		int64_t t0 = (int64_t)std::llround(h.graph->currentTime() * h.sr); // the export's frame 0
		std::string t0s = std::to_string(t0) + " / " + std::to_string(h.sr);
		std::vector<float> wav;
		bool ok = h.run("__render.begin(" + at + ".code)", "<begin>");
		for (int c = 0; ok && c < cycles; c++) {
			// Like renderPatternAudioInChunks: cycle c is scheduled when rendering reaches cycle c - 1.
			int64_t until = (int64_t)std::llround(std::max(c - 1, 0) / cps * h.sr);
			ok = h.render(until - (int64_t)wav.size() / 2, &wav) &&
				 h.run("__render.chunk(" + std::to_string(c) + ", " + std::to_string(c + 1) + ", " +
						   std::to_string(cps) + ", " + t0s + ")",
					   "<chunk>");
		}
		ok = ok && h.render(frames - (int64_t)wav.size() / 2, &wav);
		double wall = seconds(t, clock());
		float peak = 0;
		for (float v : wav)
			peak = std::max(peak, std::fabs(v));
		bool written = writeWav("build/render/" + id + ".wav", wav, h.sr);
		int peakNodes = h.peakNodes;
		int after = h.settle(baseline);

		std::vector<std::string> problems = h.errors;
		if (!ok || !written)
			problems.push_back(!ok ? "render failed" : "cannot write the wav");
		if (!(peak > 1e-3f))
			problems.push_back("silent");
		if (after != baseline)
			problems.push_back("nodes did not return to the baseline (G2)");
		if (id == "samples") { // bd sd hh*2 sd at cps 0.5: every start on its exact frame
			std::string starts = h.text("JSON.stringify(__render.starts.map((t) => Math.round(t * " +
										std::to_string(h.sr) + ") - " + std::to_string(t0) + "))");
			std::string want = "[";
			for (int c = 0; c < cycles; c++)
				for (int f : {0, 24000, 48000, 60000, 72000})
					want += (want.size() > 1 ? "," : "") + std::to_string(c * 96000 + f);
			want += "]";
			if (starts != want)
				problems.push_back("onsets " + starts + ", want " + want);
			else
				std::printf("  onsets: %s\n", starts.c_str());
		}
		std::printf("  %s: %.2f s wall (%.1fx realtime, ASan build), peak %.3f, nodes peak %d -> %d (baseline %d)\n",
					problems.empty() ? "ok" : "FAIL", wall, cycles / cps / wall, peak, peakNodes, after, baseline);
		for (const std::string& problem : problems)
			std::printf("    %s\n", problem.c_str());
		failures += !problems.empty();
	}
	std::printf("%d patterns failed\n", failures);
	return failures ? 1 : 0;
}
