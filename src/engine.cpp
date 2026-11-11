#include "engine.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>

#include <ghc/filesystem.hpp>
#include <quickjs.h>

#include "audio/graph.hpp"
#include "audio/webaudio_js.hpp"
#include "audio/worklets.hpp"

namespace fs = ghc::filesystem;

namespace filo {

namespace {

constexpr size_t kJsStack = 8 << 20, kJsMaxStack = 6 << 20, kRenderStack = 2 << 20;
constexpr size_t kMemoryLimit = (size_t)512 << 20;
// ponytail: a query costs QuickJS ~0.7 ms whatever its window (s("bd*4") + note().midi(): 14 % of a core at 5 ms,
// 3.5 % at 20 ms); the lookahead covers the period.
constexpr double kTick = 0.02;       // s between two runs of the query loop
constexpr double kLookahead = 0.1;   // s the JS thread schedules beyond what the render thread has rendered
constexpr double kJobBudget = 0.02;  // s of promise jobs before a tick (evaluates), so ticks keep coming
constexpr double kVoiceBudget = 0.1; // s of jobs after it: superdough builds this tick's voices before anything else
constexpr double kStartDelay = 0.07; // s beyond the ring: PLAY starts there, so the first events are scheduled in time
constexpr double kPoll = 0.001;      // s between two looks at the MIDI notes while a MIDI device is chosen
// midikeys(): a note sounds the ring + a block (where the render and audio threads may be when the JS thread sees it)
// + this after its frame: tick()'s 10 ms margin, 2 ms for the poll and the JS thread's wake-up.
constexpr double kKeyMargin = 0.012;
constexpr int kQuantum = audio::kQuantum;

double now() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool startsWith(const std::string& s, const char* prefix) {
	return s.compare(0, std::strlen(prefix), prefix) == 0;
}

bool readFile(const std::string& path, std::string& bytes) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	char chunk[1 << 16];
	for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;)
		bytes.append(chunk, n);
	std::fclose(f);
	return true;
}

std::string str(JSContext* ctx, JSValueConst v) {
	size_t n = 0;
	const char* s = JS_ToCStringLen(ctx, &n, v);
	std::string out = s ? std::string(s, n) : std::string();
	JS_FreeCString(ctx, s);
	return out;
}

double num(JSContext* ctx, JSValueConst v) {
	double d = 0;
	JS_ToFloat64(ctx, &d, v);
	return d;
}

// Before res/engine.js: console (into the log; Strudel's logger also prints '%c' lines that arrive through
// strudel.log, those are dropped) and fetch over res/, the local samples folder, the disk cache and the download
// worker. Returns the function that settles a download.
const char* kPrelude = R"js((function (native) {
  'use strict';
  const text = (a) => a.map((v) => (v instanceof Error ? v.message : String(v))).join(' ');
  const print = (warning) => (...a) =>
    typeof a[0] === 'string' && a[0].startsWith('%c') ? undefined : native.log(text(a), warning);
  globalThis.console = { log: print(false), info: print(false), debug: print(false), warn: print(true), error: print(true) };
  const waiting = new Map();
  globalThis.fetch = (url) => {
    try {
      const body = native.fetch(String(url)); // an ArrayBuffer, or the id of a download to wait for
      if (typeof body !== 'number') return Promise.resolve(new Response(body));
      return new Promise((resolve, reject) => waiting.set(body, { resolve, reject, url }));
    } catch (e) {
      return Promise.reject(e);
    }
  };
  return (id, body) => {
    const w = waiting.get(id);
    if (!w) return;
    waiting.delete(id);
    body ? w.resolve(new Response(body)) : w.reject(new TypeError(`fetch ${w.url}: download failed`));
  };
}))js";

// After res/engine.js: superdough, the local samples (registered again after strudel.cc's online maps, so local names
// win), the .midi() event, highlight and Hydra shader sinks, the MIDI input, and what the C++ loop calls.
const char* kGlue = R"js((function (native, local) {
  'use strict';
  const host = __patchisserie;
  host.rackEvent = native.rack;
  host.rackHighlight = native.highlight;
  host.hydraProgram = native.hydraProgram;
  host.midiCC = native.midiCC;
  host.midiDevice = native.midiDevice;
  const register = (map) => map && samples(map, 'localsamples:', { prebake: true, tag: 'local' });
  const boot = host.initAudio().then(() => {
    register(local);
    native.booted();
    host.prebake().then(() => register(local));
  });
  boot.catch((e) => native.log(`[engine] audio init failed: ${e?.message ?? e}`, true));
  return {
    evaluate: (code, id) =>
      boot
        .then(() => host.evaluate(code, id))
        .then(
          (r) => native.evaluated(r.ok, r.error ?? '', r.line ?? 0, id),
          (e) => native.evaluated(false, String(e?.message ?? e), 0, id),
        ),
    samples: (map) => boot.then(() => register((local = map))),
    tick: host.tick,
    keys: host.keys,
    revert: host.revert,
    cancel: __webaudio.cancel,
    hydraFrame: host.hydraFrame,
    hydraHush: host.hydraHush,
  };
}))js";

} // namespace

