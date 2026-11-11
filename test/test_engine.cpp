// filo::Engine headless: the real JS and render threads, and this thread as Rack's audio thread (blocks of 256 frames
// paced at real time). A local samples folder provides bd (a DC step with superdough's 1 ms linear attack, so the
// start frame is extrapolated exactly from the first two non-zero samples); downloads fail (no network).
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include <dr_wav.h>
#include <ghc/filesystem.hpp>

#include "check.hpp"
#include "engine.hpp"

namespace fs = ghc::filesystem;
using filo::Engine;

namespace {

// Under ThreadSanitizer QuickJS is ~10x slower: the simulated audio thread runs at half speed there (the timeline is
// frames, not wall time), and still the JS thread falls behind its lookahead at times, so some events go missing
// (superdough skips what is past). There every event that sounds must still be exact, while counts and intervals are
// only reported: race detection is the point of that build.
// The same on CI runners (env CI): their shared VMs stall the JS thread past its lookahead at times (seen on lin-x64
// and mac-x64), so some events go missing there too.
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
constexpr double kPace = 0.5;
constexpr bool kTimed = false;
#else
constexpr double kPace = 1;
const bool kTimed = !std::getenv("CI");
#endif
#define TIMED_CHECK(expr) (kTimed ? CHECK(expr) : (expr) ? (void)0 : (void)std::printf("  (timing) %s\n", #expr))

double now() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string tempDir(const char* name) {
	fs::path p = fs::temp_directory_path() / ("patchisserie-" + std::string(name));
	fs::remove_all(p);
	fs::create_directories(p);
	return fs::weakly_canonical(p).u8string();
}

void writeKick(const std::string& path, unsigned rate) {
	drwav_data_format format{};
	format.container = drwav_container_riff;
	format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
	format.channels = 1;
	format.sampleRate = rate;
	format.bitsPerSample = 32;
	std::vector<float> pcm(rate / 100, 0.5f); // 10 ms
	drwav wav;
	drwav_init_file_write(&wav, path.c_str(), &format, nullptr);
	drwav_write_pcm_frames(&wav, pcm.size(), pcm.data());
	drwav_uninit(&wav);
}

struct Rig {
	std::string dir;
	fetch::Downloader download; // the worker's downloader; set before the engine requests anything (default: fails)
	std::unique_ptr<fetch::Worker> web;
	std::unique_ptr<Engine> engine;
	std::mutex logM;
	std::vector<std::string> logs;
	double sr;
	std::vector<float> L;    // per consumed frame
	std::vector<bool> rises; // some GATE channel went high
	bool gate[16] = {};
	filo::Sequencer::Inputs in;
	filo::Sequencer::Outputs out;
	int64_t clockEvery = 0;    // > 0: a CLOCK edge on every frame divisible by it
	double processSeconds = 0; // the audio thread's share
	double paceStart = 0;
	int64_t pacedFrames = 0;

	explicit Rig(const char* name, double sampleRate = 48000, double evalDeadline = 5) : sr(sampleRate) {
		dir = tempDir(name);
		fs::create_directories(dir + "/samples/bd");
		writeKick(dir + "/samples/bd/kick.wav", 48000);
		web.reset(new fetch::Worker(
			dir + "/cache", [this](const std::string& u, const std::string& f) { return download && download(u, f); },
			3));
		filo::EngineConfig c;
		c.sampleRate = sr;
		c.samplesDir = dir + "/samples";
		c.web = web.get();
		c.evalDeadline = evalDeadline;
		c.log = [this](const std::string& line, bool warning) {
			std::lock_guard<std::mutex> lock(logM);
			logs.push_back((warning ? "W " : "  ") + line);
		};
		engine.reset(new Engine(c));
		L.reserve(48000 * 120);
		rises.reserve(48000 * 120);
	}
	~Rig() {
		if (check::failures())
			dump();
	}
	void dump() {
		std::lock_guard<std::mutex> lock(logM);
		for (auto& l : logs)
			std::fprintf(stderr, "  [log] %s\n", l.substr(0, 300).c_str());
		logs.clear();
	}
	bool logged(const std::string& needle) {
		std::lock_guard<std::mutex> lock(logM);
		for (auto& l : logs)
			if (l.find(needle) != std::string::npos)
				return true;
		return false;
	}
	int64_t frame() const { return (int64_t)L.size(); }

