// Lattice: orca-c's VM (vendor/orca-c, unmodified) and its glue.
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "lattice.hpp"

// orca-c is C99: `restrict` isn't C++ (its system headers first, then the define), no extern "C" of its own.
extern "C" {
#include "base.h"
#include "vmio.h"
#define restrict __restrict
#include "sim.h"
#undef restrict
}

namespace {

/** One orca-c frame of `rows` (all rows the same width), as its TUI runs it: marks cleared, events cleared. */
struct Vm {
	std::vector<std::string> rows;
	std::vector<Mark> marks;
	Oevent_list events;
	Usz tick = 0;

	explicit Vm(std::vector<std::string> r) : rows(std::move(r)) { oevent_list_init(&events); }
	~Vm() { oevent_list_deinit(&events); }

	void frame() {
		std::string g;
		for (const std::string& r : rows)
			g += r;
		marks.assign(g.size(), 0);
		oevent_list_clear(&events);
		orca_run(&g[0], marks.data(), rows.size(), rows[0].size(), tick++, &events, 1);
		for (size_t y = 0; y < rows.size(); y++)
			rows[y] = g.substr(y * rows[0].size(), rows[0].size());
	}
};

} // namespace

TEST(orca_vm_clock_and_delay) {
	// C: frame / rate % mod below it; D: a bang below it every rate*mod frames.
	Vm vm({".C4.1D2", "......."});
	const char* clock = "0123012";
	for (int t = 0; t < 7; t++) {
		vm.frame();
		CHECK(vm.rows[1][1] == clock[t]);
		CHECK(vm.rows[1][5] == (t % 2 == 0 ? '*' : '.'));
	}
	CHECK(vm.events.count == 0);
}

TEST(orca_vm_midi_event) {
	// A bang below : emits one note: channel 0, octave 3, C, velocity and length left empty. (Not left of it: cells
	// run in reading order, and a bang erases itself when it runs.)
	Vm vm({".:03C..", ".*....."});
	vm.frame();
	CHECK(vm.events.count == 1);
	const Oevent_midi_note& n = vm.events.buffer[0].midi_note;
	CHECK(n.oevent_type == Oevent_type_midi_note);
	CHECK(n.channel == 0 && n.octave == 3 && n.note == 0);
}

TEST(orca_vm_mark_flags) {
	// Lattice's lattice::MarkFlag copies gbuffer.h's Mark_flags: the values show in what sim.c writes (PORT xors Lock).
	Vm add({"1A2", "..."});
	add.frame();
	CHECK(add.marks[0] == (lattice::kInput | lattice::kHasteInput | lattice::kLock)); // 13
	CHECK(add.marks[2] == (lattice::kInput | lattice::kLock));                        // 9
	CHECK(add.marks[4] == (lattice::kOutput | lattice::kLock));                       // 10
	Vm move({"E."});
	move.frame();
	CHECK(move.marks[1] == lattice::kSleep); // 16
}

// ---- Grid

using namespace lattice;

TEST(lattice_grid_parse_text) {
	std::string src = "A$#comment text#\n.:03C\n";
	Grid g = Grid::parse(src);
	CHECK(g.w == 16 && g.h == 2);
	CHECK(g.at(1, 0) == '$' && g.at(2, 0) == '#' && g.at(0, 1) == '.');
	CHECK(g.at(15, 1) == '.'); // ragged row padded
	CHECK(Grid::parse(g.text()).cells == g.cells);
	CHECK(g.text().back() == '\n');
	Grid crlf = Grid::parse("ab\r\ncd\r\n");
	CHECK(crlf.w == 2 && crlf.h == 2 && crlf.at(1, 0) == 'b' && crlf.at(0, 1) == 'c');
	Grid odd = Grid::parse(std::string("a b\x01\xff\n", 6));
	CHECK(odd.w == 5 && odd.text() == "a.b..\n");
	Grid empty = Grid::parse("");
	CHECK(empty.w == 1 && empty.h == 1 && empty.at(0, 0) == '.');
	Grid big = Grid::parse(std::string(300, 'x') + "\n" + std::string(300, '\n'));
	CHECK(big.w == kMaxSize && big.h == kMaxSize);
}