// --- JS thread ---

struct Engine::Js {
	Engine* e;
	JSRuntime* rt = nullptr;
	JSContext* ctx = nullptr;
	audio::WebAudioHost host;
	JSValue settle = JS_UNDEFINED, api = JS_UNDEFINED, evaluate = JS_UNDEFINED, tick = JS_UNDEFINED,
			keys = JS_UNDEFINED, revert = JS_UNDEFINED, samples = JS_UNDEFINED, cancel = JS_UNDEFINED,
			hydraFrame = JS_UNDEFINED, hydraHush = JS_UNDEFINED;
	double deadline = 0; // now() past which JS is interrupted; 0: none
	bool interrupted = false, booted = false, playAfter = false, queueFullSaid = false;
	int evaluating = 0;        // evaluates in flight
	uint32_t evaluatingId = 0; // the last one's code id
	uint32_t tickEpoch = 0;    // the epoch of the events this tick pushes
	std::string samplesDir;
	std::map<int, std::string> downloads; // id -> cache file
	int nextDownload = 0;

	explicit Js(Engine* e) : e(e) {}

	static Js* self(JSContext* ctx) { return static_cast<Js*>(JS_GetContextOpaque(ctx)); }

	static int interrupt(JSContext*, void* opaque) {
		auto* js = static_cast<Js*>(opaque);
		if (js->e->abort_.load(std::memory_order_relaxed))
			return 1;
		js->interrupted = js->deadline > 0 && now() > js->deadline;
		return js->interrupted;
	}

	void say(const std::string& line, bool warning) { e->say(line, warning); }

	static std::string interruptedText(double seconds) {
		char s[96];
		std::snprintf(s, sizeof s, "took longer than %g s and was interrupted", seconds);
		return s;
	}

	/** The pending exception as text; an interrupt as interruptedText(seconds). */
	std::string exception(JSContext* c, double seconds = 0) {
		JSValue ex = JS_GetException(c);
		std::string out = interrupted ? interruptedText(seconds) : str(c, ex);
		JSValue stack = interrupted || !JS_IsError(ex) ? JS_UNDEFINED : JS_GetPropertyStr(c, ex, "stack");
		if (JS_IsString(stack))
			out += "\n" + str(c, stack);
		JS_FreeValue(c, stack);
		JS_FreeValue(c, ex);
		return out;
	}

