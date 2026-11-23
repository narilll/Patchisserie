#pragma once
// Tart: the Push 2 as a Rack controller. E2.2: transport and clock, tracks, Note mode. The Session (what the Push
// shows and saves) is owned by Machine under one lock; the audio thread (Machine::process) handles the Push's events
// and runs the clock, the UI thread reads a View copy and edits through Machine::edit. Audio-thread paths never
// allocate, wait on a lock, throw or log.
#include <atomic>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "dsp.hpp"
#include "push2.hpp"

namespace tart {

constexpr int kTracks = 8, kScenes = 8, kPpqn = 96, kBar = 4 * kPpqn, kMaxLength = 32 * kBar; // 12288 fits uint16_t
// ponytail: per-clip ceilings. 2048 notes = 32 bars of 16ths in 4-note chords; a 32-bar 16-lane drum clip needs 8192
// (E3: raise to 8192, +36 KB/clip, or cap drum clips at 8 bars).
constexpr int kMaxNotes = 2048, kCcs = 8, kMaxPoints = 256, kNameBytes = 32, kColors = 70, kMaxOctave = 10;

// The clips (empty until E2.4). The layout is final: later commits only fill it.
struct Note {
	uint16_t tick, len;
	uint8_t pitch, vel; // pitch = MIDI note (E3: drum lane)
};
struct Point {
	uint16_t tick, value; // E4
};
struct Euclid {
	uint8_t mode, steps, pulses, rotation, probability, velocity, octaves, change;
	uint32_t seed; // E5
};
struct Clip {
	uint16_t length = 0, nNotes = 0; // length 0 = empty slot
	Note notes[kMaxNotes] = {};      // sorted (tick, pitch), unique
	uint16_t nPoints[kCcs] = {};
	Point points[kCcs][kMaxPoints] = {};
	Euclid euclid = {};
};

enum Type : uint8_t { Melodic, Drum /*E3*/, Euclidean /*E5*/ };
enum : int8_t { kNone = -1, kStopQueued = -2 };

struct Track {
	char name[kNameBytes] = {};
	uint8_t color = 0, type = Melodic, voices = 1, grid = 4 /*1/16*/;
	int8_t octave = 3;      // the bottom-left pad is 12 * octave (+ the root): 36 = C1
	int8_t playing = kNone; // launched scene; Stop clears it (as Live)
	int8_t queued = kNone;  // transient from here on
	int64_t launchTick = 0, startTick = 0;
	// E4 adds ccName[8][16], ccRange[8], level, pan, mute, solo
};

enum Popup : uint8_t { kNoPopup, kTempoPopup, kSwingPopup };
enum Pads : uint8_t { kSession, kNote };

struct Transport {
	float bpm = 120; // the internal tempo; a patched CLOCK overrides it, the TEMPO input scales it
	uint8_t swing = 0, quant = 4 /*1 Bar*/;
	bool metronome = false;
	bool playing = false, external = false; // transient from here on
	int64_t tick = 0;                       // the last tick run, 96 per beat
	float shownBpm = 120;                   // what the clock runs at now: the CV applied, or measured from CLOCK
};

/** The pad layout of Note mode (the Scale screen of E2.3 edits it; until then it stays C Major, In Key, 4ths up).
 * `layout` = 2 * (0 4ths, 1 3rds, 2 Sequential) + (0 up, 1 right). */
struct Scale {
	uint8_t root = 0, scale = 0 /*Major*/, layout = 0;
	bool inKey = true, fixed = false;
};

struct Ui {
	// saved
	// ponytail: Note until Session mode exists (E2.5 makes Session the default).
	uint8_t pads = kNote, noteLayout = 0, screen = 0, track = 0, scene = 0, fixedBars = 0;
	bool fixedOn = false;
	// transient
	uint8_t page = 0;      // E2.4
	uint8_t pad[64] = {};  // velocity of each held pad, row * 8 + col
	bool button[128] = {}; // held buttons by CC
	uint32_t touched = 0;  // bit per push2 encoder id
	float bend = 0;        // the touch strip, -1..1
	int8_t heldStep = -1;  // E2.4
	uint8_t popup = kNoPopup;
	double popupAt = 0, now = 0; // seconds of audio time
};

struct Session {
	Transport transport;
	Scale scale;
	Ui ui;
	Track track[kTracks];
};

struct State {
	Session s;
	Clip clip[kTracks][kScenes];
	State();
};
static_assert(std::is_trivially_copyable<State>::value, "");

/** What the UI thread draws from: a copy of the Session, every clip's length and the selected slot's notes. */
struct View {
	Session s;
	uint16_t length[kTracks][kScenes];
	uint16_t nNotes;
	Note notes[kMaxNotes];
};

/** The 8 default track colours (also the Slice's number colours) until E2.3 brings Live's 70. */
extern const uint8_t kTrackRgb[8][3];

/** The MIDI note of a pad in Note mode (row 0 = bottom), or -1 above 127. `root`: the pad is the scale's root,
 * `inScale`: it is in the scale (for Chromatic, which lights them). */
int padPitch(const Scale& sc, int octave, int row, int col, bool& root, bool& inScale);

/** Copies `name` (UTF-8) into the track, cut on a character boundary. */
void setName(Track& t, const char* name);

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

/** What a Slice gets on the right of a Tart: every track's voices (the audio thread fills it, ~2 KB). */
struct SliceTrack {
	float voct[16], gate[16], vel[16], cc[kCcs], level, pan;
	uint8_t channels;
	uint32_t rgb; // 0xRRGGBB
};
struct SliceFrame {
	uint32_t frame; // changes every sample: a frame that doesn't is stale
	uint8_t hop;    // Slices between this one's left neighbour and the Tart
	SliceTrack track[kTracks];
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
	// `slice`: the Slice on the right (null: none).
	void process(const Inputs& in, Outputs& out, push2::Leds& want, SliceFrame* slice = nullptr);

	// --- UI thread
	void view(View& v);
	void save(State& st); // the whole state, for the patch (heap: it is large)
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
	void pad(Session& s, int index, int vel);
	void leds(const Session& s, push2::Leds& out) const;
	void tap(Transport& t);
	void tick(const Session& s, int64_t k, Outputs& out);

	// ponytail: one lock, as lattice::Machine; the audio side only try-locks and never waits.
	std::atomic<bool> busy_{false};
	std::atomic<bool> dirty_{false};   // the UI thread changed the state: the audio thread re-reads what it mirrors
	std::unique_ptr<State> state_;     // under busy_
	std::atomic<bool> silence_{false}; // UI -> audio: load() replaced the state, drop the notes

	// Audio thread only.
	filo::CycleClock clock_;
	filo::ClockOut clk_, push_; // CLK output division; the Push's 24 pulses per beat
	filo::VoiceAllocator voice_[kTracks];
	struct Live {
		uint32_t id = 0; // 0: the pad isn't sounding
		uint8_t track = 0, pitch = 0;
	};
	Live live_[64]; // the note each held pad started, on the track that was selected then
	uint32_t nextId_ = 0;
	// What process() reads without the lock: copies of the state, refreshed under it.
	int voices_[kTracks] = {}, selected_ = 0;
	uint32_t rgb_[kTracks] = {};
	float bend_ = 0; // V
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