TEST(lattice_grid_resize_keeps_top_left) {
	Grid g = Grid::parse("abc\ndef\nghi");
	g.resize(2, 4);
	CHECK(g.text() == "ab\nde\ngh\n..\n");
	g.resize(0, 9999);
	CHECK(g.w == 1 && g.h == kMaxSize && g.at(0, 0) == 'a' && g.at(0, 2) == 'g');
	CHECK(Grid::blank(-5, 300).w == 1 && Grid::blank(-5, 300).h == kMaxSize);
}

// ---- Machine

namespace {

/** A Machine driven sample by sample at 48 kHz. */
struct Rig {
	Machine m;
	Machine::Inputs in;
	Machine::Outputs out{};
	float bpm = 120;
	int64_t sample = 0; // the next sample's index

	explicit Rig(const std::string& grid) { m.load(grid); }
	void step() {
		m.setSampleRate(48000);
		m.setBpm(bpm);
		m.process(in, out);
		in.clockEdge = in.resetEdge = false;
		sample++;
	}
	void run(int n) {
		while (n-- > 0)
			step();
	}
	void play() { m.request(Machine::Transport::Play); }
	/** Steps `n` samples; the sample indices at which a new frame showed up. */
	std::vector<int64_t> frames(int n) {
		std::vector<int64_t> at;
		for (int i = 0; i < n; i++) {
			int64_t before = m.frame();
			int64_t s = sample;
			step();
			if (m.frame() != before)
				at.push_back(s);
		}
		return at;
	}
	Grid grid;
	std::vector<uint8_t> marks;
	char cell(int x, int y) {
		m.snapshot(grid, marks);
		return grid.at(x, y);
	}
};

// A note at frame 0: `rest` after the ':' (channel, octave, note, velocity, length); the bang below runs it once.
std::string note(const std::string& rest, char op = ':') {
	return std::string(1, op) + rest + "\n*\n";
}

} // namespace

TEST(lattice_frames_internal_clock) {
	Rig r(".");
	r.play();
	CHECK((r.frames(12001) == std::vector<int64_t>{0, 6000, 12000}));
	// 130 BPM: frame k on the first sample n with n * 130/60 / 48000 >= k/4, i.e. n = ceil(k * 2880000 / 520).
	Rig q(".");
	q.bpm = 130;
	q.play();
	std::vector<int64_t> at = q.frames(80000);
	CHECK(at.size() == 15);
	for (size_t k = 0; k < at.size(); k++)
		CHECK(at[k] == ((int64_t)k * 2880000 + 519) / 520);
}

TEST(lattice_frames_external_clock) {
	// PPC 4: a frame per pulse, on the pulse's sample.
	Rig r(".");
	r.m.setPulsesPerBeat(4);
	r.play();
	std::vector<int64_t> at;
	for (int i = 0; i < 1000; i++) {
		r.in.clockConnected = true;
		r.in.clockEdge = i > 0 && i % 100 == 0;
		int64_t before = r.m.frame();
		r.step();
		if (r.m.frame() != before)
			at.push_back(i);
	}
	CHECK((at == std::vector<int64_t>{0, 100, 200, 300, 400, 500, 600, 700, 800, 900}));
	// PPC 24: a frame per 6 pulses.
	Rig p(".");
	p.m.setPulsesPerBeat(24);
	p.play();
	at.clear();
	for (int i = 0; i < 1000; i++) {
		p.in.clockConnected = true;
		p.in.clockEdge = i > 0 && i % 50 == 0;
		int64_t before = p.m.frame();
		p.step();
		if (p.m.frame() != before)
			at.push_back(i);
	}
	CHECK((at == std::vector<int64_t>{0, 300, 600, 900}));
}

TEST(lattice_frames_ppc_change_while_playing) {
	// A pulse every 100 samples. PPC 4 -> 24: frames go on (a frame per 6 pulses); 24 -> 4: no burst of frames.
	Rig r(".");
	r.m.setPulsesPerBeat(4);
	r.play();
	int i = 0;
	auto pulses = [&](int n) { // n pulses, as the samples between them
		for (int k = 0; k < n * 100; k++, i++) {
			r.in.clockConnected = true;
			r.in.clockEdge = i % 100 == 0;
			r.step();
		}
	};
	pulses(40);
	r.m.setPulsesPerBeat(24);
	int64_t f = r.m.frame();
	pulses(6 + 3);
	CHECK(r.m.frame() > f);
	pulses(24);
	CHECK(r.m.frame() >= f + 4);
	r.m.setPulsesPerBeat(4);
	f = r.m.frame();
	pulses(12);
	CHECK(r.m.frame() >= f + 10 && r.m.frame() <= f + 14);
}