	/** fn(args) with a deadline; false (logged after `what`) on an exception. */
	bool call(JSValueConst fn, int argc, JSValueConst* argv, double seconds, const char* what) {
		deadline = now() + seconds;
		interrupted = false;
		JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, argc, argv);
		deadline = 0;
		bool ok = !JS_IsException(r);
		if (!ok) {
			std::string error = exception(ctx, seconds);
			if (!e->abort_)
				say(std::string(what) + ": " + error, true);
		}
		JS_FreeValue(ctx, r);
		return ok;
	}

	/** Local file behind a URL: res/ (patchisserie:), the samples folder (localsamples:), the disk cache (http[s],
	downloaded or not yet). "" when there is none. */
	std::string file(std::string& url) {
		if (startsWith(url, "patchisserie:"))
			return fetch::inside(e->cfg_.resDir, unhash(url.substr(13)));
		if (startsWith(url, "localsamples:"))
			return fetch::inside(samplesDir, unhash(url.substr(13)));
		if (!e->cfg_.web)
			return "";
		std::string why, https = fetch::remoteUrl(url, &why); // every remote fetch passes here
		if (https.empty()) {
			say("[fetch] " + why, true);
			return "";
		}
		url = https;
		return fetch::cachePath(e->cfg_.web->root(), url);
	}

	/** superdough's loadBuffer rewrites '#' to %23; decoded before inside() validates the path. */
	static std::string unhash(std::string path) {
		for (size_t i; (i = path.find("%23")) != std::string::npos;)
			path.replace(i, 3, "#");
		return path;
	}

	/** {sound: ['sound/file.wav', ...]} for the subfolders of dir, or null. */
	JSValue localMap(const std::string& dir) {
		std::error_code ec;
		if (dir.empty() || !fs::is_directory(fs::u8path(dir), ec))
			return JS_NULL;
		std::vector<fs::path> sounds;
		for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec))
			if (it->is_directory(ec))
				sounds.push_back(it->path());
		std::sort(sounds.begin(), sounds.end());
		JSValue map = JS_NewObject(ctx);
		int n = 0;
		for (const fs::path& s : sounds) {
			std::vector<std::string> files;
			for (fs::directory_iterator it(s, ec), end; !ec && it != end; it.increment(ec)) {
				std::string ext = it->path().extension().u8string();
				std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return (char)std::tolower(c); });
				if (it->is_regular_file(ec) && (ext == ".wav" || ext == ".mp3"))
					files.push_back(s.filename().u8string() + "/" + it->path().filename().u8string());
			}
			if (files.empty())
				continue;
			std::sort(files.begin(), files.end());
			JSValue a = JS_NewArray(ctx);
			for (uint32_t i = 0; i < files.size(); i++)
				JS_SetPropertyUint32(ctx, a, i, JS_NewString(ctx, files[i].c_str()));
			JS_SetPropertyStr(ctx, map, s.filename().u8string().c_str(), a);
			n++;
		}
		say("[samples] " + std::to_string(n) + " local sounds in " + dir, false);
		return map;
	}

	// --- Natives (argv is padded with undefined up to the declared length) ---

	static JSValue nLog(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
		self(ctx)->say(str(ctx, argv[0]), JS_ToBool(ctx, argv[1]));
		return JS_UNDEFINED;
	}

	static JSValue nFetch(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
		Js* js = self(ctx);
		std::string url = str(ctx, argv[0]), path = js->file(url), bytes;
		if (path.empty())
			return JS_ThrowTypeError(ctx, "fetch %s: not available", url.c_str());
		std::error_code ec;
		if (startsWith(url, "http") && !fs::exists(fs::u8path(path), ec)) {
			int id = ++js->nextDownload;
			js->downloads[id] = path;
			js->e->cfg_.web->request(url, js->e->downloads_, id);
			return JS_NewInt32(ctx, id);
		}
		if (!readFile(path, bytes))
			return JS_ThrowTypeError(ctx, "fetch %s: cannot read %s", url.c_str(), path.c_str());
		return JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
	}

	static JSValue nRack(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
		Js* js = self(ctx);
		Engine* e = js->e;
		Event ev{};
		ev.epoch = js->tickEpoch;
		ev.kind = (uint8_t)num(ctx, argv[0]);
		ev.begin = num(ctx, argv[1]);
		ev.end = num(ctx, argv[2]);
		ev.note = (float)num(ctx, argv[3]);
		ev.vel = (float)std::min(std::max(num(ctx, argv[4]), 0.0), 1.0);
		ev.midichan = (int8_t)std::min(std::max(num(ctx, argv[5]), -1.0), 16.0);
		ev.channel = -1;
		ev.value = (float)num(ctx, argv[7]);
		if (ev.kind == Trig || ev.kind == CC) {
			std::string key = str(ctx, argv[6]);
			{
				std::lock_guard<std::mutex> lock(e->mapsM_);
				ev.channel = (int8_t)(ev.kind == Trig ? e->trigMap_ : e->ccMap_).channel(key);
			}
			if (ev.channel < 0) {
				js->say(std::string(ev.kind == Trig ? "[rack] TRIG" : "[rack] CC") + ": all 16 channels are taken, '" +
							key + "' is ignored (menu: reset the maps)",
						true);
				return JS_UNDEFINED;
			}
		}
		if (!e->seq.queue.push(ev) && !js->queueFullSaid) {
			js->queueFullSaid = true;
			js->say("[rack] too many .midi() events ahead: some were dropped", true);
		}
		return JS_UNDEFINED;
	}

	static JSValue nEvaluated(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
		Js* js = self(ctx);
		js->evaluating = std::max(0, js->evaluating - 1);
		Report r;
		r.id = (uint32_t)num(ctx, argv[3]);
		r.ok = JS_ToBool(ctx, argv[0]);
		if (r.ok) {
			if (js->playAfter)
				js->e->play();
		} else { // Strudel has logged it ([eval] error: ...); the panel gets the line too
			r.line = (int)num(ctx, argv[2]);
			r.error = str(ctx, argv[1]);
			js->e->setMessage(r.line ? "line " + std::to_string(r.line) + ": " + r.error : r.error);
		}
		js->e->setReport(r);
		std::lock_guard<std::mutex> lock(js->e->m_);
		if (!js->e->codePending_) // reported back: a restart re-evaluates without playing (the transport plays on)
			js->e->playAfter_ = false;
		return JS_UNDEFINED;
	}

	static JSValue nHighlight(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
		Engine* e = self(ctx)->e;
		Highlight h{(int64_t)num(ctx, argv[1]), (int64_t)num(ctx, argv[2]), (uint32_t)num(ctx, argv[0]),
					(int)num(ctx, argv[3]), (int)num(ctx, argv[4])};
		e->highlights_.push(h, e->playback_.load(std::memory_order_relaxed));
		return JS_UNDEFINED;
	}

	static JSValue nBooted(JSContext* ctx, JSValueConst, int, JSValueConst*) {
		self(ctx)->booted = true;
		return JS_UNDEFINED;
	}

	/** host.hydraProgram(output, frag, uniform names, which are samplers) -> the program's id. */
	static JSValue nHydraProgram(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
		int32_t output = 0;
		JS_ToInt32(ctx, &output, argv[0]);
		if (output < 0 || output >= hydra::Exchange::kOutputs)
			return JS_ThrowRangeError(ctx, "no output o%d", output);
		hydra::Exchange::Program p;
		p.frag = str(ctx, argv[1]);
		int64_t n = 0;
		JS_GetLength(ctx, argv[2], &n);
		for (int64_t i = 0; i < n; i++) {
			JSValue name = JS_GetPropertyInt64(ctx, argv[2], i), sampler = JS_GetPropertyInt64(ctx, argv[3], i);
			p.uniforms.push_back(str(ctx, name));
			p.samplers.push_back(JS_ToBool(ctx, sampler) > 0);
			JS_FreeValue(ctx, name);
			JS_FreeValue(ctx, sampler);
		}
		return JS_NewUint32(ctx, self(ctx)->e->hydra.setProgram(output, std::move(p)));
	}

	/** host.midiCC(cc, chan): that CC's latest value (0..1) on MIDI channel chan (1-16; 0: any); 0 before one
	arrives, and for numbers out of range. */
	static JSValue nMidiCC(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
		int32_t cc = 0, chan = 0;
		if (JS_ToInt32(ctx, &cc, argv[0]) || JS_ToInt32(ctx, &chan, argv[1]))
			return JS_EXCEPTION;
		bool in = cc >= 0 && cc < 128 && chan >= 0 && chan <= 16;
		return JS_NewFloat64(ctx, in ? self(ctx)->e->midiCC_[chan][cc].load(std::memory_order_relaxed) : 0);
	}

	static JSValue nMidiDevice(JSContext* ctx, JSValueConst, int, JSValueConst*) {
		return JS_NewBool(ctx, self(ctx)->e->midiDevice.load(std::memory_order_relaxed));
	}

	// --- Lifetime ---

	/** Evaluates `source` (a function expression) and calls it with args. */
	JSValue factory(const char* source, const char* name, int argc, JSValueConst* argv) {
		JSValue fn = JS_Eval(ctx, source, std::strlen(source), name, JS_EVAL_TYPE_GLOBAL);
		if (JS_IsException(fn))
			return fn;
		JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, argc, argv);
		JS_FreeValue(ctx, fn);
		return r;
	}

	bool init() {
		rt = JS_NewRuntime();
		JS_SetMemoryLimit(rt, kMemoryLimit);
		JS_SetMaxStackSize(rt, kJsMaxStack);
		JS_SetInterruptHandler(rt, interrupt, this);
		ctx = JS_NewContext(rt);
		JS_SetContextOpaque(ctx, this);
		host.graph = e->graph_.get();
		host.worklets = e->scope_;
		host.resolve = [this](const std::string& module) { // audioWorklet.addModule(): local, or cached already
			std::string url = module, path = file(url);
			std::error_code ec;
			return path.empty() || !fs::exists(fs::u8path(path), ec) ? std::string() : path;
		};
		host.log = [this](const std::string& message, const std::string& type) {
			say(message, type == "error" || type == "warning");
		};
		{
			std::lock_guard<std::mutex> lock(e->m_);
			samplesDir = e->samplesDir_;
		}
		std::string engine;
		if (!readFile(e->cfg_.resDir + "/engine.js", engine)) {
			say("[engine] cannot read " + e->cfg_.resDir + "/engine.js", true);
			return false;
		}
		deadline = now() + e->cfg_.bootDeadline;
		bool ok = audio::installWebAudio(ctx, &host);
		JSValue natives = JS_NewObject(ctx), local = JS_NULL;
		if (ok) {
			struct {
				const char* name;
				JSCFunction* fn;
				int length;
			} fns[] = {{"log", nLog, 2},
					   {"fetch", nFetch, 1},
					   {"rack", nRack, 8},
					   {"evaluated", nEvaluated, 4},
					   {"highlight", nHighlight, 5},
					   {"booted", nBooted, 0},
					   {"hydraProgram", nHydraProgram, 4},
					   {"midiCC", nMidiCC, 2},
					   {"midiDevice", nMidiDevice, 0}};
			for (auto& f : fns)
				JS_SetPropertyStr(ctx, natives, f.name, JS_NewCFunction(ctx, f.fn, f.name, f.length));
			local = localMap(samplesDir);
			settle = factory(kPrelude, "<prelude>", 1, &natives);
			ok = !JS_IsException(settle);
		}
		if (ok) {
			JSValue v = JS_Eval(ctx, engine.c_str(), engine.size(), "engine.js", JS_EVAL_TYPE_GLOBAL);
			ok = !JS_IsException(v);
			JS_FreeValue(ctx, v);
		}
		if (ok) {
			JSValue args[2] = {natives, local};
			api = factory(kGlue, "<glue>", 2, args);
			ok = !JS_IsException(api);
		}
		if (!ok)
			say("[engine] " + exception(ctx, e->cfg_.bootDeadline), true);
		JS_FreeValue(ctx, natives);
		JS_FreeValue(ctx, local);
		deadline = 0;
		if (!ok)
			return false;
		evaluate = JS_GetPropertyStr(ctx, api, "evaluate");
		tick = JS_GetPropertyStr(ctx, api, "tick");
		keys = JS_GetPropertyStr(ctx, api, "keys");
		revert = JS_GetPropertyStr(ctx, api, "revert");
		samples = JS_GetPropertyStr(ctx, api, "samples");
		cancel = JS_GetPropertyStr(ctx, api, "cancel");
		hydraFrame = JS_GetPropertyStr(ctx, api, "hydraFrame");
		hydraHush = JS_GetPropertyStr(ctx, api, "hydraHush");
		return true;
	}

	void free() {
		if (!ctx)
			return;
		for (JSValue v : {settle, api, evaluate, tick, keys, revert, samples, cancel, hydraFrame, hydraHush})
			JS_FreeValue(ctx, v);
		JS_FreeContext(ctx);
		JS_FreeRuntime(rt); // asserts that nothing leaked
		ctx = nullptr;
	}

	void runJobs(double seconds) {
		double until = now() + seconds;
		for (;;) {
			double budget = booted ? e->cfg_.evalDeadline : e->cfg_.bootDeadline;
			deadline = now() + budget;
			interrupted = false;
			JSContext* c = nullptr;
			int r = JS_ExecutePendingJob(rt, &c);
			deadline = 0;
			if (r == 0)
				return;
			if (r < 0) { // only uncatchable errors get here: interrupts, out of memory
				std::string error = exception(c, budget);
				if (e->abort_)
					return;
				if (evaluating > 0) { // its promise never settles now
					evaluating--;
					e->setMessage("evaluate " + error);
					e->setReport({evaluatingId, false, 0, "evaluate " + error});
				}
				say("[eval] error: " + error, true);
			}
			if (now() > until)
				return;
		}
	}

	void step() {
		std::string code, dir;
		uint32_t codeId = 0;
		bool haveCode = false, haveDir = false;
		{
			std::lock_guard<std::mutex> lock(e->m_);
			if (e->codePending_)
				code = e->code_, codeId = e->codeId_, playAfter = e->playAfter_, haveCode = true,
				e->codePending_ = false;
			if (e->samplesPending_)
				dir = e->samplesDir_, haveDir = true, e->samplesPending_ = false;
		}
		if (haveDir) {
			samplesDir = dir;
			JSValue map = localMap(dir);
			call(samples, 1, &map, e->cfg_.evalDeadline, "[samples]");
			JS_FreeValue(ctx, map);
		}
		for (fetch::Inbox::Done d; e->downloads_->poll(d);) {
			std::string bytes, path = downloads[d.id];
			downloads.erase(d.id);
			JSValue args[2] = {JS_NewInt32(ctx, d.id), JS_UNDEFINED};
			if (d.ok && readFile(path, bytes))
				args[1] = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
			call(settle, 2, args, e->cfg_.evalDeadline, "[fetch]");
			JS_FreeValue(ctx, args[1]);
		}
		deadline = now() + e->cfg_.evalDeadline;
		interrupted = false;
		if (!audio::pollWebAudio(ctx)) {
			std::string error = exception(ctx, e->cfg_.evalDeadline);
			if (!e->abort_)
				say("[webaudio] " + error, true);
		}
		deadline = 0;
		if (e->hush_.exchange(false))
			call(hydraHush, 0, nullptr, e->cfg_.tickDeadline, "[hydra]");
		runJobs(kJobBudget);
		Timeline t = e->timeline();
		// RESET: what the old timeline scheduled past the render position never sounds. Before the tick reads that
		// position: cycle 0 plays right after it.
		if (t.reset && t.epoch != tickEpoch)
			call(cancel, 0, nullptr, e->cfg_.tickDeadline, "[webaudio]");
		if (t.epoch != tickEpoch) // play, stop, reset: what was ahead won't play
			e->highlights_.clear();
		tickEpoch = t.epoch;
		double sr = e->cfg_.sampleRate, lookahead = e->fill_.load() / sr + kLookahead;
		JSValue args[10] = {JS_NewFloat64(ctx, (double)t.frame),
							JS_NewFloat64(ctx, t.cycle),
							JS_NewFloat64(ctx, t.cps),
							JS_NewFloat64(ctx, t.epoch),
							JS_NewBool(ctx, t.playing),
							JS_NewBool(ctx, t.external),
							JS_NewFloat64(ctx, sr),
							JS_NewFloat64(ctx, lookahead),
							JS_NewFloat64(ctx, e->graph_->currentTime() * sr),
							JS_NewBool(ctx, t.reset)};
		if (!call(tick, 10, args, e->cfg_.tickDeadline, "[pattern] error") && !e->abort_) {
			say("[pattern] back to the previous pattern", true);
			call(revert, 0, nullptr, e->cfg_.tickDeadline, "[pattern] error");
		}
		runJobs(kVoiceBudget); // superdough builds the voices of this tick
		if (haveCode) { // after them: its jobs would delay them (an evaluate can take long), it goes on next time
			JSValue args[2] = {JS_NewStringLen(ctx, code.data(), code.size()), JS_NewUint32(ctx, codeId)};
			evaluating++;
			evaluatingId = codeId;
			if (!call(evaluate, 2, args, e->cfg_.evalDeadline, "[eval] error")) {
				evaluating--;
				e->setReport({codeId, false, 0, "evaluate failed"});
			}
			JS_FreeValue(ctx, args[0]);
		}
	}

	/** midikeys(): the MIDI input's notes to host.keys() at once, with the timeline as tick() gets it. A new epoch
	waits for its first tick; notes of an older one (before a play, stop or reset) are dropped. False: none. */
	bool notes() {
		if (e->notes_.empty())
			return false;
		Timeline t = e->timeline();
		if (t.epoch != tickEpoch)
			return false;
		JSValue a = JS_NewArray(ctx);
		uint32_t n = 0;
		while (!e->notes_.empty()) {
			MidiNote k = e->notes_.shift();
			if (k.epoch == t.epoch)
				for (double v :
					 {(double)k.frame, (double)k.status, (double)k.channel, (double)k.note, (double)k.velocity})
					JS_SetPropertyUint32(ctx, a, n++, JS_NewFloat64(ctx, v));
		}
		if (n) {
			double sr = e->cfg_.sampleRate;
			JSValue args[8] = {a,
							   JS_NewFloat64(ctx, (double)t.frame),
							   JS_NewFloat64(ctx, t.cycle),
							   JS_NewFloat64(ctx, t.cps),
							   JS_NewBool(ctx, t.playing),
							   JS_NewFloat64(ctx, sr),
							   JS_NewInt32(ctx, e->keyDelay_.load()),
							   JS_NewFloat64(ctx, e->graph_->currentTime() * sr)};
			call(keys, 8, args, e->cfg_.tickDeadline, "[midikeys]");
			runJobs(kVoiceBudget); // superdough builds their voices
		}
		JS_FreeValue(ctx, a);
		return true;
	}

	/** Icing's frame: Hydra's tick() with the request's audio analysis, at the cycle Filo plays now. */
	void frame(const hydra::Exchange::Request& q) {
		Timeline t = e->timeline();
		JSValue specific = JS_NewArray(ctx);
		for (int i = 0; i < hydra::kBarkBands; i++)
			JS_SetPropertyUint32(ctx, specific, i, JS_NewFloat64(ctx, q.specific[i]));
		JSValue args[4] = {JS_NewFloat64(ctx, q.time), JS_NewFloat64(ctx, t.cycle), specific,
						   JS_NewFloat64(ctx, q.total)};
		double seconds = e->cfg_.tickDeadline; // a frame stalls the query loop like a tick
		deadline = now() + seconds;
		interrupted = false;
		JSValue r = JS_Call(ctx, hydraFrame, JS_UNDEFINED, 4, args);
		deadline = 0;
		JS_FreeValue(ctx, specific);
		if (JS_IsException(r)) { // an endless loop in a uniform function: never again, every frame
			std::string error = exception(ctx, seconds);
			if (!e->abort_) {
				say("[hydra] frame: " + error + " (hushed)", true);
				call(hydraHush, 0, nullptr, e->cfg_.tickDeadline, "[hydra]");
			}
			return;
		}
		if (JS_IsArray(r)) { // [display, time, width, height, then per output: program, n, n values]
			int64_t n = 0, k = 0;
			JS_GetLength(ctx, r, &n);
			auto next = [&](double& d) {
				d = 0;
				if (k >= n)
					return;
				JSValue v = JS_GetPropertyInt64(ctx, r, k++);
				JS_ToFloat64(ctx, &d, v);
				JS_FreeValue(ctx, v);
			};
			auto integer = [&]() { // the JS side's ints; NaN or out of range (setResolution(NaN)?): 0
				double d;
				next(d);
				return std::isfinite(d) && std::fabs(d) < 1e9 ? (int64_t)d : 0;
			};
			hydra::Exchange::Frame f;
			double time = 0;
			f.display = (int)integer();
			next(time);
			f.time = (float)time;
			f.width = (int)integer();
			f.height = (int)integer();
			for (int o = 0; o < hydra::Exchange::kOutputs; o++) {
				f.program[o] = (uint32_t)integer();
				for (int64_t m = integer(); m > 0 && k < n; m--) {
					double v;
					next(v);
					f.values[o].push_back((float)v);
				}
			}
			e->hydra.publish(std::move(f));
		}
		JS_FreeValue(ctx, r);
	}

	void run() {
		if (!init())
			free();
		double next = now(), last = next - kTick;
		while (!e->abort_.load()) {
			double t0 = now();
			e->tickLate.store(std::max(e->tickLate.load(), t0 - last - kTick));
			last = t0;
			if (ctx)
				step();
			double t1 = now();
			e->jsBusy.store(e->jsBusy.load() + (t1 - t0));
			next = std::max(next + kTick, t1);
			// Until the next tick: Icing's frames, each as soon as it is requested, and the MIDI notes, polled every
			// kPoll while a device is chosen; one at most runs past the tick.
			using Clock = std::chrono::steady_clock; // now()'s
			for (hydra::Exchange::Request q; !e->abort_.load();) {
				double f0 = now();
				if (f0 >= next)
					break;
				if (ctx && notes()) {
					e->jsBusy.store(e->jsBusy.load() + (now() - f0));
					continue;
				}
				double until = e->midiDevice.load(std::memory_order_relaxed) ? std::min(next, f0 + kPoll) : next;
				auto tp = Clock::time_point(
					std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(until)));
				if (!e->hydra.wait(tp, q))
					continue;
				f0 = now();
				if (ctx)
					frame(q);
				e->jsBusy.store(e->jsBusy.load() + (now() - f0));
			}
		}
		free();
	}
};

