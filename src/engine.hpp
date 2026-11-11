#pragma once
// One Filo's engine (plan items 1 and 5): Strudel + superdough on a JS thread, the Web Audio graph on a render thread,
// and the audio-thread side that Filo::process() calls (ring -> L/R, Sequencer -> CV). Owned by the Module, so a
// headless Rack plays too. Pure logic, no Rack headers: the plugin passes paths, the download worker and the log;
// test/test_engine.cpp runs it headless.
//
// Threads, and what each owns:
//   UI (any)  evaluate(), play(), stop(), message(), the sticky maps (mutex, shared with JS)
//   JS        QuickJS runtime #1 (res/engine.js); producer of the graph's commands and of seq.queue
//   render    renders the graph (and its worklet scope, QuickJS #2) into the ring, ahead of the audio thread
//   audio     process(): ring -> L/R, seq -> CV, transport, the timeline snapshot for the JS thread
// Between threads only atomics, SPSC queues and the two mutexes above; the audio and render threads never lock or
// allocate (graph.hpp, worklets.hpp).
//
// Timeline: graph frame == playback frame. Ring frames carry their graph frame; frames the audio thread played as
// silence (underflow) are rendered and discarded, so the graph never falls behind and nothing plays late.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <utility>
#include <vector>

#include <dsp/ringbuffer.hpp>

#include "dsp.hpp"
#include "fetch.hpp"

namespace audio {
class Graph;
class WorkletScope;
} // namespace audio

namespace filo {

struct EngineConfig {
	double sampleRate = 48000;
	std::string resDir = "res";   // engine.js, worklets.js
	std::string samplesDir;       // local samples: one subfolder per sound, its wav/mp3 files ("" = none)
	fetch::Worker* web = nullptr; // online samples + disk cache; null: offline
	/** JS thread: strudel.log, console and evaluate results (warning = errors too). */
	std::function<void(const std::string& line, bool warning)> log;
	double evalDeadline = 5, tickDeadline = 1, bootDeadline = 60; // s, then the JS is interrupted
};

class Engine {
  public:
	explicit Engine(EngineConfig config);
	~Engine(); // interrupts JS, joins both threads: never hangs (fetch::Worker downloads are not waited for)
	Engine(const Engine&) = delete;
	Engine& operator=(const Engine&) = delete;

	// --- Any thread but the audio thread ---
	/** Evaluates on the JS thread (the latest code wins while one runs); `play`: start playing if it succeeds. */
	void evaluate(const std::string& code, bool play);
	std::string code() const; // last code passed to evaluate()
	/** Last log line or evaluate error, for the panel. */
	std::string message() const;
	/** Re-scans the local samples folder (one subfolder per sound) and registers it. */
	void setSamplesDir(const std::string& dir);
	using Map = std::vector<std::pair<std::string, int>>;
	void getMaps(Map& trig, Map& cc);
	void setMaps(const Map& trig, const Map& cc);
	void resetMaps();

	// --- Any thread, applied by the next process() ---
	void play() { transport_.store(Play); }
	void stop() { transport_.store(Stop); }

	// --- Audio thread ---
	/** Owned by the audio thread: settings (voices, mode, CLK division, PPC) are set on it directly. */
	Sequencer seq;
	/** One sample; true on a start or reset (the RESET output). */
	bool process(const Sequencer::Inputs& in, Sequencer::Outputs& out, float& left, float& right);
	/** Rack's block size: the ring keeps about two blocks ahead (at least 20 ms). */
	void setBlockFrames(int frames);
	/** Restarts both threads at the new rate and re-evaluates the code. Blocks: call where process() can't run. */
	void setSampleRate(double sampleRate);

	// --- Stats (any thread) ---
	std::atomic<int64_t> missingFrames{0};        // played as silence: the ring was empty
	std::atomic<double> renderBusy{0}, jsBusy{0}; // s spent rendering / running JS (not sleeping)

	// Internals shared with engine.cpp's thread functions.
	struct Js;
	struct Timeline {
		int64_t frame = 0;
		double cycle = 0, cps = 0;
		uint32_t epoch = 0; // new on play, stop and reset: the queue drops older events, the JS starts over
		bool playing = false, external = false;
		bool reset = false; // this epoch began with a RESET edge while playing: the JS starts over at cycle 0
	};
	Timeline timeline() const; // JS thread: the audio thread's last snapshot (seqlock)

  private:
	enum Transport { None, Play, Stop };
	struct Frame {
		float l, r;
		int64_t frame;
	};
	static constexpr int kRing = 1 << 14;
	void start();
	void halt();
	void publish(const Timeline& t);
	void say(const std::string& line, bool warning); // JS thread: log + panel message
	void setMessage(const std::string& m);
	static void* jsMain(void* engine);
	static void* renderMain(void* engine);

	EngineConfig cfg_;
	pthread_t js_{}, render_{};
	bool jsRunning_ = false, renderRunning_ = false;
	std::atomic<bool> abort_{false};
	std::unique_ptr<audio::Graph> graph_;
	audio::WorkletScope* scope_ = nullptr;

	// Render -> audio.
	rack::dsp::RingBuffer<Frame, kRing> ring_;
	std::atomic<int> fill_{0};         // frames the render thread keeps in the ring
	std::atomic<int64_t> playback_{0}; // audio -> render: next playback frame

	// Audio thread.
	int64_t frame_ = 0, startAt_ = -1; // startAt_: PLAY pending, cycle 0 is at that frame
	uint32_t epoch_ = 0;
	bool reset_ = false; // Timeline::reset
	int blockFrames_ = 0;

	// Other threads -> audio.
	std::atomic<int> transport_{None};

	// Audio -> JS: the timeline snapshot, seqlock over relaxed atomics.
	std::atomic<uint32_t> tlSeq_{0};
	std::atomic<int64_t> tlFrame_{0};
	std::atomic<double> tlCycle_{0}, tlCps_{0};
	std::atomic<uint32_t> tlEpoch_{0};
	std::atomic<bool> tlPlaying_{false}, tlExternal_{false}, tlReset_{false};

	// UI -> JS.
	mutable std::mutex m_;
	std::string code_, message_, samplesDir_;
	bool codePending_ = false, playAfter_ = false, samplesPending_ = false;

	// UI <-> JS.
	std::mutex mapsM_;
	StickyMap trigMap_{StickyMap::trigDefaults()}, ccMap_{StickyMap::ccDefaults()};

	// Web -> JS.
	std::shared_ptr<fetch::Inbox> downloads_ = std::make_shared<fetch::Inbox>();
};

} // namespace filo
