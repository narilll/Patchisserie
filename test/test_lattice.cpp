// Lattice: orca-c's VM (vendor/orca-c, unmodified) and its glue.
#include <string>
#include <vector>

#include "check.hpp"

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