void* Engine::jsMain(void* engine) {
	Js(static_cast<Engine*>(engine)).run();
	return nullptr;
}

// --- Render thread ---

void* Engine::renderMain(void* engine) {
	auto* e = static_cast<Engine*>(engine);
	audio::DenormalsOff ftz; // graph.hpp contract (d)
	float L[kQuantum], R[kQuantum];
	Frame out[kQuantum];
	int64_t g = 0, sinceGc = 0; // g: graph frame of the next frame rendered
	const int64_t gcEvery = (int64_t)(e->cfg_.sampleRate / 10);
	while (!e->abort_.load(std::memory_order_relaxed)) {
		double t0 = now();
		int64_t p = e->playback_.load(std::memory_order_relaxed);
		if (p > g) { // the audio thread played these as silence (underflow): rendered, never heard
			int n = (int)std::min<int64_t>(p - g, 4096);
			e->graph_->discard(n);
			g += n;
		} else if ((int)e->ring_.size() + kQuantum <= e->fill_.load(std::memory_order_relaxed)) {
			e->graph_->render(L, R, kQuantum);
			for (int i = 0; i < kQuantum; i++)
				out[i] = {L[i], R[i], g + i};
			e->ring_.pushBuffer(out, kQuantum);
			g += kQuantum;
			sinceGc += kQuantum;
		} else { // full: the worklet scope's cycle collection goes here, then wait
			if (sinceGc >= gcEvery) {
				e->scope_->collectGarbage();
				sinceGc = 0;
			}
			e->renderBusy.store(e->renderBusy.load() + (now() - t0));
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}
		e->renderBusy.store(e->renderBusy.load() + (now() - t0));
	}
	return nullptr;
}