	/** `frames` samples of the audio thread; paced: at real time, in Rack-like blocks. */
	void run(int64_t frames, bool paced = true) {
		if (!paced || !paceStart)
			paceStart = now(), pacedFrames = 0;
		for (int64_t done = 0; done < frames;) {
			int n = (int)std::min<int64_t>(256, frames - done);
			double t0 = now();
			for (int i = 0; i < n; i++) {
				float l, r;
				in.clockEdge = clockEvery && frame() % clockEvery == 0;
				engine->process(in, out, l, r);
				L.push_back(l);
				bool rise = false;
				for (int c = 0; c < 16; c++) {
					bool g = out.gate[c] > 0;
					rise |= g && !gate[c];
					gate[c] = g;
				}
				rises.push_back(rise);
			}
			processSeconds += now() - t0;
			done += n;
			if (paced) {
				pacedFrames += n;
				double wait = paceStart + pacedFrames / (sr * kPace) - now();
				if (wait > 0)
					std::this_thread::sleep_for(std::chrono::duration<double>(wait));
			}
		}
		if (!paced)
			paceStart = 0;
	}
	void seconds(double s) { run((int64_t)(s * sr)); }
	/** Runs until pred() or `timeout` s of audio. */
	template <class F> bool until(F pred, double timeout) {
		for (int64_t end = frame() + (int64_t)(timeout * sr); frame() < end;) {
			run(256);
			if (pred())
				return true;
		}
		return false;
	}
	bool gateRiseAfter(int64_t from) {
		for (int64_t i = std::max<int64_t>(from, 0); i < frame(); i++)
			if (rises[i])
				return true;
		return false;
	}

	std::vector<int64_t> gateRises(int64_t from, int64_t to) const {
		std::vector<int64_t> v;
		for (int64_t i = std::max<int64_t>(from, 0); i < std::min(to, frame()); i++)
			if (rises[i])
				v.push_back(i);
		return v;
	}
	/** Start frames of the kicks: first non-zero sample after silence, minus the attack ramp's samples. An underflow
	(silence) inside the attack ramp resumes on the same ramp, which extrapolates to the same start: counted once. */
	std::vector<int64_t> kicks(int64_t from, int64_t to) const {
		std::vector<int64_t> v;
		for (int64_t i = std::max<int64_t>(from, 1); i + 1 < std::min(to, frame()); i++)
			if (L[i - 1] == 0 && L[i] != 0 && L[i + 1] != L[i])
				v.push_back(i - std::llround(L[i] / (L[i + 1] - L[i])));
		v.erase(std::unique(v.begin(), v.end()), v.end());
		return v;
	}
	/** Kicks and gate rises in [from, to) on the same frames (!kTimed: every kick on a gate); returns how many. */
	int aligned(int64_t from, int64_t to) {
		std::vector<int64_t> k = kicks(from, to), g = gateRises(from, to);
		bool same = kTimed ? k == g : std::includes(g.begin(), g.end(), k.begin(), k.end());
		CHECK(same);
		if (!same) {
			std::fprintf(stderr, "  kicks:");
			for (int64_t f : k)
				std::fprintf(stderr, " %lld", (long long)f);
			std::fprintf(stderr, "\n  gates:");
			for (int64_t f : g)
				std::fprintf(stderr, " %lld", (long long)f);
			std::fprintf(stderr, "\n");
		}
		return same ? (int)k.size() : 0;
	}
	/** Frames between consecutive gate rises in [from, to). */
	std::vector<int64_t> intervals(int64_t from, int64_t to) const {
		std::vector<int64_t> g = gateRises(from, to), d;
		for (size_t i = 1; i < g.size(); i++)
			d.push_back(g[i] - g[i - 1]);
		return d;
	}
	/** Gate rises in [from, to) `step` frames apart: mostly exactly, else whole multiples (an event the JS thread
	could not schedule in time under load is dropped, from audio and .midi() alike). */
	bool every(int64_t step, int64_t from, int64_t to) const {
		std::vector<int64_t> d = intervals(from, to);
		bool ok = 2 * std::count(d.begin(), d.end(), step) > (long)d.size();
		for (int64_t i : d)
			ok &= i % step == 0;
		if (!ok) {
			std::fprintf(stderr, "  gate rises from %lld (want every %lld):", (long long)from, (long long)step);
			for (int64_t f : gateRises(from, to))
				std::fprintf(stderr, " %lld", (long long)(f - from));
			std::fprintf(stderr, "\n");
		}
		return ok;
	}
	/** Evaluates and runs until the engine says so (Strudel's "[eval] code updated" or an error). */
	bool evaluate(const std::string& code, bool play = false, double timeout = 60) {
		{
			std::lock_guard<std::mutex> lock(logM);
			logs.push_back("> " + code);
		}
		size_t n = 0;
		{
			std::lock_guard<std::mutex> lock(logM);
			n = logs.size();
		}
		engine->evaluate(code, play);
		return until(
			[&] {
				std::lock_guard<std::mutex> lock(logM);
				for (size_t i = n; i < logs.size(); i++)
					if (logs[i].find("[eval]") != std::string::npos)
						return true;
				return false;
			},
			timeout);
	}
};

const char* kAligned = "$: s(\"bd*4\")\n$: note(\"c*4\").midi()";

} // namespace

