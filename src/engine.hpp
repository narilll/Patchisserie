#pragma once
// One Filo's engine (plan items 1 and 5): Strudel + superdough on a JS thread, the Web Audio graph on a render thread,
// and the audio-thread side that Filo::process() calls (ring -> L/R, Sequencer -> CV). Owned by the Module, so a
// headless Rack plays too. Pure logic, no Rack headers: the plugin passes paths, the download worker and the log;
// test/test_engine.cpp runs it headless.
//
// Threads, and what each owns:
//   UI (any)  evaluate(), play(), stop(), message(), report(), highlights(), the sticky maps (mutexes, shared with JS)
//             and Icing's Hydra frames (hydra::Exchange)
//   JS        QuickJS runtime #1 (res/engine.js, Hydra too); producer of the graph's commands and of seq.queue
//   render    renders the graph (and its worklet scope, QuickJS #2) into the ring, ahead of the audio thread
//   audio     process(): ring -> L/R, seq -> CV, transport, the timeline snapshot, MIDI CCs and notes for the JS thread
// Between threads only atomics, SPSC queues and the two mutexes above; the audio and render threads never lock or
// allocate (graph.hpp, worklets.hpp).
//
// Timeline: graph frame == playback frame. Ring frames carry their graph frame; frames the audio thread played as
// silence (underflow) are rendered and discarded, so the graph never falls behind and nothing plays late.
#include <algorithm>
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
#include "hydra.hpp"

namespace audio {
class Graph;
class WorkletScope;
} // namespace audio

namespace filo {

/** What plays when, as a span of the code (the editor highlights it): a hap's mini-notation location. */
struct Highlight {
	int64_t from, to; // playback frames [from, to)
	uint32_t id;      // the evaluate() whose code the offsets are in
	int start, end;   // UTF-16 offsets into that code (JS string positions)
};

/** JS thread -> UI: the highlights scheduled ahead and playing. Bounded, behind a mutex (no audio thread here). */
class Highlights {
  public:
	static constexpr size_t kMax = 1024;
	/** Full: drops the ones over by `now` first, then `h` if it is still full. */
	void push(const Highlight& h, int64_t now) {
		std::lock_guard<std::mutex> lock(m_);
		if (v_.size() >= kMax)
			v_.erase(std::remove_if(v_.begin(), v_.end(), [&](const Highlight& x) { return x.to <= now; }), v_.end());
		if (v_.size() < kMax)
			v_.push_back(h);
	}
	void clear() {
		std::lock_guard<std::mutex> lock(m_);
		v_.clear();
	}
	size_t size() const {
		std::lock_guard<std::mutex> lock(m_);
		return v_.size();
	}
	/** The ones playing at `frame`. */
	void playing(int64_t frame, std::vector<Highlight>& out) const {
		std::lock_guard<std::mutex> lock(m_);
		out.clear();
		for (const Highlight& h : v_)
			if (h.from <= frame && frame < h.to)
				out.push_back(h);
	}

  private:
	mutable std::mutex m_;
	std::vector<Highlight> v_;
};

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
	/** Evaluates on the JS thread (the latest code wins while one runs); `play`: start playing if it succeeds.
	Returns the code's id (1, 2, ...): report() and the highlights refer to it. */
	uint32_t evaluate(const std::string& code, bool play);
	std::string code() const; // last code passed to evaluate()
	/** Last log line or evaluate error, for the panel. */
	std::string message() const;
	/** How an evaluate() turned out (an evaluate superseded before the JS thread took it never reports). */
	struct Report {
		uint32_t id = 0; // 0: none yet
		bool ok = true;
		int line = 0; // syntax error: its line (1-based) in that code; 0: none, or a runtime error
		std::string error;
	};
	Report report() const; // the last one
	/** The code spans of what plays now (none while stopped). */
	void highlights(std::vector<Highlight>& out) const;
	/** Re-scans the local samples folder (one subfolder per sound) and registers it. */
	void setSamplesDir(const std::string& dir);
	using Map = std::vector<std::pair<std::string, int>>;
	void getMaps(Map& trig, Map& cc);
	void setMaps(const Map& trig, const Map& cc);
	void resetMaps();

	// --- Any thread, applied by the next process() ---
	void play() { transport_.store(Play); }
	/** Also hushes Hydra (strudel.cc clears it on stop). */
	void stop() {
		transport_.store(Stop);
		hush_.store(true);
	}