// --- Engine ---

Engine::Engine(EngineConfig config) : cfg_(std::move(config)) {
	samplesDir_ = cfg_.samplesDir;
	seq.setSampleRate((float)cfg_.sampleRate);
	start();
}

Engine::~Engine() {
	halt();
}

void Engine::start() {
	abort_ = false;
	downloads_ =
		std::make_shared<fetch::Inbox>(); // completions of the previous runtime's downloads land in the old one
	graph_.reset(new audio::Graph(cfg_.sampleRate));
	scope_ = new audio::WorkletScope(cfg_.sampleRate);
	ring_.clear();
	hydra.clear(); // Hydra lives in the JS runtime: the code's next evaluate makes it again
	frame_ = 0;
	playback_ = 0;
	notes_.clear(); // the old timeline's
	if (startAt_ >= 0)
		startAt_ = 0;             // a pending PLAY starts at once: the new JS thread needs its boot time anyway
	seq.queue.setEpoch(++epoch_); // the new JS thread queries everything again
	reset_ = false;
	setBlockFrames(blockFrames_);
	auto spawn = [](pthread_t& t, size_t stack, void* (*fn)(void*), void* arg) {
		pthread_attr_t a;
		pthread_attr_init(&a);
		pthread_attr_setstacksize(&a, stack);
		bool ok = pthread_create(&t, &a, fn, arg) == 0;
		pthread_attr_destroy(&a);
		return ok;
	};
	jsRunning_ = spawn(js_, kJsStack, jsMain, this);
	renderRunning_ = spawn(render_, kRenderStack, renderMain, this);
	if (!jsRunning_ || !renderRunning_)
		say("[engine] cannot start its threads", true);
}