TEST(engine_realtime_alignment_underflow_tempo_stop) {
	auto r = std::make_unique<Rig>("engine-rt");
	double t = now();
	CHECK(r->evaluate(kAligned, true));
	CHECK(r->until([&] { return r->gateRiseAfter(0); }, 10));
	std::printf("  boot + evaluate + play: %.2f s\n", now() - t);
	r->engine->jsBusy = 0, r->engine->renderBusy = 0, r->processSeconds = 0;
	int64_t a = r->frame(), missing = r->engine->missingFrames;
	r->seconds(4);
	double audio = (r->frame() - a) / r->sr;
	std::printf("  realtime: render thread %.1f %%, JS thread %.1f %%, audio thread %.2f us/sample\n",
				100 * r->engine->renderBusy / audio, 100 * r->engine->jsBusy / audio,
				1e6 * r->processSeconds / (r->frame() - a));
	// (a) G1 realtime: superdough's kick and the .midi() gate on the same playback frame.
	int n = r->aligned(a, r->frame() - 4800);
	std::printf("  %d kicks on their gate's frame\n", n);
	TIMED_CHECK(n >= 7);
	TIMED_CHECK(r->every(24000, a, r->frame()));
	std::printf("  underflow at real time: %lld frames\n", (long long)(r->engine->missingFrames - missing));

	// (b) Underflow: a worklet stalls the render thread for 3 x 40 ms (each call within its budget), more than the
	// ring holds, while the audio thread goes on. It plays silence, those frames are rendered and dropped; then
	// everything is on its frames again.
	std::string stall = fetch::cachePath(r->dir + "/cache", "https://test.invalid/stall.js");
	fs::create_directories(fs::u8path(stall).parent_path());
	std::ofstream(stall) << "registerProcessor('stall', class extends AudioWorkletProcessor { process() {\n"
							"  for (const t = Date.now(); Date.now() - t < 40;);\n"
							"  return (this.calls = (this.calls ?? 0) + 1) < 3;\n} });";
	CHECK(r->evaluate("await getAudioContext().audioWorklet.addModule('https://test.invalid/stall.js');\n"
					  "new AudioWorkletNode(getAudioContext(), 'stall').connect(getAudioContext().destination);\n" +
					  std::string(kAligned)));
	CHECK(r->until([&] { return r->engine->missingFrames > missing; }, 2));
	int64_t b = r->frame() + (int64_t)(0.5 * r->sr);
	r->seconds(4);
	std::printf("  render thread stalled: %lld frames played as silence\n",
				(long long)(r->engine->missingFrames - missing));
	TIMED_CHECK(r->aligned(b, r->frame() - 4800) >= 6);

	// (e) Tempo: setcps() and .cps() reach the Sequencer (and superdough's onsets follow it).
	CHECK(r->evaluate("setcps(1)\n" + std::string(kAligned)));
	int64_t e = r->frame() + (int64_t)r->sr; // after the lookahead: what was scheduled already plays at 0.5 cps
	r->seconds(3);
	CHECK(r->engine->seq.clock.cps() == 1.0);
	TIMED_CHECK(r->every(12000, e, r->frame()));
	TIMED_CHECK(r->aligned(e, r->frame() - 4800) >= 6);
	CHECK(r->evaluate("$: s(\"bd*4\")\n$: note(\"c*4\").cps(\"<0.5 1>\").midi()"));
	e = r->frame() + (int64_t)(0.5 * r->sr);
	r->seconds(8);
	std::vector<int64_t> d = r->intervals(e, r->frame());
	TIMED_CHECK(std::count(d.begin(), d.end(), 12000) >= 3 && std::count(d.begin(), d.end(), 24000) >= 3);
	TIMED_CHECK(r->aligned(e, r->frame() - 4800) >= 10);

	// (d) STOP: gates down at once, nothing new starts (sounds already handed to superdough may still sound, up to
	// the lookahead).
	r->engine->stop();
	int64_t s = r->frame();
	r->seconds(1.5);
	CHECK(r->gateRises(s, r->frame()).empty());
	bool down = true;
	for (int c = 0; c < 16; c++)
		down &= r->out.gate[c] == 0;
	CHECK(down);
	CHECK(r->kicks(s + (int64_t)(0.25 * r->sr), r->frame()).empty());
	CHECK(r->engine->missingFrames == r->engine->missingFrames.load()); // (stats readable from any thread)
	// Evaluate + play from STOP (Cmd/Ctrl+Enter) at a new tempo: from cycle 0, aligned from the first onset (the
	// tempo event at cycle 0 waits with the clock for the start).
	CHECK(r->evaluate("setcps(0.75)\n" + std::string(kAligned), true));
	int64_t p = r->frame();
	CHECK(r->until([&] { return r->gateRiseAfter(p); }, 2));
	r->seconds(2);
	TIMED_CHECK(r->aligned(p, r->frame() - 4800) >= 5);
	TIMED_CHECK(r->every(16000, p, r->frame()));
}

