#pragma once
// Tart: the Push 2 as a Rack controller. E2.5: transport and clock, tracks, Session and Note modes, colours, Scale
// mode, clips. The Session (what the Push shows and saves) is owned by Machine under one lock; the audio thread
// (Machine::process) handles the Push's events and runs the clock, the UI thread reads a View copy and edits through
// Machine::edit. Audio-thread paths never allocate, wait on a lock, throw or log.
#include <atomic>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "dsp.hpp"
#include "push2.hpp"

namespace tart {

// Ableton's MOMENTARY_DELAY: a button held this long is momentary (Session, Note, Scale, Clip, Quantize) or not a tap
// (Fixed Length).
constexpr double kHoldSeconds = 0.3;
constexpr int kTracks = 8, kScenes = 8, kPpqn = 96, kBar = 4 * kPpqn, kMaxLength = 32 * kBar; // 12288 fits uint16_t
// ponytail: per-clip ceilings. 4096 notes (Clip 36.5 KB, State 2.3 MB) = 32 bars of 16ths in 8-note chords or a 16-bar
// 16-lane drum clip; a full 32-bar one needs 8192 (+36 KB/clip, State 4.2 MB), and a clip past the ceiling says "Clip
// full".
constexpr int kMaxNotes = 4096, kCcs = 8, kMaxPoints = 256, kNameBytes = 32, kColors = 70, kMaxOctave = 10;
// The automation lanes of a clip: the 8 CCs, then LEVEL and PAN. ponytail: 256 points a lane is about 64 isolated step
// locks.
constexpr int kAutos = kCcs + 2, kLevel = kCcs, kPan = kCcs + 1, kCcNameBytes = 16;

// The clips. The layout is final: later commits only fill it.
struct Note {
	uint16_t tick, len;
	uint8_t pitch, vel; // pitch = MIDI note; on a drum track lane n is pitch kDrumNote + n
};
struct Point {
	uint16_t tick, value; // value: 0..65535 across the lane's range
};
constexpr int kLanes = 16, kDrumNote = 36; // Euclid needs kLanes: drum track lane n is the MIDI note kDrumNote + n
constexpr int kEuclidRhythm = kLanes, kMaxSteps = 32, kMaxSeed = 999, kMaxOctaves = 4;
/** One Euclid rhythm: `pulses` spread over `steps` (Bjorklund), turned `rotation` steps later. Pulses above steps
 * count as steps and rotation counts modulo steps, so shortening a pattern loses nothing. */
struct EuclidLane {
	uint8_t steps = 16, pulses = 0, rotation = 0, probability = 100 /*%*/, velocity = 100;
};
enum EuclidMode : uint8_t { kEuclidDrum, kEuclidMelodic };
/** A clip's Euclid settings, played while its track is a Euclid track: Drum mode plays lane[0..15] on the 16 drum
 * lanes, Melodic mode lane[kEuclidRhythm] with pitches from the scale. */
struct Euclid {
	uint16_t seed = 0; // 0..kMaxSeed
	uint8_t mode = kEuclidDrum, octaves = 1 /*1..4*/, change = 0 /*%*/;
	uint8_t grid = 4;  // the step size, an index of kGrid (the scene buttons): 1/16
	uint8_t spare = 0; // keeps the size even, so there is no padding
	EuclidLane lane[kLanes + 1];
};
static_assert(sizeof(EuclidLane) == 5 && sizeof(Euclid) == 92, "no padding: the JSON compares with memcmp");
// ponytail: 256 locks a clip (16 lanes x 16 steps of one CC, or one lane's 32 steps x 8 CCs); "Clip full" past it.
constexpr int kMaxLocks = 256;
/** A pattern-step lock: automation lane `target` (0..kAutos-1) holds `value` on step `step` of the unrotated pattern of
 * Euclid lane `lane` (0..15 drum, kEuclidRhythm melodic). */
struct Lock {
	uint8_t lane, step, target, spare;
	uint16_t value;
};
struct Clip {
	uint16_t length = 0, nNotes = 0; // length 0 = empty slot
	Note notes[kMaxNotes] = {};      // sorted (tick, pitch), unique
	uint16_t nPoints[kAutos] = {};
	Point points[kAutos][kMaxPoints] = {}; // per lane sorted by tick; two on one tick are a jump (arriving, leaving)
	Euclid euclid = {};
	uint16_t nLocks = 0;
	Lock locks[kMaxLocks] = {}; // sorted (lane, step, target), unique
};

enum Type : uint8_t { Melodic, Drum, Euclidean };
/** What Browse lists: the track types by name. Drum tracks have kLanes lanes, lane n is the MIDI note kDrumNote + n and
 * plays on the voice channel n of the track's 16. */
constexpr int kBrowseTypes = 3;
extern const char* const kTypeNames[kBrowseTypes];
enum : int8_t { kNone = -1, kStopQueued = -2 };

struct Track {
	char name[kNameBytes] = {};
	uint8_t color = 0, type = Melodic, voices = 1, grid = 4 /*1/16*/, lane = 0 /*selected drum pad*/;
	int8_t octave = 3;      // the bottom-left pad is 12 * octave (+ the root): 36 = C1
	int8_t playing = kNone; // launched scene; Stop clears it (as Live)
	int8_t queued = kNone;  // transient from here on
	int64_t launchTick = 0, startTick = 0;
	char ccName[kCcs][kCcNameBytes] = {}; // "" shows "CC n"
	uint8_t bipolar = 0;                  // bit c: CC c is -5 V to +5 V (else 0 V to 10 V)
	uint16_t value[kAutos] = {};          // what each lane outputs without automation; a playing clip writes it
	uint16_t overridden = 0;              // transient: bit j: lane j ignores the playing clip until it launches again
	bool mute = false, solo = false;      // saved: MUTE and SOLO (see silent)
};

enum Popup : uint8_t {
	kNoPopup,
	kTempoPopup,
	kSwingPopup,
	kClipFullPopup,
	kUndoPopup,
	kRedoPopup,
	kQuantizedPopup,
	kLoopLengthPopup,
	kLoopTooLongPopup
};
enum Pads : uint8_t { kSession, kNote };
enum Screens : uint8_t { kHome, kScaleScreen, kClipScreen, kBrowseScreen, kDeviceScreen, kMixScreen };

struct Transport {
	float bpm = 120; // the internal tempo; a patched CLOCK overrides it, the TEMPO input scales it
	uint8_t swing = 0, quant = 4 /*1 Bar*/;
	bool metronome = false;
	bool playing = false, external = false; // transient from here on
	int64_t tick = 0;                       // the last tick run, 96 per beat
	float shownBpm = 120;                   // what the clock runs at now: the CV applied, or measured from CLOCK
};

/** The pad layout of Note mode (the Scale screen edits it). `layout` = 2 * (0 4ths, 1 3rds, 2 Sequential) + (0 up,
 * 1 right). */
struct Scale {
	uint8_t root = 0, scale = 0 /*Major*/, layout = 0;
	bool inKey = true, fixed = false;
};

struct Ui {
	// saved
	// noteLayout: 0 64 notes, 1 the Melodic Sequencer, 2 Sequencer + 32 Notes. fixedBars: Fixed Length, 0..7 = 1 Beat,
	// 2 Beats, 1 .. 32 Bars.
	uint8_t pads = kSession, noteLayout = 0, screen = 0, track = 0, scene = 0, fixedBars = 3;
	bool fixedOn = false;
	// Quantize: quantTo and recQuant index kQuantToNames, quantAmount is a percent, recQuantOn is Record Quantize.
	uint8_t quantTo = 4 /*1/16*/, quantAmount = 100, recQuant = 4;
	bool recQuantOn = false;
	bool automate = false;     // AUTOMATE: a take records the encoders of the Device and Mix screens
	uint8_t defaultLevel = 10; // Default LEVEL in volts (0..10): what Delete + touch resets a LEVEL to
	bool mixPans = false;      // the Mix screen's encoders are PAN (else LEVEL)
	// transient
	uint8_t page = 0;       // the sequencer's page of 8 steps (a drum track: 32)
	uint8_t pad[64] = {};   // velocity of each held pad, row * 8 + col
	bool button[128] = {};  // held buttons by CC
	uint32_t touched = 0;   // bit per push2 encoder id
	float bend = 0;         // the touch strip, -1..1
	uint64_t chord[2] = {}; // the split layout's chord: a bit per MIDI note, none: the bottom-left pad
	int8_t heldStep = -1;   // the sequencer pad (row * 8 + col) that is held, -1: none
	bool heldHad = false,
		 heldEdited = false; // the held step had a note when pressed; a long note or an encoder edited it
	double fixedAt = -1;     // when the press of Fixed Length began (-1: none, or used as a menu)
	uint8_t popup = kNoPopup;
	double popupAt = 0, now = 0; // seconds of audio time
	double screenAt = -1;        // when the press that opened the Scale or Clip screen began (-1: none)
	double padsAt = -1;          // when the press of Session or Note began (-1: none); held kHoldSeconds: momentary
	uint8_t prevPads = kSession; // the mode before that press
	int8_t dup = -1;             // Duplicate held: the slot to copy (track * 8 + scene), -1: none yet
	uint8_t browse = 0;          // the Browse screen's cursor (a type)
	double quantizeAt = -1;      // when the press of Quantize began (-1: none)
	bool quantizeUsed = false;   // that press turned an encoder or pressed a pad: no tap
	// The take (RECORD): its slot (recTrack -1: none), whether the clip was made by it, whether its length is open
	// (kMaxLength until it ends) and the tick at which an open take ends once RECORD is pressed again (-1: not yet).
	int8_t recTrack = -1, recScene = 0;
	bool recFresh = false, recOpen = false;
	int64_t recEnd = -1;
	uint16_t writing = 0; // the take's lanes that are being written (bit j: lane j of recTrack)
	// MUTE and SOLO share one state: muteSoloLock is the mode a long press locked (0 none, 1 Mute, 2 Solo), muteSoloAt
	// when the press that is held began (-1: none, or already used) and muteSoloUsed whether a lower button was pressed
	// meanwhile (no tap).
	uint8_t muteSoloLock = 0;
	double muteSoloAt = -1;
	bool muteSoloUsed = false;
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
	int16_t held = -1;     // the index in `notes` of the note on the held sequencer step, -1: none
	uint8_t heldCount = 0; // how many notes the held step has (split layout; else 0)
	// Device screen: the lanes with points per track (the selected clip for the selected track, the playing clip for
	// the others), the values at the held step, and the lowest touched encoder's lane (255: none) with its points.
	uint16_t automated[kTracks] = {}, at[kAutos] = {};
	uint8_t lane = 255;
	uint16_t nShown = 0;
	Point shown[kMaxPoints];
	Euclid euclid = {};  // the selected slot's
	uint16_t locked = 0; // a Euclid step is held: bit j: lane j has a lock on it
};

/** The Euclid notes that start in [t0, t0 + length) ticks after the clip's launch, at ticks relative to t0, before
 * swing, sorted, at most `max`. */
int euclidNotes(const Session& s, int track, const Euclid& e, int64_t t0, int length, Note* out, int max);

/** The pattern of a lane: bit p set = a pulse on step p, the rotation applied. */
uint32_t euclidMask(const EuclidLane& l);

/** Live's 70 clip colours as the Push shows them, by index (a track's `color`). They are also the Slice's number
 * colours. */
extern const uint8_t kLiveColors[kColors][3];

/** Palette slots of the Push's LEDs for the tracks: 100 + t full, 108 + t a quarter of it. They avoid the white LEDs'
 * indices (16, 48, 127) and the fixed colours 122..127. */
constexpr uint8_t kPaletteFull = 100, kPaletteDim = kPaletteFull + kTracks;
/** The 17th palette slot: yellow, for MUTE. ponytail: calibrate on the hardware. */
constexpr uint8_t kPaletteYellow = kPaletteFull + 2 * kTracks;

/** A length as the Home screen names it: "1 Bar", "2 Bars", "3 Beats" or "100 ticks". */
void lengthName(char* buf, size_t size, int length);

/** The text of the popup that `v.s.ui.popup` is. */
void popupText(const View& v, char* buf, size_t size);

/** What Ui::quantTo and Ui::recQuant are called, in order. */
extern const char* const kQuantToNames[8];

/** Whether the Quantize settings show: Quantize is held, and for kHoldSeconds already (a shorter press is a tap). */
inline bool quantizeSettings(const Ui& u) {
	return u.button[push2::cc::Quantize] && u.quantizeAt >= 0 && u.now - u.quantizeAt >= kHoldSeconds;
}

/** Whether the Euclid screen shows: a Euclid track is selected and the Home screen shows its settings (not Quantize's
 * or Fixed Length's), in Session and Note mode alike. */
inline bool euclidHome(const Session& s) {
	const Ui& u = s.ui;
	return s.track[u.track].type == Euclidean && u.screen == kHome && !quantizeSettings(u) &&
		   !u.button[push2::cc::FixedLength];
}

/** What Transport::quant (the launch quantization) is called, in order: None, 8 Bars ... 1/32. */
extern const char* const kQuantNames[14];

/** Where the Scale screen's root buttons go: the six upper buttons (2..7), then the six lower ones (2..7). */
constexpr uint8_t kRoots[12] = {0, 7, 2, 9, 4, 11, 5, 10, 3, 8, 1, 6};

/** Live's scales: the semitones above the root as a bitmask. */
struct ScaleDef {
	const char* name;
	uint16_t mask;
};
extern const ScaleDef kScales[];
extern const int kScaleCount;

/** The MIDI note of a pad in Note mode (row 0 = bottom), or -1 above 127. `root`: the pad is the scale's root,
 * `inScale`: it is in the scale (for Chromatic, which lights them). */
int padPitch(const Scale& sc, int octave, int row, int col, bool& root, bool& inScale);

/** Copies `text` (UTF-8) into `dst` of `size` bytes, cut on a character boundary, the rest zero. */
void setText(char* dst, size_t size, const char* text);

/** Copies `name` (UTF-8) into the track, cut on a character boundary. */
void setName(Track& t, const char* name);

/** Whether track `t` is silent: muted, or another track is soloed and it is not. */
inline bool silent(const Session& s, int t) {
	bool any = false;
	for (const Track& k : s.track)
		any |= k.solo;
	return s.track[t].mute || (any && !s.track[t].solo);
}

/** The LEVEL value of `volts` (0..10 V). */
uint16_t levelValue(int volts);

/** The volts of a lane's value `v`: LEVEL 0 to 10 V, PAN and bipolar CCs -5 to +5 V, other CCs 0 to 10 V. */
float volts(const Track& t, int lane, int v);

/** The clip's envelope on `lane` at `tick`: linear between points, the first value before the first and the last after
 * the last. Where two points share a tick, `arriving` takes the first, else the second. -1: the lane has no points. */
int envelope(const Clip& c, int lane, int tick, bool arriving = false);

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
	bool palette = false;  // the track colours changed (or never went out): send `rgb`, then reapply
	uint32_t rgb[17] = {}; // 0xRRGGBB for palette index kPaletteFull + k (kPaletteDim is k = 8..15, kPaletteYellow 16);
						   // valid with `ledsDone`
};