void Engine::halt() {
	abort_ = true; // interrupts whatever JS runs
	if (jsRunning_)
		pthread_join(js_, nullptr);
	if (renderRunning_)
		pthread_join(render_, nullptr);
	jsRunning_ = renderRunning_ = false;
	graph_.reset();         // after the JS runtime: its finalizers post to the graph
	audio::release(scope_); // after the graph: worklet nodes hold references
	scope_ = nullptr;
}

void Engine::setSampleRate(double sampleRate) {
	if (sampleRate == cfg_.sampleRate)
		return;
	halt();
	cfg_.sampleRate = sampleRate;
	seq.setSampleRate((float)sampleRate);
	{
		std::lock_guard<std::mutex> lock(m_);
		codePending_ = !code_.empty(); // playAfter_ stays: an evaluate that hasn't reported back is still to play
	}
	start();
}

void Engine::setBlockFrames(int frames) {
	blockFrames_ = frames;
	int fill = std::max((int)std::ceil(cfg_.sampleRate * 0.02), 2 * frames);
	fill = (fill + kQuantum - 1) / kQuantum * kQuantum;
	fill_.store(std::min(fill, kRing - kQuantum));
	keyDelay_.store(fill_.load() + frames + (int)std::ceil(kKeyMargin * cfg_.sampleRate));
}