TEST(engine_interrupts_never_hang) {
	auto r = std::make_unique<Rig>("engine-loops", 48000, 2);
	CHECK(r->evaluate("$: note(\"c*4\").midi()", true));
	CHECK(r->until([&] { return r->gateRiseAfter(0); }, 10));

	// (c) An endless loop in the code: interrupted, the previous pattern plays on.
	CHECK(r->evaluate("while (true) {}", false, 10));
	CHECK(r->logged("interrupted"));
	CHECK(r->engine->message().find("interrupted") != std::string::npos);
	int64_t a = r->frame();
	CHECK(r->until([&] { return r->gateRiseAfter(a); }, 2));

	// In a pattern function: the query loop is interrupted after 1 s and goes back to the previous pattern.
	CHECK(r->evaluate("$: note(\"c*4\").fmap((v) => { for (;;); }).midi()"));
	CHECK(r->until([&] { return r->logged("back to the previous pattern"); }, 5));
	a = r->frame();
	CHECK(r->until([&] { return r->gateRiseAfter(a); }, 2));

	// In a worklet (render thread): that call is interrupted after its budget, the node stays silent, the timeline
	// stays where it was (frames missed meanwhile are dropped) and the pattern plays on.
	std::string spin = fetch::cachePath(r->dir + "/cache", "https://test.invalid/spin.js");
	fs::create_directories(fs::u8path(spin).parent_path());
	std::ofstream(spin)
		<< "registerProcessor('spin', class extends AudioWorkletProcessor { process() { for (;;); } });";
	CHECK(r->evaluate("await getAudioContext().audioWorklet.addModule('https://test.invalid/spin.js');\n"
					  "new AudioWorkletNode(getAudioContext(), 'spin').connect(getAudioContext().destination);\n"
					  "$: note(\"c*4\").midi()"));
	CHECK(r->until([&] { return r->logged("budget"); }, 5));
	a = r->frame();
	CHECK(r->until([&] { return r->gateRiseAfter(a); }, 2));

	// Destroying the engine while its JS loops forever returns promptly.
	r->engine->evaluate("while (true) {}", false);
	r->seconds(0.5);
	double t = now();
	r->engine.reset();
	double took = now() - t;
	std::printf("  destroyed during an endless loop in %.0f ms\n", 1e3 * took);
	CHECK(took < 1);
}

TEST(engine_sample_rate_restart) {
	auto r = std::make_unique<Rig>("engine-sr", 44100);
	CHECK(r->evaluate(kAligned, true));
	CHECK(r->until([&] { return r->gateRiseAfter(0); }, 10));
	int64_t a = r->frame();
	r->seconds(2);
	TIMED_CHECK(r->every(22050, a, r->frame()));
	// Rack switches to 48 kHz: the engine restarts there (process() isn't running meanwhile) and re-evaluates; the
	// transport plays on. The test kick is a 48 kHz file, so from here its onsets are exact again.
	double t = now();
	r->engine->setSampleRate(48000);
	r->sr = 48000;
	r->paceStart = 0;
	std::printf("  restart: %.0f ms\n", 1e3 * (now() - t));
	int64_t s = r->frame();
	CHECK(r->until([&] { return !r->kicks(s, r->frame()).empty(); }, 20));
	a = r->frame() + 4800;
	r->seconds(3);
	TIMED_CHECK(r->every(24000, a, r->frame()));
	TIMED_CHECK(r->aligned(a, r->frame() - 4800) >= 4);
}