class Machine {
  public:
	Machine() : state_(new State), undo_(new Snapshot[kUndo]) { push_.setDivision(24); }
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
	void handle(State& st, const push2::Event& e);
	void pad(State& st, int index, int vel);
	void sequencer(State& st, int index, int vel);
	void splitPad(State& st, int index, int vel);
	void play(Session& s, int index, int pitch, int vel);
	void drumPad(State& st, int index, int vel);
	void loopPad(State& st, int index, int vel);
	bool browseButton(State& st, int button);
	void noteEncoder(State& st, int encoder, int delta);
	void valueEncoder(State& st, int encoder, int track, int lane, int delta);
	void stepLock(State& st, int lane, int delta);
	void deleteLane(State& st, int track, int lane);
	void write(State& st, int lane, int pos);
	void endWrite(State& st, int lane, bool stop = true);
	void session(State& st, int index);
	void sceneButton(State& st, int scene);
	void launch(Session& s, int track, int scene);
	void stopTrack(Session& s, int track, bool now);
	void stopClips(Session& s);
	void vacate(State& st, int track, int scene);
	void clear(State& st, int track, int scene);
	void copy(State& st, int track, int scene, int toTrack, int toScene);
	void leds(const State& st, push2::Leds& out) const;
	bool scaleButton(Scale& sc, int button); // on the Scale screen: whether it was one of its buttons
	void tap(Transport& t);
	void setLength(State& st, int ticks);
	void tick(State& st, int64_t k, Outputs& out);
	void layout(const State& st, int t);
	void euclidTick(State& st, int t, int64_t k);
	void euclidPad(State& st, int i, int vel);
	void euclidEncoder(State& st, int encoder, int delta);
	void euclidModeButton(State& st, int mode);
	void euclidGridButton(State& st, int grid);
	void euclidReset(State& st, int encoder);
	void euclidLock(State& st, int lane, int delta);
	Euclid& euclidEdit(State& st);
	void checkpoint(int track, int scene);
	void record(State& st);
	void endTake(bool discard = false);
	void recordNote(State& st, int pad);
	void quantizeClip(State& st, int p0, int p1);
	void doubleLoop(State& st);
	void quantizeEncoder(State& st, int encoder, int delta);
	bool undo(State& st, bool redo);