uint32_t Engine::evaluate(const std::string& code, bool play) {
	std::lock_guard<std::mutex> lock(m_);
	code_ = code;
	codePending_ = true;
	playAfter_ = play;
	return ++codeId_;
}

std::string Engine::code() const {
	std::lock_guard<std::mutex> lock(m_);
	return code_;
}

std::string Engine::message() const {
	std::lock_guard<std::mutex> lock(m_);
	return message_;
}

Engine::Report Engine::report() const {
	std::lock_guard<std::mutex> lock(m_);
	return report_;
}

void Engine::setReport(const Report& r) {
	std::lock_guard<std::mutex> lock(m_);
	report_ = r;
}

void Engine::highlights(std::vector<Highlight>& out) const {
	Timeline t = timeline();
	out.clear();
	if (t.playing)
		highlights_.playing(t.frame, out);
}

void Engine::setMessage(const std::string& m) {
	std::lock_guard<std::mutex> lock(m_);
	message_ = m;
}

void Engine::say(const std::string& line, bool warning) {
	setMessage(line);
	if (cfg_.log)
		cfg_.log(line, warning);
}

void Engine::setSamplesDir(const std::string& dir) {
	std::lock_guard<std::mutex> lock(m_);
	samplesDir_ = dir;
	samplesPending_ = true;
}