namespace {

int evals(Rig& r) {
	int n = 0;
	std::lock_guard<std::mutex> lock(r.logM);
	for (auto& l : r.logs)
		n += l.find("[eval]") != std::string::npos;
	return n;
}

} // namespace

// A sample-rate restart before the evaluate reported back must not lose its auto-play; one after it must not start
// a stopped transport.
TEST(engine_restart_keeps_pending_autoplay) {
	{
		auto r = std::make_unique<Rig>("engine-sr-pending", 44100);
		r->engine->evaluate("$: note(\"c*4\").midi()", true); // not reported back yet
		r->seconds(0.02);
		r->engine->setSampleRate(48000);
		r->sr = 48000;
		r->paceStart = 0;
		CHECK(r->until([&] { return r->gateRiseAfter(0); }, 30));
	}
	{
		auto r = std::make_unique<Rig>("engine-sr-stopped", 44100);
		CHECK(r->evaluate("$: note(\"c*4\").midi()", true));
		CHECK(r->until([&] { return r->gateRiseAfter(0); }, 10));
		r->engine->stop();
		r->seconds(0.5);
		int n = evals(*r);
		r->engine->setSampleRate(48000);
		r->sr = 48000;
		r->paceStart = 0;
		CHECK(r->until([&] { return evals(*r) > n; }, 30)); // re-evaluated, and stays stopped
		int64_t a = r->frame();
		r->seconds(2);
		CHECK(!r->gateRiseAfter(a));
	}
}

// A completion of the old runtime (restart) must not settle the new runtime's fetch with the same id.
TEST(engine_stale_download_after_restart) {
	std::mutex m;
	std::condition_variable cv;
	std::set<std::string> requested, released;
	auto release = [&](const std::string& u) {
		std::lock_guard<std::mutex> lock(m);
		released.insert(u);
		cv.notify_all();
	};
	auto r = std::make_unique<Rig>("engine-stale-dl", 44100);
	r->download = [&](const std::string& url, const std::string& file) {
		if (url.find("test.invalid/") == std::string::npos)
			return false;
		std::unique_lock<std::mutex> lock(m);
		requested.insert(url);
		cv.notify_all();
		cv.wait(lock, [&] { return released.count(url) > 0; });
		std::ofstream(file, std::ios::binary) << "0123456789";
		return true;
	};
	auto wasRequested = [&](const char* u) {
		std::lock_guard<std::mutex> lock(m);
		return requested.count(u) > 0;
	};
	r->engine->evaluate("const u = 'https://test.invalid/' + getAudioContext().sampleRate + '.bin';\n"
						"fetch(u).then((x) => x.arrayBuffer()).then((b) => console.log('FETCH ok ' + u + ' ' + "
						"b.byteLength), (e) => console.log('FETCH fail ' + u + ' ' + e.message));",
						false);
	CHECK(r->until([&] { return wasRequested("https://test.invalid/44100.bin"); }, 30));
	r->engine->setSampleRate(48000);
	r->sr = 48000;
	r->paceStart = 0;
	CHECK(r->until([&] { return wasRequested("https://test.invalid/48000.bin"); }, 30));
	release("https://test.invalid/44100.bin"); // the old runtime's download lands
	r->seconds(1);
	CHECK(!r->logged("FETCH fail") && !r->logged("[fetch]"));
	release("https://test.invalid/48000.bin");
	CHECK(r->until([&] { return r->logged("FETCH ok https://test.invalid/48000.bin 10"); }, 10));
	CHECK(!r->logged("FETCH fail") && !r->logged("[fetch]"));
	release("https://test.invalid/44100.bin");
}

// superdough fetches 'keys/C#3.wav' as 'keys/C%233.wav'.
TEST(engine_local_sample_with_hash) {
	auto r = std::make_unique<Rig>("engine-hash", 48000);
	fs::create_directories(r->dir + "/samples/keys");
	writeKick(r->dir + "/samples/keys/C#3.wav", 48000);
	r->engine->setSamplesDir(r->dir + "/samples");
	CHECK(r->evaluate("$: s(\"keys\")", true));
	CHECK(r->until([&] { return !r->kicks(0, r->frame()).empty(); }, 10));
}

