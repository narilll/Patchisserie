// Offline render harness (make render; plan gates G1-G3 via node --test test/render.test.mjs, G2 here, G4: --bench).
// res/engine.js + superdough in QuickJS on the webaudio_js binding, with this loop rendering the realtime graph
// (and its worklet scope) where the plugin has its render thread. Scheduling mirrors upstream renderPatternAudio
// (strudel.cc's WAV export, used for the Chrome references): per cycle, onset haps queried and awaited through
// superdough() one cycle ahead.
// Writes build/render/<id>.wav; asserts non-silence, no logged errors, exact sample onsets (`samples`), that the node
// count returns to its baseline after every pattern and that a disabled worklet ('stretch') is ignored.
// https URLs come from build/render-cache/<host>/<path>.
// Flags: --bench        CPU per pattern and for all patterns stacked (G4), no WAVs (make render-bench: release build)
//        --loop N       the set N times: after the first pass, nodes and native resources return to the same count
//                       after every pattern, and RSS is printed per pass (G2)
//        --js-worklets  supersaw/wavetable as their JS processors instead of the native ports
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>

#include <sys/resource.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

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
	const std::string https = "https://", res = "patchisserie:";
	if (url.compare(0, res.size(), res) == 0)
		return "res/" + url.substr(res.size());
	if (url.compare(0, https.size(), https) != 0)
		return url.find("://") == std::string::npos ? url : "";
	return "build/render-cache/" + url.substr(https.size(), url.find('?') - https.size());
}

/** Resident set size in MiB (peak where the platform has no cheap current value). */
double rssMiB() {
#ifdef __APPLE__
	mach_task_basic_info info;
	mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n) == KERN_SUCCESS)
		return info.resident_size / 1048576.0;
#endif
	rusage u{};
	getrusage(RUSAGE_SELF, &u);
	return u.ru_maxrss / 1024.0; // Linux: KiB
}

