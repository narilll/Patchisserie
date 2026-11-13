#include "lattice.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

// orca-c is C99: `restrict` isn't C++ (its system headers first, then the define).
extern "C" {
#include "base.h"
#include "vmio.h"
#define restrict __restrict
#include "sim.h"
#undef restrict
}

namespace lattice {

// An arpeggio C E G A on channel 0 (eighth notes) and a kick on channel 1 (every beat). The clocks' output cells are
// locked by the D/C that writes them, so the bang a D leaves stays until the D rewrites it.
const char* const kDefaultGrid = "...2C4\n"
								 ".1D2.4TCEGA\n"
								 "...:03..2\n"
								 "...\n"
								 ".1D4\n"
								 "...:11C\n";

namespace {

constexpr double kEps = 1e-9; // internal clock: absorbs float error so frame k lands on the exact sample

int clampSize(int v) {
	return std::min(std::max(v, 1), kMaxSize);
}

} // namespace

// ---- Grid

Grid Grid::blank(int w, int h) {
	Grid g;
	g.w = clampSize(w), g.h = clampSize(h);
	g.cells.assign((size_t)g.w * g.h, '.');
	return g;
}

Grid Grid::parse(const std::string& text) {
	std::vector<std::string> rows;
	size_t width = 1;
	for (size_t i = 0; i < text.size() && rows.size() < (size_t)kMaxSize;) {
		size_t e = text.find('\n', i);
		if (e == std::string::npos)
			e = text.size();
		size_t len = e - i;
		if (len && text[e - 1] == '\r')
			len--;
		len = std::min(len, (size_t)kMaxSize);
		rows.push_back(text.substr(i, len));
		width = std::max(width, len);
		i = e + 1;
	}
	Grid g = blank((int)width, (int)rows.size());
	for (size_t y = 0; y < rows.size(); y++)
		for (size_t x = 0; x < rows[y].size(); x++)
			if (isGlyph(rows[y][x]))
				g.cells[y * g.w + x] = rows[y][x];
	return g;
}

std::string Grid::text() const {
	std::string s;
	s.reserve(cells.size() + h);
	for (int y = 0; y < h; y++) {
		s.append(&cells[(size_t)y * w], w);
		s += '\n';
	}
	return s;
}

void Grid::resize(int nw, int nh) {
	Grid g = blank(nw, nh);
	for (int y = 0; y < std::min(h, g.h); y++)
		std::memcpy(&g.cells[(size_t)y * g.w], &cells[(size_t)y * w], std::min(w, g.w));
	*this = std::move(g);
}

// ---- Machine

struct Machine::Events {
	Oevent_list list{};
	~Events() { oevent_list_deinit(&list); }
};

Machine::Machine() : events_(new Events), scratchEvents_(new Events) {
	for (int v = 0; v < 16; v++)
		voiceChan_[v] = v;
	clock_.setCps(bpm_ / 60.0);
	clock_.setPulsesPerCycle(4);
	clockOut_.setDivision(4);
	voices_.setMode(filo::VoiceAllocator::MPE);
	Grid g = Grid::parse(kDefaultGrid);
	install(g.w, g.h, std::move(g.cells), false);
}

Machine::~Machine() = default;

void Machine::lock() {
	while (!tryLock())
		while (busy_.load(std::memory_order_relaxed))
			std::this_thread::yield();
}

// ---- Audio thread

void Machine::setSampleRate(float sr) {
	if (sr != sr_)
		sr_ = sr, clock_.setSampleRate(sr);
}

void Machine::setBpm(float bpm) {
	bpm = std::min(std::max(bpm, 20.f), 300.f);
	if (bpm != bpm_) // setCps re-anchors the clock
		bpm_ = bpm, clock_.setCps(bpm / 60.0);
}

void Machine::setPulsesPerBeat(int ppc) {
	clock_.setPulsesPerCycle(ppc);
}

void Machine::setClockDivision(int n) {
	clockOut_.setDivision(n);
}

void Machine::setVoices(int n) {
	voices_.setChannels(n);
}

// Stop and reset: nothing sounds, bends re-centre; CC knobs hold.
void Machine::silence() {
	voices_.allOff();
	std::memset(notes_, 0, sizeof notes_);
	std::memset(bend_, 0, sizeof bend_);
	std::fill(mono_, mono_ + 16, MonoSlot{});
}

void Machine::release(int c, int n) {
	Note& e = notes_[c][n];
	voices_.noteOff(e.id);
	if (mono_[c].id == e.id)
		mono_[c].id = 0;
	e.id = 0;
}

// ponytail: one scan of all 2048 entries per frame; an active list if it is ever measured.
void Machine::releaseDue(int64_t k) {
	for (int c = 0; c < 16; c++)
		for (int n = 0; n < 128; n++)
			if (notes_[c][n].id && notes_[c][n].end <= k)
				release(c, n);
}

bool Machine::runFrame(int64_t k, Outputs& out) {
	if (!tryLock()) // the UI is editing: this frame waits for the next sample
		return false;
	std::memset(marks_.data(), 0, marks_.size());
	Oevent_list& list = events_->list;
	oevent_list_clear(&list);
	orca_run(cells_.data(), marks_.data(), (Usz)h_, (Usz)w_, (Usz)k, &list, 1);
	mapEvents(k, out);
	version_.fetch_add(1, std::memory_order_relaxed);
	unlock();
	nextFrame_ = k + 1;
	frame_.store(k, std::memory_order_relaxed);
	return true;
}

void Machine::mapEvents(int64_t k, Outputs& out) {
	const Oevent_list& list = events_->list;
	int lastMono[16];
	std::fill(lastMono, lastMono + 16, -1);
	for (Usz i = 0; i < list.count; i++) {
		const Oevent& e = list.buffer[i];
		if (e.any.oevent_type == Oevent_type_midi_note && e.midi_note.mono)
			lastMono[e.midi_note.channel & 15] = (int)i;
	}
	for (Usz i = 0; i < list.count; i++) {
		const Oevent& e = list.buffer[i];
		switch (e.any.oevent_type) {
		case Oevent_type_midi_note: {
			const Oevent_midi_note& m = e.midi_note;
			int c = m.channel & 15;
			if (m.mono && lastMono[c] != (int)i) // within a frame only the last `%` of a channel sounds
				break;
			// Orca JS's transpose: :03C is MIDI 60 = 0 V; the octave carries from the note and clamps at 8.
			int n = std::min(12 * std::min(m.octave + m.note / 12, 8) + m.note % 12 + 24, 127);
			if (m.mono && mono_[c].id) // one `%` voice per channel, as JS mono.js
				release(c, mono_[c].note);
			if (notes_[c][n].id) // retrigger: the allocator leaves the gate low for one sample
				release(c, n);
			uint32_t id = ++nextId_;
			if (!id)
				id = ++nextId_;
			int v = voices_.noteOn(id, (float)n, m.velocity / 127.f, c + 1);
			voiceChan_[v] = c;
			notes_[c][n] = {id, k + std::max(1, (int)m.duration)};
			if (m.mono)
				mono_[c] = {id, n};
			out.trig[c] = true;
			break;
		}
		case Oevent_type_midi_cc:
			if (e.midi_cc.control < 16)
				cc_[e.midi_cc.control] = e.midi_cc.value / 127.f;
			break;
		case Oevent_type_midi_pb: {
			// MIDI's default range: +-2 semitones. ponytail: fixed, a menu if +-12/24 is ever needed.
			int v = (e.midi_pb.msb << 7) | e.midi_pb.lsb;
			bend_[e.midi_pb.channel & 15] = (v - 8192) / 8192.f * (2.f / 12.f);
			break;
		}
		default: // OSC and UDP: nothing to send them to (and no logging on the audio thread)
			break;
		}
	}
}

void Machine::process(const Inputs& in, Outputs& out) {
	std::memset(out.trig, 0, sizeof out.trig);
	bool reset = in.resetEdge;
	switch (pending_.exchange(Transport::None, std::memory_order_acq_rel)) {
	case Transport::Play:
		if (!clock_.started())
			clock_.start(), reset = true;
		break;
	case Transport::Resume:
		if (!clock_.started())
			clock_.start();
		break;
	case Transport::Stop:
		if (clock_.started())
			clock_.stop(), silence();
		break;
	case Transport::None:
		break;
	}
	out.reset = reset;
	if (reset) { // the cycle restarts at 0: frame 0 runs on this sample
		clock_.step(in.clockConnected, in.clockEdge, true);
		silence();
		nextFrame_ = 0, lastAt_ = -1;
	} else {
		clock_.step(in.clockConnected, in.clockEdge, false);
	}
	// External: no epsilon, the PLL holds just below the next pulse and an epsilon would fire frames early.
	double at = clock_.cycle() + (in.clockConnected ? 0 : kEps);
	if (clock_.started()) {
		int64_t f = (int64_t)std::floor(at * 4); // 4 frames per beat, like Orca
		// ponytail: at most 4 frames per sample, the rest follow on the next samples; only PPC 1/2 jump that far, and
		// a length-1 note of a caught-up frame turns on and off within one sample.
		for (int n = 0; n < 4 && nextFrame_ <= f; n++) {
			releaseDue(nextFrame_);
			if (!runFrame(nextFrame_, out))
				break;
		}
	}
	out.clk = clock_.started() && clockOut_.clk(lastAt_, at);
	lastAt_ = at;
	for (int v = 0; v < 16; v++) {
		out.voct[v] = voices_.pitchV(v) + bend_[voiceChan_[v]];
		out.gate[v] = voices_.gate(v) ? 10.f : 0.f;
		out.vel[v] = voices_.velV(v);
		out.cc[v] = cc_[v] * 10.f;
	}
	out.channels = voices_.channels();
	voices_.tick();
	bool started = clock_.started();
	if (playing_.exchange(started, std::memory_order_relaxed) != started)
		version_.fetch_add(1, std::memory_order_relaxed); // the UI swaps between the frame's and the paused marks
}

// ---- UI thread

// Swaps in a prepared grid. Everything that allocates happens outside the lock; inside it are memcpys and swaps.
// The audio event list is sized so orca_run never reallocates: each emitting operator locks its right neighbour (or
// that cell is '.'), so a row holds at most ceil(w/2) events.
void Machine::install(int w, int h, std::vector<char> cells, bool keepOld) {
	std::vector<uint8_t> marks((size_t)w * h);
	size_t capacity = std::max<size_t>(16, (size_t)h * ((w + 1) / 2));
	// ponytail: malloc unchecked, like orca-c's own realloc.
	Oevent* buffer = (Oevent*)std::malloc(capacity * sizeof(Oevent));
	lock();
	if (keepOld)
		for (int y = 0; y < std::min(h, h_); y++)
			std::memcpy(&cells[(size_t)y * w], &cells_[(size_t)y * w_], std::min(w, w_));
	cells_.swap(cells);
	marks_.swap(marks);
	w_ = w, h_ = h;
	Oevent_list& list = events_->list;
	std::swap(list.buffer, buffer);
	list.capacity = capacity, list.count = 0;
	version_.fetch_add(1, std::memory_order_relaxed);
	unlock();
	std::free(buffer);
}

void Machine::resize(int w, int h) {
	w = clampSize(w), h = clampSize(h);
	if (w != w_ || h != h_)
		install(w, h, Grid::blank(w, h).cells, true);
}

void Machine::load(const std::string& text) {
	Grid g = Grid::parse(text);
	install(g.w, g.h, std::move(g.cells), false);
}

std::string Machine::write(Rect r, const std::string& glyphs) {
	if (r.w <= 0 || r.h <= 0)
		return {};
	size_t n = (size_t)r.w * r.h;
	std::string prev(n, '.'), next(n, '.');
	for (size_t i = 0; i < n && i < glyphs.size(); i++)
		if (isGlyph(glyphs[i]))
			next[i] = glyphs[i];
	int x0 = std::max(r.x, 0), x1 = std::min(r.x + r.w, w_);
	int y0 = std::max(r.y, 0), y1 = std::min(r.y + r.h, h_);
	lock();
	if (x1 > x0)
		for (int y = y0; y < y1; y++) {
			size_t o = (size_t)(y - r.y) * r.w + (x0 - r.x);
			char* cell = &cells_[(size_t)y * w_ + x0];
			std::memcpy(&prev[o], cell, x1 - x0);
			std::memcpy(cell, &next[o], x1 - x0);
		}
	version_.fetch_add(1, std::memory_order_relaxed);
	unlock();
	return prev;
}

bool Machine::snapshot(Grid& g, std::vector<uint8_t>& marks) {
	size_t n = (size_t)w_ * h_;
	g.cells.resize(n);
	marks.resize(n);
	lock();
	uint32_t version = version_.load(std::memory_order_relaxed);
	bool changed = version != seen_;
	if (changed) {
		std::memcpy(g.cells.data(), cells_.data(), n);
		std::memcpy(marks.data(), marks_.data(), n);
	}
	unlock();
	if (!changed)
		return false;
	seen_ = version;
	g.w = w_, g.h = h_;
	if (!playing()) {
		scratch_ = g.cells;
		std::fill(marks.begin(), marks.end(), 0);
		oevent_list_clear(&scratchEvents_->list); // may grow: this is the UI thread
		orca_run(scratch_.data(), marks.data(), (Usz)h_, (Usz)w_, (Usz)(frame() + 1), &scratchEvents_->list, 1);
	}
	return true;
}

std::string Machine::text() {
	Grid g;
	g.w = w_, g.h = h_;
	g.cells.resize((size_t)w_ * h_);
	lock();
	std::memcpy(g.cells.data(), cells_.data(), g.cells.size());
	unlock();
	return g.text();
}

size_t Machine::eventCapacity() {
	lock();
	size_t n = events_->list.capacity;
	unlock();
	return n;
}

} // namespace lattice