// RESET on exactly the frame a pending PLAY starts: the epoch changes, so the JS sends cycle 0's events again.
TEST(engine_reset_on_start_frame_bumps_epoch) {
	filo::EngineConfig c;
	c.resDir = "res";
	Engine e(c);
	filo::Sequencer::Inputs in;
	filo::Sequencer::Outputs out;
	float l, r;
	e.play();
	e.process(in, out, l, r);
	filo::Engine::Timeline t = e.timeline();
	CHECK(t.playing && !e.seq.started());
	int64_t startAt = t.frame;
	CHECK(startAt > 1);
	for (int64_t f = 1; f < startAt; f++)
		e.process(in, out, l, r);
	CHECK(!e.seq.started());
	in.resetEdge = true; // this is frame startAt
	e.process(in, out, l, r);
	CHECK(e.seq.started());
	CHECK(e.timeline().epoch == t.epoch + 1);
}

namespace {

// RESET while playing, between two beats: cycle 0 starts over at once, "late but complete". Its kick and its gate land
// on one frame, the first both can still make; the later beats on the new timeline; what the old timeline scheduled
// beyond the render position never sounds. Internal tempo, or an external CLOCK at the same tempo (the edge on a
// pulse, as a sequencer sends them).
void resetWhilePlaying(bool external) {
	std::printf("  %s:\n", external ? "external CLOCK" : "internal tempo");
	auto r = std::make_unique<Rig>(external ? "engine-reset-clock" : "engine-reset");
	if (external)
		r->in.clockConnected = true, r->clockEvery = 6000; // 16 PPC at 0.5 cps
	CHECK(r->evaluate(kAligned, true));
	CHECK(r->until([&] { return r->gateRises(0, r->frame()).size() >= 3; }, 15));
	// Internal: 80 ms before the old timeline's next beat, which is scheduled but not rendered yet. External: the
	// pulse before it (a beat sounds one frame after its pulse).
	int64_t edge = r->gateRises(0, r->frame()).back() + 24000 - (external ? 6001 : 3840);
	while (edge <= r->frame())
		edge += 24000;
	r->run(edge - r->frame());
	r->in.resetEdge = true;
	r->run(1);
	r->in.resetEdge = false;
	int64_t zero = edge + 1; // cycle 0: the clock stands still on the edge's frame
	int64_t missing = r->engine->missingFrames;
	r->seconds(2.5);
	std::printf("  underflow: %lld frames\n", (long long)(r->engine->missingFrames - missing));
	std::vector<int64_t> gates = r->gateRises(zero, r->frame()), kicks = r->kicks(zero, r->frame());
	// (a) cycle 0 sounds and gates (before cycle 0.25), (b) on the same frame. An underflow on a slow CI runner can
	// silence the kick (played as silence), never move or double it: hence the strict <= 1.
	CHECK(!gates.empty() && gates[0] < zero + 24000);
	int64_t first = gates.empty() ? zero : gates[0];
	CHECK(std::count(kicks.begin(), kicks.end(), first) <= 1);
	TIMED_CHECK(std::count(kicks.begin(), kicks.end(), first) == 1);
	// (c) As soon as both can: within the ring (1024 frames), a tick (20 ms), the 10 ms margin and the query.
	std::printf("  RESET -> cycle 0: %lld frames (%.1f ms)\n", (long long)(first - zero), 1e3 * (first - zero) / r->sr);
	TIMED_CHECK(first - zero <= 1024 + 960 + 480 + 480);
	// (d) The later beats exactly on the new timeline: cycle c on zero + c / cps * sr.
	for (int64_t g : gates)
		CHECK(g == first || (g - zero) % 24000 == 0);
	TIMED_CHECK(gates.size() >= 5);
	r->aligned(zero + 12000, r->frame() - 4800); // from cycle 0.125 on
	// (e) Old-timeline kicks: only what was rendered when the JS handled the reset (its render position is first -
	// 480; a source may start within the quantum being rendered then).
	std::vector<int64_t> old;
	for (int64_t k : kicks)
		if (k != first && (k - zero) % 24000 != 0)
			old.push_back(k);
	for (int64_t k : old) {
		std::printf("  old kick at %+lld\n", (long long)(k - zero));
		CHECK(k < first - 480 + 128);
	}
	TIMED_CHECK(old.empty()); // the beat 80 ms after the edge was cancelled
}

} // namespace

TEST(engine_reset_input_late_but_complete) {
	resetWhilePlaying(false);
	resetWhilePlaying(true);
}