double now() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Harness {
	// Frames per JS tick (~5 ms, like the plugin's query loop). Every render is whole quanta, so currentTime (the next
	// quantum) is the output position and t0 below is the WAV's frame 0.
	static constexpr int kStep = 2 * kQuantum;
	double sr = 48000;
	std::unique_ptr<Graph> graph;
	WorkletScope* worklets = nullptr;
	WebAudioHost host;
	JSRuntime* rt = nullptr;
	JSContext* ctx = nullptr;
	std::vector<std::string> errors; // logged by the current pattern
	int peakNodes = 0;
	double renderSeconds = 0; // time spent in Graph::render (the plugin's render thread)

	~Harness() {
		if (ctx)
			JS_FreeContext(ctx);
		if (rt)
			JS_FreeRuntime(rt); // asserts that the binding leaked no JS object
		graph.reset();
		release(worklets);
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

	bool init(bool nativeWorklets, bool quiet) {
		graph.reset(new Graph(sr));
		worklets = new WorkletScope(sr);
		host.graph = graph.get();
		host.worklets = worklets;
		host.nativeWorklets = nativeWorklets;
		host.resolve = cachePath;
		host.log = [this, quiet](const std::string& message, const std::string& type) {
			if (!quiet)
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
			double t = now();
			{
				DenormalsOff ftz; // contract (d)
				graph->render(L, R, n);
			}
			renderSeconds += now() - t;
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
			worklets->collectGarbage(); // the render thread's GC hook (here: the same thread)
		}
		render(kStep); // apply the last disposals
		graph->collect();
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

struct Result {
	int64_t t0; // the export's frame 0 on the graph's timeline
	double wall, render, peak;
	int peakNodes, nodesAfter;
	std::vector<std::string> problems;
};

/** One pattern, like renderPatternAudio: `cycles` cycles of `code` from the graph's current time. */
Result renderPattern(Harness& h, const std::string& code, double cps, int cycles, int baseline, const char* wavPath) {
	// Start on a whole second: a float32 AudioParam holds that time exactly, as at the references' t = 0. Worklet
	// sources gate on float32 begin/end params (`currentTime <= begin`): a begin rounded below t0 starts a quantum
	// early.
	const int64_t second = (int64_t)h.sr;
	h.render((second - (int64_t)std::llround(h.graph->currentTime() * h.sr) % second) % second);
	h.errors.clear();
	h.peakNodes = 0;
	h.renderSeconds = 0;
	double t = now();
	int64_t frames = (int64_t)std::llround(cycles / cps * h.sr);
	int64_t t0 = (int64_t)std::llround(h.graph->currentTime() * h.sr); // the export's frame 0
	std::string t0s = std::to_string(t0) + " / " + std::to_string(h.sr);
	std::vector<float> wav;
	JSValue str = JS_NewStringLen(h.ctx, code.data(), code.size());
	JSValue global = JS_GetGlobalObject(h.ctx);
	JS_SetPropertyStr(h.ctx, global, "__code", str);
	JS_FreeValue(h.ctx, global);
	bool ok = h.run("__render.begin(__code)", "<begin>");
	for (int c = 0; ok && c < cycles; c++) {
		// Like renderPatternAudioInChunks: cycle c is scheduled when rendering reaches cycle c - 1.
		int64_t until = (int64_t)std::llround(std::max(c - 1, 0) / cps * h.sr);
		ok = h.render(until - (int64_t)wav.size() / 2, &wav) &&
			 h.run("__render.chunk(" + std::to_string(c) + ", " + std::to_string(c + 1) + ", " + std::to_string(cps) +
					   ", " + t0s + ")",
				   "<chunk>");
	}
	ok = ok && h.render(frames - (int64_t)wav.size() / 2, &wav);
	Result r{t0, now() - t, h.renderSeconds, 0, h.peakNodes, 0, {}};
	for (float v : wav)
		r.peak = std::max(r.peak, (double)std::fabs(v));
	bool written = !wavPath || writeWav(wavPath, wav, h.sr);
	r.nodesAfter = h.settle(baseline);
	r.problems = h.errors;
	if (!ok || !written)
		r.problems.push_back(!ok ? "render failed" : "cannot write the wav");
	if (!(r.peak > 1e-3))
		r.problems.push_back("silent");
	if (r.nodesAfter != baseline)
		r.problems.push_back("nodes did not return to the baseline (G2)");
	return r;
}

} // namespace

int main(int argc, char** argv) {
	bool bench = false, jsWorklets = false;
	int loops = 1;
	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--bench"))
			bench = true;
		else if (!std::strcmp(argv[i], "--js-worklets"))
			jsWorklets = true;
		else if (!std::strcmp(argv[i], "--loop") && i + 1 < argc)
			loops = std::max(1, std::atoi(argv[++i]));
	}
	bool quiet = bench || loops > 1;
	Harness h;
	double t = now();
	if (!h.init(!jsWorklets, quiet))
		return 1;
	h.sr = h.number("__render.set.sampleRate");
	double cps = h.number("__render.set.cps");
	int cycles = (int)h.number("__render.set.cycles");
	int patterns = (int)h.number("__render.set.patterns.length");
	int baseline = h.settle(-1, 20);
	std::printf("engine + preload: %.2f s, baseline %d nodes, %d resources%s\n", now() - t, baseline,
				Resource::live().load(), jsWorklets ? ", JS supersaw/wavetable" : "");
	std::system("mkdir -p build/render");
	const double audio = cycles / cps;

	int failures = 0, warmResources = -1;
	double warmRss = 0;
	std::string stacked;
	for (int loop = 0; loop < loops; loop++) {
		bool table = bench && loop == 0;
		if (table)
			std::printf("%-10s %8s %8s %8s %6s\n", "pattern", "render%", "js%", "total%", "nodes");
		for (int p = 0; p < patterns; p++) {
			std::string at = "__render.set.patterns[" + std::to_string(p) + "]";
			std::string id = h.text(at + ".id"), code = h.text(at + ".code");
			if (loop == 0)
				stacked += (stacked.empty() ? "" : ", ") + code;
			if (!quiet)
				std::printf("%s: %s\n", id.c_str(), code.c_str());
			std::string wav = "build/render/" + id + ".wav";
			Result r = renderPattern(h, code, cps, cycles, baseline, bench ? nullptr : wav.c_str());
			int resources = Resource::live().load();
			if (warmResources >= 0 && resources != warmResources)
				r.problems.push_back("native resources " + std::to_string(resources) + ", after the first pass " +
									 std::to_string(warmResources) + " (G2)");
			if (id == "samples") { // bd sd hh*2 sd at cps 0.5: every start on its exact frame
				std::string starts = h.text("JSON.stringify(__render.starts.map((t) => Math.round(t * " +
											std::to_string(h.sr) + ") - " + std::to_string(r.t0) + "))");
				std::string want = "[";
				for (int c = 0; c < cycles; c++)
					for (int f : {0, 24000, 48000, 60000, 72000})
						want += (want.size() > 1 ? "," : "") + std::to_string(c * 96000 + f);
				want += "]";
				if (starts != want)
					r.problems.push_back("onsets " + starts + ", want " + want);
				else if (!quiet)
					std::printf("  onsets: %s\n", starts.c_str());
			}
			if (table)
				std::printf("%-10s %7.1f%% %7.1f%% %7.1f%% %6d\n", id.c_str(), 100 * r.render / audio,
							100 * (r.wall - r.render) / audio, 100 * r.wall / audio, r.peakNodes);
			else if (!quiet)
				std::printf("  %s: %.2f s wall (%.1fx realtime), peak %.3f, nodes peak %d -> %d (baseline %d), "
							"resources %d\n",
							r.problems.empty() ? "ok" : "FAIL", r.wall, audio / r.wall, r.peak, r.peakNodes,
							r.nodesAfter, baseline, resources);
			for (const std::string& problem : r.problems)
				std::printf("    %s: %s\n", id.c_str(), problem.c_str());
			failures += !r.problems.empty();
		}
		double stackRender = 0;
		if (bench) { // G4: the reference patch, all patterns at once
			Result r = renderPattern(h, "stack(" + stacked + ")", cps, cycles, baseline, nullptr);
			stackRender = r.render / audio;
			if (table)
				std::printf("%-10s %7.1f%% %7.1f%% %7.1f%% %6d  (all patterns at once)\n", "stack", 100 * stackRender,
							100 * (r.wall - r.render) / audio, 100 * r.wall / audio, r.peakNodes);
			if (stackRender > 0.25)
				r.problems.push_back("G4: the render thread needs more than 25 % of one core");
			for (const std::string& problem : r.problems)
				std::printf("    stack: %s\n", problem.c_str());
			failures += !r.problems.empty();
		}
		if (warmResources < 0)
			warmResources = Resource::live().load();
		double rss = rssMiB();
		if (loop == 1)
			warmRss = rss;
		if (loops > 1) {
			std::printf("pass %d: RSS %.1f MiB, %d nodes, %d native resources", loop + 1, rss, h.graph->nodeCount(),
						Resource::live().load());
			if (bench)
				std::printf(", stack render %.1f %%", 100 * stackRender);
			std::printf("\n");
		}
		if (loop > 1 && rss > warmRss + 8) { // ponytail: allocator noise is a few MiB; a leak grows every pass
			std::printf("    RSS grew %.1f MiB since pass 2 (G2)\n", rss - warmRss);
			failures++;
		}
	}

	// A disabled processor is ignored (warning only): the voice plays like the same pattern without it (off t = 0:
	// superdough starts stretch 40 ms early).
	Result r = renderPattern(h, "note(\"~ c3\").s(\"sawtooth\").stretch(2)", cps, 1, baseline, nullptr);
	Result plain = renderPattern(h, "note(\"~ c3\").s(\"sawtooth\")", cps, 1, baseline, nullptr);
	bool same = r.problems.empty() && plain.problems.empty() && std::fabs(r.peak - plain.peak) < 1e-3;
	std::printf("stretch: %s, peak %.4f (plain %.4f), nodes %d (baseline %d)\n",
				same ? "ignored, plays un-stretched" : "FAIL", r.peak, plain.peak, r.nodesAfter, baseline);
	failures += !same || r.nodesAfter != baseline;

	std::printf("%d patterns failed\n", failures);
	return failures ? 1 : 0;
}