void Engine::getMaps(Map& trig, Map& cc) {
	std::lock_guard<std::mutex> lock(mapsM_);
	trig = trigMap_.entries();
	cc = ccMap_.entries();
}

void Engine::setMaps(const Map& trig, const Map& cc) {
	std::lock_guard<std::mutex> lock(mapsM_);
	trigMap_.load(trig);
	ccMap_.load(cc);
}

void Engine::resetMaps() {
	std::lock_guard<std::mutex> lock(mapsM_);
	trigMap_.reset();
	ccMap_.reset();
}

// --- Audio thread ---

bool Engine::process(const Sequencer::Inputs& input, Sequencer::Outputs& out, float& left, float& right) {
	Sequencer::Inputs in = input;
	uint32_t epoch = epoch_;
	if (transport_.load(std::memory_order_relaxed) != None) {
		int t = transport_.exchange(None);
		if (t == Stop && (seq.started() || startAt_ >= 0)) {
			seq.stop();
			startAt_ = -1;
			epoch_++;
		}
		if (t == Play && !seq.started() && startAt_ < 0) {
			// Cycle 0 a little ahead, published right away: the JS thread schedules the first events in time.
			startAt_ = frame_ + fill_.load(std::memory_order_relaxed) + (int64_t)(kStartDelay * cfg_.sampleRate);
			in.resetEdge = true; // stopped: the clock goes to 0 and waits there
			epoch_++;
		}
	}
	bool started = false;
	if (startAt_ >= 0 && frame_ >= startAt_) {
		seq.start();
		startAt_ = -1;
		started = true;
	}
	if (input.resetEdge) {
		epoch_++;
		started = seq.started();
	}
	if (epoch_ != epoch) {
		seq.queue.setEpoch(epoch_);
		reset_ = input.resetEdge && (started || startAt_ >= 0);
	}

	left = right = 0;
	bool heard = false;
	while (!heard && !ring_.empty()) { // older frames were played as silence already: dropped
		Frame f = ring_.shift();
		if (f.frame == frame_)
			left = f.l, right = f.r, heard = true;
	}
	if (!heard)
		missingFrames.store(missingFrames.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);

	seq.process(in, out);
	frame_++;
	playback_.store(frame_, std::memory_order_relaxed);
	bool pending = startAt_ >= 0;
	publish({pending ? startAt_ : frame_, seq.clock.cycle(), seq.clock.cps(), epoch_, pending || seq.started(),
			 in.clockConnected, reset_});
	return started;
}

void Engine::publish(const Timeline& t) {
	uint32_t s = tlSeq_.load(std::memory_order_relaxed);
	tlSeq_.store(s + 1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	tlFrame_.store(t.frame, std::memory_order_relaxed);
	tlCycle_.store(t.cycle, std::memory_order_relaxed);
	tlCps_.store(t.cps, std::memory_order_relaxed);
	tlEpoch_.store(t.epoch, std::memory_order_relaxed);
	tlPlaying_.store(t.playing, std::memory_order_relaxed);
	tlExternal_.store(t.external, std::memory_order_relaxed);
	tlReset_.store(t.reset, std::memory_order_relaxed);
	tlSeq_.store(s + 2, std::memory_order_release);
}

Engine::Timeline Engine::timeline() const {
	for (;;) {
		uint32_t s = tlSeq_.load(std::memory_order_acquire);
		Timeline t;
		t.frame = tlFrame_.load(std::memory_order_relaxed);
		t.cycle = tlCycle_.load(std::memory_order_relaxed);
		t.cps = tlCps_.load(std::memory_order_relaxed);
		t.epoch = tlEpoch_.load(std::memory_order_relaxed);
		t.playing = tlPlaying_.load(std::memory_order_relaxed);
		t.external = tlExternal_.load(std::memory_order_relaxed);
		t.reset = tlReset_.load(std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_acquire);
		if (!(s & 1) && tlSeq_.load(std::memory_order_relaxed) == s)
			return t;
	}
}

} // namespace filo