TEST(lattice_frames_slowing_clock_never_early) {
	Rig r(".");
	r.m.setPulsesPerBeat(4);
	r.play();
	int64_t pulses = 0, next = 100, period = 100;
	for (int i = 0; i < 20000; i++) {
		r.in.clockConnected = true;
		r.in.clockEdge = i == next;
		if (r.in.clockEdge)
			pulses++, period += 10, next += period;
		r.step();
		CHECK(r.m.frame() <= pulses);
	}
	CHECK(pulses > 40 && r.m.frame() >= pulses - 1);
}

TEST(lattice_frames_ppc1_catch_up) {
	// ".I9" counts the frames run in the cell below it. With no tempo estimate yet, the first pulse jumps the cycle a
	// whole beat: frames 1..4 arrive on its sample, none lost.
	Rig r(".I9\n...");
	r.m.setPulsesPerBeat(1);
	r.play();
	for (int i = 0; i < 300; i++) {
		r.in.clockConnected = true;
		r.in.clockEdge = i == 200;
		r.step();
		if (i == 199)
			CHECK(r.m.frame() == 0 && r.cell(1, 1) == '1');
		if (i == 200)
			CHECK(r.m.frame() == 4 && r.cell(1, 1) == '5');
	}
}

TEST(lattice_note_pitch_gate_velocity) {
	Rig r(note("03C"));
	r.play();
	r.step();
	CHECK(r.out.voct[0] == 0.f && r.out.gate[0] == 10.f && r.out.vel[0] == 10.f && r.out.channels == 16);
	CHECK(r.out.trig[0] && !r.out.trig[1]);
	r.step();
	CHECK(!r.out.trig[0]);
	// velocity 8 -> 63/127
	Rig v(note("03C8"));
	v.play();
	v.step();
	CHECK_NEAR(v.out.vel[0], 63 / 127.f * 10, 1e-5);
	// Octave carry, clamped at 8: 9b = octave 10 -> 8 -> 120; J is the C of the next octave (H, I are A, B);
	// octave 8 B = 131 -> 127.
	struct {
		const char* rest;
		float midi;
	} cases[] = {{"09b", 120}, {"03J", 72}, {"03H", 69}, {"08B", 127}, {"0.C", 0}, {"00C", 24}};
	for (auto& c : cases) {
		Rig n(note(c.rest));
		n.play();
		n.step();
		if (c.midi)
			CHECK_NEAR(n.out.voct[0], (c.midi - 60) / 12.f, 1e-6); // "0.C": no octave, no note
		else
			CHECK(n.out.gate[0] == 0.f);
	}
}

TEST(lattice_note_length) {
	// Length 2 ends on frame 2's sample; '.' and '0' are 1 frame.
	Rig r(note("03C.2"));
	r.play();
	r.run(12000);
	CHECK(r.out.gate[0] == 10.f); // sample 11999
	r.step();
	CHECK(r.out.gate[0] == 0.f); // sample 12000 = frame 2
	for (const char* len : {"03C..", "03C.0"}) {
		Rig n(note(len));
		n.play();
		n.run(6000);
		CHECK(n.out.gate[0] == 10.f);
		n.step();
		CHECK(n.out.gate[0] == 0.f);
	}
}

TEST(lattice_note_retrigger_gap) {
	// 1D1 bangs every frame: the second note-on is the same pitch, still sounding.
	Rig r("1D1\n..:03C.8\n");
	r.m.load("1D1....\n..:03C.8\n");
	r.play();
	r.run(6000);
	CHECK(r.out.gate[0] == 10.f); // sample 5999
	r.step();
	CHECK(r.out.gate[0] == 0.f && r.out.trig[0]); // sample 6000: released and re-triggered
	r.step();
	CHECK(r.out.gate[0] == 10.f);
}

