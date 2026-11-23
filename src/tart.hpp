#pragma once
// Tart: the Push 2 as a Rack controller. E2.1: transport and clock. The Session (what the Push shows and saves) is
// owned by Machine under one lock; the audio thread (Machine::process) handles the Push's events and runs the clock,
// the UI thread reads a View copy and edits through Machine::edit. Audio-thread paths never allocate, wait on a lock,
// throw or log.
#include <atomic>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "dsp.hpp"
#include "push2.hpp"

namespace tart {

constexpr int kTracks = 8, kScenes = 8, kPpqn = 96, kBar = 4 * kPpqn, kNameBytes = 32;

struct Track {
	char name[kNameBytes] = {};
	uint8_t color = 0;
};

enum Popup : uint8_t { kNoPopup, kTempoPopup, kSwingPopup };

struct Transport {
	float bpm = 120; // the internal tempo; a patched CLOCK overrides it, the TEMPO input scales it
	uint8_t swing = 0, quant = 4 /*1 Bar*/;
	bool metronome = false;
	bool playing = false, external = false; // transient from here on
	int64_t tick = 0;                       // the last tick run, 96 per beat
	float shownBpm = 120;                   // what the clock runs at now: the CV applied, or measured from CLOCK
};

struct Ui {
	uint8_t screen = 0, track = 0, scene = 0;
	bool button[128] = {}; // held buttons by CC
	uint32_t touched = 0;  // bit per push2 encoder id
	uint8_t popup = kNoPopup;
	double popupAt = 0, now = 0; // seconds of audio time
};

struct Session {
	Transport transport;
	Ui ui;
	Track track[kTracks];
};

struct State {
	Session s;
	State();
};
static_assert(std::is_trivially_copyable<State>::value, "");

/** What the UI thread draws from: a copy of the Session. */
struct View {
	Session s;
};

/** Clamps everything that came from a patch, resets what is transient. */
void sanitize(State& st);

struct Inputs {
	float sampleRate = 48000;
	int64_t frame = 0;
	bool play = false, stop = false, reset = false, clockConnected = false, clockEdge = false;
	float tempoCv = 0; // volts
	int ppqnIn = 4, ppqnOut = 4;
	const push2::Event* events = nullptr; // decoded, not yet handled
	int nEvents = 0;
	bool leds = false; // refresh `want` this sample
};

struct Outputs {
	bool consumed = false;                                       // `events` were handled
	bool ledsDone = false;                                       // `want` is current
	bool clk = false, reset = false, run = false, metro = false; // this sample; the caller makes pulses of them
	float metroV = 0;
	bool midiClock = false, midiStart = false;
};

class Machine {
  public:
	Machine() : state_(new State) { push_.setDivision(24); }
	Machine(const Machine&) = delete;
	Machine& operator=(const Machine&) = delete;

	// --- Audio thread, once per sample
	void process(const Inputs& in, Outputs& out, push2::Leds& want);

	// --- UI thread
	void view(View& v);
	void save(State& st); // the whole state, for the patch (heap: it will be large)
	void load(const State& st);
	template <class F> void edit(F f) {
		lock();
		f(*state_);
		unlock();
		dirty_.store(true, std::memory_order_release);
	}

  private:
	enum class Request : uint8_t { None, Play, Stop };

	bool tryLock() { return !busy_.exchange(true, std::memory_order_acquire); }
	void unlock() { busy_.store(false, std::memory_order_release); }
	void lock();
	void handle(Session& s, const push2::Event& e);
	void tap(Transport& t);
	void tick(const Session& s, int64_t k, Outputs& out);

	// ponytail: one lock, as lattice::Machine; the audio side only try-locks and never waits.
	std::atomic<bool> busy_{false};
	std::atomic<bool> dirty_{false}; // the UI thread changed the state: the audio thread re-reads what it mirrors
	std::unique_ptr<State> state_;   // under busy_

	// Audio thread only.
	filo::CycleClock clock_;
	filo::ClockOut clk_, push_; // CLK output division; the Push's 24 pulses per beat
	float sr_ = 48000, bpm_ = 120, cps_ = 0, freePhase_ = 0;
	int64_t nextTick_ = 0, frame_ = 0;
	double lastAt_ = -1;
	Request request_ = Request::None;
	bool sync_ = true; // take the lock this sample
	int accTempo_ = 0, accSwing_ = 0;
	int64_t taps_[5] = {};
	int nTaps_ = 0;
};

/** Paints the Home screen into a push2::Screen frame (UI thread, GL). `sans`/`mono` are NanoVG font ids; `status`
 * (null: none) is the dim line over the middle, `message` (null: none) replaces the whole screen. */
void paint(NVGcontext* vg, const View& v, int sans, int mono, const char* status, const char* message);

} // namespace tart