	// ponytail: one lock, as lattice::Machine; the audio side only try-locks and never waits.
	std::atomic<bool> busy_{false};
	std::atomic<bool> dirty_{false}; // the UI thread changed the state: the audio thread re-reads what it mirrors
	std::unique_ptr<State> state_;   // under busy_
	// Undo: a ring of the clips as they were before each edit (the entries of one gesture, a scene row's, share a
	// step), under busy_. Monotonic counters: [bottom, top) can be undone, [top, end) redone. ponytail: whole-clip
	// snapshots, 32 of them, ~1.4 MB allocated once here; a delta log if a deeper history is wanted.
	struct Snapshot {
		uint32_t step;
		uint8_t track, scene;
		Clip clip;
	};
	static constexpr int kUndo = 32;
	std::unique_ptr<Snapshot[]> undo_;
	uint32_t undoBottom_ = 0, undoTop_ = 0, undoEnd_ = 0, gesture_ = 0; // gesture_: bumped by every pad or button press
	std::atomic<bool> silence_{false}; // UI -> audio: load() replaced the state, drop the notes

	// Audio thread only.
	filo::CycleClock clock_;
	filo::ClockOut clk_, push_; // CLK output division; the Push's 24 pulses per beat
	filo::VoiceAllocator voice_[kTracks];
	struct Live {
		uint32_t id = 0; // 0: the pad isn't sounding
		uint8_t track = 0, pitch = 0, vel = 0;
		int8_t rec = -1;  // the scene of the take it is recorded into (-1: not recorded)
		uint16_t pos = 0; // where in that clip it started
		int64_t at = 0;   // the tick it started at
	};
	Live live_[64]; // the note each held pad started, on the track that was selected then
	// The notes the clips have started and when they end (tick), per track. ponytail: a track that has 64 notes
	// sounding drops new ones.
	static constexpr int kActive = 64;
	struct Active {
		uint32_t id;
		int64_t off;
	};
	Active active_[kTracks][kActive] = {};
	int nActive_[kTracks] = {};
	bool stopClips_ = false,
		 restartClips_ = false; // the transport stopped, or went back to its start: do it under the lock
	uint32_t nextId_ = 0;
	// What process() reads without the lock: copies of the state, refreshed under it.
	int voices_[kTracks] = {}, selected_ = 0;
	bool muted_[kTracks] = {};   // silent(): LEVEL 0 V and the gates closed
	uint8_t type_[kTracks] = {}; // a change silences the track (see layout)
	uint8_t layout_[kTracks] =
		{}; // Drum: 16 channels, one for each lane; else the track's Voices (Euclid: by the clip)
	uint32_t rgb_[kTracks] = {};
	float bend_ = 0; // V
	float sr_ = 48000, bpm_ = 120, cps_ = 0, freePhase_ = 0;
	int64_t nextTick_ = 0, frame_ = 0, atTicks_ = 0; // atTicks_: the clock's position now, rounded, for the pads
	double lastAt_ = -1;
	Request request_ = Request::None;
	bool sync_ = true; // take the lock this sample
	int accTempo_ = 0, accSwing_ = 0, accScale_[8] = {}, accNote_[4] = {}, accClip_ = 0, accBrowse_ = 0,
		accQuantTo_ = 0, accAmount_ = 0, accRecQuant_ = 0, accEuclid_[8] = {};
	// What the Slice reads: each lane's volts at the last tick and at the next, mirrored under the lock.
	float from_[kTracks][kAutos] = {}, to_[kTracks][kAutos] = {};
	int64_t lastTick_ = 0;
	// The automation writer, one swinging door per lane of the take's track: `on` while a pass is being written, `prev`
	// the last tick written, (t0, v0) the last point put in the clip, vl the value at prev, [lo, hi] the slopes from
	// (t0, v0) that stay within the tolerance of every sample since, `pre` the lane's value before its first turn and
	// oldLeft the old curve's last point at or before prev (`old`: the lane had a curve). writeBy_: the lane each
	// encoder is writing (-1: none), so that the release of its touch ends it.
	struct Door {
		bool on = false, old = false;
		int prev = 0, t0 = 0;
		uint16_t v0 = 0, vl = 0, pre = 0;
		float lo = 0, hi = 0;
		Point oldLeft = {};
	};
	Door door_[kAutos];
	int8_t writeBy_[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
	uint8_t sentColor_[kTracks] = {255, 255, 255, 255, 255, 255, 255, 255}; // what the Push's palette has; 255: none
	int64_t taps_[5] = {};
	int nTaps_ = 0;
};

/** Paints the Home screen into a push2::Screen frame (UI thread, GL). `sans`/`mono` are NanoVG font ids; `status`
 * (null: none) is the dim line over the middle, `message` (null: none) replaces the whole screen. */
void paint(NVGcontext* vg, const View& v, int sans, int mono, const char* status, const char* message);

} // namespace tart