TEST(lattice_mono_releases_only_its_own) {
	// Rotate gives every note its own voice. %C and :E at frame 0; %G later releases C but not E.
	Rig r("%03C.8\n*\n\n:03E.8\n*\n\n%03G.8\n.\n");
	r.m.setPolyMode(filo::VoiceAllocator::Rotate);
	r.play();
	r.step();
	CHECK(r.out.gate[0] == 10.f && r.out.gate[1] == 10.f && r.out.gate[2] == 0.f);
	r.m.write({0, 7, 1, 1}, "*");
	r.run(6000);
	CHECK(r.out.gate[0] == 0.f && r.out.gate[1] == 10.f && r.out.gate[2] == 10.f);
	CHECK_NEAR(r.out.voct[2], 7 / 12.f, 1e-6);
}

TEST(lattice_mono_last_in_a_frame_wins) {
	Rig r("%03C.8\n*\n\n%03G.8\n*\n");
	r.m.setPolyMode(filo::VoiceAllocator::Rotate);
	r.play();
	r.step();
	CHECK(r.out.gate[0] == 10.f && r.out.gate[1] == 0.f);
	CHECK_NEAR(r.out.voct[0], 7 / 12.f, 1e-6);
	CHECK(r.out.trig[0]);
}

TEST(lattice_stop_forgets_mono_slot) {
	// A `%` note, Stop, a new grid whose `:` C sounds, then a `%`: the stale slot must not release that C.
	Rig r("%03C.z\n*\n");
	r.m.setPolyMode(filo::VoiceAllocator::Rotate);
	r.play();
	r.run(100);
	r.m.request(Machine::Transport::Stop);
	r.step();
	CHECK(r.out.gate[0] == 0.f);
	r.m.load(":03C.z\n*\n\n%03G.z\n.\n");
	r.play();
	r.step(); // reset, frame 0: C on (the next voice in rotation)
	CHECK(r.out.gate[1] == 10.f);
	r.m.write({0, 4, 1, 1}, "*");
	r.run(6000);
	CHECK(r.out.gate[1] == 10.f && r.out.gate[2] == 10.f);
}

TEST(lattice_cc) {
	Rig r(note("05i", '!'));
	r.play();
	r.step();
	CHECK_NEAR(r.out.cc[5], 65 / 127.f * 10, 1e-5);
	r.run(7000);
	CHECK_NEAR(r.out.cc[5], 65 / 127.f * 10, 1e-5); // held
	Rig ignored(note("0gz", '!'));                  // knob 16
	ignored.play();
	ignored.step();
	for (int k = 0; k < 16; k++)
		CHECK(ignored.out.cc[k] == 0.f);
}

TEST(lattice_pitch_bend) {
	// Channel 0 bends, channel 1 doesn't.
	Rig r(":03C..\n*\n?0zz\n*\n:13C..\n*\n");
	r.play();
	r.step();
	CHECK_NEAR(r.out.voct[0], 2 / 12.f * (16383 - 8192) / 8192.f, 1e-6);
	CHECK(r.out.voct[1] == 0.f);
	r.run(6000); // note off at frame 1: the bend stays
	CHECK(r.out.gate[0] == 0.f);
	CHECK_NEAR(r.out.voct[0], 2 / 12.f * (16383 - 8192) / 8192.f, 1e-6);
	r.m.request(Machine::Transport::Stop);
	r.step();
	CHECK(r.out.voct[0] == 0.f);
	// Minimum, and a reset input re-centres.
	Rig q(":03C.z\n*\n?0..\n*\n");
	q.play();
	q.step();
	CHECK_NEAR(q.out.voct[0], -2 / 12.f, 1e-6);
	q.in.resetEdge = true;
	q.step();
	CHECK(q.out.voct[0] == 0.f && q.out.gate[0] == 0.f);
}

TEST(lattice_osc_udp_dollar_are_inert) {
	Rig r("=a15\n*\n;abc\n*\n$$$\n");
	r.play();
	r.run(7000);
	for (int v = 0; v < 16; v++)
		CHECK(r.out.gate[v] == 0.f && r.out.cc[v] == 0.f && !r.out.trig[v]);
	CHECK(r.cell(0, 4) == '$' && r.cell(2, 4) == '$');
}

