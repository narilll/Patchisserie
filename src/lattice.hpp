#pragma once
// Lattice: the Orca livecoding language (Hundred Rabbits) on orca-c's VM, as Rack outputs. The grid, the transport
// and the event mapping live here; orca-c's headers stay in lattice.cpp. Audio-thread paths (Machine::process and
// its setters) never allocate, wait on a lock, throw or log.
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dsp.hpp"

namespace lattice {

// orca-c's Mark_flags (gbuffer.h is C only: it doesn't compile as C++).
// ponytail: copied values; test_lattice pins them through orca_run. Include gbuffer.h if orca-c ever gets a C++ guard.
enum MarkFlag : uint8_t { kInput = 1, kOutput = 2, kHasteInput = 4, kLock = 8, kSleep = 16 };

// What the panel's view shows at 12 px: 38 columns of Plex Mono (7.2 px) in 95.6 mm minus the pads, 12 lines of 16 px
// above the status line in 76 mm (test/glcheck.cpp's panel case measures it).
constexpr int kDefaultWidth = 38, kDefaultHeight = 12;
constexpr int kMaxSize = 256; // ponytail: grid ceiling per side (memory + one frame's CPU on the audio thread)

struct Rect {
	int x = 0, y = 0, w = 0, h = 0;
};

struct Grid {
	int w = 0, h = 0;
	std::vector<char> cells; // row-major w*h

	static Grid blank(int w, int h); // '.' everywhere, w/h clamped to 1..kMaxSize
	/** .orca text: one row per line, ragged rows padded with '.', anything but printable ASCII becomes '.'. */
	static Grid parse(const std::string& text);
	std::string text() const;  // rows joined by '\n', trailing '\n'; parse(text()) round-trips
	void resize(int w, int h); // keeps the top-left, fills '.', clamps to 1..kMaxSize
	char at(int x, int y) const { return cells[(size_t)y * w + x]; }
};

// Printable ASCII 33..126, not orca_is_valid_glyph: that one rejects '$' and the text of comments, which Orca files
// contain; orca-c treats unknown glyphs as inert.
inline bool isGlyph(char c) {
	return c >= 33 && c <= 126;
}

/** The default grid (at its top left, padded to kDefaultWidth x kDefaultHeight): plays an arpeggio on Orca channel 0
 * and a kick on channel 1. */
extern const char* const kDefaultGrid;

class Machine {
  public:
	struct Inputs {
		bool clockConnected = false, clockEdge = false, resetEdge = false;
	};
	struct Outputs {
		float voct[16], gate[16], vel[16], cc[16]; // volts; gate 0/10
		bool trig[16], clk, reset;                 // this sample: the caller turns them into 1 ms pulses
		int channels;                              // V/OCT, GATE, VEL channel count (voices)
	};
	enum class Transport : uint8_t { None, Play, Resume, Stop };

	Machine(); // kDefaultGrid, 120 BPM, 16 voices, MPE, CLK division 4, 4 pulses per beat
	~Machine();
	Machine(const Machine&) = delete;
	Machine& operator=(const Machine&) = delete;

	// --- Any thread
	/** Applied at the start of the next process(); the last request wins. */
	void request(Transport t) { pending_.store(t, std::memory_order_release); }
	bool playing() const { return playing_.load(std::memory_order_relaxed); }
	int64_t frame() const { return frame_.load(std::memory_order_relaxed); } // last frame run, -1 before the first

	// --- Audio thread. Module::process calls the setters every sample: each is cheap and re-applies only on change.
	void setSampleRate(float sr);
	void setBpm(float bpm);         // 20..300; one beat = one CycleClock cycle = 4 frames
	void setPulsesPerBeat(int ppc); // CLOCK input pulses per beat
	void setClockDivision(int n);   // CLK output pulses per beat
	void setVoices(int n);
	void setPolyMode(filo::VoiceAllocator::Mode m) { voices_.setMode(m); }
	void process(const Inputs& in, Outputs& out); // once per sample

	// --- UI thread (only one)
	int width() const { return w_; }
	int height() const { return h_; }
	/** Writes `glyphs` (r.w*r.h, row-major) into r clipped to the grid; returns what was there (same shape, '.' outside
	 * the grid), read and written under one lock: undo's diff. */
	std::string write(Rect r, const std::string& glyphs);
	/** Copies the grid and its marks into g/marks if anything changed since the last call (else returns false). While
	 * stopped the marks come from running orca on a scratch copy, as orca-c's TUI does, so typed operators show their
	 * ports; the grid itself isn't touched. Pass the same g/marks every time. */
	bool snapshot(Grid& g, std::vector<uint8_t>& marks);
	void resize(int w, int h);
	void load(const std::string& text);
	std::string text();
	size_t eventCapacity(); // for tests: the audio event list's capacity

  private:
	struct Events; // orca-c's Oevent_list (an anonymous C struct: no forward declaration)
	struct Note {
		uint32_t id = 0; // 0 = free
		int64_t end = 0; // frame
	};
	struct MonoSlot {
		uint32_t id = 0;
		int note = 0;
	};

	bool tryLock() { return !busy_.exchange(true, std::memory_order_acquire); }
	void unlock() { busy_.store(false, std::memory_order_release); }
	void lock();
	void install(int w, int h, std::vector<char> cells, bool keepOld);
	void silence();
	void release(int c, int n);
	void releaseDue(int64_t k);
	bool runFrame(int64_t k, Outputs& out);
	void mapEvents(int64_t k, Outputs& out);

	// ponytail: one lock; the audio side only try-locks at frame ticks and never waits. Command/snapshot rings if
	// delayed frames are ever measured.
	std::atomic<bool> busy_{false};
	std::atomic<uint32_t> version_{0}; // bumped by anything the UI shows
	std::atomic<Transport> pending_{Transport::None};
	std::atomic<bool> playing_{false};
	std::atomic<int64_t> frame_{-1};

	// Under busy_: the grid, its marks and the audio event list. w_/h_ are written by the UI thread only.
	int w_ = 0, h_ = 0;
	std::vector<char> cells_;
	std::vector<uint8_t> marks_;
	std::unique_ptr<Events> events_;

	// UI thread only: paused marks.
	uint32_t seen_ = UINT32_MAX;
	std::vector<char> scratch_;
	std::unique_ptr<Events> scratchEvents_;

	// Audio thread only.
	filo::CycleClock clock_;
	filo::VoiceAllocator voices_;
	filo::ClockOut clockOut_;
	float sr_ = 48000, bpm_ = 120;
	int64_t nextFrame_ = 0;
	double lastAt_ = -1;
	uint32_t nextId_ = 0;
	Note notes_[16][128]; // by Orca channel and MIDI number
	MonoSlot mono_[16];
	int voiceChan_[16];
	float bend_[16] = {}, cc_[16] = {}; // volts, 0..1
};

} // namespace lattice