	// --- Icing (its UI thread) ---
	/** Hydra's frames and shaders: the JS thread serves each request between its ticks. */
	hydra::Exchange hydra;
	/** A line for the panel's message and the log (Icing's shader errors). */
	void log(const std::string& line, bool warning) { say(line, warning); }

	// --- Audio thread ---
	/** Owned by the audio thread: settings (voices, mode, CLK division, PPC) are set on it directly. */
	Sequencer seq;
	/** One sample; true on a start or reset (the RESET output). */
	bool process(const Sequencer::Inputs& in, Sequencer::Outputs& out, float& left, float& right);
	/** Rack's block size: the ring keeps about two blocks ahead (at least 20 ms). */
	void setBlockFrames(int frames);
	/** Restarts both threads at the new rate and re-evaluates the code. Blocks: call where process() can't run. */
	void setSampleRate(double sampleRate);
	/** A message from Filo's MIDI input (Rack's midi::Message: status nibble, channel 0-15, data bytes). midin()'s
	cc() reads the CCs, value / 127, per channel and the latest of any channel; note-ons and -offs go to midikeys() on
	the JS thread with this frame (full: dropped, counted); the rest is ignored. */
	void midiMessage(int status, int channel, int data1, int data2) {
		if (status == 0x8 || status == 0x9) {
			if (notes_.full())
				droppedNotes.store(droppedNotes.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
			else
				notes_.push({frame_, epoch_, (uint8_t)status, (uint8_t)(channel & 0xf), (uint8_t)(data1 & 0x7f),
							 (uint8_t)(data2 & 0x7f)});
			return;
		}
		if (status != 0xb)
			return;
		double v = (data2 & 0x7f) / 127.;
		midiCC_[0][data1 & 0x7f].store(v, std::memory_order_relaxed);
		midiCC_[1 + (channel & 0xf)][data1 & 0x7f].store(v, std::memory_order_relaxed);
	}
	/** Filo's menu has a MIDI device chosen (midin() warns when not). */
	std::atomic<bool> midiDevice{false};

	// --- Stats (any thread) ---
	std::atomic<int64_t> missingFrames{0};        // played as silence: the ring was empty
	std::atomic<double> renderBusy{0}, jsBusy{0}; // s spent rendering / running JS (not sleeping)
	std::atomic<double> tickLate{0};              // s a query loop run came after 20 ms + this, at worst (reset it)
	std::atomic<int64_t> droppedNotes{0};         // MIDI notes midiMessage() dropped: the JS thread was behind

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
	// Any thread -> JS: stop() was called.
	std::atomic<bool> hush_{false};

	// Audio -> JS: the timeline snapshot, seqlock over relaxed atomics.
	std::atomic<uint32_t> tlSeq_{0};
	std::atomic<int64_t> tlFrame_{0};
	std::atomic<double> tlCycle_{0}, tlCps_{0};
	std::atomic<uint32_t> tlEpoch_{0};
	std::atomic<bool> tlPlaying_{false}, tlExternal_{false}, tlReset_{false};

	// Audio -> JS: the MIDI input's latest CC values, [MIDI channel 1-16, 0: any][CC]. They outlive a restart.
	std::atomic<double> midiCC_[17][128] = {};
	// Audio -> JS: the MIDI input's note-ons and -offs (midikeys()), with their frame and epoch. SPSC; the JS thread
	// polls it every 1 ms (the audio thread never wakes anyone). ponytail: 256, then dropped (the JS thread stalled).
	struct MidiNote {
		int64_t frame;
		uint32_t epoch;
		uint8_t status, channel, note, velocity;
	};
	rack::dsp::RingBuffer<MidiNote, 256> notes_;
	std::atomic<int> keyDelay_{0}; // frames from a note's arrival to its sound (setBlockFrames())

	// UI -> JS.
	mutable std::mutex m_;
	std::string code_, message_, samplesDir_;
	uint32_t codeId_ = 0; // code_'s
	Report report_;
	bool codePending_ = false, playAfter_ = false, samplesPending_ = false;
	void setReport(const Report& r);

	// JS -> UI.
	Highlights highlights_;

	// UI <-> JS.
	std::mutex mapsM_;
	StickyMap trigMap_{StickyMap::trigDefaults()}, ccMap_{StickyMap::ccDefaults()};

	// Web -> JS.
	std::shared_ptr<fetch::Inbox> downloads_ = std::make_shared<fetch::Inbox>();
};

} // namespace filo