TEST(lattice_transport) {
	Rig r(".");
	r.play();
	r.step();
	CHECK(r.out.reset && r.m.frame() == 0 && r.m.playing());
	r.step();
	CHECK(!r.out.reset);
	r.run(98);
	r.play(); // already playing: ignored, no reset, the frame clock keeps its course
	r.step();
	CHECK(!r.out.reset && r.m.frame() == 0);
	std::vector<int64_t> at = r.frames(6000);
	CHECK((at == std::vector<int64_t>{6000}));
	// Resume continues from the next frame, no reset.
	r.m.request(Machine::Transport::Stop);
	r.step();
	CHECK(!r.m.playing());
	int64_t frame = r.m.frame();
	r.run(500);
	r.m.request(Machine::Transport::Resume);
	r.step();
	CHECK(r.m.playing() && !r.out.reset && r.m.frame() == frame);
	at = r.frames(12000);
	CHECK(at.size() == 2 && r.m.frame() == frame + 2);
}

TEST(lattice_stop_brings_gates_down) {
	Rig r(note("03C.z"));
	r.play();
	r.run(100);
	CHECK(r.out.gate[0] == 10.f);
	r.m.request(Machine::Transport::Stop);
	r.step();
	CHECK(r.out.gate[0] == 0.f);
}

TEST(lattice_reset_while_stopped_rewinds_frame) {
	Rig r(".");
	r.play();
	r.run(7000);
	CHECK(r.m.frame() == 1);
	r.m.request(Machine::Transport::Stop);
	r.step();
	r.in.resetEdge = true;
	r.step();
	CHECK(r.m.frame() == -1 && !r.m.playing());
	r.m.request(Machine::Transport::Resume);
	r.step();
	CHECK(r.m.frame() == 0);
}

TEST(lattice_reset_input) {
	Rig r(note("03C.z"));
	r.play();
	r.run(7000); // frame 1
	CHECK(r.m.frame() == 1 && r.out.gate[0] == 10.f);
	r.in.resetEdge = true;
	r.step();
	CHECK(r.out.reset && r.m.frame() == 0 && r.out.gate[0] == 0.f);
	r.step();
	CHECK(!r.out.reset);
}

TEST(lattice_clk_output) {
	// CLK division 4 = one pulse per frame (and one at the start, like Filo's Sequencer).
	Rig r(".");
	r.m.setClockDivision(4);
	r.play();
	int n = 0;
	for (int i = 0; i < 12001; i++)
		r.step(), n += r.out.clk;
	CHECK(n == 3);
}

TEST(lattice_clk_output_resume) {
	// Resume of a fresh machine pulses on frame 0; Resume mid-frame after a Stop doesn't pulse again.
	Rig r(".");
	r.m.setClockDivision(4);
	r.m.request(Machine::Transport::Resume);
	r.step();
	CHECK(r.m.frame() == 0 && r.out.clk);
	r.run(100);
	r.m.request(Machine::Transport::Stop);
	r.step();
	r.run(50);
	r.m.request(Machine::Transport::Resume);
	int n = 0;
	for (int i = 0; i < 5000; i++)
		r.step(), n += r.out.clk;
	CHECK(n == 0);
	r.run(1000);
	CHECK(r.m.frame() == 1);
	// RESET while stopped, then Resume: frame 0 pulses although the stopped samples ran in between.
	r.m.request(Machine::Transport::Stop);
	r.step();
	r.in.resetEdge = true;
	r.step();
	r.run(10);
	r.m.request(Machine::Transport::Resume);
	r.step();
	CHECK(r.m.frame() == 0 && r.out.clk);
}

TEST(lattice_no_allocation_on_the_audio_thread) {
	// Worst cases: ';' under a bang row (every other cell emits, one frame), "=**" and ":*0C.." rows whose bangs their
	// operators lock, so they emit every frame.
	for (int size : {64, 256}) {
		std::string g;
		for (int y = 0; y < size; y++) {
			const char* row[] = {";.", "*", "=**", ":*0C.."};
			const char* tile = row[y % 4];
			for (int x = 0; x < size; x++)
				g += tile[x % strlen(tile)];
			g += '\n';
		}
		Rig r(g);
		size_t capacity = r.m.eventCapacity();
		CHECK(capacity == (size_t)size * ((size + 1) / 2));
		r.m.setPulsesPerBeat(4);
		r.play();
		for (int i = 0; i < 300; i++) { // a frame per sample once the reset's guard is over
			r.in.clockConnected = true;
			r.in.clockEdge = true;
			r.step();
		}
		CHECK(r.m.frame() > 200);
		CHECK(r.m.eventCapacity() == capacity);
	}
}

TEST(lattice_paused_marks) {
	Machine m;
	m.load("1A2\n...");
	Grid g;
	std::vector<uint8_t> marks;
	CHECK(m.snapshot(g, marks));
	CHECK(marks[0] == (kInput | kHasteInput | kLock) && marks[2] == (kInput | kLock) && marks[4] == (kOutput | kLock));
	CHECK(g.text() == "1A2\n...\n"); // orca ran on a copy
	CHECK(!m.snapshot(g, marks));
	// Moving E: paused, the marks are the next frame's (E about to move); playing, the last frame's (E moved).
	m.load("E..");
	CHECK(m.snapshot(g, marks) && g.at(0, 0) == 'E' && marks[1] == kSleep && marks[2] == 0);
	Machine::Inputs in;
	Machine::Outputs out{};
	m.request(Machine::Transport::Play);
	m.process(in, out);
	CHECK(m.snapshot(g, marks)); // play/stop changes what the marks show
	CHECK(g.at(1, 0) == 'E' && marks[1] == kSleep && marks[2] == 0);
}

TEST(lattice_write) {
	Machine m;
	m.load("abc\ndef\nghi");
	CHECK(m.write({1, 1, 2, 2}, "XYZW") == "efhi");
	CHECK(m.text() == "abc\ndXY\ngZW\n");
	CHECK(m.write({2, 2, 2, 2}, "1234") == "W..."); // clipped at the edges
	CHECK(m.write({-1, 0, 2, 1}, "QR") == ".a");
	CHECK(m.text() == "Rbc\ndXY\ngZ1\n");
	CHECK(m.write({0, 0, 1, 1}, " ") == "R"); // not a glyph
	CHECK(m.text()[0] == '.');
	Grid g;
	std::vector<uint8_t> marks;
	CHECK(m.snapshot(g, marks) && !m.snapshot(g, marks));
	m.write({0, 0, 1, 1}, "x");
	CHECK(m.snapshot(g, marks) && g.at(0, 0) == 'x' && !m.snapshot(g, marks));
	m.resize(5, 2);
	CHECK(m.width() == 5 && m.height() == 2 && m.snapshot(g, marks) && g.w == 5 && g.h == 2);
	CHECK(m.text() == "xbc..\ndXY..\n");
	m.load("z");
	CHECK(m.snapshot(g, marks) && g.w == 1 && g.cells[0] == 'z');
}

TEST(lattice_default_grid_plays) {
	Machine m;
	CHECK(m.width() <= 38 && m.height() <= 12);
	Rig r(kDefaultGrid);
	r.play();
	bool arp = false, kick = false;
	std::vector<float> pitches;
	for (int i = 0; i < 32 * 6000; i++) {
		r.step();
		arp |= r.out.gate[0] == 10.f;
		kick |= r.out.trig[1];
		if (r.out.trig[0])
			pitches.push_back(r.out.voct[0]);
	}
	CHECK(arp && kick);
	CHECK(pitches.size() == 16); // an eighth note every 2 frames
	const float arp4[4] = {0, 4, 7, 9};
	for (size_t i = 0; i < 4 && i < pitches.size(); i++)
		CHECK_NEAR(pitches[i], arp4[i] / 12.f, 1e-6);
}

TEST(lattice_threads) {
	// UI thread: every call that takes the lock. Audio thread (here): process at 300 BPM. ASan/TSan find the rest.
	Machine m;
	m.setBpm(300);
	m.request(Machine::Transport::Play);
	std::atomic<bool> done{false};
	std::thread ui([&] {
		auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
		Grid g;
		std::vector<uint8_t> marks;
		for (int i = 0; std::chrono::steady_clock::now() < end; i++) {
			m.write({i % 7, i % 5, 3, 2}, "A3C:03");
			m.snapshot(g, marks);
			if (i % 11 == 0)
				m.resize(10 + i % 40, 8 + i % 23);
			if (i % 13 == 0)
				m.load(i % 2 ? kDefaultGrid : "E.\n.E\n*:03C");
			if (i % 5 == 0)
				m.text();
			if (i % 17 == 0)
				m.request(i % 2 ? Machine::Transport::Stop : Machine::Transport::Resume);
		}
		done = true;
	});
	Machine::Inputs in;
	Machine::Outputs out{};
	while (!done)
		m.process(in, out);
	ui.join();
	CHECK(m.frame() > 0);
}
