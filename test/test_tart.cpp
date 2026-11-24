// Tart: the Push 2 decoder, LED diff, display frame and the echo state.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "check.hpp"
#include "push2.hpp"
#include "tart.hpp"

using push2::Event;

namespace {

Event dec(std::initializer_list<uint8_t> bytes) {
	std::vector<uint8_t> v(bytes);
	Event e;
	push2::decode(v.data(), v.size(), e);
	return e;
}

} // namespace

TEST(push2_relative) {
	CHECK(push2::relative(1) == 1);
	CHECK(push2::relative(63) == 63);
	CHECK(push2::relative(64) == -64);
	CHECK(push2::relative(127) == -1);
}

TEST(push2_pad_notes) {
	for (int r = 0; r < 8; r++)
		for (int c = 0; c < 8; c++) {
			int rr = -1, cc = -1;
			CHECK(push2::padOf(push2::padNote(r, c), rr, cc) && rr == r && cc == c);
		}
	CHECK(push2::padNote(0, 0) == 36 && push2::padNote(7, 7) == 99);
	int r, c;
	CHECK(!push2::padOf(35, r, c));
	CHECK(!push2::padOf(100, r, c));
}

TEST(push2_user_port) {
	for (const char* n : {"Ableton Push 2 User Port", "MIDIIN2 (Ableton Push 2)", "MIDIOUT2 (Ableton Push 2)",
						  "MIDIIN2 (Ableton Push 2) 3", "Ableton Push 2:Ableton Push 2 MIDI 2 20:1",
						  "Ableton Push 2:Ableton Push 2 User Port 20:1"})
		CHECK(push2::isUserPort(n));
	for (const char* n :
		 {"Ableton Push 2 Live Port", "Ableton Push 2", "Ableton Push 2 1", "Ableton Push 2:Ableton Push 2 MIDI 1 20:0",
		  "Ableton Push 2:Ableton Push 2 Live Port 20:0", ""})
		CHECK(!push2::isUserPort(n));
}

TEST(push2_decode) {
	Event e = dec({0x90, 36, 127});
	CHECK(e.type == Event::Pad && e.index == 0 && e.value == 127 && e.size == 3 && e.raw[1] == 36);
	e = dec({0x90, 99, 5});
	CHECK(e.type == Event::Pad && e.index == 63 && e.value == 5);
	e = dec({0x80, 99, 0});
	CHECK(e.type == Event::Pad && e.index == 63 && e.value == 0);
	e = dec({0x90, 36, 0});
	CHECK(e.type == Event::Pad && e.value == 0);

	e = dec({0xB0, push2::cc::Play, 127});
	CHECK(e.type == Event::Button && e.index == push2::cc::Play && e.value == 1);
	e = dec({0xB0, push2::cc::Play, 0});
	CHECK(e.type == Event::Button && e.value == 0);

	e = dec({0xB0, 71, 1});
	CHECK(e.type == Event::Encoder && e.index == 0 && e.value == 1);
	e = dec({0xB0, 78, 127});
	CHECK(e.type == Event::Encoder && e.index == 7 && e.value == -1);
	e = dec({0xB0, 14, 124});
	CHECK(e.type == Event::Encoder && e.index == push2::kTempoId && e.value == -4);

	e = dec({0x90, 3, 127});
	CHECK(e.type == Event::Touch && e.index == 3 && e.value == 1);
	e = dec({0x90, 3, 0});
	CHECK(e.type == Event::Touch && e.index == 3 && e.value == 0);
	e = dec({0x90, 12, 127});
	CHECK(e.type == Event::Touch && e.index == push2::kStrip && e.value == 1);

	e = dec({0xE0, 0x7F, 0x7F});
	CHECK(e.type == Event::Bend && e.value == 16383);
	e = dec({0xE0, 0, 0x40});
	CHECK(e.type == Event::Bend && e.value == 8192);

	CHECK(dec({0xA0, 36, 10}).type == Event::None);        // poly aftertouch
	CHECK(dec({0xD0, 10}).type == Event::None);            // channel pressure
	CHECK(dec({0xF0, 0, 0x21, 0xF7}).type == Event::None); // sysex
	CHECK(dec({0xF8}).type == Event::None);                // clock
	CHECK(dec({0xB0, 1, 5}).type == Event::None);          // mod wheel (touch strip's other mode)
	CHECK(dec({0x90, 36}).type == Event::None);            // truncated
	CHECK(dec({0xB0, 71, 0xFF}).type == Event::None);      // data byte with bit 7 set
}

TEST(push2_diff) {
	push2::Leds want, sent;
	uint8_t out[8][3];
	CHECK(push2::diff(want, sent, false, out, 8) == 0);

	want.note[36] = {127, 0};
	want.cc[85] = {126, 14};
	CHECK(push2::diff(want, sent, false, out, 8) == 2);
	CHECK(out[0][0] == 0x90 && out[0][1] == 36 && out[0][2] == 127);
	CHECK(out[1][0] == (0xB0 | 14) && out[1][1] == 85 && out[1][2] == 126);
	CHECK(push2::diff(want, sent, false, out, 8) == 0);

	want.cc[20] = {1, 0};
	want.cc[21] = {2, 0};
	CHECK(push2::diff(want, sent, false, out, 1) == 1 && out[0][1] == 20);
	CHECK(push2::diff(want, sent, false, out, 1) == 1 && out[0][1] == 21);
	CHECK(push2::diff(want, sent, false, out, 1) == 0);

	static uint8_t big[256][3];
	CHECK(push2::diff(want, sent, true, big, 256) == 256);
	CHECK(big[0][0] == 0x90 && big[128][0] == 0xB0);
}

TEST(push2_diff_base) {
	push2::Leds want, sent;
	uint8_t out[8][3];
	// An animation whose base is not what was last sent static: the base goes first, then the animation.
	want.note[40] = {push2::kBlack, push2::kPulseHalf, 60};
	CHECK(push2::diff(want, sent, false, out, 8) == 2);
	CHECK(out[0][0] == 0x90 && out[0][1] == 40 && out[0][2] == 60);
	CHECK(out[1][0] == (0x90 | push2::kPulseHalf) && out[1][1] == 40 && out[1][2] == push2::kBlack);
	CHECK(push2::diff(want, sent, false, out, 8) == 0);
	// The same base again (the colour changed): only the animation.
	want.note[40].color = 7;
	CHECK(push2::diff(want, sent, false, out, 8) == 1 && out[0][0] == (0x90 | push2::kPulseHalf) && out[0][2] == 7);
	// Another base: both again.
	want.note[40].base = 61;
	CHECK(push2::diff(want, sent, false, out, 8) == 2 && out[0][0] == 0x90 && out[0][2] == 61);
	// Back to static: one message, and the next animation from a base it sent before needs no base.
	want.note[40] = {61, 0};
	CHECK(push2::diff(want, sent, false, out, 8) == 1 && out[0][0] == 0x90 && out[0][2] == 61);
	want.note[40] = {9, push2::kBlinkQuarter, 61};
	CHECK(push2::diff(want, sent, false, out, 8) == 1 && out[0][0] == (0x90 | push2::kBlinkQuarter));
	// Both count toward max: with room for one, the base goes now and the animation next time.
	want.cc[30] = {5, push2::kPulseHalf, 3};
	CHECK(push2::diff(want, sent, false, out, 1) == 1 && out[0][0] == 0xB0 && out[0][2] == 3);
	CHECK(push2::diff(want, sent, false, out, 1) == 1 && out[0][0] == (0xB0 | push2::kPulseHalf) && out[0][2] == 5);
	CHECK(push2::diff(want, sent, false, out, 8) == 0);
	// All of it: an animation sends its base too, even when the base is what was sent.
	push2::Leds all, known = {};
	all.note[0] = {5, push2::kPulseHalf, 0};
	CHECK(push2::diff(all, known, true, out, 8) == 8);
	CHECK(out[0][0] == 0x90 && out[0][2] == 0 && out[1][0] == (0x90 | push2::kPulseHalf) && out[1][2] == 5);
}

TEST(push2_to_frame) {
	std::vector<uint8_t> rgba((size_t)push2::kWidth * push2::kHeight * 4, 0);
	std::vector<uint8_t> frame(push2::kFrameBytes);
	// Red at the top-left of the image = the first pixel of the last row of rgba; white next to it.
	size_t top = (size_t)(push2::kHeight - 1) * push2::kWidth * 4;
	rgba[top] = 255, rgba[top + 3] = 255;
	for (int i = 4; i < 8; i++)
		rgba[top + i] = 255;
	push2::toFrame(rgba.data(), frame.data());
	// red = 0x001F little endian 1F 00, white = FF FF; XOR E7 F3 / E7 FF
	CHECK(frame[0] == (0x1F ^ 0xE7) && frame[1] == (0x00 ^ 0xF3));
	CHECK(frame[2] == (0xFF ^ 0xE7) && frame[3] == (0xFF ^ 0xFF));
	// black pixel: just the pattern; filler byte (zero before the XOR) too, from the start of the line's pattern
	CHECK(frame[4] == 0xE7 && frame[5] == 0xF3);
	CHECK(frame[1920] == 0xE7 && frame[1921] == 0xF3 && frame[1922] == 0xE7 && frame[1923] == 0xFF);
	CHECK(frame[2047] == 0xFF);
	// the second line starts the pattern again
	CHECK(frame[push2::kLineBytes] == 0xE7);
}

namespace {

/** The module's process() loop around a Machine: pulses counted per jack, events fed once. */
struct Rig {
	tart::Machine m;
	tart::Inputs in;
	tart::Outputs out;
	push2::Leds want;
	int clk = 0, midiClock = 0, midiStart = 0, reset = 0, metro = 0;
	std::vector<float> metroV;
	bool run = false;
	tart::SliceFrame* slice = nullptr;

	/** Most tests play in Note mode; Session is the default of a new module. */
	explicit Rig(bool note = true) {
		in.sampleRate = 48000;
		if (note)
			m.edit([](tart::State& st) { st.s.ui.pads = tart::kNote; });
	}
	void step(const push2::Event* ev = nullptr, int n = 1) {
		in.events = ev, in.nEvents = ev ? n : 0;
		m.process(in, out, want, slice);
		in.frame++, in.play = in.stop = in.reset = in.clockEdge = false;
		clk += out.clk, midiClock += out.midiClock, midiStart += out.midiStart, reset += out.reset;
		if (out.metro)
			metro++, metroV.push_back(out.metroV);
		run = out.run;
	}
	void run_(int n) {
		while (n-- > 0)
			step();
	}
	void press(int cc, int value = 127) {
		push2::Event e = dec({0xB0, (uint8_t)cc, (uint8_t)value});
		step(&e);
	}
	void turn(int id, int delta) { // the encoder CC for the id, delta as the Push sends it
		uint8_t cc = id < 8                  ? (uint8_t)(push2::cc::kTrack1 + id)
					 : id == push2::kTempoId ? push2::cc::kTempo
											 : push2::cc::kSwing;
		push2::Event e = dec({0xB0, cc, (uint8_t)(delta & 127)});
		step(&e);
	}
	void pad(int row, int col, int vel) {
		push2::Event e = dec({vel ? (uint8_t)0x90 : (uint8_t)0x80, push2::padNote(row, col), (uint8_t)vel});
		step(&e);
	}
	tart::View view() {
		tart::View v;
		m.view(v);
		return v;
	}
	void tap(int row, int col) {
		pad(row, col, 100);
		pad(row, col, 0);
	}
	void click(int cc) { // a button pressed and let go at once
		press(cc);
		press(cc, 0);
	}
	std::unique_ptr<tart::State> state() {
		auto st = std::make_unique<tart::State>();
		m.save(*st);
		return st;
	}
};

/** A Rig with a Slice on its right: `f` is the frame a Slice would read. */
struct SliceRig : Rig {
	tart::SliceFrame f = {};
	explicit SliceRig(bool note = true) : Rig(note) { slice = &f; }
};

} // namespace

TEST(tart_clock_internal) {
	Rig r;
	r.in.play = true;
	r.in.ppqnOut = 4;
	r.step();
	CHECK(r.out.clk && r.out.reset && r.out.midiStart && r.out.midiClock); // the downbeat, on the first sample
	r.run_(48000);                                                         // 1 s at 120 BPM and 48 kHz, inclusive
	CHECK(r.clk == 9 && r.midiClock == 49 && r.midiStart == 1 && r.reset == 1 && r.run);
	CHECK(r.view().s.transport.playing);
}

TEST(tart_tempo_cv) {
	Rig r;
	r.in.play = true;
	r.in.tempoCv = 1;
	r.run_(48001);
	CHECK(r.clk == 17); // 240 BPM: 16 sixteenths in a second, plus the downbeat
	Rig s;
	s.in.play = true;
	s.in.tempoCv = -10;
	s.run_(48001);
	CHECK_NEAR(s.view().s.transport.shownBpm, 20, 0.01); // clamped
	CHECK(s.clk == 2);                                   // 20 BPM: 1 per 0.75 s
}

TEST(tart_external_clock) {
	Rig r;
	r.in.clockConnected = true;
	r.in.play = true;
	r.run_(100);                  // past the 1 ms the clock ignores edges after a start
	for (int n = 0; n < 8; n++) { // 8 pulses at PPQN 4 and 120 BPM: 6000 samples apart
		r.in.clockEdge = true;
		r.run_(1);
		r.run_(5999);
	}
	r.run_(1);
	CHECK(r.view().s.transport.tick / 24 == 8); // 8 pulses = 192 ticks; the clock then runs on towards the ninth
	CHECK(r.view().s.transport.external);
}

TEST(tart_metro) {
	Rig r;
	r.run_(100000); // stopped
	CHECK(r.metro == 0);
	r.press(push2::cc::Metronome);
	r.press(push2::cc::Metronome, 0);
	CHECK(r.view().s.transport.metronome);
	r.in.play = true;
	r.run_(4 * 24000 + 10); // 4 beats at 120 BPM, then the next downbeat
	CHECK(r.metroV.size() == 5);
	const float want[5] = {10, 5, 5, 5, 10};
	for (size_t i = 0; i < r.metroV.size() && i < 5; i++)
		CHECK(r.metroV[i] == want[i]);
	r.press(push2::cc::Metronome);
	r.press(push2::cc::Metronome, 0);
	int n = r.metro;
	r.run_(24000 * 2);
	CHECK(r.metro == n); // off
	CHECK(r.want.cc[push2::cc::Metronome].channel == push2::kStatic &&
		  r.want.cc[push2::cc::Metronome].color == push2::kWhiteFull);
}

TEST(tart_tempo_encoder) {
	Rig r;
	r.turn(push2::kTempoId, 1);
	r.turn(push2::kTempoId, 1);
	r.turn(push2::kTempoId, 1);
	CHECK_NEAR(r.view().s.transport.bpm, 123, 0.001);
	CHECK(r.view().s.ui.popup == tart::kTempoPopup);
	r.press(push2::cc::Shift);
	r.turn(push2::kTempoId, 1);
	CHECK_NEAR(r.view().s.transport.bpm, 123.1, 0.001);
	r.press(push2::cc::Shift, 0);
	r.turn(push2::kTempoId, -1);
	CHECK_NEAR(r.view().s.transport.bpm, 122.1, 0.001);
	for (int i = 0; i < 400; i++)
		r.turn(push2::kTempoId, 1);
	CHECK_NEAR(r.view().s.transport.bpm, 300, 0.001);
	r.run_(1);
	CHECK(r.view().s.transport.shownBpm > 0);
}

TEST(tart_swing_and_popup) {
	Rig r;
	for (int i = 0; i < 6; i++)
		r.turn(push2::kSwingId, 1);
	CHECK(r.view().s.transport.swing == 3 && r.view().s.ui.popup == tart::kSwingPopup);
	for (int i = 0; i < 300; i++)
		r.turn(push2::kSwingId, 1);
	CHECK(r.view().s.transport.swing == 100);
}

TEST(tart_tap_tempo) {
	Rig r;
	for (int i = 0; i < 4; i++) {
		r.press(push2::cc::TapTempo);
		r.press(push2::cc::TapTempo, 0);
		r.run_(23998);
	}
	CHECK_NEAR(r.view().s.transport.bpm, 120, 0.1);
	CHECK(!r.view().s.transport.playing); // never starts the transport
	r.run_(3 * 48000);
	r.press(push2::cc::TapTempo);
	r.press(push2::cc::TapTempo, 0);
	CHECK_NEAR(r.view().s.transport.bpm, 120, 0.1); // a lone tap after a gap changes nothing
	r.run_(36000 - 2);
	r.press(push2::cc::TapTempo); // 0.75 s after the lone tap: 80 BPM
	CHECK_NEAR(r.view().s.transport.bpm, 80, 0.1);
}

TEST(tart_play_reset) {
	Rig r;
	r.press(push2::cc::Play);
	r.press(push2::cc::Play, 0);
	r.run_(2);
	CHECK(r.run && r.view().s.transport.playing && r.reset == 1 && r.midiStart == 1);
	r.run_(30000);
	CHECK(r.view().s.transport.tick > 90);
	r.in.reset = true;
	r.step();
	CHECK(r.reset == 2 && r.view().s.transport.tick == 0);
	CHECK(r.want.cc[push2::cc::Play].color == push2::kGreen);
	r.press(push2::cc::Play); // toggles: stop
	r.press(push2::cc::Play, 0);
	r.run_(2);
	CHECK(!r.run && !r.view().s.transport.playing);
	CHECK(r.want.cc[push2::cc::Play].color == push2::kRgbWhite);
	int clocks = r.midiClock;
	r.run_(48000); // the Push keeps its clock while stopped
	CHECK(r.midiClock - clocks >= 47);
	r.in.play = true; // the jack starts from the beginning
	r.step();
	CHECK(r.run && r.reset == 3);
}

TEST(tart_leds) {
	Rig r;
	r.in.leds = true;
	r.step();
	CHECK(r.out.ledsDone);
	CHECK(r.want.cc[push2::cc::Shift].color == push2::kWhiteDark &&
		  r.want.cc[push2::cc::TapTempo].color == push2::kWhiteDark);
	CHECK(r.want.cc[push2::cc::Undo].color == 0 && r.want.cc[push2::cc::Session].color == push2::kWhiteDark);
	r.press(push2::cc::Shift);
	CHECK(r.want.cc[push2::cc::Shift].color == push2::kWhiteFull);
	r.press(push2::cc::Metronome);
	CHECK(r.want.cc[push2::cc::Metronome].channel == push2::kPulse24th);
}

TEST(sanitize_transport) {
	auto st = std::make_unique<tart::State>();
	tart::Transport& t = st->s.transport;
	t.bpm = std::nanf("");
	t.swing = 200, t.quant = 99, t.playing = true;
	st->s.ui.track = 9, st->s.ui.popup = 1;
	tart::sanitize(*st);
	CHECK(t.bpm == 120 && t.swing == 100 && t.quant == 13 && !t.playing && st->s.ui.track == 7 && !st->s.ui.popup);
	t.bpm = 500;
	tart::sanitize(*st);
	CHECK(t.bpm == 300);
	t.bpm = 5;
	tart::sanitize(*st);
	CHECK(t.bpm == 20);
}

TEST(tart_machine_threads) {
	tart::Machine m;
	std::atomic<bool> done{false};
	std::thread audio([&] {
		tart::Inputs in;
		tart::Outputs out;
		push2::Leds want;
		push2::Event ev[56];
		ev[0] = dec({0xB0, push2::cc::kTempo, 1});
		ev[1] = dec({0xB0, push2::cc::Metronome, 127});
		ev[2] = dec({0x90, 36, 100}); // a pad and the touch strip
		ev[3] = dec({0xE0, 0, 0x60});
		ev[4] = dec({0xB0, push2::cc::Delete, 127}); // Delete on a slot the UI thread fills: a checkpoint
		ev[5] = dec({0x90, 36, 90});
		ev[6] = dec({0xB0, push2::cc::Delete, 0});
		ev[7] = dec({0xB0, push2::cc::Undo, 127}); // Undo, then Shift + Undo
		ev[8] = dec({0xB0, push2::cc::Undo, 0});
		ev[9] = dec({0xB0, push2::cc::Shift, 127});
		ev[10] = dec({0xB0, push2::cc::Undo, 127});
		ev[11] = dec({0xB0, push2::cc::Undo, 0});
		ev[12] = dec({0xB0, push2::cc::Shift, 0});
		ev[13] = dec({0xB0, push2::cc::Record, 127}); // a take on whichever slot is selected, pads recorded in it
		ev[14] = dec({0xB0, push2::cc::Record, 0});
		ev[15] = dec({0xB0, push2::cc::Device, 127}); // the Device screen, a turn and a touch
		ev[16] = dec({0xB0, 71, 3});
		ev[17] = dec({0x90, 0, 127});
		ev[18] = dec({0xB0, push2::cc::Device, 0});
		ev[19] = dec({0xB0, push2::cc::Automate, 127}); // Automate: the touch and the turns of the take write a lane
		ev[20] = dec({0xB0, push2::cc::Automate, 0});
		ev[21] = dec({0xB0, 71, 40});
		ev[22] = dec({0x90, 0, 0});
		ev[23] = dec({0xB0, push2::cc::Mute, 127}); // MUTE and SOLO taps, a lower button while one is held
		ev[24] = dec({0xB0, push2::cc::Mute, 0});
		ev[25] = dec({0xB0, push2::cc::Solo, 127});
		ev[26] = dec({0xB0, push2::cc::Track1Below + 2, 127});
		ev[27] = dec({0xB0, push2::cc::Solo, 0});
		ev[28] = dec({0xB0, push2::cc::Mix, 127}); // the Mix screen and a turn of track 2's LEVEL
		ev[29] = dec({0xB0, 72, 3});
		ev[30] = dec({0xB0, push2::cc::Mix, 0});
		ev[31] = dec({0xB0, push2::cc::Track1Below + 1, 127}); // select track 2, whose type the UI thread keeps
		ev[32] = dec({0xB0, push2::cc::Track1Below + 1, 0});   // changing: the Euclid screen's encoders (a turn, a
		ev[33] = dec({0xB0, 71, 8});                           // touch, a turn), MELODIC, a scene button, then track 1
		ev[34] = dec({0x90, 1, 127});
		ev[35] = dec({0xB0, 72, 8});
		ev[36] = dec({0x90, 1, 0});
		ev[37] = dec({0xB0, push2::cc::Track1Above + 1, 127});
		ev[38] = dec({0xB0, push2::cc::Track1Above + 1, 0});
		ev[39] = dec({0xB0, push2::cc::Scene8 + 2, 127});
		ev[40] = dec({0xB0, push2::cc::Scene8 + 2, 0});
		ev[41] = dec({0xB0, push2::cc::Note, 127}); // a step pad held on track 2 on the Device screen: a turn makes a
		ev[42] = dec({0xB0, push2::cc::Note, 0});   // lock, a DELETE + touch takes it off, DELETE + the pad again
		ev[43] = dec({0xB0, push2::cc::Device, 127});
		ev[44] = dec({0xB0, push2::cc::Device, 0});
		ev[45] = dec({0x90, push2::padNote(7, 3), 100});
		ev[46] = dec({0xB0, 71, 5});
		ev[47] = dec({0xB0, push2::cc::Delete, 127});
		ev[48] = dec({0x90, 0, 127});
		ev[49] = dec({0x90, 0, 0});
		ev[50] = dec({0x80, push2::padNote(7, 3), 0});
		ev[51] = dec({0x90, push2::padNote(7, 3), 100});
		ev[52] = dec({0x80, push2::padNote(7, 3), 0});
		ev[53] = dec({0xB0, push2::cc::Delete, 0});
		ev[54] = dec({0xB0, push2::cc::Track1Below, 127});
		ev[55] = dec({0xB0, push2::cc::Track1Below, 0});
		tart::SliceFrame frame;
		in.play = true;
		for (int i = 0; i < 20000; i++) {
			in.events = ev, in.nEvents = i % 7 == 0 ? 56 : 0, in.frame = i;
			m.process(in, out, want, &frame);
			in.play = false;
		}
		done = true;
	});
	int n = 0;
	auto v = std::make_unique<tart::View>();
	while (!done) {
		m.view(*v);
		m.edit([&](tart::State& st) {
			st.s.transport.swing = (uint8_t)(n % 100), st.s.track[0].voices = (uint8_t)(n % 16 + 1);
			st.clip[0][7].length = (uint16_t)(n % 3 * tart::kBar);
			tart::Clip& e = st.clip[1][0]; // track 2 plays a Euclid clip whose settings and type the UI keeps changing
			e.length = tart::kBar, e.euclid.lane[0].pulses = (uint8_t)(n % 9), e.euclid.mode = (uint8_t)(n / 5 % 2);
			st.s.track[1].playing = 0;
			st.s.track[1].type = (uint8_t)(n++ / 7 % 3);
		});
	}
	audio.join();
	// The loop above may never get the lock (the UI side here locks back to back, Rack's at most 60 times a second):
	// with the UI thread done, one more sample takes it for sure.
	tart::Inputs in;
	tart::Outputs out;
	push2::Leds want;
	tart::SliceFrame frame;
	push2::Event tempo = dec({0xB0, push2::cc::kTempo, 1});
	in.events = &tempo, in.nEvents = 1, in.frame = 20000;
	m.process(in, out, want, &frame);
	CHECK(out.consumed);
	m.view(*v);
	CHECK(v->s.transport.playing && v->s.transport.bpm > 120);
	auto st = std::make_unique<tart::State>(); // whatever the takes wrote is sorted and fits
	m.save(*st);
	for (auto& row : st->clip)
		for (const tart::Clip& c : row)
			for (int j = 0; j < tart::kAutos; j++) {
				CHECK(c.nPoints[j] <= tart::kMaxPoints);
				for (int k = 1; k < c.nPoints[j]; k++)
					CHECK(c.points[j][k - 1].tick <= c.points[j][k].tick);
			}
	CHECK(st->s.ui.recTrack >= 0 || st->s.ui.writing == 0);
}

TEST(slice_bus_previous_frame) {
	tart::SliceBus bus;
	CHECK(bus.write(5) == nullptr); // nobody reads yet: nothing is filled
	auto put = [&](int64_t n, float v) {
		tart::SliceFrame* f = bus.write(n);
		CHECK(f);
		if (f)
			f->frame = (uint32_t)n, f->track[3].level = v;
	};
	bus.read(0);
	put(1, 1.f);
	const tart::SliceFrame* f = bus.read(2); // the frame written at 1
	CHECK(f && f->frame == 1 && f->track[3].level == 1.f);
	put(2, 2.f);
	f = bus.read(3);
	CHECK(f && f->frame == 2 && f->track[3].level == 2.f);
	CHECK(bus.read(4) == nullptr && bus.read(5) == nullptr); // frames 3 and 4 were never written
	CHECK(bus.write(6) && bus.write(1000) == nullptr);       // a Slice that stopped reading: the Tart stops filling
	CHECK(bus.read(6) == nullptr);                           // the writer skipped 3..5 (bypassed): stale
	put(7, 7.f);
	f = bus.read(8);
	CHECK(f && f->frame == 7 && f->track[3].level == 7.f);
}

TEST(slice_bus_threads) { // make tsan: a writer and a reader, a barrier per sample as Rack's engine has
	tart::SliceBus bus;
	std::atomic<int> arrived{0};
	std::atomic<int> bad{0}, seen{0};
	auto barrier = [&](int n) {
		arrived.fetch_add(1, std::memory_order_acq_rel);
		while (arrived.load(std::memory_order_acquire) < 2 * (n + 1))
			std::this_thread::yield();
	};
	const int N = 10000;
	std::thread writer([&] {
		for (int n = 1; n <= N; n++) {
			if (tart::SliceFrame* f = bus.write(n)) {
				f->frame = (uint32_t)n;
				float sum = 0;
				for (int i = 0; i < 16; i++)
					sum += f->track[0].voct[i] = (float)(n + i);
				f->track[7].pan = sum;
			}
			barrier(n - 1);
		}
	});
	for (int n = 1; n <= N; n++) {
		if (const tart::SliceFrame* f = bus.read(n)) {
			float sum = 0;
			for (int i = 0; i < 16; i++)
				sum += f->track[0].voct[i];
			bad += f->frame != (uint32_t)(n - 1) || f->track[7].pan != sum || f->track[0].voct[0] != (float)(n - 1);
			seen++;
		}
		barrier(n - 1);
	}
	writer.join();
	CHECK(bad == 0 && seen > N - 10);
}

TEST(state_layout) {
	CHECK(std::is_trivially_copyable<tart::State>::value);
	CHECK(tart::kMaxNotes == 4096);
	CHECK(sizeof(tart::State) < 3 * 1024 * 1024); // heap only: ~2.34 MB
	CHECK(sizeof(tart::Machine) < 16 * 1024);     // ~14.4 KB: from_/to_ add 640 bytes
	CHECK(sizeof(tart::Clip) < 36 * 1024 &&
		  tart::kAutos == 10); // ~36.5 KB: 8 CCs, LEVEL, PAN, Euclid (92 bytes), locks
	CHECK(tart::kMaxLocks == 256 && sizeof(tart::Lock) == 6);
	CHECK(sizeof(tart::SliceFrame) < 2048);
	CHECK(sizeof(tart::State) > 2 * 1024 * 1024);
	auto st = std::make_unique<tart::State>();
	CHECK(std::string(st->s.track[0].name) == "Track 1" && std::string(st->s.track[7].name) == "Track 8");
	CHECK(st->s.track[0].voices == 1 && st->s.track[0].octave == 3 && st->s.track[0].playing == tart::kNone);
	CHECK(st->clip[7][7].length == 0 && st->clip[0][0].nNotes == 0);
}

TEST(pad_pitch) {
	bool root, in;
	tart::Scale c; // C Major, In Key, 4ths up
	CHECK(tart::padPitch(c, 3, 0, 0, root, in) == 36 && root && in);
	CHECK(tart::padPitch(c, 3, 0, 1, root, in) == 38 && !root && in);
	CHECK(tart::padPitch(c, 3, 1, 0, root, in) == 41);
	CHECK(tart::padPitch(c, 3, 0, 7, root, in) == 36 + 12 && root);     // the octave
	CHECK(tart::padPitch(c, 3, 7, 7, root, in) == 36 + 12 * 4 && root); // degree 28: four octaves up
	c.layout = 1;                                                       // 4ths, right: rows and columns trade places
	CHECK(tart::padPitch(c, 3, 0, 1, root, in) == 41 && tart::padPitch(c, 3, 1, 0, root, in) == 38);
	c.layout = 2; // 3rds up: a row is two scale steps
	CHECK(tart::padPitch(c, 3, 1, 0, root, in) == 40 && tart::padPitch(c, 3, 0, 1, root, in) == 38);
	c.layout = 4;                                               // Sequential: a row is eight scale steps
	CHECK(tart::padPitch(c, 3, 1, 0, root, in) == 50 && !root); // eight steps: an octave and a degree, D
	c.layout = 0;
	c.root = 2; // D Major: the bottom-left pad is D, the root
	CHECK(tart::padPitch(c, 3, 0, 0, root, in) == 38 && root);
	c.fixed = true;  // Fixed: the pads stay where they were
	c.inKey = false; // Chromatic, so the root doesn't move the layout either
	CHECK(tart::padPitch(c, 3, 0, 0, root, in) == 36 && !root && !in);
	CHECK(tart::padPitch(c, 3, 0, 2, root, in) == 38 && root && in);
	c = tart::Scale();
	c.inKey = false; // Chromatic 4ths: a row is five semitones, out-of-key pads are not in the scale
	CHECK(tart::padPitch(c, 3, 1, 0, root, in) == 41 && in);
	CHECK(tart::padPitch(c, 3, 0, 1, root, in) == 37 && !in && !root);
	CHECK(tart::padPitch(c, 10, 7, 7, root, in) == -1); // above 127
	CHECK(tart::padPitch(c, 10, 0, 7, root, in) == 127);
}

TEST(live_notes) {
	SliceRig r;
	r.run_(2);
	CHECK(r.f.track[0].gate[0] == 0 && r.f.track[0].channels == 1);
	r.pad(0, 0, 100); // C1
	CHECK(r.f.track[0].gate[0] == 10 && r.f.track[0].voct[0] == -2.f);
	CHECK_NEAR(r.f.track[0].vel[0], 100 / 127.f * 10, 1e-4);
	CHECK(r.f.track[1].gate[0] == 0);
	CHECK(r.view().s.ui.pad[0] == 100);
	CHECK(r.want.note[push2::padNote(0, 0)].color == push2::kGreen); // pressed
	r.in.leds = true;
	r.step();
	CHECK(r.want.note[push2::padNote(0, 0)].color == push2::kGreen);
	r.pad(0, 3, 100); // F: pad (1, 0) has the same note and lights up too
	CHECK(r.want.note[push2::padNote(1, 0)].color == push2::kGreen);
	r.pad(0, 3, 0);
	CHECK(r.want.note[push2::padNote(1, 0)].color == push2::kRgbWhite);
	r.pad(0, 0, 0);
	CHECK(r.f.track[0].gate[0] == 0);
	CHECK(r.view().s.ui.pad[0] == 0);
}

TEST(live_notes_voices_and_tracks) {
	SliceRig r;
	r.m.edit([](tart::State& st) { st.s.track[0].voices = 3; });
	r.run_(2);
	r.pad(0, 0, 100);
	r.pad(0, 1, 90);
	r.pad(0, 2, 80);
	CHECK(r.f.track[0].channels == 3);
	int on = 0;
	for (int c = 0; c < 3; c++)
		on += r.f.track[0].gate[c] > 0;
	CHECK(on == 3);
	r.pad(0, 2, 0);
	r.pad(0, 1, 0);
	// A pad held across a track switch keeps sounding on its old track and stops there.
	r.press(push2::cc::Track1Below + 3);
	CHECK(r.view().s.ui.track == 3);
	CHECK(r.f.track[0].gate[0] == 10 && r.f.track[3].gate[0] == 0);
	r.pad(1, 0, 70); // a new pad goes to the new track
	CHECK(r.f.track[3].gate[0] == 10 && r.f.track[0].gate[0] == 10);
	r.pad(0, 0, 0);
	CHECK(r.f.track[0].gate[0] == 0 && r.f.track[3].gate[0] == 10);
	r.pad(1, 0, 0);
	CHECK(r.f.track[3].gate[0] == 0);
	// Pads above 127 are silent.
	r.m.edit([](tart::State& st) { st.s.track[3].octave = 10; });
	r.run_(1);
	r.pad(7, 7, 100);
	CHECK(r.f.track[3].gate[0] == 0 && r.view().s.ui.pad[63] == 0);
}

TEST(track_buttons_and_octave) {
	Rig r;
	r.in.leds = true;
	r.step();
	CHECK(r.want.cc[push2::cc::Track1Below].color == push2::kRgbWhite &&
		  r.want.cc[push2::cc::Track1Below + 1].color == tart::kPaletteFull + 1); // the others in their colour
	r.press(push2::cc::Track1Below + 5);
	r.press(push2::cc::Track1Below + 5, 0);
	CHECK(r.view().s.ui.track == 5);
	CHECK(r.want.cc[push2::cc::Track1Below + 5].color == push2::kRgbWhite &&
		  r.want.cc[push2::cc::Track1Below].color == tart::kPaletteFull);
	r.press(push2::cc::Left);
	CHECK(r.view().s.ui.track == 4);
	for (int i = 0; i < 10; i++)
		r.press(push2::cc::Right);
	CHECK(r.view().s.ui.track == 7);
	for (int i = 0; i < 10; i++)
		r.press(push2::cc::Left);
	CHECK(r.view().s.ui.track == 0);
	r.press(push2::cc::OctaveUp);
	CHECK(r.view().s.track[0].octave == 4 && r.view().s.track[1].octave == 3);
	for (int i = 0; i < 20; i++)
		r.press(push2::cc::OctaveUp);
	CHECK(r.view().s.track[0].octave == tart::kMaxOctave && r.want.cc[push2::cc::OctaveUp].color == 0);
	CHECK(r.want.cc[push2::cc::OctaveDown].color == push2::kWhiteDark);
	for (int i = 0; i < 20; i++)
		r.press(push2::cc::OctaveDown);
	CHECK(r.view().s.track[0].octave == 0 && r.want.cc[push2::cc::OctaveDown].color == 0);
	// pad LEDs: the root (C) is lit in the track's colour (selected track 0), other in-key notes white
	CHECK(r.want.note[push2::padNote(0, 0)].color == tart::kPaletteFull &&
		  r.want.note[push2::padNote(0, 1)].color == push2::kRgbWhite);
	CHECK(r.want.cc[push2::cc::Note].color == push2::kWhiteFull &&
		  r.want.cc[push2::cc::Session].color == push2::kWhiteDark);
}

TEST(bend) {
	SliceRig r;
	r.run_(2);
	r.pad(0, 0, 100);
	r.press(push2::cc::Track1Below + 1);
	r.pad(1, 0, 100);
	push2::Event e = dec({0xE0, 16320 & 127, (uint8_t)(16320 >> 7)});
	r.step(&e);
	CHECK_NEAR(r.view().s.ui.bend, 0.9922, 0.001);
	CHECK_NEAR(r.f.track[1].voct[0], (41 - 60) / 12.f + 2 / 12.f, 0.003); // the selected track bends
	CHECK_NEAR(r.f.track[0].voct[0], (36 - 60) / 12.f, 1e-5);             // the other does not
	e = dec({0xE0, 0, 0x40});                                             // 8192: the strip is back in the middle
	r.step(&e);
	CHECK_NEAR(r.f.track[1].voct[0], (41 - 60) / 12.f, 1e-5);
	e = dec({0xE0, 0, 0});
	r.step(&e);
	CHECK_NEAR(r.f.track[1].voct[0], (41 - 60) / 12.f - 2 / 12.f, 1e-4); // down
	e = dec({0x90, 12, 0});                                              // touch released: bend resets
	r.step(&e);
	CHECK_NEAR(r.f.track[1].voct[0], (41 - 60) / 12.f, 1e-5);
}

TEST(slice_frame) {
	SliceRig r;
	r.m.edit([](tart::State& st) { st.s.track[2].voices = 5, st.s.track[2].color = 3; }); // EDDA3C
	r.run_(2);
	for (int t = 0; t < tart::kTracks; t++) {
		const tart::SliceTrack& s = r.f.track[t];
		CHECK(s.level == 10 && s.pan == 0);
		for (float cc : s.cc)
			CHECK(cc == 0);
		CHECK(s.channels == (t == 2 ? 5 : 1));
	}
	CHECK(r.f.track[2].rgb == (0xEDu << 16 | 0xDAu << 8 | 0x3Cu));
	uint32_t before = r.f.frame;
	r.run_(1);
	CHECK(r.f.frame != before); // changes every sample
}

TEST(sanitize_tracks) {
	auto st = std::make_unique<tart::State>();
	tart::Track& t = st->s.track[1];
	std::memset(t.name, 'a', tart::kNameBytes);       // no terminator
	t.name[30] = (char)0xC3, t.name[31] = (char)0xA9; // an 'e acute' cut by the field's end
	t.color = 200, t.voices = 0, t.octave = 99, t.grid = 9, t.type = 4, t.playing = 3, t.queued = 2;
	st->s.track[2].voices = 40, st->s.track[2].octave = -3;
	st->s.scale.root = 12, st->s.scale.scale = 99, st->s.scale.layout = 6;
	st->s.ui.pads = 5, st->s.ui.fixedBars = 9, st->s.ui.scene = 20, st->s.ui.bend = 1, st->s.ui.pad[3] = 9;
	tart::sanitize(*st);
	CHECK(std::strlen(t.name) == 30 && t.name[29] == 'a'); // cut before the broken character
	CHECK(t.color == 69 && t.voices == 1 && t.octave == tart::kMaxOctave && t.grid == 7 && t.type == tart::Melodic);
	st->s.track[3].type = tart::Drum, st->s.track[3].lane = 99, st->s.track[4].type = tart::Euclidean;
	st->s.ui.browse = 5, st->s.ui.screen = tart::kBrowseScreen;
	tart::sanitize(*st);
	CHECK(st->s.track[3].type == tart::Drum && st->s.track[3].lane == tart::kLanes - 1);
	CHECK(st->s.track[4].type == tart::Euclidean && st->s.ui.browse == 0 && st->s.ui.screen == tart::kHome);
	CHECK(t.playing == tart::kNone && t.queued == tart::kNone);
	CHECK(st->s.track[2].voices == 16 && st->s.track[2].octave == 0);
	CHECK(st->s.scale.root == 11 && st->s.scale.scale == 0 && st->s.scale.layout == 5);
	CHECK(st->s.ui.pads == tart::kNote && st->s.ui.fixedBars == 7 && st->s.ui.scene == 7 && st->s.ui.bend == 0 &&
		  st->s.ui.pad[3] == 0);
	std::string long_name = "Bass";
	for (int i = 0; i < 20; i++)
		long_name += "\xC3\xA9";
	tart::setName(st->s.track[0], long_name.c_str());
	CHECK(std::strlen(st->s.track[0].name) == 30); // 4 + 13 two-byte characters would be 30 bytes, the 14th doesn't fit
}

TEST(load_silences_notes) {
	SliceRig r;
	r.run_(2);
	r.pad(0, 0, 100);
	CHECK(r.f.track[0].gate[0] == 10);
	auto st = std::make_unique<tart::State>();
	r.m.save(*st);
	tart::sanitize(*st);
	r.m.load(*st);
	r.run_(2);
	CHECK(r.f.track[0].gate[0] == 0);
	r.pad(0, 0, 0); // the stale release does nothing
	CHECK(r.f.track[0].gate[0] == 0);
}

TEST(palette_bytes) {
	uint8_t m[17];
	push2::paletteEntry(125, 0, 0, 255, 126, m); // the manual's example
	const uint8_t want[] = {0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x03, 0x7D, 0x00,
							0x00, 0x00, 0x00, 0x7F, 0x01, 0x7E, 0x00, 0xF7};
	CHECK(std::memcmp(m, want, 17) == 0);
	push2::paletteEntry(100, 0xED, 0x59, 0x38, 0, m); // high bit of r, then the low 7 bits
	CHECK(m[7] == 100 && m[8] == 0x6D && m[9] == 1 && m[10] == 0x59 && m[11] == 0 && m[12] == 0x38 && m[16] == 0xF7);
	const uint8_t reapply[] = {0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x05, 0xF7};
	CHECK(sizeof push2::kReapplyPalette == 8 && std::memcmp(push2::kReapplyPalette, reapply, 8) == 0);
	// the slots stay off the white LEDs' indices and the fixed colours
	for (int k = 0; k < 16; k++) {
		int i = tart::kPaletteFull + k;
		CHECK(i != push2::kWhiteDark && i != push2::kWhiteLight && i != push2::kWhiteFull && i < push2::kRgbWhite);
	}
}

TEST(track_colors) {
	auto st = std::make_unique<tart::State>();
	for (int i = 0; i < tart::kTracks; i++)
		for (int j = 0; j < i; j++) {
			const uint8_t *a = tart::kLiveColors[st->s.track[i].color], *b = tart::kLiveColors[st->s.track[j].color];
			CHECK(a[0] != b[0] || a[1] != b[1] || a[2] != b[2]); // the 8 defaults differ
		}
	CHECK(tart::kLiveColors[69][0] == 0x58 && tart::kLiveColors[12][2] == 0x32);
}

TEST(palette_on_change) {
	Rig r;
	r.in.leds = true;
	r.step();
	CHECK(r.out.palette && r.out.ledsDone); // the first pass: the Push has no colours yet
	CHECK(r.out.rgb[0] == 0xFF1E32 &&
		  r.out.rgb[tart::kTracks] == (0x3Fu << 16 | 0x07u << 8 | 0x0Cu)); // track 1, full and a quarter
	r.in.leds = false;
	r.step();
	CHECK(!r.out.palette);
	r.m.edit([](tart::State& st) { st.s.track[2].color = 0; }); // from the UI thread
	r.step();
	CHECK(r.out.palette && r.out.rgb[2] == 0xED5938 &&
		  r.out.rgb[tart::kTracks + 2] == (0x3Bu << 16 | 0x16u << 8 | 0x0Eu));
	r.step();
	CHECK(!r.out.palette);
	CHECK(r.want.cc[push2::cc::Track1Below + 2].color == tart::kPaletteFull + 2); // LED indices stay put
	auto st = std::make_unique<tart::State>();
	r.m.save(*st);
	st->s.track[0].color = 99; // a bad patch is clamped by sanitize
	tart::sanitize(*st);
	CHECK(st->s.track[0].color == tart::kColors - 1);
}

TEST(scale_layouts) {
	bool root, in;
	tart::Scale c;
	c.scale = 12; // Major Pentatonic: C D E G A
	CHECK(std::string(tart::kScales[12].name) == "Major Pentatonic");
	CHECK(tart::padPitch(c, 3, 0, 4, root, in) == 45 && tart::padPitch(c, 3, 0, 5, root, in) == 48 && root);
	CHECK(tart::padPitch(c, 3, 1, 0, root, in) == 43); // 4ths: three steps up, G
	c.scale = 1;                                       // D Minor In Key walks from the root
	c.root = 2;
	CHECK(tart::padPitch(c, 3, 0, 0, root, in) == 38 && root);
	CHECK(tart::padPitch(c, 3, 0, 2, root, in) == 41 && !root && in); // F, the third
	// Fixed + In Key: the bottom-left pad stays C (or the first note of the scale above it), the root moves the lights
	c = tart::Scale();
	c.fixed = true;
	c.root = 2;                                                       // D Major: D E F# G A B C#
	CHECK(tart::padPitch(c, 3, 0, 0, root, in) == 37 && !root && in); // C#
	CHECK(tart::padPitch(c, 3, 0, 1, root, in) == 38 && root && in);  // D
	CHECK(tart::padPitch(c, 3, 0, 7, root, in) == 37 + 12);
	c.root = 7; // G Major: G A B C D E F#; C is in it, so the pad is C
	CHECK(tart::padPitch(c, 3, 0, 0, root, in) == 36 && !root);
	CHECK(tart::padPitch(c, 3, 0, 3, root, in) == 42); // C D E F#
	CHECK(tart::padPitch(c, 3, 0, 4, root, in) == 43 && root);
	c.root = 9, c.scale = 1; // A Minor: A B C D E F G
	CHECK(tart::padPitch(c, 3, 0, 0, root, in) == 36 && tart::padPitch(c, 3, 0, 5, root, in) == 45 && root);
	CHECK(tart::padPitch(c, 3, 0, 6, root, in) == 47);
	c.inKey = false; // Chromatic keeps C at the bottom left, A lights as the root
	CHECK(tart::padPitch(c, 3, 0, 0, root, in) == 36 && in && !root);
	CHECK(tart::padPitch(c, 3, 0, 1, root, in) == 37 && !in);
	CHECK(tart::padPitch(c, 3, 0, 9, root, in) == 45 && root);
	// the table: every scale has its root and no duplicate name
	CHECK(tart::kScaleCount == 37);
	for (int i = 0; i < tart::kScaleCount; i++) {
		CHECK(tart::kScales[i].mask & 1 && tart::kScales[i].mask < 4096);
		for (int j = 0; j < i; j++)
			CHECK(std::string(tart::kScales[i].name) != tart::kScales[j].name);
	}
	CHECK(tart::kScales[0].mask == 0xAB5 && std::string(tart::kScales[1].name) == "Minor");
}

TEST(scale_buttons) {
	using namespace push2::cc;
	Rig r;
	r.in.leds = true;
	r.step();
	r.m.edit([](tart::State& st) {
		st.clip[0][0].length = 384, st.clip[0][0].nNotes = 1, st.clip[0][0].notes[0] = {0, 24, 60, 100};
	});
	// A tap toggles the screen; the second tap closes it.
	r.press(Scale);
	CHECK(r.view().s.ui.screen == tart::kScaleScreen && r.want.cc[Scale].color == push2::kWhiteFull);
	r.run_(100);
	r.press(Scale, 0);
	CHECK(r.view().s.ui.screen == tart::kScaleScreen); // short: stays
	r.press(Scale);
	CHECK(r.view().s.ui.screen == tart::kHome && r.want.cc[Scale].color == push2::kWhiteFull); // still held
	r.press(Scale, 0);
	CHECK(r.want.cc[Scale].color == push2::kWhiteDark);
	// Held for 0.6 s: momentary.
	r.press(Scale);
	r.run_(48000 * 6 / 10);
	r.press(Scale, 0);
	CHECK(r.view().s.ui.screen == tart::kHome);
	// Held but under 0.3 s: stays.
	r.press(Scale);
	r.run_(48000 * 2 / 10);
	r.press(Scale, 0);
	CHECK(r.view().s.ui.screen == tart::kScaleScreen);
	// Roots: upper 2..7 C G D A E B; lower 2..7 F Bb Eb Ab Db Gb. The lower buttons don't select tracks here.
	const int upper[] = {0, 7, 2, 9, 4, 11}, lower[] = {5, 10, 3, 8, 1, 6};
	for (int i = 0; i < 6; i++) {
		r.press(Track1Above + 1 + i);
		CHECK(r.view().s.scale.root == upper[i]);
		CHECK(r.want.cc[Track1Above + 1 + i].color == push2::kRgbWhite &&
			  r.want.cc[Track1Above + 1 + (i + 1) % 6].color == push2::kRgbDarkGray);
		r.press(Track1Below + 1 + i);
		CHECK(r.view().s.scale.root == lower[i] && r.view().s.ui.track == 0);
		CHECK(r.want.cc[Track1Below + 1 + i].color == push2::kRgbWhite &&
			  r.want.cc[Track1Above + 1 + i].color == push2::kRgbDarkGray);
	}
	CHECK(r.want.cc[Track1Above].color == 0 && r.want.cc[Track1Above + 7].color == 0);
	r.press(Track1Above + 1);
	// In Key / Chromatic and Fixed.
	CHECK(r.view().s.scale.inKey && !r.view().s.scale.fixed && r.want.cc[Track1Below].color == push2::kRgbWhite);
	CHECK(r.want.cc[Track1Below + 7].color == push2::kRgbDarkGray);
	r.press(Track1Below);
	r.press(Track1Below + 7);
	CHECK(!r.view().s.scale.inKey && r.view().s.scale.fixed && r.want.cc[Track1Below + 7].color == push2::kRgbWhite);
	r.press(Track1Below);
	r.press(Track1Below + 7);
	// Encoders: 1 layout, 8 direction, 2..7 the scale list.
	r.turn(0, 30);
	CHECK(r.view().s.scale.layout == 2);
	r.turn(0, 30), r.turn(0, 30), r.turn(0, 30);
	CHECK(r.view().s.scale.layout == 4); // the end of the list
	r.turn(0, -30);
	CHECK(r.view().s.scale.layout == 2);
	r.turn(7, 30);
	CHECK(r.view().s.scale.layout == 3);
	r.turn(7, -30);
	CHECK(r.view().s.scale.layout == 2);
	r.turn(3, 8);
	CHECK(r.view().s.scale.scale == 1);
	r.turn(6, -8 + 128); // a negative delta as the Push sends it
	CHECK(r.view().s.scale.scale == 0);
	r.turn(1, -8 + 128);
	CHECK(r.view().s.scale.scale == 0); // clamped
	r.turn(1, 63), r.turn(1, 63), r.turn(1, 63), r.turn(1, 63), r.turn(1, 63), r.turn(1, 63);
	CHECK(r.view().s.scale.scale == tart::kScaleCount - 1); // 47 steps, but the list ends
	// The arrows: up and down by one, left and right by four.
	r.press(Down);
	CHECK(r.view().s.scale.scale == tart::kScaleCount - 1);
	r.press(Left);
	r.press(Left);
	CHECK(r.view().s.scale.scale == tart::kScaleCount - 9 && r.view().s.ui.track == 0);
	r.press(Up);
	CHECK(r.view().s.scale.scale == tart::kScaleCount - 10);
	r.press(Right);
	CHECK(r.view().s.scale.scale == tart::kScaleCount - 6);
	// Back on Home the buttons select tracks again; the scale never touched a clip.
	r.press(Scale);
	r.press(Scale, 0);
	r.press(Track1Below + 4);
	CHECK(r.view().s.ui.track == 4);
	tart::View v = r.view();
	CHECK(v.length[0][0] == 384);
	auto st = std::make_unique<tart::State>();
	r.m.save(*st);
	CHECK(st->clip[0][0].nNotes == 1 && st->clip[0][0].notes[0].pitch == 60);
}

TEST(scale_leds) {
	Rig r;
	r.in.leds = true;
	r.step();
	// The pads follow the scale: D Major Fixed, In Key: C# at the bottom left, the D pads in the track's colour.
	r.m.edit([](tart::State& st) { st.s.scale.root = 2, st.s.scale.fixed = true; });
	r.step();
	CHECK(r.want.note[push2::padNote(0, 0)].color == push2::kRgbWhite);   // C#, not the root
	CHECK(r.want.note[push2::padNote(0, 1)].color == tart::kPaletteFull); // D
	CHECK(r.want.note[push2::padNote(0, 7)].color == push2::kRgbWhite);   // C# an octave up
	// Chromatic: out-of-key pads are dark.
	r.m.edit([](tart::State& st) { st.s.scale.inKey = false; });
	r.step();
	CHECK(r.want.note[push2::padNote(0, 0)].color == push2::kBlack);    // C is not in D Major
	CHECK(r.want.note[push2::padNote(0, 1)].color == push2::kRgbWhite); // C#
	CHECK(r.want.note[push2::padNote(0, 2)].color == tart::kPaletteFull);
}

namespace {

/** The samples at which a gate rose and fell. */
struct Edges {
	std::vector<int> up, down;
};

/** Plays (sample 0) and watches the first gate of `track` for `n` samples. */
Edges gateEdges(SliceRig& r, int n, int track = 0) {
	Edges e;
	bool prev = false;
	for (int i = 0; i < n; i++) {
		r.in.play = i == 0;
		r.step();
		bool g = r.f.track[track].gate[0] > 5;
		if (g && !prev)
			e.up.push_back(i);
		if (!g && prev)
			e.down.push_back(i);
		prev = g;
	}
	return e;
}

const int kMajor[8] = {0, 2, 4, 5, 7, 9, 11, 12};

/** The pitch of the sequencer's row `row` in C major from C1. */
int rowPitch(int row) {
	return 36 + kMajor[row];
}

int rowOf(int pitch) {
	for (int k = 0; k < 8; k++)
		if (rowPitch(k) == pitch)
			return k;
	return -1;
}

} // namespace

TEST(seq_toggle) {
	SliceRig r;
	r.click(push2::cc::Layout);
	CHECK(r.view().s.ui.noteLayout == 1);
	r.tap(0, 0);
	auto v = r.view();
	CHECK(v.length[0][0] == 768 && v.nNotes == 1); // Fixed Length off: its setting all the same, 2 Bars
	CHECK(v.notes[0].tick == 0 && v.notes[0].len == 24 && v.notes[0].pitch == 36 && v.notes[0].vel == 100);
	CHECK(v.s.track[0].playing == 0); // stopped: it is the playing clip
	CHECK(r.f.track[0].gate[0] == 0); // the pads make no sound in the sequencer
	r.tap(0, 0);
	v = r.view();
	CHECK(v.nNotes == 0 && v.length[0][0] == 768); // the clip stays, empty

	// Fixed Length on, 1 Beat: the clip of another track
	r.click(push2::cc::FixedLength);
	r.press(push2::cc::FixedLength);
	r.click(push2::cc::Track1Below);
	r.press(push2::cc::FixedLength, 0);
	v = r.view();
	CHECK(v.s.ui.fixedOn && v.s.ui.fixedBars == 0);
	r.click(push2::cc::Track1Below + 1);
	r.tap(3, 2);
	v = r.view();
	CHECK(v.length[1][0] == 96 && v.nNotes == 1 && v.notes[0].tick == 48 && v.notes[0].pitch == rowPitch(3));
	r.tap(0, 4); // step 4 starts at tick 96: past the end of the clip
	CHECK(r.view().nNotes == 1);

	// 1000 random taps on a 32-bar clip keep the notes sorted and unique, and agree with a model.
	r.click(push2::cc::Track1Below + 2);
	r.press(push2::cc::FixedLength);
	r.click(push2::cc::Track1Below + 7);
	r.press(push2::cc::FixedLength, 0);
	std::set<std::pair<int, int>> model; // (step, row)
	uint32_t rng = 12345;
	auto next = [&] { return rng = rng * 1664525u + 1013904223u, rng >> 16; };
	int page = 0;
	for (int i = 0; i < 1000; i++) {
		uint32_t x = next();
		if (x % 8 == 0) {
			r.click(x / 8 % 2 ? push2::cc::PageRight : push2::cc::PageLeft);
			page = r.view().s.ui.page;
			continue;
		}
		int row = x / 8 % 8, col = x / 64 % 8;
		r.tap(row, col);
		auto key = std::make_pair(page * 8 + col, row);
		if (!model.erase(key))
			model.insert(key);
	}
	auto st = r.state();
	const tart::Clip& c = st->clip[2][0];
	CHECK(c.length == 12288 && c.nNotes == model.size() && model.size() > 50);
	bool ordered = true, same = true;
	for (int i = 0; i < c.nNotes; i++) {
		const tart::Note& n = c.notes[i];
		ordered &=
			i == 0 || c.notes[i - 1].tick < n.tick || (c.notes[i - 1].tick == n.tick && c.notes[i - 1].pitch < n.pitch);
		same &= n.tick % 24 == 0 && n.len == 24 && model.count({n.tick / 24, rowOf(n.pitch)}) == 1;
	}
	CHECK(ordered && same);
}

TEST(seq_controls) {
	Rig r;
	r.m.edit([](tart::State& st) { st.s.ui.fixedBars = 2; }); // new clips: 1 Bar
	r.click(push2::cc::Layout);
	r.click(push2::cc::Scene8 + 2); // 1/8: the third scene button from the bottom
	CHECK(r.view().s.track[0].grid == 2);
	r.tap(0, 3); // step 3 of 48 ticks
	auto v = r.view();
	CHECK(v.nNotes == 1 && v.notes[0].tick == 144 && v.notes[0].len == 48);
	r.press(push2::cc::Delete); // Delete and a step takes the note off, and makes no clip
	r.tap(0, 3);
	r.tap(5, 5);
	r.press(push2::cc::Delete, 0);
	v = r.view();
	CHECK(v.nNotes == 0 && v.s.ui.heldStep == -1);
	r.click(push2::cc::PageRight); // a bar of 1/8 steps is 8 steps: one page
	CHECK(r.view().s.ui.page == 0);
	r.click(push2::cc::Scene8 + 7); // 1/32t: 8 ticks, 48 steps in the bar, 6 pages
	r.click(push2::cc::PageRight);
	r.click(push2::cc::PageRight);
	CHECK(r.view().s.ui.page == 2);
	r.click(push2::cc::Scene8 + 4); // 1/16: 16 steps, 2 pages: the page is cut back
	CHECK(r.view().s.ui.page == 1);
	r.in.leds = true;
	r.step();
	auto led = [&](int row, int col) { return r.want.note[push2::padNote(row, col)].color; };
	CHECK(led(0, 0) == push2::kRgbWhite && led(1, 0) == push2::kRgbDarkGray); // the root row, the others
	CHECK(r.want.cc[push2::cc::Scene8 + 4].color == push2::kRgbWhite &&
		  r.want.cc[push2::cc::Scene8].color == push2::kRgbDarkGray);
	CHECK(r.want.cc[push2::cc::PageLeft].color == push2::kWhiteDark && r.want.cc[push2::cc::PageRight].color == 0);
	r.click(push2::cc::PageLeft);
	r.tap(2, 1);
	r.step();
	CHECK(led(2, 1) == tart::kPaletteFull);
	r.pad(2, 1, 100); // hold it: white; softer: the dim color
	r.step();
	CHECK(led(2, 1) == push2::kRgbWhite);
	r.turn(3, -60);
	r.pad(2, 1, 0);
	r.step();
	CHECK(led(2, 1) == tart::kPaletteDim && r.view().nNotes == 1);
	r.click(push2::cc::Layout), r.click(push2::cc::Layout); // via Sequencer + 32 Notes
	CHECK(r.view().s.ui.noteLayout == 0);

	r.m.edit([](tart::State& st) { st.s.ui.fixedBars = 3; });
	// Fixed Length: its buttons while it is held, and a tap on it
	r.press(push2::cc::FixedLength);
	r.press(push2::cc::Track1Above);
	r.press(push2::cc::Track1Above, 0);
	r.step();
	CHECK(r.view().s.ui.fixedOn && r.want.cc[push2::cc::Track1Below + 3].color == push2::kRgbWhite);
	r.press(push2::cc::FixedLength, 0);
	CHECK(r.view().s.ui.fixedOn); // choosing is not a tap
	r.click(push2::cc::FixedLength);
	CHECK(!r.view().s.ui.fixedOn);
	r.press(push2::cc::FixedLength); // held for 0.6 s: no toggle
	r.run_(30000);
	r.press(push2::cc::FixedLength, 0);
	CHECK(!r.view().s.ui.fixedOn);
}

TEST(seq_create_launch) {
	Rig r;
	r.click(push2::cc::Layout);
	r.in.play = true;
	r.run_(1000); // tick 4
	r.tap(0, 0);
	auto v = r.view();
	CHECK(v.s.track[0].playing == tart::kNone && v.s.track[0].queued == 0 && v.s.track[0].launchTick == 384);
	r.run_(48000 * 2);
	v = r.view();
	CHECK(v.s.track[0].playing == 0 && v.s.track[0].queued == tart::kNone && v.s.track[0].startTick == 384);
}

TEST(seq_playback_sample_exact) {
	SliceRig r;
	r.click(push2::cc::Layout);
	r.tap(0, 0);
	r.tap(0, 4);
	Edges e = gateEdges(r, 24100);
	CHECK(e.up.size() == 2 && e.up[0] == 0 && e.up[1] == 24000);
	CHECK(e.down.size() == 1 && e.down[0] == 6000); // 24 ticks of a 1/16 step
	CHECK_NEAR(r.f.track[0].vel[0], 100.f / 127 * 10, 1e-4);
	CHECK_NEAR(r.f.track[0].voct[0], -2.f, 1e-6);
}

TEST(note_length_and_retrigger_gap) {
	SliceRig r;
	r.click(push2::cc::Layout);
	r.tap(0, 0);
	r.tap(0, 1); // the second note starts on the tick the first one ends
	Edges e = gateEdges(r, 13000);
	CHECK(e.up.size() == 2 && e.up[0] == 0 && e.up[1] == 6001); // one sample low between them
	CHECK(e.down.size() == 2 && e.down[0] == 6000 && e.down[1] == 12000);
}

TEST(swing) {
	for (int swing : {0, 50, 100}) { // the note on tick 24 starts round(swing * 12 / 100) ticks late
		SliceRig r;
		r.click(push2::cc::Layout);
		r.tap(0, 1);
		r.m.edit([=](tart::State& st) { st.s.transport.swing = (uint8_t)swing; });
		Edges e = gateEdges(r, 12000);
		CHECK(e.up.size() == 1 && e.up[0] == (24 + (swing * 12 + 50) / 100) * 250);
	}
	{ // the notes on the beat and the eighth stay where they are
		SliceRig r;
		r.click(push2::cc::Layout);
		r.tap(0, 0);
		r.tap(0, 2);
		r.m.edit([](tart::State& st) { st.s.transport.swing = 100; });
		Edges e = gateEdges(r, 13000);
		CHECK(e.up.size() == 2 && e.up[0] == 0 && e.up[1] == 12000);
	}
	{ // a clip of 30 ticks: the note on 24 is late by 12 ticks, which is tick 6 of the next pass (from the first)
		SliceRig r;
		auto st = r.state();
		st->clip[0][0].length = 30;
		st->clip[0][0].nNotes = 1;
		st->clip[0][0].notes[0] = {24, 4, 60, 100};
		st->s.track[0].playing = 0;
		st->s.transport.swing = 100;
		r.m.load(*st);
		Edges e = gateEdges(r, 17000);
		CHECK(e.up.size() == 3 && e.up[0] == 6 * 250 && e.up[1] == 36 * 250 && e.up[2] == 66 * 250);
	}
}

TEST(loop) {
	SliceRig r;
	r.click(push2::cc::Layout);
	r.click(push2::cc::FixedLength);
	r.press(push2::cc::FixedLength);
	r.click(push2::cc::Track1Below); // 1 Beat
	r.press(push2::cc::FixedLength, 0);
	r.tap(0, 0);
	CHECK(r.view().length[0][0] == 96);
	Edges e = gateEdges(r, 48100);
	CHECK(e.up.size() == 3 && e.up[0] == 0 && e.up[1] == 24000 && e.up[2] == 48000);
}

TEST(hold_step_encoders) {
	Rig r;
	r.click(push2::cc::Layout);
	r.tap(0, 2); // tick 48
	r.pad(0, 2, 100);
	auto v = r.view();
	CHECK(v.s.ui.heldStep == 2 && v.held == 0);
	r.turn(0, 6); // Nudge: 2 raw units a tick
	CHECK(r.view().notes[0].tick == 51);
	r.turn(0, 60);
	CHECK(r.view().notes[0].tick == 59); // half a step on, no further
	r.turn(0, -63);
	r.turn(0, -63);
	CHECK(r.view().notes[0].tick == 36); // half a step back
	r.turn(1, 8);                        // Length: a step
	CHECK(r.view().notes[0].len == 48);
	r.turn(2, 6); // Length fine: 3 ticks
	CHECK(r.view().notes[0].len == 51);
	r.turn(1, -63); // never below one tick
	r.turn(1, -63);
	CHECK(r.view().notes[0].len == 1);
	r.turn(3, 10); // Velocity
	CHECK(r.view().notes[0].vel == 110);
	r.turn(3, 63);
	r.turn(3, 63);
	r.turn(3, 63);
	CHECK(r.view().notes[0].vel == 127);
	r.pad(0, 2, 0); // the hold edited it: it stays
	v = r.view();
	CHECK(v.nNotes == 1 && v.s.ui.heldStep == -1 && v.held == -1);
	r.turn(3, -5); // not held: nothing
	CHECK(r.view().notes[0].vel == 127);
}

TEST(long_note) {
	Rig r;
	r.click(push2::cc::Layout);
	r.tap(0, 0);
	r.pad(0, 0, 100);
	r.pad(1, 3, 100); // another row: nothing
	r.pad(1, 3, 0);
	CHECK(r.view().notes[0].len == 24 && r.view().nNotes == 1);
	r.pad(0, 3, 100); // the same row, later: the note reaches the end of that step
	r.pad(0, 3, 0);
	r.pad(0, 0, 0);
	auto v = r.view();
	CHECK(v.nNotes == 1 && v.notes[0].tick == 0 && v.notes[0].len == 96);
	r.pad(0, 0, 100); // a tap on it takes it off, long as it is
	r.pad(0, 0, 0);
	CHECK(r.view().nNotes == 0);
}

TEST(stop_releases_clip_not_pads) {
	SliceRig r;
	r.click(push2::cc::Layout);
	r.tap(0, 0);
	r.pad(0, 0, 100);
	r.pad(0, 7, 100); // eight steps: a long note
	r.pad(0, 7, 0);
	r.pad(0, 0, 0);
	r.click(push2::cc::Layout);
	r.click(push2::cc::Track1Below + 1); // a pad on track 2 is held through the Stop
	r.pad(0, 0, 100);
	r.click(push2::cc::Play);
	r.run_(3000);
	CHECK(r.f.track[0].gate[0] == 10 && r.f.track[1].gate[0] == 10);
	CHECK(r.view().s.track[0].playing == 0);
	r.click(push2::cc::Play); // Stop
	r.run_(3);
	CHECK(r.f.track[0].gate[0] == 0 && r.f.track[1].gate[0] == 10);
	CHECK(r.view().s.track[0].playing == tart::kNone && !r.view().s.transport.playing);
	r.click(push2::cc::Play); // Play again: the clips have to be launched again
	r.run_(3000);
	CHECK(r.f.track[0].gate[0] == 0 && r.view().s.transport.playing);
	r.pad(0, 0, 0);
	CHECK(r.f.track[1].gate[0] == 0);
}

TEST(sanitize_clips) {
	auto st = std::make_unique<tart::State>();
	tart::Clip& c = st->clip[1][2];
	c.length = 20000, c.nNotes = 5000;
	for (int i = 0; i < tart::kMaxNotes; i++)
		c.notes[i] = {(uint16_t)(tart::kMaxNotes - 1 - i), 0, (uint8_t)(i % 100), 0};
	c.notes[0] = {5, 10, 200, 60};   // pitch too high: dropped
	c.notes[1] = {7, 60000, 60, 0};  // length and velocity clamped
	c.notes[2] = {7, 3, 60, 99};     // the same tick and pitch: one of them stays
	c.notes[3] = {20000, 3, 60, 99}; // tick beyond any clip: dropped
	tart::Clip& e = st->clip[0][0];
	e.length = 0, e.nNotes = 9;
	tart::Clip& s = st->clip[0][1];
	s.length = 96, s.nNotes = 2, s.notes[0] = {50, 3, 70, 90}, s.notes[1] = {10, 3, 71, 90};
	st->s.track[0].playing = 1;
	tart::sanitize(*st);
	CHECK(c.length == tart::kMaxLength && c.nNotes == tart::kMaxNotes - 3);
	bool ok = true;
	int at7 = 0;
	for (int i = 0; i < c.nNotes; i++) {
		ok &= c.notes[i].pitch <= 127 && c.notes[i].vel >= 1 && c.notes[i].vel <= 127 && c.notes[i].len >= 1 &&
			  c.notes[i].len <= tart::kMaxLength && c.notes[i].tick < tart::kMaxLength;
		ok &= i == 0 || c.notes[i - 1].tick < c.notes[i].tick ||
			  (c.notes[i - 1].tick == c.notes[i].tick && c.notes[i - 1].pitch < c.notes[i].pitch);
		at7 += c.notes[i].tick == 7 && c.notes[i].pitch == 60;
	}
	CHECK(ok && at7 == 1);
	CHECK(e.nNotes == 0);
	CHECK(s.nNotes == 2 && s.notes[0].tick == 10 && s.notes[1].tick == 50);
	CHECK(st->s.track[0].playing == tart::kNone); // a patch loads stopped
}

namespace {

/** A clip of `bars` bars with one note on its first tick (pitch 60 + track), in the slot. */
void fill(Rig& r, int track, int scene, int bars = 1) {
	r.m.edit([=](tart::State& st) {
		tart::Clip& c = st.clip[track][scene];
		c.length = (uint16_t)(bars * tart::kBar), c.nNotes = 1;
		c.notes[0] = {0, 24, (uint8_t)(60 + track), 100};
	});
}

/** The Session pad of a slot: row 0 is the bottom pad row, scene 1 the top one. */
void slot(Rig& r, int track, int scene, int vel) {
	r.pad(7 - scene, track, vel);
}
void tapSlot(Rig& r, int track, int scene) {
	slot(r, track, scene, 100);
	slot(r, track, scene, 0);
}
void setQuant(Rig& r, int quant) {
	r.m.edit([=](tart::State& st) { st.s.transport.quant = (uint8_t)quant; });
}

} // namespace

TEST(launch_quant) {
	Rig r(false);
	fill(r, 0, 0);
	setQuant(r, 4);
	r.in.play = true;
	while (r.view().s.transport.tick < 100)
		r.step();
	CHECK(r.view().s.transport.tick == 100);
	tapSlot(r, 0, 0);
	auto v = r.view();
	CHECK(v.s.track[0].queued == 0 && v.s.track[0].launchTick == 384 && v.s.track[0].playing == tart::kNone); // 1 Bar
	setQuant(r, 0);
	tapSlot(r, 0, 0);
	CHECK(r.view().s.track[0].launchTick == 101); // None: the next tick
	setQuant(r, 8);
	tapSlot(r, 0, 0);
	CHECK(r.view().s.track[0].launchTick == 128); // 1/4T: 64 ticks
	setQuant(r, 1);
	tapSlot(r, 0, 0);
	CHECK(r.view().s.track[0].launchTick == 3072); // 8 Bars
	setQuant(r, 8);
	tapSlot(r, 0, 0);
	r.run_(48000); // it starts at its tick, from the start of the clip
	v = r.view();
	CHECK(v.s.track[0].playing == 0 && v.s.track[0].startTick == 128 && v.s.track[0].queued == tart::kNone);
}

TEST(launch_while_stopped) {
	SliceRig r(false);
	fill(r, 0, 0);
	CHECK(!r.view().s.transport.playing);
	tapSlot(r, 0, 0);
	r.run_(3000);
	auto v = r.view();
	CHECK(v.s.transport.playing && v.s.track[0].playing == 0 && v.s.track[0].startTick == 0);
	CHECK(r.f.track[0].gate[0] == 10); // from tick 0, whatever the quantization
	CHECK(v.s.ui.track == 0 && v.s.ui.scene == 0);
	// a pad of an empty slot does not start it
	r.click(push2::cc::Play);
	r.run_(10);
	CHECK(!r.view().s.transport.playing);
	tapSlot(r, 1, 3);
	r.run_(10);
	CHECK(!r.view().s.transport.playing && r.view().s.ui.track == 1 && r.view().s.ui.scene == 3);
}

TEST(scene_launch_stops_empty) {
	Rig r(false);
	setQuant(r, 0);
	fill(r, 0, 1);
	fill(r, 1, 1);
	fill(r, 2, 0);
	fill(r, 3, 0);
	r.in.play = true;
	r.step();
	tapSlot(r, 2, 0);
	tapSlot(r, 3, 0);
	r.run_(1000);
	CHECK(r.view().s.track[2].playing == 0 && r.view().s.track[3].playing == 0);
	r.click(push2::cc::Scene1 - 1); // scene 2
	auto v = r.view();
	CHECK(v.s.ui.scene == 1 && v.s.track[0].queued == 1 && v.s.track[1].queued == 1);
	CHECK(v.s.track[2].queued == tart::kStopQueued && v.s.track[3].queued == tart::kStopQueued);
	CHECK(v.s.track[4].queued == tart::kNone); // nothing played there
	r.run_(1000);
	v = r.view();
	CHECK(v.s.track[0].playing == 1 && v.s.track[1].playing == 1);
	CHECK(v.s.track[2].playing == tart::kNone && v.s.track[3].playing == tart::kNone &&
		  v.s.track[2].queued == tart::kNone);
}

TEST(stop_clip_track_and_all) {
	SliceRig r(false);
	setQuant(r, 0);
	for (int t = 0; t < 3; t++)
		fill(r, t, 0);
	r.click(push2::cc::Scene1);
	r.run_(1000);
	auto v = r.view();
	CHECK(v.s.track[0].playing == 0 && v.s.track[1].playing == 0 && v.s.track[2].playing == 0);
	r.press(push2::cc::StopClip);
	r.in.leds = true;
	r.step();
	auto lower = [&](int t) { return r.want.cc[push2::cc::Track1Below + t]; };
	CHECK(lower(0).channel == push2::kPulseHalf && lower(0).base == push2::kRed && lower(5).channel == 0 &&
		  lower(5).color == 0);
	r.click(push2::cc::Track1Below + 1); // stops track 2; does not select it
	v = r.view();
	CHECK(v.s.track[1].queued == tart::kStopQueued && v.s.ui.track == 0);
	r.step();
	CHECK(lower(1).channel == push2::kBlinkQuarter && lower(1).base == push2::kRed);
	r.press(push2::cc::StopClip, 0);
	r.run_(1000);
	v = r.view();
	CHECK(v.s.track[0].playing == 0 && v.s.track[1].playing == tart::kNone && v.s.track[2].playing == 0);
	r.press(push2::cc::Shift);
	r.click(push2::cc::StopClip);
	r.press(push2::cc::Shift, 0);
	r.run_(1000);
	v = r.view();
	CHECK(v.s.track[0].playing == tart::kNone && v.s.track[2].playing == tart::kNone && v.s.transport.playing);
	// without Stop Clip a track button selects
	r.click(push2::cc::Track1Below + 4);
	CHECK(r.view().s.ui.track == 4);
}

TEST(empty_pad_stops) {
	SliceRig r(false);
	fill(r, 0, 0);
	r.click(push2::cc::Scene1);
	r.run_(1000);
	CHECK(r.view().s.track[0].playing == 0 && r.f.track[0].gate[0] == 10);
	tapSlot(r, 0, 3); // an empty slot of that track: its stop is queued at the quantization (1 Bar)
	auto v = r.view();
	CHECK(v.s.track[0].queued == tart::kStopQueued && v.s.track[0].launchTick == 384 && v.s.track[0].playing == 0);
	CHECK(v.s.ui.scene == 3);
	r.run_(48000 * 2);
	CHECK(r.view().s.track[0].playing == tart::kNone && r.view().s.track[0].queued == tart::kNone);
	tapSlot(r, 5, 5); // nothing plays on that track: nothing to stop
	CHECK(r.view().s.track[5].queued == tart::kNone);
}

TEST(delete_pad_and_scene) {
	SliceRig r(false);
	setQuant(r, 0);
	for (int t = 0; t < 3; t++)
		fill(r, t, 0), fill(r, t, 1);
	r.click(push2::cc::Scene1);
	r.run_(1000);
	CHECK(r.f.track[0].gate[0] == 10);
	r.press(push2::cc::Delete);
	tapSlot(r, 0, 0); // the clip goes, and with it its notes, at once
	r.run_(3);
	auto v = r.view();
	CHECK(v.length[0][0] == 0 && v.length[0][1] == tart::kBar && v.s.track[0].playing == tart::kNone);
	CHECK(r.f.track[0].gate[0] == 0 && v.s.track[1].playing == 0);
	CHECK(r.state()->clip[0][0].nNotes == 0);
	r.click(push2::cc::Scene1 - 1); // Delete and a scene button empty the row
	r.press(push2::cc::Delete, 0);
	v = r.view();
	CHECK(v.length[0][1] == 0 && v.length[1][1] == 0 && v.length[2][1] == 0 && v.length[1][0] == tart::kBar);
	CHECK(v.s.track[1].queued == tart::kNone);
}

TEST(duplicate_pad_and_scene) {
	Rig r(false);
	fill(r, 0, 0, 2);
	r.m.edit([](tart::State& st) { st.clip[0][0].nNotes = 1, st.clip[0][0].notes[0].vel = 77; });
	fill(r, 1, 0);
	r.press(push2::cc::Duplicate);
	slot(r, 0, 0, 100); // the source, then two destinations
	slot(r, 0, 0, 0);
	tapSlot(r, 0, 3);
	tapSlot(r, 2, 5);
	tapSlot(r, 0, 0); // onto itself: nothing
	r.press(push2::cc::Duplicate, 0);
	auto st = r.state();
	CHECK(st->clip[0][3].length == 768 && st->clip[0][3].notes[0].vel == 77 && st->clip[2][5].length == 768);
	CHECK(st->clip[0][0].length == 768 && st->s.ui.track == 0 && st->s.ui.scene == 0);
	CHECK(st->s.transport.playing == false && st->s.track[0].playing == tart::kNone);
	// an empty source copies nothing and the next press is not a destination
	r.press(push2::cc::Duplicate);
	tapSlot(r, 6, 6);
	tapSlot(r, 0, 3);
	r.press(push2::cc::Duplicate, 0);
	CHECK(r.view().length[0][3] == 768);
	// Duplicate and a scene button: the row goes into the next scene, over what was there
	r.press(push2::cc::Duplicate);
	r.click(push2::cc::Scene1);
	r.press(push2::cc::Duplicate, 0);
	auto v = r.view();
	CHECK(v.length[0][1] == 768 && v.length[1][1] == tart::kBar && v.length[2][1] == 0);
	r.press(push2::cc::Duplicate);
	r.click(push2::cc::Scene1 - 7); // the last scene: nowhere to go
	r.press(push2::cc::Duplicate, 0);
	CHECK(!r.view().s.transport.playing);
}

TEST(new_selects_empty) {
	Rig r(false);
	setQuant(r, 0);
	fill(r, 0, 0);
	fill(r, 0, 1);
	fill(r, 0, 3);
	r.click(push2::cc::Scene1);
	r.in.play = true;
	r.run_(1000);
	r.click(push2::cc::New); // the playing clip stops and the next empty slot below is selected
	auto v = r.view();
	CHECK(v.s.ui.track == 0 && v.s.ui.scene == 2 && v.s.track[0].queued == tart::kStopQueued);
	r.run_(1000);
	CHECK(r.view().s.track[0].playing == tart::kNone);
	r.click(push2::cc::New); // already on an empty slot: it stays
	CHECK(r.view().s.ui.scene == 2);
	r.click(push2::cc::Down);
	r.click(push2::cc::Down);
	r.click(push2::cc::New);
	CHECK(r.view().s.ui.scene == 4); // 3 is a clip, 4 is not
	for (int sc = 0; sc < 8; sc++)
		fill(r, 0, sc);
	r.click(push2::cc::New); // none below: it stays
	CHECK(r.view().s.ui.scene == 4);
}

TEST(select_without_launch) {
	Rig r(false);
	fill(r, 2, 4);
	r.press(push2::cc::Select);
	tapSlot(r, 2, 4);
	auto v = r.view();
	CHECK(v.s.ui.track == 2 && v.s.ui.scene == 4);
	CHECK(v.s.track[2].playing == tart::kNone && v.s.track[2].queued == tart::kNone && !v.s.transport.playing);
	tapSlot(r, 6, 7);               // empty slots too, and no stop is queued
	r.click(push2::cc::Scene1 - 2); // and a scene button selects its scene without launching
	r.press(push2::cc::Select, 0);
	v = r.view();
	CHECK(v.s.ui.track == 6 && v.s.ui.scene == 2 && !v.s.transport.playing && v.s.track[2].queued == tart::kNone);
	// the arrows move the slot, with no scrolling and no wrapping
	r.click(push2::cc::Up);
	r.click(push2::cc::Right);
	CHECK(r.view().s.ui.scene == 1 && r.view().s.ui.track == 7);
	r.click(push2::cc::Right);
	r.click(push2::cc::Up);
	r.click(push2::cc::Up);
	CHECK(r.view().s.ui.scene == 0 && r.view().s.ui.track == 7);
	r.click(push2::cc::Left);
	r.click(push2::cc::Down);
	CHECK(r.view().s.ui.scene == 1 && r.view().s.ui.track == 6);
}

TEST(momentary_mode) {
	Rig r(false);
	CHECK(r.view().s.ui.pads == tart::kSession); // the default
	r.press(push2::cc::Note);                    // held for 0.6 s: Note only while it is down
	CHECK(r.view().s.ui.pads == tart::kNote);
	r.run_(30000);
	r.press(push2::cc::Note, 0);
	CHECK(r.view().s.ui.pads == tart::kSession);
	r.click(push2::cc::Note); // a tap stays
	CHECK(r.view().s.ui.pads == tart::kNote);
	r.press(push2::cc::Session);
	r.run_(30000);
	CHECK(r.view().s.ui.pads == tart::kSession);
	r.press(push2::cc::Session, 0);
	CHECK(r.view().s.ui.pads == tart::kNote);
	r.click(push2::cc::Session);
	CHECK(r.view().s.ui.pads == tart::kSession);
}

TEST(session_leds) {
	Rig r(false);
	setQuant(r, 0);
	fill(r, 0, 0);
	fill(r, 1, 0);
	fill(r, 1, 2);
	r.in.leds = true;
	r.step();
	auto pad = [&](int t, int sc) { return r.want.note[push2::padNote(7 - sc, t)]; };
	auto scene = [&](int sc) { return r.want.cc[push2::cc::Scene1 - sc]; };
	push2::Led still = {(uint8_t)tart::kPaletteFull, push2::kStatic};
	CHECK(pad(0, 0).color == still.color && pad(0, 0).channel == 0); // stopped: the track's color, static
	CHECK(pad(1, 2).color == tart::kPaletteFull + 1 && pad(0, 1).color == 0 && pad(5, 5).color == 0);
	CHECK(scene(0).color == push2::kGreen && scene(2).color == push2::kGreen && scene(1).color == 0 &&
		  scene(0).channel == 0);
	CHECK(r.want.cc[push2::cc::Session].color == push2::kWhiteFull &&
		  r.want.cc[push2::cc::Note].color == push2::kWhiteDark);
	CHECK(r.want.cc[push2::cc::Delete].color == push2::kWhiteDark &&
		  r.want.cc[push2::cc::StopClip].color == push2::kWhiteDark);
	r.press(push2::cc::Duplicate);
	CHECK(r.want.cc[push2::cc::Duplicate].color == push2::kWhiteFull);
	r.press(push2::cc::Duplicate, 0);
	r.in.play = true;
	r.step();
	r.in.leds = true;
	tapSlot(r, 0, 0); // queued for the next tick: blinking green from the color
	CHECK(pad(0, 0).channel == push2::kBlinkQuarter && pad(0, 0).color == push2::kGreen &&
		  pad(0, 0).base == tart::kPaletteFull);
	CHECK(scene(0).channel == push2::kBlinkQuarter && scene(0).color == push2::kBlack &&
		  scene(0).base == push2::kGreen);
	r.run_(1000);
	r.in.leds = true;
	r.step();
	CHECK(pad(0, 0).channel == push2::kPulseHalf && pad(0, 0).color == tart::kPaletteDim &&
		  pad(0, 0).base == tart::kPaletteFull); // playing: pulses
	CHECK(pad(1, 0).channel == 0 && pad(1, 0).color == tart::kPaletteFull + 1);
	CHECK(scene(0).channel == 0 && scene(0).color == push2::kGreen);
	tapSlot(r, 0, 1); // a stop queued: the playing clip blinks to dark
	r.in.leds = true;
	r.step();
	CHECK(pad(0, 0).channel == push2::kBlinkQuarter && pad(0, 0).color == push2::kBlack);
	r.run_(1000);
	r.in.leds = true;
	r.step();
	CHECK(pad(0, 0).channel == 0 && pad(0, 0).color == tart::kPaletteFull);
	r.click(push2::cc::Note); // Note mode: no more session pads
	r.in.leds = true;
	r.step();
	CHECK(r.want.cc[push2::cc::Scene1].color == 0);
}

TEST(note_added_to_a_stopped_clip_does_not_launch) {
	SliceRig r;
	fill(r, 0, 0);
	r.click(push2::cc::Layout);
	r.tap(1, 1); // the slot has a clip: a note goes in, the clip does not start
	auto v = r.view();
	CHECK(v.nNotes == 2 && v.s.track[0].playing == tart::kNone && v.s.track[0].queued == tart::kNone);
	r.in.play = true;
	r.run_(3000);
	r.tap(2, 2);
	v = r.view();
	CHECK(v.nNotes == 3 && v.s.track[0].playing == tart::kNone && v.s.track[0].queued == tart::kNone);
	CHECK(r.f.track[0].gate[0] == 0);
	// another slot, edited through the selection: the first step of an empty slot creates the clip and launches it
	r.click(push2::cc::Down);
	r.tap(0, 0);
	v = r.view();
	CHECK(v.s.ui.scene == 1 && v.length[0][1] == 2 * tart::kBar && v.s.track[0].queued == 1);
}

TEST(clip_screen_momentary) {
	using namespace push2::cc;
	Rig r(false);
	r.in.leds = true;
	r.step();
	CHECK(r.want.cc[Clip].color == push2::kWhiteDark);
	r.press(Clip);
	CHECK(r.view().s.ui.screen == tart::kClipScreen && r.want.cc[Clip].color == push2::kWhiteFull);
	r.run_(100);
	r.press(Clip, 0);
	CHECK(r.view().s.ui.screen == tart::kClipScreen && r.want.cc[Clip].color == push2::kWhiteFull); // short: stays
	r.press(Clip);
	CHECK(r.view().s.ui.screen == tart::kHome);
	r.press(Clip, 0);
	CHECK(r.want.cc[Clip].color == push2::kWhiteDark);
	r.press(Clip); // held 0.6 s: momentary
	r.run_(48000 * 6 / 10);
	r.press(Clip, 0);
	CHECK(r.view().s.ui.screen == tart::kHome);
	r.press(Clip); // held 0.2 s: stays
	r.run_(48000 * 2 / 10);
	r.press(Clip, 0);
	CHECK(r.view().s.ui.screen == tart::kClipScreen);
}

TEST(clip_length_encoder) {
	using namespace push2::cc;
	Rig r(false);
	r.m.edit([](tart::State& st) {
		tart::Clip& c = st.clip[0][0];
		c.length = 384, c.nNotes = 3;
		c.notes[0] = {0, 24, 60, 100}, c.notes[1] = {200, 24, 62, 100}, c.notes[2] = {380, 4, 64, 100};
		st.clip[2][0].length = 96;
	});
	r.turn(0, 4); // not on the Clip screen: nothing
	CHECK(r.view().length[0][0] == 384);
	r.press(Clip);
	r.turn(0, 4);
	CHECK(r.view().length[0][0] == 480);
	r.turn(0, -4 + 128);
	CHECK(r.view().length[0][0] == 384);
	r.turn(0, 2); // half a beat: kept for the next turn
	CHECK(r.view().length[0][0] == 384);
	r.turn(0, 2);
	CHECK(r.view().length[0][0] == 480);
	r.turn(0, -4 + 128);
	r.turn(0, -8 + 128); // 2 beats
	auto v = r.view();
	CHECK(v.length[0][0] == 192 && v.nNotes == 3 && v.notes[2].tick == 380); // kept, beyond the end
	r.turn(0, 8);
	CHECK(r.view().length[0][0] == 384 && r.view().nNotes == 3); // restored
	for (int i = 0; i < 6; i++)
		r.turn(0, 63);
	for (int i = 0; i < 7; i++)
		r.turn(0, -63 + 128);
	CHECK(r.view().length[0][0] == 96); // 1 beat at least
	for (int i = 0; i < 12; i++)
		r.turn(0, 63);
	CHECK(r.view().length[0][0] == 128 * tart::kPpqn && r.view().length[0][0] <= tart::kMaxLength);
	// An empty slot stays empty; the other clips are untouched.
	r.click(Track1Below + 1);
	r.turn(0, 40);
	v = r.view();
	CHECK(v.length[1][0] == 0 && v.length[2][0] == 96 && v.length[0][0] == 128 * tart::kPpqn);
	// The length is saved, and not a screen of its own.
	CHECK(r.state()->clip[0][0].length == 128 * tart::kPpqn);
}

TEST(clip_shorten_releases_gate) {
	SliceRig r(false);
	r.m.edit([](tart::State& st) {
		tart::Clip& c = st.clip[0][0];
		c.length = 8 * tart::kPpqn, c.nNotes = 1, c.notes[0] = {0, 700, 60, 100};
	});
	tapSlot(r, 0, 0);
	r.run_(250 * 300); // tick ~300 of 768: the note still sounds
	CHECK(r.f.track[0].gate[0] == 10);
	r.press(push2::cc::Clip);
	r.turn(0, -6 * 4 + 128); // 2 beats: the position is past the new end
	r.run_(3);
	CHECK(r.f.track[0].gate[0] == 0);
	r.run_(250 * 100); // tick ~400: the loop of 192 has wrapped and the note starts again
	CHECK(r.f.track[0].gate[0] == 10);
}

TEST(hold_threshold) { // one constant, 0.3 s, for every momentary hold
	using namespace push2::cc;
	Rig r(false);
	auto held = [&](int cc, double seconds) {
		r.press(cc);
		r.run_((int)(48000 * seconds));
		r.press(cc, 0);
	};
	for (int cc : {Scale, Clip}) { // 0.25 s stays, 0.35 s only while held
		uint8_t on = cc == Scale ? tart::kScaleScreen : tart::kClipScreen;
		held(cc, 0.25);
		CHECK(r.view().s.ui.screen == on);
		r.click(cc);
		held(cc, 0.35);
		CHECK(r.view().s.ui.screen == tart::kHome);
	}
	held(Note, 0.25);
	CHECK(r.view().s.ui.pads == tart::kNote);
	held(Session, 0.35);
	CHECK(r.view().s.ui.pads == tart::kNote); // went to Session while held, back to Note
	held(Session, 0.25);
	CHECK(r.view().s.ui.pads == tart::kSession);
	held(FixedLength, 0.25); // a tap toggles; a hold doesn't
	CHECK(r.view().s.ui.fixedOn);
	held(FixedLength, 0.35);
	CHECK(r.view().s.ui.fixedOn);
}

TEST(step_clip_length_is_fixed_length_setting) {
	Rig r;
	r.click(push2::cc::Layout);
	r.tap(0, 0); // Fixed Length off, 2 Bars at the start
	CHECK(!r.view().s.ui.fixedOn && r.view().length[0][0] == 768);
	r.m.edit([](tart::State& st) { st.s.ui.fixedBars = 0; });
	r.click(push2::cc::Track1Below + 1);
	r.tap(0, 0);
	CHECK(r.view().length[1][0] == 96); // 1 Beat, still off
	r.m.edit([](tart::State& st) { st.s.ui.fixedBars = 7, st.s.ui.fixedOn = true; });
	r.click(push2::cc::Track1Below + 2);
	r.tap(0, 0);
	CHECK(r.view().length[2][0] == 32 * tart::kBar); // on: the same
}

TEST(clip_full_popup) {
	Rig r;
	r.click(push2::cc::Layout);
	r.m.edit([](tart::State& st) { // every step of every row of a 32-bar clip but one, and a note off the grid
		tart::Clip& c = st.clip[0][0];
		c.length = 32 * tart::kBar;
		for (int step = 0; step < 512; step++)
			for (int row = 0; row < 8; row++)
				if (step || row < 7)
					c.notes[c.nNotes++] = {(uint16_t)(step * 24), 24, (uint8_t)rowPitch(row), 100};
		c.notes[c.nNotes++] = {12288 - 1, 1, 100, 100};
	});
	CHECK(r.view().nNotes == tart::kMaxNotes);
	std::unique_ptr<tart::State> before = r.state();
	r.pad(7, 0, 100);
	CHECK(r.view().nNotes == tart::kMaxNotes && r.view().s.ui.popup == tart::kClipFullPopup);
	std::unique_ptr<tart::State> after = r.state();
	CHECK(std::memcmp(&before->clip[0][0], &after->clip[0][0], sizeof(tart::Clip)) == 0);
	CHECK(r.view().s.ui.heldStep == -1);
}

TEST(drum_pads_slice) {
	SliceRig r;
	r.m.edit([](tart::State& st) { st.s.track[0].type = tart::Drum, st.s.track[0].voices = 3; }); // Voices is ignored
	r.run_(2);
	const tart::SliceTrack& t = r.f.track[0];
	CHECK(t.channels == 16 && r.f.track[1].channels == 1);
	CHECK(t.voct[0] == -2.f && t.voct[15] == -0.75f && t.voct[7] == (7 - 24) / 12.f);
	r.pad(0, 0, 100); // lane 0, channel 0
	int on = 0;
	for (int c = 0; c < 16; c++)
		on += t.gate[c] > 5;
	CHECK(t.gate[0] == 10 && on == 1 && t.voct[0] == -2.f);
	CHECK_NEAR(t.vel[0], 100 / 127.f * 10, 1e-4);
	CHECK(r.view().s.track[0].lane == 0 && r.view().s.ui.pad[0] == 100);
	r.pad(0, 0, 0);
	CHECK(t.gate[0] == 0);
	r.pad(1, 3, 90); // row 1, column 3: lane 7
	on = 0;
	for (int c = 0; c < 16; c++)
		on += t.gate[c] > 5;
	CHECK(t.gate[7] == 10 && on == 1 && r.view().s.track[0].lane == 7);
	r.pad(0, 0, 100); // two at once
	CHECK(t.gate[0] == 10 && t.gate[7] == 10);
	r.pad(3, 3, 100); // lane 15
	CHECK(t.gate[15] == 10 && t.voct[15] == -0.75f);
	r.pad(0, 0, 0), r.pad(1, 3, 0), r.pad(3, 3, 0);
	for (int c = 0; c < 16; c++)
		CHECK(t.gate[c] == 0);
	// Select: selects without playing.
	r.pad(3, 3, 100), r.pad(3, 3, 0);
	CHECK(r.view().s.track[0].lane == 15);
	r.press(push2::cc::Select);
	r.pad(0, 2, 100);
	CHECK(t.gate[2] == 0 && r.view().s.track[0].lane == 2);
	r.pad(0, 2, 0);
	r.press(push2::cc::Select, 0);
	// The octave, the layout, the touch strip and the lights.
	r.click(push2::cc::OctaveUp);
	r.click(push2::cc::Layout);
	push2::Event bend = dec({0xE0, 0x7F, 0x7F});
	r.step(&bend);
	auto v = r.view();
	CHECK(v.s.track[0].octave == 3 && v.s.ui.noteLayout == 0 && v.s.ui.bend > 0.9f);
	r.run_(2);
	CHECK(t.voct[0] == -2.f && t.voct[15] == -0.75f);
	r.in.leds = true;
	r.step();
	auto led = [&](int row, int col) { return r.want.note[push2::padNote(row, col)].color; };
	CHECK(led(0, 2) == push2::kRgbWhite && led(0, 0) == tart::kPaletteDim && led(3, 3) == tart::kPaletteDim);
	CHECK(led(0, 4) == 0 && led(4, 0) == push2::kRgbDarkGray && led(7, 7) == push2::kRgbDarkGray);
	CHECK(r.want.cc[push2::cc::OctaveUp].color == 0 && r.want.cc[push2::cc::Layout].color == 0);
	r.pad(2, 1, 100); // lane 9 sounds: green, the selected lane white
	CHECK(led(2, 1) == push2::kGreen && led(0, 2) == tart::kPaletteDim); // pressing selects it too
	r.pad(2, 1, 0);
	// A melodic track (track 2) keeps its rotating voices.
	r.click(push2::cc::Track1Below + 1);
	r.pad(0, 0, 100);
	CHECK(r.f.track[1].channels == 1 && r.f.track[1].gate[0] == 10);
	CHECK_NEAR(r.f.track[1].voct[0], -2.f + 1.f / 6, 1e-3); // the strip bends a melodic track
}

TEST(drum_clip_playback) {
	SliceRig r;
	r.m.edit([](tart::State& st) {
		tart::Clip& c = st.clip[0][0];
		c.length = tart::kBar, c.nNotes = 5;
		c.notes[0] = {0, 24, 41, 100};  // lane 5
		c.notes[1] = {48, 24, 35, 100}; // outside the lanes: silent
		c.notes[2] = {96, 24, 60, 100};
		c.notes[3] = {144, 24, 52, 100};
		c.notes[4] = {192, 24, 36, 100}; // lane 0
		st.s.track[0].type = tart::Drum, st.s.track[0].playing = 0;
	});
	int first5 = -1, first0 = -1;
	bool other = false;
	for (int i = 0; i < 48100; i++) {
		r.in.play = i == 0;
		r.step();
		for (int c = 0; c < 16; c++) {
			if (r.f.track[0].gate[c] <= 5)
				continue;
			int& first = c == 5 ? first5 : first0;
			if (c != 5 && c != 0)
				other = true;
			else if (first < 0)
				first = i;
		}
	}
	CHECK(first5 == 0 && r.f.track[0].gate[5] == 0); // channel 5 at sample 0, for 24 ticks
	CHECK(!other);                                   // 35, 52 and 60 are silent
	CHECK(first0 == 48000);                          // lane 0 at tick 192
	// Switching the type loses nothing: as a Melodic track the pitch 60 note plays, on voice 0.
	r.m.edit([](tart::State& st) { st.s.track[0].type = tart::Melodic; });
	CHECK(r.state()->clip[0][0].nNotes == 5);
	r.in.reset = true;
	bool seen = false;
	for (int i = 0; i < 24100; i++) {
		r.step();
		seen |= i > 24000 && r.f.track[0].gate[0] > 5 && r.f.track[0].voct[0] == 0.f;
	}
	CHECK(seen);
}

TEST(browse_load_close) {
	using namespace push2::cc;
	Rig r;
	r.in.leds = true;
	r.step();
	CHECK(r.want.cc[Browse].color == push2::kWhiteDark);
	r.press(Browse);
	r.press(Browse, 0);
	auto v = r.view();
	CHECK(v.s.ui.screen == tart::kBrowseScreen && v.s.ui.browse == tart::Melodic);
	CHECK(r.want.cc[Browse].color == push2::kWhiteFull);
	CHECK(r.want.cc[Track8Below].color == push2::kRgbWhite && r.want.cc[Track1Above].color == push2::kRgbWhite);
	for (int b = Track1Below; b < Track8Below; b++)
		CHECK(r.want.cc[b].color == 0); // dark meanwhile
	for (int b = Track1Above + 1; b <= Track8Above; b++)
		CHECK(r.want.cc[b].color == 0);
	r.click(Track1Below + 3); // the track buttons and the arrows don't move the selection
	r.click(Right);
	CHECK(r.view().s.ui.track == 0 && r.view().s.ui.screen == tart::kBrowseScreen);
	r.click(Up); // already at the top
	CHECK(r.view().s.ui.browse == 0);
	r.click(Down);
	CHECK(r.view().s.ui.browse == 1 && r.view().s.ui.scene == 0);
	r.click(Down);
	CHECK(r.view().s.ui.browse == 2);
	r.click(Down);
	CHECK(r.view().s.ui.browse == 2); // three types: stops at the end
	r.click(Up);
	r.click(Up);
	r.turn(6, 8); // encoders 1..7
	CHECK(r.view().s.ui.browse == 1);
	r.turn(2, -8);
	CHECK(r.view().s.ui.browse == 0);
	r.turn(7, 8); // encoder 8 is not the list's
	CHECK(r.view().s.ui.browse == 0);
	r.click(Track1Above); // CLOSE: no change
	v = r.view();
	CHECK(v.s.ui.screen == tart::kHome && v.s.track[0].type == tart::Melodic);
	r.click(Track1Below + 2); // select track 3, open Browse, choose Drum, LOAD
	r.click(Browse);
	r.click(Down);
	r.click(Track8Below);
	v = r.view();
	CHECK(v.s.ui.screen == tart::kHome && v.s.track[2].type == tart::Drum && v.s.track[0].type == tart::Melodic);
	r.step();
	CHECK(r.want.cc[Browse].color == push2::kWhiteDark);
	r.click(Browse); // the cursor starts on the track's type
	CHECK(r.view().s.ui.browse == tart::Drum);
	r.click(Browse); // BROWSE again closes
	CHECK(r.view().s.ui.screen == tart::kHome && r.view().s.track[2].type == tart::Drum);
	r.click(Browse), r.click(Up), r.click(Track8Below); // back to Melodic
	CHECK(r.view().s.track[2].type == tart::Melodic);
	r.click(Browse), r.click(Down), r.click(Down), r.click(Track8Below); // Euclid
	CHECK(r.view().s.track[2].type == tart::Euclidean);
	r.click(Browse); // the cursor starts on the track's type
	CHECK(r.view().s.ui.browse == tart::Euclidean);
}

TEST(type_switch_silences) {
	SliceRig r;
	r.m.edit([](tart::State& st) {
		tart::Clip& c = st.clip[1][0];
		c.length = tart::kBar, c.nNotes = 1, c.notes[0] = {0, 380, 41, 100}; // sounds for the whole bar
		st.s.track[1].type = tart::Drum, st.s.track[1].playing = 0;
	});
	r.run_(2);
	r.pad(0, 0, 100); // a held pad on track 1, melodic
	r.in.play = true;
	r.run_(10);
	CHECK(r.f.track[0].gate[0] == 10 && r.f.track[1].gate[5] == 10);
	// By the menu (an edit from the UI thread): both tracks go quiet at once.
	r.m.edit([](tart::State& st) { st.s.track[0].type = tart::Drum, st.s.track[1].type = tart::Melodic; });
	r.run_(2);
	for (int c = 0; c < 16; c++) // a Slice reads only the channels there are
		CHECK((c < r.f.track[0].channels ? r.f.track[0].gate[c] : 0) == 0 &&
			  (c < r.f.track[1].channels ? r.f.track[1].gate[c] : 0) == 0);
	CHECK(r.f.track[0].channels == 16 && r.f.track[1].channels == 1);
	r.pad(0, 0, 0); // the stale release is harmless
	r.run_(2);
	for (int c = 0; c < 16; c++) // a Slice reads only the channels there are
		CHECK((c < r.f.track[0].channels ? r.f.track[0].gate[c] : 0) == 0 &&
			  (c < r.f.track[1].channels ? r.f.track[1].gate[c] : 0) == 0);
	// By Browse: Melodic -> Drum on track 1 stops the note of the pad.
	r.click(push2::cc::Track1Below + 1);
	r.pad(0, 0, 100);
	CHECK(r.f.track[1].gate[0] == 10);
	r.click(push2::cc::Browse), r.click(push2::cc::Down), r.click(push2::cc::Track8Below);
	r.run_(2);
	CHECK(r.f.track[1].channels == 16);
	for (int c = 0; c < 16; c++)
		CHECK(r.f.track[1].gate[c] == 0);
}

namespace {

/** The drum sequencer: step k (0..31) of the page is the pad (7 - k / 8, k % 8), loop pad p (0..15) is (3 - p / 4, 4 +
 * p % 4), lane n is (n / 4, n % 4). */
void drumTrack(Rig& r, int track = 0) {
	r.m.edit([=](tart::State& st) { st.s.track[track].type = tart::Drum; });
}
void dstep(Rig& r, int k, int vel = 100) {
	r.pad(7 - k / 8, k % 8, vel);
}
void dtap(Rig& r, int k) {
	dstep(r, k), dstep(r, k, 0);
}
void dloop(Rig& r, int p, int vel = 100) {
	r.pad(3 - p / 4, 4 + p % 4, vel);
}
void dlane(Rig& r, int n) {
	r.tap(n / 4, n % 4);
}

} // namespace

TEST(drum_steps_and_lanes) {
	Rig r;
	drumTrack(r);
	dlane(r, 5);
	dtap(r, 0); // an empty slot: the Fixed Length setting's 2 Bars, launched
	auto v = r.view();
	CHECK(v.length[0][0] == 768 && v.nNotes == 1 && v.s.track[0].playing == 0);
	CHECK(v.notes[0].tick == 0 && v.notes[0].len == 24 && v.notes[0].pitch == 41 && v.notes[0].vel == 100);
	dtap(r, 9); // the second row, column 1
	v = r.view();
	CHECK(v.nNotes == 2 && v.notes[1].tick == 216 && v.notes[1].pitch == 41);
	dtap(r, 0); // a tap on a note takes it off
	CHECK(r.view().nNotes == 1);
	dlane(r, 0); // another lane: its own steps
	dtap(r, 0);
	dtap(r, 31);
	v = r.view();
	CHECK(v.nNotes == 3 && v.notes[0].pitch == 36 && v.notes[0].tick == 0 && v.notes[2].tick == 31 * 24);
	dlane(r, 5); // Delete and a step: that lane's note
	r.press(push2::cc::Delete);
	dtap(r, 9);
	dtap(r, 31); // not a note of lane 5
	r.press(push2::cc::Delete, 0);
	v = r.view();
	CHECK(v.nNotes == 3 - 1 && v.s.ui.heldStep == -1);
	// Step size: the scene buttons, as the melodic sequencer.
	r.click(push2::cc::Scene8 + 2); // 1/8
	CHECK(r.view().s.track[0].grid == 2);
	dtap(r, 1);
	v = r.view();
	CHECK(v.nNotes == 3 && v.notes[1].tick == 48 && v.notes[1].len == 48 && v.notes[1].pitch == 41);
	// Pages: 4 Bars is 64 steps of 1/16.
	r.m.edit([](tart::State& st) { st.s.ui.fixedBars = 4; });
	r.click(push2::cc::Scene8 + 4);
	r.click(push2::cc::Track1Below + 1);
	drumTrack(r, 1);
	dtap(r, 0);
	CHECK(r.view().length[1][0] == 1536);
	r.click(push2::cc::PageRight);
	CHECK(r.view().s.ui.page == 1);
	r.click(push2::cc::PageRight); // the last page
	CHECK(r.view().s.ui.page == 1);
	dtap(r, 3);
	v = r.view();
	CHECK(v.nNotes == 2 && v.notes[1].tick == (32 + 3) * 24);
	// A page of 1 bar: the steps past its end do nothing.
	r.m.edit([](tart::State& st) { st.s.ui.fixedBars = 2; });
	r.click(push2::cc::Track1Below + 2);
	drumTrack(r, 2);
	dtap(r, 15);
	dtap(r, 16);
	v = r.view();
	CHECK(v.length[2][0] == 384 && v.nNotes == 1 && v.notes[0].tick == 15 * 24);
}

TEST(drum_hold_encoders) {
	Rig r;
	drumTrack(r);
	dlane(r, 2);
	dtap(r, 2); // tick 48
	dstep(r, 2);
	auto v = r.view();
	CHECK(v.s.ui.heldStep == (7 - 0) * 8 + 2 && v.held == 0);
	r.turn(0, 6); // Nudge: 2 raw units a tick
	CHECK(r.view().notes[0].tick == 51);
	r.turn(0, 60);
	CHECK(r.view().notes[0].tick == 59); // half a step on, no further
	r.turn(1, 8);                        // Length: a step
	CHECK(r.view().notes[0].len == 48);
	r.turn(2, 6); // Length fine
	CHECK(r.view().notes[0].len == 51);
	r.turn(3, 10); // Velocity
	CHECK(r.view().notes[0].vel == 110);
	dstep(r, 2, 0); // the hold edited it: it stays
	v = r.view();
	CHECK(v.nNotes == 1 && v.s.ui.heldStep == -1 && v.held == -1);
	r.turn(3, -5); // not held: nothing
	CHECK(r.view().notes[0].vel == 110);
	// A long note: hold a step and press a later one, in another row of the page; an earlier one does nothing.
	dtap(r, 0);
	dstep(r, 0);
	dtap(r, 12);
	dstep(r, 0, 0);
	v = r.view();
	CHECK(v.nNotes == 2 && v.notes[0].tick == 0 && v.notes[0].len == 13 * 24 && v.notes[1].tick == 59);
	dtap(r, 0); // a tap on a long note takes it off
	CHECK(r.view().nNotes == 1);
	// A hold with no note on it makes one that the encoder edits.
	dstep(r, 7);
	r.turn(3, -20);
	dstep(r, 7, 0);
	v = r.view();
	CHECK(v.nNotes == 2 && v.notes[1].tick == 168 && v.notes[1].vel == 80);
}

TEST(loop_selector) {
	Rig r;
	drumTrack(r);
	r.m.edit([](tart::State& st) { st.s.ui.fixedBars = 5; }); // not what a loop pad makes
	dloop(r, 0), dloop(r, 0, 0);                              // an empty slot: 1 bar, launched
	auto v = r.view();
	CHECK(v.length[0][0] == 384 && v.s.track[0].playing == 0 && v.nNotes == 0 && v.s.ui.page == 0);
	dloop(r, 0); // hold 0, tap 3: 4 bars
	dloop(r, 3), dloop(r, 3, 0);
	dloop(r, 0, 0);
	v = r.view();
	CHECK(v.length[0][0] == 4 * 384 && v.s.ui.pad[3 * 8 + 4] == 0 && v.s.ui.pad[3 * 8 + 7] == 0);
	dloop(r, 2), dloop(r, 2, 0); // inside: views page 1 (768 ticks a page)
	CHECK(r.view().s.ui.page == 1 && r.view().length[0][0] == 4 * 384);
	dloop(r, 7), dloop(r, 7, 0); // outside: the clip ends with it
	v = r.view();
	CHECK(v.length[0][0] == 8 * 384 && v.s.ui.page == 1);
	dloop(r, 1), dloop(r, 1, 0);
	CHECK(r.view().s.ui.page == 0);
	dloop(r, 1); // hold 1, tap 0: ends with the later one
	dloop(r, 0), dloop(r, 0, 0);
	dloop(r, 1, 0);
	CHECK(r.view().length[0][0] == 2 * 384);
	dloop(r, 5), dloop(r, 5, 0); // outside, on a clip: 6 bars exactly
	CHECK(r.view().length[0][0] == 6 * 384);
	// An empty slot and pad 5: 6 bars; the clip is empty and launched.
	r.click(push2::cc::Track1Below + 1);
	drumTrack(r, 1);
	dloop(r, 5), dloop(r, 5, 0);
	v = r.view();
	CHECK(v.length[1][0] == 6 * 384 && v.s.track[1].playing == 0);
	// Shortening to a page that is not the one viewed brings the view back.
	dloop(r, 2), dloop(r, 2, 0);
	CHECK(r.view().s.ui.page == 1);
	r.m.edit([](tart::State& st) { st.clip[1][0].nNotes = 1, st.clip[1][0].notes[0] = {1000, 24, 36, 100}; });
	dloop(r, 0);
	dloop(r, 1), dloop(r, 1, 0);
	dloop(r, 0, 0);
	v = r.view();
	CHECK(v.length[1][0] == 768 && v.s.ui.page == 0);
	auto st = r.state();
	CHECK(st->clip[1][0].nNotes == 1); // the note past the new end stays
}

TEST(drum_delete_lane_and_page) {
	SliceRig r;
	drumTrack(r);
	r.m.edit([](tart::State& st) {
		tart::Clip& c = st.clip[0][0];
		c.length = 4 * tart::kBar;
		for (int k = 0; k < 4; k++) // notes on lanes 0 and 1 at ticks 0, 300, 600 and 900
			c.notes[c.nNotes++] = {(uint16_t)(k * 300), 24, 36, 100},
			c.notes[c.nNotes++] = {(uint16_t)(k * 300), 24, 37, 100};
		std::sort(c.notes, c.notes + c.nNotes, [](const tart::Note& a, const tart::Note& b) {
			return a.tick != b.tick ? a.tick < b.tick : a.pitch < b.pitch;
		});
		st.s.track[0].lane = 7;
	});
	r.press(push2::cc::Delete);
	dlane(r, 1); // no sound, the lane selected stays
	r.press(push2::cc::Delete, 0);
	auto v = r.view();
	CHECK(v.nNotes == 4 && v.s.track[0].lane == 7 && r.f.track[0].gate[1] == 0);
	for (int i = 0; i < v.nNotes; i++)
		CHECK(v.notes[i].pitch == 36);
	r.press(push2::cc::Delete); // a loop pad is a bar: 0..383 holds the notes at 0 and 300
	dloop(r, 0), dloop(r, 0, 0);
	r.press(push2::cc::Delete, 0);
	v = r.view();
	CHECK(v.nNotes == 2 && v.notes[0].tick == 600 && v.s.ui.page == 0 && v.length[0][0] == 4 * 384);
	r.press(push2::cc::Delete);
	dloop(r, 1), dloop(r, 1, 0); // 384..767: 600
	r.press(push2::cc::Delete, 0);
	v = r.view();
	CHECK(v.nNotes == 1 && v.notes[0].tick == 900);
	r.press(push2::cc::Delete); // an empty slot: Delete and a loop pad makes no clip
	r.click(push2::cc::Track1Below + 1);
	drumTrack(r, 1);
	dloop(r, 3), dloop(r, 3, 0);
	r.press(push2::cc::Delete, 0);
	CHECK(r.view().length[1][0] == 0);
}

TEST(drum_triplet_loop_pad) {
	Rig r;
	drumTrack(r);
	r.click(push2::cc::Scene8 + 7); // 1/32t: 8 ticks, a page of 32 steps is 256 ticks
	CHECK(r.view().s.track[0].grid == 7);
	dloop(r, 0), dloop(r, 0, 0); // 256 ticks round up to a bar
	CHECK(r.view().length[0][0] == 384);
	dloop(r, 5), dloop(r, 5, 0); // outside: it ends with the pad, 6 * 256
	CHECK(r.view().length[0][0] == 1536);
	dloop(r, 3), dloop(r, 3, 0); // inside: its page (768 ticks is page 3)
	CHECK(r.view().s.ui.page == 3);
	// A step: all 8 columns, 8 ticks each.
	dtap(r, 7);
	auto v = r.view();
	CHECK(v.nNotes == 1 && v.notes[0].tick == (3 * 32 + 7) * 8 && v.notes[0].len == 8);
	// The pads show the group of 16 that holds the viewed page: 16..31 from page 16 on.
	r.m.edit([](tart::State& st) {
		st.clip[0][0].length = tart::kMaxLength;
		st.s.ui.page = 17;
	});
	r.in.leds = true;
	r.step();
	auto led = [&](int row, int col) { return r.want.note[push2::padNote(row, col)].color; };
	CHECK(led(3, 4) == push2::kRgbLightGray && led(3, 5) == push2::kRgbWhite && led(3, 6) == push2::kRgbLightGray);
	CHECK(led(0, 7) == push2::kRgbLightGray); // pad 31, inside: 31 * 256 = 7936 < 12288
	r.m.edit([](tart::State& st) { st.clip[0][0].length = 20 * 256; });
	r.step();
	CHECK(led(3, 4) == push2::kRgbLightGray && led(3, 7) == push2::kRgbLightGray && led(2, 4) == push2::kBlack);
	dloop(r, 0), dloop(r, 0, 0); // pad 16 of the group: page 16
	CHECK(r.view().s.ui.page == 16);
	dloop(r, 15), dloop(r, 15, 0); // pad 31: 32 * 256 = 8192 bars... ends there
	CHECK(r.view().length[0][0] == 32 * 256);
}

TEST(drum_leds) {
	Rig r;
	drumTrack(r);
	r.m.edit([](tart::State& st) {
		tart::Clip& c = st.clip[0][0];
		c.length = 4 * tart::kBar;
		c.nNotes = 4;
		c.notes[0] = {0, 24, 37, 100};  // lane 1, step 0
		c.notes[1] = {24, 24, 37, 50};  // lane 1, step 1, soft
		c.notes[2] = {48, 24, 40, 100}; // lane 4
		c.notes[3] = {72, 24, 37, 100}; // lane 1, step 3
		st.s.track[0].lane = 1;
	});
	r.in.leds = true;
	r.step();
	auto led = [&](int row, int col) { return r.want.note[push2::padNote(row, col)].color; };
	// Lanes: the selected one white, with notes the track's colour, the rest dim.
	CHECK(led(0, 1) == push2::kRgbWhite && led(1, 0) == tart::kPaletteFull && led(0, 0) == tart::kPaletteDim);
	// Steps of lane 1: the top row is steps 1..8.
	CHECK(led(7, 0) == tart::kPaletteFull && led(7, 1) == tart::kPaletteDim && led(7, 2) == push2::kRgbDarkGray &&
		  led(7, 3) == tart::kPaletteFull && led(6, 0) == push2::kRgbDarkGray && led(4, 7) == push2::kRgbDarkGray);
	// Loop pads: 4 bars is pads 0..3, the viewed page (768 ticks) is pads 0 and 1.
	CHECK(led(3, 4) == push2::kRgbWhite && led(3, 5) == push2::kRgbWhite && led(3, 6) == push2::kRgbLightGray &&
		  led(3, 7) == push2::kRgbLightGray && led(2, 4) == push2::kBlack && led(0, 7) == push2::kBlack);
	// Past the end of a 1 bar clip: black, and the steps it holds.
	r.m.edit([](tart::State& st) { st.clip[0][0].length = tart::kBar; });
	r.step();
	CHECK(led(6, 7) == push2::kRgbDarkGray && led(5, 0) == push2::kBlack && led(4, 0) == push2::kBlack);
	CHECK(led(3, 4) == push2::kRgbWhite && led(3, 5) == push2::kBlack);
	// A held step is white; the page buttons and the step size follow the melodic sequencer.
	dstep(r, 3);
	r.step();
	CHECK(led(7, 3) == push2::kRgbWhite);
	dstep(r, 3, 0);
	CHECK(r.want.cc[push2::cc::Scene8 + 4].color == push2::kRgbWhite && r.want.cc[push2::cc::PageRight].color == 0);
	// The playhead: green on its step and its loop pad (a bar of 1/16: step 1 from tick 24).
	r.m.edit([](tart::State& st) { st.s.track[0].playing = 0; });
	r.in.play = true;
	r.run_(250 * 30); // tick 30
	r.step();
	CHECK(led(7, 1) == push2::kGreen && led(3, 4) == push2::kGreen && led(7, 0) == tart::kPaletteFull);
	// A lane that sounds is green: lane 4 at its note, tick 48.
	r.run_(250 * 20);
	r.step();
	CHECK(led(1, 0) == push2::kGreen);
}

TEST(drum_step_gates_slice_channel) {
	SliceRig r;
	drumTrack(r);
	dlane(r, 3);
	dtap(r, 1); // lane 3 at tick 24
	int up = -1;
	bool other = false;
	for (int i = 0; i < 7000; i++) {
		r.in.play = i == 0;
		r.step();
		for (int c = 0; c < 16; c++)
			if (r.f.track[0].gate[c] > 5)
				c == 3 ? up = up < 0 ? i : up : other = true;
	}
	CHECK(up == 24 * 250 && !other);
	CHECK_NEAR(r.f.track[0].voct[3], (39 - 60) / 12.f, 1e-6);
}

namespace {

/** Undo, or Shift + Undo (redo), as a Push press. */
void undoClick(Rig& r, bool redo = false) {
	if (redo)
		r.press(push2::cc::Shift);
	r.click(push2::cc::Undo);
	if (redo)
		r.press(push2::cc::Shift, 0);
}

} // namespace

TEST(undo_redo_steps) {
	Rig r;
	r.click(push2::cc::Layout);
	CHECK(r.want.cc[push2::cc::Undo].color == push2::kBlack); // nothing to undo: dark
	r.tap(0, 0), r.tap(0, 1), r.tap(0, 2);
	CHECK(r.view().nNotes == 3 && r.want.cc[push2::cc::Undo].color == push2::kWhiteDark);
	r.press(push2::cc::Undo);
	CHECK(r.want.cc[push2::cc::Undo].color == push2::kWhiteFull); // held
	r.press(push2::cc::Undo, 0);
	auto v = r.view();
	CHECK(v.nNotes == 2 && v.s.ui.popup == tart::kUndoPopup);
	r.press(push2::cc::Shift);
	CHECK(r.want.cc[push2::cc::Undo].color == push2::kWhiteDark); // there is something to redo
	r.press(push2::cc::Shift, 0);
	undoClick(r);
	CHECK(r.view().nNotes == 1);
	undoClick(r);
	v = r.view();
	CHECK(v.nNotes == 0 && v.length[0][0] == 0 && r.want.cc[push2::cc::Undo].color == push2::kBlack);
	undoClick(r); // nothing left
	CHECK(r.view().nNotes == 0);
	undoClick(r, true);
	v = r.view();
	CHECK(v.nNotes == 1 && v.length[0][0] == 768 && v.s.ui.popup == tart::kRedoPopup);
	undoClick(r, true), undoClick(r, true);
	CHECK(r.view().nNotes == 3);
	undoClick(r, true); // nothing more to redo
	CHECK(r.view().nNotes == 3);
	undoClick(r), undoClick(r);
	r.tap(1, 0); // a new edit drops what could be redone
	CHECK(r.view().nNotes == 2);
	undoClick(r, true);
	CHECK(r.view().nNotes == 2);
	undoClick(r);
	CHECK(r.view().nNotes == 1);
}

TEST(undo_hold_encoder_one_step) {
	Rig r;
	r.click(push2::cc::Layout);
	r.tap(0, 2); // tick 48
	r.pad(0, 2, 100);
	r.turn(0, 6), r.turn(0, 6), r.turn(1, 8), r.turn(3, 10);
	r.pad(0, 2, 0);
	auto v = r.view();
	CHECK(v.nNotes == 1 && v.notes[0].tick == 51 + 3 && v.notes[0].len == 48 && v.notes[0].vel == 110);
	undoClick(r); // all the turns at once
	v = r.view();
	CHECK(v.nNotes == 1 && v.notes[0].tick == 48 && v.notes[0].len == 24 && v.notes[0].vel == 100);
	undoClick(r, true);
	CHECK(r.view().notes[0].vel == 110);
	undoClick(r), undoClick(r); // the note itself
	CHECK(r.view().nNotes == 0);
	// A tap on a note takes it off: one step; so is a long note.
	r.tap(0, 0);
	r.tap(0, 0);
	CHECK(r.view().nNotes == 0);
	undoClick(r);
	CHECK(r.view().nNotes == 1);
	r.pad(0, 0, 100), r.tap(0, 3), r.pad(0, 0, 0);
	CHECK(r.view().notes[0].len == 4 * 24);
	undoClick(r);
	CHECK(r.view().notes[0].len == 24);
}

TEST(undo_delete_clip_and_row) {
	SliceRig r(false);
	setQuant(r, 0);
	for (int t = 0; t < 3; t++)
		fill(r, t, 0), fill(r, t, 1, 2);
	r.click(push2::cc::Scene1);
	r.run_(1000);
	r.press(push2::cc::Delete);
	tapSlot(r, 0, 0);
	r.run_(3);
	CHECK(r.view().length[0][0] == 0 && r.f.track[0].gate[0] == 0);
	undoClick(r); // the clip is back, stopped
	auto v = r.view();
	auto st = r.state();
	CHECK(v.length[0][0] == tart::kBar && v.s.track[0].playing == tart::kNone);
	CHECK(st->clip[0][0].nNotes == 1 && st->clip[0][0].notes[0].pitch == 60);
	r.press(push2::cc::Delete); // a row: one step for the three clips
	r.click(push2::cc::Scene1 - 1);
	r.press(push2::cc::Delete, 0);
	CHECK(r.view().length[0][1] == 0 && r.view().length[2][1] == 0);
	undoClick(r);
	v = r.view();
	CHECK(v.length[0][1] == 2 * tart::kBar && v.length[1][1] == 2 * tart::kBar && v.length[2][1] == 2 * tart::kBar);
	undoClick(r, true);
	v = r.view();
	CHECK(v.length[0][1] == 0 && v.length[1][1] == 0 && v.length[2][1] == 0 && v.length[0][0] == tart::kBar);
	undoClick(r);
	// Duplicate a row over the next scene, undone in one step.
	r.press(push2::cc::Duplicate);
	r.click(push2::cc::Scene1);
	r.press(push2::cc::Duplicate, 0);
	CHECK(r.view().length[0][1] == tart::kBar && r.view().length[1][1] == tart::kBar);
	undoClick(r);
	v = r.view();
	CHECK(v.length[0][1] == 2 * tart::kBar && v.length[1][1] == 2 * tart::kBar && v.length[2][1] == 2 * tart::kBar);
	// Duplicate a pad.
	r.press(push2::cc::Duplicate);
	tapSlot(r, 0, 0);
	tapSlot(r, 5, 5);
	r.press(push2::cc::Duplicate, 0);
	CHECK(r.view().length[5][5] == tart::kBar);
	undoClick(r);
	CHECK(r.view().length[5][5] == 0);
}

TEST(undo_capacity_32) {
	Rig r;
	r.click(push2::cc::Layout);
	fill(r, 0, 0, 8);
	for (int i = 0; i < 33; i++)
		r.tap(i / 8, i % 8);
	CHECK(r.view().nNotes == 34);
	for (int i = 0; i < 32; i++)
		undoClick(r);
	CHECK(r.view().nNotes == 2); // the oldest checkpoint is gone
	undoClick(r);
	CHECK(r.view().nNotes == 2);
	for (int i = 0; i < 32; i++)
		undoClick(r, true);
	CHECK(r.view().nNotes == 34);
}

TEST(undo_created_clip_stops_track) {
	Rig r;
	r.click(push2::cc::Layout);
	r.tap(0, 0);
	CHECK(r.view().s.track[0].playing == 0);
	undoClick(r);
	auto v = r.view();
	CHECK(v.length[0][0] == 0 && v.s.track[0].playing == tart::kNone && v.s.ui.heldStep == -1);
	// Running: the clip is only queued, and the launch is cancelled.
	r.click(push2::cc::Play);
	r.run_(10);
	r.click(push2::cc::Track1Below + 1);
	r.tap(0, 0);
	CHECK(r.view().s.track[1].queued == 0);
	undoClick(r);
	v = r.view();
	CHECK(v.length[1][0] == 0 && v.s.track[1].queued == tart::kNone);
	// A clip that plays: undoing its creation stops it at once.
	r.click(push2::cc::Track1Below + 2);
	r.tap(0, 0);
	r.run_(120000); // the next bar line
	CHECK(r.view().s.track[2].playing == 0);
	undoClick(r);
	CHECK(r.view().s.track[2].playing == tart::kNone);
}

TEST(undo_cleared_by_load) {
	Rig r;
	r.click(push2::cc::Layout);
	r.tap(0, 0), r.tap(0, 1);
	undoClick(r);
	auto st = r.state();
	r.m.load(*st);
	undoClick(r), undoClick(r, true);
	CHECK(r.view().nNotes == 1);
	CHECK(r.want.cc[push2::cc::Undo].color == push2::kBlack);
	r.tap(0, 2);
	undoClick(r);
	CHECK(r.view().nNotes == 1);
}

TEST(undo_drum_step) {
	Rig r;
	drumTrack(r);
	dlane(r, 5);
	dtap(r, 0), dtap(r, 9);
	undoClick(r);
	auto v = r.view();
	CHECK(v.nNotes == 1 && v.notes[0].tick == 0);
	undoClick(r, true);
	CHECK(r.view().nNotes == 2);
	r.press(push2::cc::Delete); // a lane
	dlane(r, 5);
	r.press(push2::cc::Delete, 0);
	CHECK(r.view().nNotes == 0);
	undoClick(r);
	CHECK(r.view().nNotes == 2);
	dstep(r, 9); // hold + encoder: one step
	r.turn(3, 10);
	dstep(r, 9, 0);
	CHECK(r.view().notes[1].vel == 110);
	undoClick(r);
	CHECK(r.view().notes[1].vel == 100);
}

TEST(undo_loop_selector_length) {
	Rig r;
	drumTrack(r);
	dloop(r, 0), dloop(r, 0, 0); // creates a bar
	dloop(r, 2), dloop(r, 2, 0); // ends the clip with page 2: 3 bars
	CHECK(r.view().length[0][0] == 3 * 384);
	undoClick(r);
	CHECK(r.view().length[0][0] == 384);
	undoClick(r, true);
	CHECK(r.view().length[0][0] == 3 * 384);
	undoClick(r), undoClick(r);
	auto v = r.view();
	CHECK(v.length[0][0] == 0 && v.s.track[0].playing == tart::kNone);
	// Delete + a loop pad
	fill(r, 0, 0, 4);
	r.press(push2::cc::Delete);
	dloop(r, 0), dloop(r, 0, 0);
	r.press(push2::cc::Delete, 0);
	CHECK(r.view().nNotes == 0);
	undoClick(r);
	CHECK(r.view().nNotes == 1);
}

TEST(undo_clip_mode_length) {
	Rig r(false);
	fill(r, 0, 0);
	r.press(push2::cc::Clip);
	r.turn(0, 4), r.turn(0, 4);
	CHECK(r.view().length[0][0] == 576);
	r.turn(0, 4); // still the same press of CLIP
	undoClick(r);
	CHECK(r.view().length[0][0] == tart::kBar);
	undoClick(r, true);
	CHECK(r.view().length[0][0] == 672);
}

// ---- Recording (RECORD, CC 86)

namespace {

/** Runs until the clock has played tick `tick`: the next sample is within a few hundredths of a tick of it. */
void runTo(Rig& r, int64_t tick) {
	while (r.view().s.transport.tick < tick)
		r.run_(10);
}

/** The notes of the selected clip. */
std::vector<tart::Note> notesOf(Rig& r) {
	tart::View v = r.view();
	return std::vector<tart::Note>(v.notes, v.notes + v.nNotes);
}

/** The pad at (0, 0) of Note mode (pitch 36) held from tick `from` to tick `to`. */
void hold(Rig& r, int64_t from, int64_t to, int col = 0, int vel = 100) {
	runTo(r, from);
	r.pad(0, col, vel);
	runTo(r, to);
	r.pad(0, col, 0);
}

/** A take on the empty slot 1/1 (open length, 1 Bar quantization) that has run to tick 100 with one note. */
void openTake(Rig& r) {
	r.click(push2::cc::Record);
	hold(r, 24, 48);
	runTo(r, 100);
}

} // namespace

TEST(record_new_clip) {
	Rig r;
	r.click(push2::cc::Record); // the transport is stopped: it starts, the clip launches at once
	auto v = r.view();
	CHECK(v.s.ui.recTrack == 0 && v.s.ui.recScene == 0 && v.s.ui.recOpen && v.s.track[0].playing == 0);
	CHECK(v.length[0][0] == tart::kMaxLength);
	r.run_(4);
	CHECK(r.view().s.transport.playing);
	hold(r, 24, 48);
	runTo(r, 60);
	r.run_(160); // 60.6 ticks: rounded to 61
	r.pad(0, 1, 90);
	runTo(r, 70);
	r.pad(0, 1, 0);
	runTo(r, 100);
	r.click(push2::cc::Record); // ends at the next whole bar: waiting
	v = r.view();
	CHECK(v.s.ui.recTrack == 0 && v.s.ui.recEnd == 384 && v.length[0][0] == tart::kMaxLength);
	runTo(r, 383);
	CHECK(r.view().s.ui.recTrack == 0);
	runTo(r, 384);
	v = r.view();
	CHECK(v.s.ui.recTrack == -1 && v.length[0][0] == 384 && v.s.track[0].playing == 0); // the clip plays on
	CHECK(v.nNotes == 2);
	CHECK(v.notes[0].tick == 24 && v.notes[0].len == 24 && v.notes[0].pitch == 36 && v.notes[0].vel == 100);
	CHECK(v.notes[1].tick == 61 && v.notes[1].len == 9 && v.notes[1].pitch == 38 && v.notes[1].vel == 90);
}

TEST(record_unit_is_at_least_a_beat) {
	Rig r;
	setQuant(r, 0); // None: a beat
	r.click(push2::cc::Record);
	runTo(r, 100);
	r.click(push2::cc::Record);
	CHECK(r.view().s.ui.recEnd == 192);
	runTo(r, 192);
	CHECK(r.view().length[0][0] == 192 && r.view().s.ui.recTrack == -1);
	Rig q;
	setQuant(q, 1); // 8 Bars
	q.click(push2::cc::Record);
	runTo(q, 100);
	q.click(push2::cc::Record);
	CHECK(q.view().s.ui.recEnd == 3072);
}

TEST(record_overdub_wraps) {
	Rig r;
	fill(r, 0, 0); // a bar with a note at 0, stopped
	r.click(push2::cc::Record);
	auto v = r.view();
	CHECK(v.s.ui.recTrack == 0 && !v.s.ui.recOpen && v.s.track[0].playing == 0 && v.length[0][0] == 384);
	hold(r, 400, 410);          // 16 into the second pass
	r.click(push2::cc::Record); // overdub ends at once
	v = r.view();
	CHECK(v.s.ui.recTrack == -1 && v.length[0][0] == 384 && v.s.track[0].playing == 0);
	CHECK(v.nNotes == 2 && v.notes[0].tick == 0 && v.notes[0].pitch == 60);
	CHECK(v.notes[1].tick == 16 && v.notes[1].len == 10 && v.notes[1].pitch == 36);
}

TEST(record_fixed_length_loops) {
	Rig r;
	r.m.edit([](tart::State& st) { st.s.ui.fixedOn = true, st.s.ui.fixedBars = 2; }); // 1 Bar
	r.click(push2::cc::Record);
	auto v = r.view();
	CHECK(v.length[0][0] == 384 && !v.s.ui.recOpen && v.s.ui.recTrack == 0 && v.s.track[0].playing == 0);
	hold(r, 100, 110);
	hold(r, 384 + 200, 384 + 210, 1);     // the second pass: another pad, another place
	hold(r, 768 + 100, 768 + 105, 0, 50); // the third: the same tick and pitch as the first, which stays
	runTo(r, 900);
	v = r.view();
	CHECK(v.s.ui.recTrack == 0 && v.length[0][0] == 384 && v.nNotes == 2); // still taking
	CHECK(v.notes[0].tick == 100 && v.notes[0].len == 10 && v.notes[0].vel == 100);
	CHECK(v.notes[1].tick == 200 && v.notes[1].pitch == 38);
	r.click(push2::cc::Record);
	v = r.view();
	CHECK(v.s.ui.recTrack == -1 && v.length[0][0] == 384 && v.s.track[0].playing == 0);
	// Fixed Length on, but a slot with a clip: an overdub of that clip
	Rig q;
	q.m.edit([](tart::State& st) { st.s.ui.fixedOn = true, st.s.ui.fixedBars = 0; });
	fill(q, 0, 0, 2);
	q.click(push2::cc::Record);
	CHECK(q.view().length[0][0] == 768 && !q.view().s.ui.recFresh);
}

TEST(record_waits_for_the_launch) {
	Rig r;
	r.press(push2::cc::Play), r.press(push2::cc::Play, 0);
	runTo(r, 100); // running: the launch waits for the next bar
	r.click(push2::cc::Record);
	auto v = r.view();
	CHECK(v.s.ui.recTrack == 0 && v.s.track[0].playing == tart::kNone && v.s.track[0].queued == 0);
	r.in.leds = true;
	r.step();
	CHECK(r.want.cc[push2::cc::Record].channel == push2::kBlinkHalf &&
		  r.want.cc[push2::cc::Record].base == push2::kRed && r.want.cc[push2::cc::Record].color == push2::kBlack);
	hold(r, 110, 120); // not recorded: the clip has not begun
	runTo(r, 384);
	v = r.view();
	CHECK(v.s.track[0].playing == 0 && v.nNotes == 0);
	r.in.leds = true;
	r.step();
	CHECK(r.want.cc[push2::cc::Record].channel == 0 && r.want.cc[push2::cc::Record].color == push2::kRed);
	hold(r, 400, 410);
	CHECK(r.view().nNotes == 1 && r.view().notes[0].tick == 16);
	// RECORD while waiting cancels: a clip the take made goes again
	Rig q;
	q.press(push2::cc::Play), q.press(push2::cc::Play, 0);
	runTo(q, 100);
	q.click(push2::cc::Record);
	q.click(push2::cc::Record);
	v = q.view();
	CHECK(v.s.ui.recTrack == -1 && v.s.track[0].queued == tart::kNone && v.length[0][0] == 0);
	// an existing clip: only the launch is canceled
	fill(q, 0, 0);
	q.click(push2::cc::Record);
	CHECK(q.view().s.track[0].queued == 0);
	q.click(push2::cc::Record);
	v = q.view();
	CHECK(v.s.ui.recTrack == -1 && v.s.track[0].queued == tart::kNone && v.length[0][0] == 384);
}

TEST(record_ended_by_new) {
	Rig r;
	openTake(r);
	r.click(push2::cc::New);
	auto v = r.view();
	CHECK(v.s.ui.recTrack == -1 && v.length[0][0] == 384 && r.state()->clip[0][0].nNotes == 1);
	CHECK(v.s.track[0].queued == tart::kStopQueued && v.s.ui.scene == 1);
}

TEST(record_ended_by_transport_stop) {
	Rig r;
	openTake(r);
	r.click(push2::cc::Play);
	r.run_(3);
	auto v = r.view();
	CHECK(!v.s.transport.playing && v.s.ui.recTrack == -1 && v.length[0][0] == 384 && v.nNotes == 1);
	CHECK(v.s.track[0].playing == tart::kNone);
}

TEST(record_ended_by_launch) {
	Rig r(false);
	fill(r, 0, 1);
	r.click(push2::cc::Note);
	r.click(push2::cc::Record);
	hold(r, 24, 48);
	runTo(r, 100);
	r.click(push2::cc::Session);
	tapSlot(r, 0, 1); // another clip of the track
	auto v = r.view();
	CHECK(v.s.ui.recTrack == -1 && v.length[0][0] == 384 && v.s.track[0].queued == 1);
	// a launch of a clip on another track does not end it
	Rig q(false);
	fill(q, 1, 0);
	q.click(push2::cc::Note);
	q.click(push2::cc::Record);
	q.click(push2::cc::Session);
	tapSlot(q, 1, 0);
	CHECK(q.view().s.ui.recTrack == 0);
	// a stop of the track ends it too
	q.press(push2::cc::StopClip);
	q.click(push2::cc::Track1Below);
	q.press(push2::cc::StopClip, 0);
	CHECK(q.view().s.ui.recTrack == -1);
}

TEST(record_ended_by_delete) {
	Rig r(false);
	r.click(push2::cc::Note);
	openTake(r);
	r.click(push2::cc::Session);
	r.press(push2::cc::Delete);
	tapSlot(r, 0, 0);
	r.press(push2::cc::Delete, 0);
	auto v = r.view();
	CHECK(v.s.ui.recTrack == -1 && v.length[0][0] == 0 && v.s.track[0].playing == tart::kNone);
	// a take that has not begun: its slot deleted
	Rig q(false);
	q.press(push2::cc::Play), q.press(push2::cc::Play, 0);
	runTo(q, 100);
	q.click(push2::cc::Record);
	q.press(push2::cc::Delete);
	tapSlot(q, 0, 0);
	q.press(push2::cc::Delete, 0);
	v = q.view();
	CHECK(v.s.ui.recTrack == -1 && v.length[0][0] == 0 && v.s.track[0].queued == tart::kNone);
	// Duplicate over the slot of a take ends it too
	Rig d(false);
	fill(d, 0, 0);
	fill(d, 1, 3);
	d.click(push2::cc::Note);
	d.click(push2::cc::Record);
	d.click(push2::cc::Session);
	d.press(push2::cc::Duplicate);
	tapSlot(d, 1, 3);
	tapSlot(d, 0, 0);
	d.press(push2::cc::Duplicate, 0);
	CHECK(d.view().s.ui.recTrack == -1);
}

TEST(record_ended_by_undo) {
	Rig r;
	openTake(r);
	r.click(push2::cc::Undo);
	auto v = r.view();
	CHECK(v.s.ui.recTrack == -1 && v.length[0][0] == 0 && v.nNotes == 0 && v.s.track[0].playing == tart::kNone);
	// an overdub: the take's notes are undone, the clip is as it was
	Rig q;
	fill(q, 0, 0);
	q.click(push2::cc::Record);
	hold(q, 100, 110);
	CHECK(q.view().nNotes == 2);
	q.click(push2::cc::Undo);
	v = q.view();
	CHECK(v.s.ui.recTrack == -1 && v.nNotes == 1 && v.notes[0].pitch == 60 && v.s.track[0].playing == 0);
}

TEST(record_caps_32_bars) {
	Rig r;
	r.m.edit([](tart::State& st) { st.s.transport.bpm = 300; });
	r.click(push2::cc::Record);
	hold(r, 12000, 12100);
	runTo(r, 12287);
	CHECK(r.view().s.ui.recTrack == 0);
	runTo(r, 12288);
	auto v = r.view();
	CHECK(v.s.ui.recTrack == -1 && v.length[0][0] == tart::kMaxLength && v.s.track[0].playing == 0);
	CHECK(v.nNotes == 1 && v.notes[0].tick == 12000);
	// RECORD late in the last bar: the end is capped at 32 bars
	Rig q;
	q.m.edit([](tart::State& st) {
		st.s.transport.bpm = 300;
		st.s.transport.quant = 1;
	});
	q.click(push2::cc::Record);
	runTo(q, 3100);
	q.click(push2::cc::Record);
	CHECK(q.view().s.ui.recEnd == 6144);
}

TEST(record_take_is_one_undo) {
	Rig r;
	fill(r, 0, 0);
	r.click(push2::cc::Record);
	hold(r, 50, 60), hold(r, 100, 110, 1), hold(r, 150, 160, 2);
	r.click(push2::cc::Record);
	CHECK(r.view().nNotes == 4);
	undoClick(r);
	CHECK(r.view().nNotes == 1);
	undoClick(r, true);
	CHECK(r.view().nNotes == 4);
	// an open take on an empty slot: one undo takes the clip away
	Rig q;
	openTake(q);
	q.click(push2::cc::Record);
	runTo(q, 384);
	CHECK(q.view().length[0][0] == 384);
	undoClick(q);
	CHECK(q.view().length[0][0] == 0 && q.view().s.track[0].playing == tart::kNone);
	undoClick(q, true);
	CHECK(q.view().length[0][0] == 384 && q.view().nNotes == 1);
}

TEST(record_held_pad_across_the_end) {
	// a pad still held when the take ends is recorded when you let go, with its real length
	Rig r;
	fill(r, 0, 0);
	r.click(push2::cc::Record);
	runTo(r, 50);
	r.pad(0, 1, 100);
	runTo(r, 60);
	r.click(push2::cc::Record);
	CHECK(r.view().s.ui.recTrack == -1);
	runTo(r, 80);
	r.pad(0, 1, 0);
	auto n = notesOf(r);
	CHECK(n.size() == 2 && n[1].tick == 50 && n[1].len == 30 && n[1].pitch == 38);
	// Undo ends the take and throws such a note away
	Rig q;
	fill(q, 0, 0);
	q.click(push2::cc::Record);
	runTo(q, 50);
	q.pad(0, 1, 100);
	q.click(push2::cc::Undo);
	runTo(q, 80);
	q.pad(0, 1, 0);
	CHECK(q.view().nNotes == 1);
	// an open take: the held pad of the end is inside the clip it made
	Rig o;
	o.click(push2::cc::Record);
	runTo(o, 30);
	o.pad(0, 0, 100);
	o.click(push2::cc::Record);
	runTo(o, 384);
	CHECK(o.view().length[0][0] == 384);
	runTo(o, 400);
	o.pad(0, 0, 0);
	auto m = notesOf(o);
	CHECK(m.size() == 1 && m[0].tick == 30 && m[0].len == 370);
}

TEST(record_only_the_takes_track) {
	Rig r;
	r.click(push2::cc::Record);
	r.click(push2::cc::Track1Below + 1); // another track is selected: its pads are not recorded
	hold(r, 24, 48);
	CHECK(r.view().nNotes == 0 && r.view().s.ui.recTrack == 0);
	r.click(push2::cc::Track1Below); // back
	r.pad(0, 0, 100);
	r.click(push2::cc::Track1Below + 1); // selected away while held: still recorded when let go
	runTo(r, 70);
	r.pad(0, 0, 0);
	r.click(push2::cc::Track1Below);
	CHECK(r.view().nNotes == 1);
	r.click(push2::cc::Layout); // the sequencer's pads are steps, not played
	r.tap(0, 0);
	CHECK(r.view().nNotes == 2);
	CHECK(r.state()->clip[1][0].length == 0);
}

TEST(record_drum_lane) {
	Rig r;
	drumTrack(r);
	r.click(push2::cc::Record);
	runTo(r, 48);
	r.pad(1, 1, 90); // lane 5: note 41
	runTo(r, 72);
	r.pad(1, 1, 0);
	r.press(push2::cc::Select); // selects the lane without playing it
	r.tap(0, 3);
	r.press(push2::cc::Select, 0);
	dstep(r, 3), dstep(r, 3, 0); // a step of the selected lane 4 is edited, not played (and it joins the take's clip)
	auto n = notesOf(r);
	CHECK(n.size() == 2);
	CHECK(n[0].tick == 48 && n[0].len == 24 && n[0].pitch == 41 && n[0].vel == 90);
	CHECK(n[1].tick == 72 && n[1].pitch == 39);
	r.click(push2::cc::Record);
	runTo(r, 384);
	CHECK(r.view().length[0][0] == 384 && r.view().s.ui.recTrack == -1);
}

TEST(record_clip_full) {
	Rig r;
	r.m.edit([](tart::State& st) {
		tart::Clip& c = st.clip[0][0];
		c.length = 32 * tart::kBar;
		for (int i = 0; i < tart::kMaxNotes; i++)
			c.notes[c.nNotes++] = {(uint16_t)(i * 3), 1, 100, 100};
	});
	r.click(push2::cc::Record);
	hold(r, 24, 48);
	auto v = r.view();
	CHECK(v.nNotes == tart::kMaxNotes && v.s.ui.popup == tart::kClipFullPopup);
}

TEST(record_leds) {
	Rig r;
	auto leds = [&]() {
		r.in.leds = true;
		r.step();
	};
	leds();
	CHECK(r.want.cc[push2::cc::Record].color == push2::kWhiteDark && r.want.cc[push2::cc::Record].channel == 0);
	drumTrack(r);
	r.click(push2::cc::Record);
	runTo(r, 10);
	leds();
	CHECK(r.want.cc[push2::cc::Record].color == push2::kRed && r.want.cc[push2::cc::Record].channel == 0);
	// a pressed lane pad and the playhead are red during the take, green outside it
	r.pad(0, 0, 100);
	leds();
	CHECK(r.want.note[push2::padNote(0, 0)].color == push2::kRed);
	CHECK(r.want.note[push2::padNote(7, 0)].color == push2::kRed); // the playhead on step 1 of the page
	r.pad(0, 0, 0);
	r.click(push2::cc::Session);
	leds();
	auto pad = r.want.note[push2::padNote(7, 0)];
	CHECK(pad.channel == push2::kPulseHalf && pad.color == push2::kRed && pad.base == tart::kPaletteFull);
	// waiting to end: the button blinks
	r.click(push2::cc::Record);
	leds();
	CHECK(r.want.cc[push2::cc::Record].channel == push2::kBlinkHalf);
	runTo(r, 384);
	r.click(push2::cc::Note);
	r.pad(0, 0, 100);
	leds();
	CHECK(r.want.note[push2::padNote(0, 0)].color == push2::kGreen);
	CHECK(r.want.cc[push2::cc::Record].color == push2::kWhiteDark);
	r.pad(0, 0, 0);
	// a queued take: the pad blinks red and black
	Rig q(false);
	q.press(push2::cc::Play), q.press(push2::cc::Play, 0);
	runTo(q, 100);
	q.click(push2::cc::Record);
	q.in.leds = true;
	q.step();
	pad = q.want.note[push2::padNote(7, 0)];
	CHECK(pad.channel == push2::kBlinkQuarter && pad.color == push2::kBlack && pad.base == push2::kRed);
}

TEST(record_state_is_transient) {
	auto st = std::make_unique<tart::State>();
	st->s.ui.recTrack = 3, st->s.ui.recOpen = st->s.ui.recFresh = true, st->s.ui.recEnd = 99;
	tart::sanitize(*st);
	CHECK(st->s.ui.recTrack == -1 && !st->s.ui.recOpen && !st->s.ui.recFresh && st->s.ui.recEnd == -1);
}

namespace {

/** The clip of slot (0, 0): `length` ticks and the notes. */
void setNotes(Rig& r, int length, std::initializer_list<tart::Note> notes) {
	r.m.edit([=](tart::State& st) {
		tart::Clip& c = st.clip[0][0];
		c = tart::Clip();
		c.length = (uint16_t)length;
		for (const tart::Note& n : notes)
			c.notes[c.nNotes++] = n;
	});
}
void setQuantize(Rig& r, int to, int amount, int recTo = 4, bool recOn = false) {
	r.m.edit([=](tart::State& st) {
		st.s.ui.quantTo = (uint8_t)to, st.s.ui.quantAmount = (uint8_t)amount, st.s.ui.recQuant = (uint8_t)recTo;
		st.s.ui.recQuantOn = recOn;
	});
}
/** Quantize held for `seconds`, with the Push's LEDs refreshed on the way, then let go. */
void holdQuantize(Rig& r, double seconds) {
	r.press(push2::cc::Quantize);
	r.in.leds = true;
	r.run_((int)(48000 * seconds));
}
std::string popupOf(Rig& r) {
	char buf[64];
	tart::popupText(r.view(), buf, sizeof buf);
	return buf;
}

} // namespace

TEST(quantize_clip) {
	Rig r;
	setNotes(r, 384, {{5, 10, 60, 100}, {30, 10, 61, 100}, {50, 10, 62, 100}});
	r.click(push2::cc::Quantize); // 1/16 at 100 %
	auto v = r.view();
	CHECK(v.nNotes == 3 && v.notes[0].tick == 0 && v.notes[1].tick == 24 && v.notes[2].tick == 48);
	CHECK(v.notes[0].len == 10 && v.notes[1].len == 10); // the ends move with the starts: lengths stay
	CHECK(v.s.ui.popup == tart::kQuantizedPopup && popupOf(r) == "Quantized to: 1/16, 100%");
	setNotes(r, 384, {{5, 10, 60, 100}});
	setQuantize(r, 4, 50);
	r.click(push2::cc::Quantize);
	CHECK(r.view().notes[0].tick == 3 && popupOf(r) == "Quantized to: 1/16, 50%");
	setNotes(r, 384, {{30, 10, 60, 100}, {10, 10, 61, 100}});
	setQuantize(r, 3, 100); // 1/8+T: the nearer of the 8th and the triplet 8th
	r.click(push2::cc::Quantize);
	v = r.view();
	CHECK(v.notes[0].tick == 0 && v.notes[0].pitch == 61 && v.notes[1].tick == 32 && v.notes[1].pitch == 60);
	CHECK(popupOf(r) == "Quantized to: 1/8+T, 100%");
	setNotes(r, 384, {{20, 10, 60, 100}, {380, 10, 61, 100}}); // 380 is nearest the line at the end: tick 0
	setQuantize(r, 4, 100);
	r.click(push2::cc::Quantize);
	v = r.view();
	CHECK(v.nNotes == 2 && v.notes[0].tick == 0 && v.notes[0].pitch == 61 && v.notes[1].tick == 24);
	setNotes(r, 384,
			 {{0, 10, 60, 100}, {5, 10, 60, 90}, {5, 10, 61, 100}, {500, 10, 60, 100}}); // collision; one past the end
	r.click(push2::cc::Quantize);
	v = r.view();
	CHECK(v.nNotes == 3 && v.notes[0].tick == 0 && v.notes[0].pitch == 60 && v.notes[1].tick == 0 &&
		  v.notes[1].pitch == 61 && v.notes[2].tick == 500);
	r.click(push2::cc::Undo); // one undo step brings every note back
	v = r.view();
	CHECK(v.nNotes == 4 && v.notes[0].tick == 0 && v.notes[1].tick == 5 && v.notes[2].tick == 5);
	setNotes(r, 0, {}); // an empty slot: nothing happens
	r.m.edit([](tart::State& st) { st.s.ui.popup = tart::kNoPopup; });
	r.click(push2::cc::Quantize);
	CHECK(r.view().s.ui.popup == tart::kNoPopup);
}

TEST(quantize_hold_no_tap) {
	Rig r;
	setNotes(r, 384, {{5, 10, 60, 100}});
	holdQuantize(r, 0.35); // the settings show while held; letting go does nothing
	r.step();
	CHECK(tart::quantizeSettings(r.view().s.ui) && r.want.cc[push2::cc::Quantize].color == push2::kWhiteFull);
	r.press(push2::cc::Quantize, 0);
	CHECK(r.view().notes[0].tick == 5 && r.view().s.ui.popup == tart::kNoPopup);
	CHECK(!tart::quantizeSettings(r.view().s.ui));
	holdQuantize(r, 0.1); // a short press is not the settings
	CHECK(!tart::quantizeSettings(r.view().s.ui));
	r.press(push2::cc::Quantize, 0);
	CHECK(r.view().notes[0].tick == 0); // a tap
	setNotes(r, 384, {{5, 10, 60, 100}});
	r.press(push2::cc::Quantize); // an encoder turned meanwhile cancels the tap
	r.turn(1, 8);
	r.press(push2::cc::Quantize, 0);
	CHECK(r.view().notes[0].tick == 5);
	r.press(push2::cc::Quantize); // so does a pad
	r.pad(0, 0, 100), r.pad(0, 0, 0);
	r.press(push2::cc::Quantize, 0);
	CHECK(r.view().notes[0].tick == 5);
	r.press(push2::cc::Quantize); // and the upper button 5
	r.press(push2::cc::Track1Above + 4), r.press(push2::cc::Track1Above + 4, 0);
	r.press(push2::cc::Quantize, 0);
	CHECK(r.view().notes[0].tick == 5 && r.view().s.ui.recQuantOn);
	r.click(push2::cc::Quantize); // the next press is a tap again
	CHECK(r.view().notes[0].tick == 0);
}

TEST(quantize_settings) {
	Rig r;
	r.press(push2::cc::Quantize);
	r.turn(0, 4); // SWING: the global swing, 2 raw steps for a percent
	CHECK(r.view().s.transport.swing == 2);
	r.turn(1, 8), r.turn(1, 8); // QUANTIZE TO: 8 steps for a choice, from 1/16
	CHECK(r.view().s.ui.quantTo == 6);
	r.turn(1, 40);
	CHECK(r.view().s.ui.quantTo == 7);
	r.turn(1, -60);
	CHECK(r.view().s.ui.quantTo == 0);
	r.turn(2, -20); // AMOUNT
	CHECK(r.view().s.ui.quantAmount == 90);
	r.turn(2, 30);
	CHECK(r.view().s.ui.quantAmount == 100);
	r.turn(4, -8); // REC QUANT
	CHECK(r.view().s.ui.recQuant == 3);
	r.turn(3, 8), r.turn(5, 8); // the other encoders do nothing
	CHECK(r.view().s.ui.quantTo == 0 && r.view().s.ui.quantAmount == 100 && r.view().s.ui.recQuant == 3);
	r.press(push2::cc::Track1Above + 4), r.press(push2::cc::Track1Above + 4, 0);
	CHECK(r.view().s.ui.recQuantOn);
	r.in.leds = true;
	r.run_((int)(48000 * 0.35));
	CHECK(r.want.cc[push2::cc::Track1Above + 4].color == push2::kRgbWhite);
	r.press(push2::cc::Track1Above + 4), r.press(push2::cc::Track1Above + 4, 0);
	r.step();
	CHECK(!r.view().s.ui.recQuantOn && r.want.cc[push2::cc::Track1Above + 4].color == push2::kRgbDarkGray);
	r.press(push2::cc::Quantize, 0);
	r.step();
	CHECK(r.want.cc[push2::cc::Track1Above + 4].color == push2::kBlack);
	r.turn(1, 8); // not held: Quantize To stays
	CHECK(r.view().s.ui.quantTo == 0);
	auto st = std::make_unique<tart::State>();
	st->s.ui.quantTo = 200, st->s.ui.quantAmount = 250, st->s.ui.recQuant = 9, st->s.ui.recQuantOn = true;
	st->s.ui.quantizeAt = 5, st->s.ui.quantizeUsed = true;
	tart::sanitize(*st);
	CHECK(st->s.ui.quantTo == 7 && st->s.ui.quantAmount == 100 && st->s.ui.recQuant == 7 && st->s.ui.recQuantOn);
	CHECK(st->s.ui.quantizeAt == -1 && !st->s.ui.quantizeUsed);
	const tart::Session fresh; // (not a whole State: 2 MB overflow the 1 MB stack of Windows)
	CHECK(fresh.ui.quantTo == 4 && fresh.ui.quantAmount == 100 && fresh.ui.recQuant == 4 && !fresh.ui.recQuantOn);
	CHECK(std::string(tart::kQuantToNames[4]) == "1/16" && std::string(tart::kQuantToNames[6]) == "1/16+T");
}

TEST(quantize_lane) {
	Rig r;
	drumTrack(r);
	setNotes(r, 384, {{5, 10, 36, 100}, {5, 10, 37, 100}, {30, 10, 37, 100}});
	r.press(push2::cc::Quantize);
	dlane(r, 1); // Quantize + a lane pad: that lane only, and it neither plays nor selects
	r.press(push2::cc::Quantize, 0);
	auto v = r.view();
	CHECK(v.nNotes == 3 && v.notes[0].tick == 0 && v.notes[0].pitch == 37 && v.notes[1].tick == 5 &&
		  v.notes[1].pitch == 36 && v.notes[2].tick == 24 && v.notes[2].pitch == 37);
	CHECK(v.s.track[0].lane == 0 && v.s.ui.popup == tart::kQuantizedPopup); // the release was no tap: still 36 at 5
	r.click(push2::cc::Quantize);                                           // a tap does all lanes
	CHECK(r.view().notes[0].tick == 0 && r.view().notes[1].tick == 0);
}

TEST(quantize_held_pads_do_nothing) {
	Rig r(false); // Session: a clip pad with Quantize held doesn't launch
	setNotes(r, 384, {{0, 24, 60, 100}});
	r.press(push2::cc::Quantize);
	r.pad(7, 0, 100);
	r.pad(7, 0, 0);
	r.press(push2::cc::Quantize, 0);
	auto v = r.view();
	CHECK(v.s.track[0].playing == tart::kNone && v.s.track[0].queued == tart::kNone && !v.s.transport.playing);
	Rig q; // the Melodic Sequencer: a step with Quantize held neither removes its note nor adds one
	q.m.edit([](tart::State& st) { st.s.ui.noteLayout = 1; });
	setNotes(q, 384, {{0, 24, 36, 100}});
	q.press(push2::cc::Quantize);
	q.pad(0, 0, 100);
	q.pad(0, 1, 100);
	q.press(push2::cc::Quantize, 0);
	q.pad(0, 0, 0);
	q.pad(0, 1, 0);
	v = q.view();
	CHECK(v.nNotes == 1 && v.notes[0].tick == 0 && v.notes[0].pitch == 36);
}

TEST(rec_quant) {
	Rig r;
	setQuantize(r, 4, 100, 7, true); // Record Quantize on, 1/32
	r.click(push2::cc::Record);
	hold(r, 30, 60);
	runTo(r, 70);
	auto v = r.view();
	CHECK(v.nNotes == 1 && v.notes[0].tick == 36 && v.notes[0].len >= 29 && v.notes[0].len <= 31);
	setQuantize(r, 4, 100, 4, false); // off: the played tick
	hold(r, 100, 120, 1);
	v = r.view();
	CHECK(v.nNotes == 2 && v.notes[1].tick >= 99 && v.notes[1].tick <= 101);
	setQuantize(r, 4, 100, 4, true); // 1/16: 140 is nearest 144
	hold(r, 140, 150, 2);
	v = r.view();
	CHECK(v.nNotes == 3 && v.notes[2].tick == 144);
	Rig w; // near the end of the clip: the line at the end is the start
	fill(w, 0, 0);
	setQuantize(w, 4, 100, 4, true);
	w.click(push2::cc::Record);
	hold(w, 380, 390);
	v = w.view();
	CHECK(v.nNotes == 2 && v.notes[0].tick == 0 && v.notes[0].pitch == 36 && v.notes[1].pitch == 60);
}

TEST(double_loop) {
	Rig r;
	setNotes(r, 384, {{0, 24, 60, 100}, {100, 10, 61, 90}, {400, 10, 62, 80}}); // the last starts past the end
	CHECK(r.want.cc[push2::cc::DoubleLoop].color == push2::kBlack);
	r.in.leds = true;
	r.step();
	CHECK(r.want.cc[push2::cc::DoubleLoop].color == push2::kWhiteDark);
	r.press(push2::cc::DoubleLoop);
	CHECK(r.want.cc[push2::cc::DoubleLoop].color == push2::kWhiteFull);
	r.press(push2::cc::DoubleLoop, 0);
	auto v = r.view();
	CHECK(v.length[0][0] == 768 && v.nNotes == 4 && v.s.ui.popup == tart::kLoopLengthPopup);
	CHECK(v.notes[0].tick == 0 && v.notes[1].tick == 100 && v.notes[2].tick == 384 && v.notes[3].tick == 484);
	CHECK(v.notes[3].len == 10 && v.notes[3].pitch == 61 && v.notes[3].vel == 90);
	CHECK(popupOf(r) == "New loop length: 2 Bars");
	r.click(push2::cc::DoubleLoop);
	CHECK(r.view().length[0][0] == 1536 && popupOf(r) == "New loop length: 4 Bars");
	r.click(push2::cc::Undo); // each is one undo step
	v = r.view();
	CHECK(v.length[0][0] == 768 && v.nNotes == 4);
	r.click(push2::cc::Undo);
	v = r.view();
	CHECK(v.length[0][0] == 384 && v.nNotes == 3 && v.notes[2].tick == 400); // the note past the end is back
	setNotes(r, 17 * 384, {{0, 24, 60, 100}});                               // 34 bars would be too long
	r.click(push2::cc::DoubleLoop);
	v = r.view();
	CHECK(v.length[0][0] == 17 * 384 && v.s.ui.popup == tart::kLoopTooLongPopup);
	CHECK(popupOf(r) == "Loop is 32 Bars at most");
	setNotes(r, 384, {});
	r.m.edit([](tart::State& st) {
		tart::Clip& c = st.clip[0][0];
		for (int i = 0; i < tart::kMaxNotes / 2 + 1; i++)
			c.notes[c.nNotes++] = {(uint16_t)(i % 384), 1, (uint8_t)(i / 384), 100};
		std::sort(c.notes, c.notes + c.nNotes, [](const tart::Note& a, const tart::Note& b) {
			return a.tick != b.tick ? a.tick < b.tick : a.pitch < b.pitch;
		});
	});
	r.click(push2::cc::DoubleLoop); // the notes would not fit
	v = r.view();
	CHECK(v.length[0][0] == 384 && v.nNotes == tart::kMaxNotes / 2 + 1 && v.s.ui.popup == tart::kClipFullPopup);
	r.m.edit([](tart::State& st) { st.clip[0][0].nNotes--; }); // exactly half: fits
	r.click(push2::cc::DoubleLoop);
	v = r.view();
	CHECK(v.length[0][0] == 768 && v.nNotes == tart::kMaxNotes);
	r.click(push2::cc::Track1Below + 1); // an empty slot: nothing
	r.m.edit([](tart::State& st) { st.s.ui.popup = tart::kNoPopup; });
	r.click(push2::cc::DoubleLoop);
	CHECK(r.view().length[1][0] == 0 && r.view().s.ui.popup == tart::kNoPopup);
	Rig t; // an open-length take sets the length: refused
	t.click(push2::cc::Record);
	t.click(push2::cc::DoubleLoop);
	CHECK(t.view().length[0][0] == tart::kMaxLength && t.view().s.ui.popup == tart::kNoPopup);
}

namespace {

/** The split layout (Sequencer + 32 Notes) in Note mode; steps are the top four rows, step k at pad (7 - k / 8, k % 8).
 */
void split(Rig& r) {
	r.click(push2::cc::Layout), r.click(push2::cc::Layout);
}
void sstep(Rig& r, int k, int vel = 100) {
	r.pad(7 - k / 8, k % 8, vel);
}
void stap(Rig& r, int k) {
	sstep(r, k), sstep(r, k, 0);
}

} // namespace

TEST(layout_cycles) {
	Rig r;
	int seen[4];
	for (int& n : seen)
		r.click(push2::cc::Layout), n = r.view().s.ui.noteLayout;
	CHECK(seen[0] == 1 && seen[1] == 2 && seen[2] == 0 && seen[3] == 1);
	auto st = std::make_unique<tart::State>();
	st->s.ui.noteLayout = 9;
	tart::sanitize(*st);
	CHECK(st->s.ui.noteLayout == 2); // the split layout is saved
	st->s.ui.chord[0] = 5;
	tart::sanitize(*st);
	CHECK(st->s.ui.chord[0] == 0); // the chord is not
}

TEST(split_chord_step) {
	Rig r;
	split(r);
	stap(r, 0); // the default chord: the bottom-left pad, 36
	auto v = r.view();
	CHECK(v.length[0][0] == 768 && v.nNotes == 1 && v.notes[0].tick == 0 && v.notes[0].len == 24 &&
		  v.notes[0].pitch == 36 && v.notes[0].vel == 100);
	stap(r, 0); // a step with notes: tapped off
	CHECK(r.view().nNotes == 0);
	r.pad(0, 1, 100), r.pad(0, 2, 100); // held together: 38 and 40
	r.pad(0, 1, 0), r.pad(0, 2, 0);
	stap(r, 1);
	auto n = notesOf(r);
	CHECK(n.size() == 2 && n[0].tick == 24 && n[0].pitch == 38 && n[1].tick == 24 && n[1].pitch == 40);
	r.tap(0, 4); // a lone press replaces the chord (43)
	stap(r, 2);
	n = notesOf(r);
	CHECK(n.size() == 3 && n[2].tick == 48 && n[2].pitch == 43);
	// hold a step, press a later one: a long note for each of its notes
	sstep(r, 1), stap(r, 5), sstep(r, 1, 0);
	n = notesOf(r);
	CHECK(n.size() == 3 && n[0].len == 5 * 24 + 24 - 24 && n[1].len == 5 * 24 + 24 - 24);
	stap(r, 1); // a tap on a stretched step still takes all its notes off
	CHECK(r.view().nNotes == 1);
	stap(r, 31); // the last step of the 2 Bar clip
	CHECK(r.view().nNotes == 2 && notesOf(r)[1].tick == 744);
	r.click(push2::cc::PageRight); // the page has 32 steps: one 2 Bar clip of 1/16 is 32 steps, no second page
	CHECK(r.view().s.ui.page == 0);
}

TEST(split_toggle_pitch) {
	Rig r;
	split(r);
	sstep(r, 3); // an empty step: the chord's note is added, the step stays held
	r.tap(0, 2); // 40 joins it
	r.tap(0, 4); // and 43
	auto n = notesOf(r);
	CHECK(n.size() == 3 && n[1].pitch == 40 && n[2].pitch == 43 && n[0].tick == 72);
	r.tap(0, 2); // pressed again: it goes
	r.tap(0, 0); // so does the chord's 36
	n = notesOf(r);
	CHECK(n.size() == 1 && n[0].pitch == 43);
	CHECK(r.view().s.ui.chord[0] == 0 && r.view().s.ui.chord[1] == 0); // the chord is untouched
	sstep(r, 3, 0);                                                    // the hold edited the step: it stays
	CHECK(r.view().nNotes == 1);
	sstep(r, 3), r.tap(0, 4), sstep(r, 3, 0); // the last note taken off: an empty step
	CHECK(r.view().nNotes == 0);
}

TEST(split_encoders_all_notes) {
	Rig r;
	split(r);
	r.pad(0, 0, 100), r.pad(0, 2, 100), r.pad(0, 0, 0), r.pad(0, 2, 0); // 36 and 40
	stap(r, 2);
	sstep(r, 2);
	auto v = r.view();
	CHECK(v.s.ui.heldStep == 7 * 8 + 2 && v.heldCount == 2 && v.held == 0);
	r.turn(3, 10); // Velocity
	r.turn(0, 6);  // Nudge
	r.turn(1, 8);  // Length: a step
	auto n = notesOf(r);
	CHECK(n.size() == 2 && n[0].vel == 110 && n[1].vel == 110 && n[0].tick == 51 && n[1].tick == 51 && n[0].len == 48 &&
		  n[1].len == 48);
	r.turn(2, 6); // Length fine
	n = notesOf(r);
	CHECK(n[0].len == 51 && n[1].len == 51);
	sstep(r, 2, 0);
	CHECK(r.view().nNotes == 2 && r.view().heldCount == 0);
}

TEST(split_records) {
	Rig r;
	split(r);
	r.click(push2::cc::Record);
	hold(r, 24, 48, 1); // the lower pads play and record: 38 at tick 24
	runTo(r, 72);
	stap(r, 5); // a step is edited, not played or recorded
	auto n = notesOf(r);
	CHECK(n.size() == 2 && n[0].tick == 24 && n[0].len == 24 && n[0].pitch == 38 && n[1].tick == 120);
	CHECK(r.view().s.ui.recTrack == 0);
}

TEST(split_leds) {
	Rig r;
	split(r);
	r.m.edit([](tart::State& st) {
		tart::Clip& c = st.clip[0][0];
		c.length = tart::kBar;
		c.nNotes = 3;
		c.notes[0] = {0, 24, 36, 100};
		c.notes[1] = {24, 24, 36, 50};
		c.notes[2] = {24, 24, 43, 100};
	});
	auto led = [&]() {
		r.in.leds = true;
		r.step();
	};
	auto at = [&](int row, int col) { return r.want.note[push2::padNote(row, col)].color; };
	led();
	// steps: velocity shading (the louder note of a step), empty dark gray, past the end black
	CHECK(at(7, 0) == tart::kPaletteFull && at(7, 1) == tart::kPaletteFull && at(7, 2) == push2::kRgbDarkGray &&
		  at(5, 0) == push2::kBlack);
	// the lower pads as Note mode, the default chord (36) dim, the others in scale white
	CHECK(at(0, 0) == tart::kPaletteDim && at(0, 1) == push2::kRgbWhite && at(0, 7) != tart::kPaletteDim);
	r.pad(0, 1, 100), r.pad(0, 2, 100); // pressed pads are green, and become the chord
	led();
	CHECK(at(0, 1) == push2::kGreen && at(0, 2) == push2::kGreen);
	r.pad(0, 1, 0), r.pad(0, 2, 0);
	led();
	CHECK(at(0, 1) == tart::kPaletteDim && at(0, 2) == tart::kPaletteDim && at(0, 0) == tart::kPaletteFull);
	sstep(r, 1); // a held step: white, and its notes (36, 43) dim below instead of the chord
	led();
	CHECK(at(7, 1) == push2::kRgbWhite && at(0, 0) == tart::kPaletteDim && at(0, 4) == tart::kPaletteDim &&
		  at(0, 1) == push2::kRgbWhite);
	sstep(r, 1, 0);
}

TEST(split_undo) {
	Rig r;
	split(r);
	r.pad(0, 0, 100), r.pad(0, 2, 100), r.pad(0, 0, 0), r.pad(0, 2, 0);
	stap(r, 0); // creates the clip
	CHECK(r.view().nNotes == 2);
	sstep(r, 0);
	r.turn(3, 10);
	r.turn(3, 10); // two turns of a hold: one step for both notes
	sstep(r, 0, 0);
	CHECK(r.view().notes[0].vel == 120 && r.view().notes[1].vel == 120);
	r.click(push2::cc::Undo);
	auto n = notesOf(r);
	CHECK(n.size() == 2 && n[0].vel == 100 && n[1].vel == 100);
	r.click(push2::cc::Undo); // the creation: the clip is gone again
	CHECK(r.view().nNotes == 0 && r.view().length[0][0] == 0);
	undoClick(r, true), undoClick(r, true);
	CHECK(r.view().nNotes == 2);
}

namespace {

/** Lane `lane` of the clip in slot (0, 0): a 1-bar clip with these points. */
void setPoints(Rig& r, int lane, std::initializer_list<tart::Point> pts, int length = tart::kBar) {
	r.m.edit([=](tart::State& st) {
		tart::Clip& c = st.clip[0][0];
		c.length = (uint16_t)length;
		c.nPoints[lane] = 0;
		for (const tart::Point& p : pts)
			c.points[lane][c.nPoints[lane]++] = p;
	});
}

} // namespace

TEST(envelope_semantics) {
	auto c = std::make_unique<tart::Clip>();
	CHECK(tart::envelope(*c, 0, 10) == -1);
	c->nPoints[0] = 4;
	c->points[0][0] = {10, 100}, c->points[0][1] = {20, 200}, c->points[0][2] = {20, 1000}, c->points[0][3] = {30, 500};
	CHECK(tart::envelope(*c, 0, 0) == 100 &&
		  tart::envelope(*c, 0, 10) == 100); // the first value before the first point
	CHECK(tart::envelope(*c, 0, 15) == 150);
	CHECK(tart::envelope(*c, 0, 20, true) == 200 && tart::envelope(*c, 0, 20) == 1000); // a jump: arriving, leaving
	CHECK(tart::envelope(*c, 0, 19) == 190 && tart::envelope(*c, 0, 25) == 750);
	CHECK(tart::envelope(*c, 0, 30) == 500 && tart::envelope(*c, 0, 9999) == 500); // the last value after the last
	c->nPoints[1] = 2; // the 64-bit interpolation: the full range over many ticks
	c->points[1][0] = {0, 0}, c->points[1][1] = {12000, 65535};
	CHECK(tart::envelope(*c, 1, 6000) == 32767 && tart::envelope(*c, 1, 11999) == 65529);
	tart::Track t;
	CHECK_NEAR(tart::volts(t, 0, 65535), 10.f, 1e-5);
	CHECK_NEAR(tart::volts(t, tart::kLevel, 0), 0.f, 1e-6);
	CHECK_NEAR(tart::volts(t, tart::kPan, 0), -5.f, 1e-5);
	CHECK_NEAR(tart::volts(t, tart::kPan, 32768), 0.f, 1e-6);
	t.bipolar = 1 << 3;
	CHECK_NEAR(tart::volts(t, 3, 0), -5.f, 1e-5);
	CHECK_NEAR(tart::volts(t, 3, 32768), 0.f, 1e-6);
	CHECK_NEAR(tart::volts(t, 2, 32768), 5.f, 1e-3); // another CC is still unipolar
}

TEST(automation_playback_sample_exact) {
	SliceRig r(false);
	fill(r, 0, 0);
	setPoints(r, 0, {{0, 0}, {96, 65535}}, 4 * tart::kPpqn);
	setPoints(r, tart::kLevel, {{0, 65535}, {96, 0}}, 4 * tart::kPpqn);
	r.m.edit([](tart::State& st) { st.s.track[0].playing = 0; }); // playing, the transport stopped: Play starts at 0
	r.in.play = true;
	float cc0 = -1, ccMid = -1, ccMid2 = -1, level = -1;
	for (int i = 0; i <= 12000 + 200; i++) {
		r.step();
		if (i == 0)
			cc0 = r.f.track[0].cc[0];
		if (i == 12000)
			ccMid = r.f.track[0].cc[0], level = r.f.track[0].level;
		if (i == 12000 + 125) // half a tick on (one tick is 250 samples)
			ccMid2 = r.f.track[0].cc[0];
	}
	CHECK(r.view().s.transport.playing);
	CHECK_NEAR(cc0, 0.f, 1e-6);
	CHECK_NEAR(ccMid, 5.f, 0.01);
	CHECK_NEAR(level, 5.f, 0.01);
	CHECK_NEAR(ccMid2, 5.f + 10.f / 96 / 2, 0.01); // between two ticks it is interpolated
	CHECK(r.f.track[0].cc[1] == 0 && r.f.track[1].level == 10 && r.f.track[0].pan == 0); // other lanes and tracks
	r.run_(20000); // past the last point: held at 10 V; the value stays when the clip stops
	CHECK_NEAR(r.f.track[0].cc[0], 10.f, 0.01);
	r.click(push2::cc::Play);
	r.run_(100);
	CHECK_NEAR(r.f.track[0].cc[0], 10.f, 0.01);
	CHECK(r.state()->s.track[0].value[0] == 65535);
}

TEST(automation_value_without_points) {
	SliceRig r;
	r.m.edit([](tart::State& st) {
		tart::Track& t = st.s.track[1];
		t.value[0] = 65535, t.value[1] = 32768, t.bipolar = 1 << 1, t.value[tart::kLevel] = 0,
		t.value[tart::kPan] = 65535;
	});
	r.run_(3);
	const tart::SliceTrack& s = r.f.track[1];
	CHECK_NEAR(s.cc[0], 10.f, 1e-4);
	CHECK_NEAR(s.cc[1], 0.f, 1e-6); // bipolar: the middle is 0 V
	CHECK_NEAR(s.level, 0.f, 1e-6);
	CHECK_NEAR(s.pan, 5.f, 1e-3);
	r.m.edit([](tart::State& st) { st.s.track[1].value[1] = 0; });
	r.run_(3);
	CHECK_NEAR(s.cc[1], -5.f, 1e-4);
	CHECK(r.f.track[0].level == 10 && r.f.track[0].pan == 0 && r.f.track[0].cc[1] == 0);
}

TEST(sanitize_automation) {
	auto st = std::make_unique<tart::State>();
	CHECK(st->s.track[3].value[tart::kLevel] == 65535 && st->s.track[3].value[tart::kPan] == 32768);
	tart::Clip& c = st->clip[0][0];
	c.length = 384;
	c.nPoints[0] = 5;
	c.points[0][0] = {300, 5}, c.points[0][1] = {100, 6}, c.points[0][2] = {20000, 7}, c.points[0][3] = {200, 8},
	c.points[0][4] = {100, 9};
	c.nPoints[1] = 9999; // more than a lane holds
	for (int i = 0; i < tart::kMaxPoints; i++)
		c.points[1][i] = {(uint16_t)i, (uint16_t)i};
	c.nPoints[tart::kLevel] = 1; // a lane of an empty slot goes
	st->clip[1][0].nPoints[2] = 3;
	std::memset(st->s.track[0].ccName[0], 'a', tart::kCcNameBytes); // no terminator
	std::memset(st->s.track[0].ccName[1], 'b', 14);
	st->s.track[0].ccName[1][14] = (char)0xC3, st->s.track[0].ccName[1][15] = (char)0xA9; // cut inside a character
	tart::sanitize(*st);
	CHECK(c.nPoints[0] == 4 && c.points[0][0].tick == 100 && c.points[0][1].tick == 100 && c.points[0][2].tick == 200 &&
		  c.points[0][3].tick == 300); // the point past 32 bars is dropped, the rest sorted
	CHECK(c.nPoints[1] == tart::kMaxPoints && c.nPoints[tart::kLevel] == 1);
	CHECK(st->clip[1][0].nPoints[2] == 0);
	CHECK(std::strlen(st->s.track[0].ccName[0]) == tart::kCcNameBytes - 1);
	CHECK(std::strlen(st->s.track[0].ccName[1]) == 14);
	st->clip[0][0].length = 0;
	tart::sanitize(*st);
	CHECK(c.nPoints[1] == 0 && c.nPoints[tart::kLevel] == 0);
}

TEST(double_loop_automation) {
	Rig r;
	setNotes(r, 384, {{0, 24, 60, 100}});
	setPoints(r, 0, {{0, 1000}, {100, 2000}, {300, 4000}}, 384);
	setPoints(r, 3, {{0, 7}, {384 + 10, 9}}, 384); // a point past the end isn't kept
	r.click(push2::cc::DoubleLoop);
	auto st = r.state();
	const tart::Clip& c = st->clip[0][0];
	CHECK(c.length == 768 && c.nPoints[0] == 8 && c.nPoints[tart::kLevel] == 0);
	CHECK(c.points[0][0].tick == 0 && c.points[0][0].value == 1000 && c.points[0][3].tick == 384 &&
		  c.points[0][3].value == 4000); // the end of the first half holds the last value
	CHECK(c.points[0][4].tick == 384 && c.points[0][4].value == 1000 && c.points[0][5].tick == 484 &&
		  c.points[0][7].tick == 768 && c.points[0][7].value == 4000);
	CHECK(tart::envelope(c, 0, 384, true) == 4000 && tart::envelope(c, 0, 384) == 1000); // a jump at the seam
	CHECK(c.nPoints[3] == 4 && c.points[3][1].tick == 384 &&
		  c.points[3][1].value == 8); // 9 is a point at 394, 10 ticks on
	r.click(push2::cc::Undo);         // the points come back with the clip
	st = r.state();
	CHECK(st->clip[0][0].length == 384 && st->clip[0][0].nPoints[0] == 3 && st->clip[0][0].nPoints[3] == 2);
	// a lane that would not fit refuses, whole
	setPoints(r, 0, {}, 384);
	r.m.edit([](tart::State& st) {
		tart::Clip& c = st.clip[0][0];
		c.nPoints[0] = tart::kMaxPoints / 2 - 1; // 127 isolated points + 2 frame points = 129: doubled is too many
		for (int i = 0; i < c.nPoints[0]; i++)
			c.points[0][i] = {(uint16_t)(i + 1), 5};
		c.nPoints[1] = 3, c.points[1][0] = {1, 1}, c.points[1][1] = {2, 2}, c.points[1][2] = {3, 3};
	});
	r.m.edit([](tart::State& st) { st.s.ui.popup = tart::kNoPopup; });
	r.click(push2::cc::DoubleLoop);
	CHECK(r.view().length[0][0] == 384 && r.view().s.ui.popup == tart::kClipFullPopup);
	r.m.edit([](tart::State& st) { st.clip[0][0].nPoints[0]--; }); // 126 + 2 = 128: doubled is exactly 256
	r.click(push2::cc::DoubleLoop);
	st = r.state();
	CHECK(st->clip[0][0].length == 768 && st->clip[0][0].nPoints[0] == tart::kMaxPoints &&
		  st->clip[0][0].nPoints[1] == 10);
}

TEST(automation_undo_and_copy) {
	Rig r(false);
	fill(r, 0, 0);
	setPoints(r, 2, {{0, 5}, {50, 9}});
	r.press(push2::cc::Duplicate);
	slot(r, 0, 0, 100);
	slot(r, 0, 0, 0);
	tapSlot(r, 3, 4);
	r.press(push2::cc::Duplicate, 0);
	auto st = r.state();
	CHECK(st->clip[3][4].nPoints[2] == 2 && st->clip[3][4].points[2][1].value == 9); // the copy carries them
	r.press(push2::cc::Delete);                                                      // delete the slot, then undo it
	slot(r, 0, 0, 100);
	slot(r, 0, 0, 0);
	r.press(push2::cc::Delete, 0);
	CHECK(r.state()->clip[0][0].nPoints[2] == 0);
	r.click(push2::cc::Undo);
	st = r.state();
	CHECK(st->clip[0][0].nPoints[2] == 2 && st->clip[0][0].points[2][0].value == 5);
}

namespace {

/** An encoder touched (id 0..7 are the track encoders) or let go. */
void touch(Rig& r, int id, bool on = true) {
	push2::Event e = dec({0x90, (uint8_t)id, (uint8_t)(on ? 127 : 0)});
	r.step(&e);
}

/** The points of a lane of the clip in slot (0, 0), as (tick, value) pairs. */
std::vector<std::pair<int, int>> pointsOf(Rig& r, int lane) {
	auto st = r.state();
	const tart::Clip& c = st->clip[0][0];
	std::vector<std::pair<int, int>> out;
	for (int i = 0; i < c.nPoints[lane]; i++)
		out.push_back({c.points[lane][i].tick, c.points[lane][i].value});
	return out;
}

using Pts = std::vector<std::pair<int, int>>;

} // namespace

TEST(device_encoder_value) {
	using namespace push2::cc;
	Rig r(false);
	r.click(Device);
	r.turn(0, 1);
	CHECK(r.state()->s.track[0].value[0] == 312); // one raw unit
	r.turn(0, 5);
	CHECK(r.state()->s.track[0].value[0] == 312 * 6);
	r.press(Shift); // 31 times finer
	r.turn(1, 1);
	CHECK(r.state()->s.track[0].value[1] == 10);
	r.turn(1, -2);
	CHECK(r.state()->s.track[0].value[1] == 0); // clamped at the bottom
	r.press(Shift, 0);
	for (int i = 0; i < 4; i++)
		r.turn(0, 63);
	CHECK(r.state()->s.track[0].value[0] == 65535); // and at the top
	r.m.edit([](tart::State& st) { st.s.track[0].bipolar = 1 << 2, st.s.track[0].value[2] = 32768; });
	r.turn(2, 63);
	r.turn(2, 63);
	r.turn(2, 63);
	r.turn(2, 63);
	auto st = r.state();
	CHECK(st->s.track[0].value[2] == 65535 && st->s.track[0].overridden == 0); // no clip, nothing to override
	CHECK_NEAR(tart::volts(st->s.track[0], 2, 65535), 5.f, 1e-3);
	r.click(Track1Below + 1); // the selected track's CCs
	r.turn(0, 1);
	st = r.state();
	CHECK(st->s.track[1].value[0] == 312 && st->s.track[0].value[0] == 65535);
	r.click(Device); // closed: the encoder does nothing
	r.turn(0, 1);
	CHECK(r.state()->s.track[1].value[0] == 312);
}

TEST(step_lock_points) {
	using namespace push2::cc;
	Rig r;
	r.click(Layout);
	r.tap(0, 2); // a 2-bar clip; step 3 is ticks 48..72
	r.click(Device);
	r.pad(0, 2, 100);
	r.turn(0, 1);
	CHECK((pointsOf(r, 0) == Pts{{48, 0}, {48, 312}, {72, 312}, {72, 0}})); // flat over the step, the line before/after
	r.turn(0, 1);
	CHECK((pointsOf(r, 0) == Pts{{48, 0}, {48, 624}, {72, 624}, {72, 0}})); // a turn on moves the same segment
	r.pad(0, 2, 0);
	CHECK(r.view().nNotes == 1); // the lock was an edit: the note stays
	auto st = r.state();
	CHECK(tart::envelope(st->clip[0][0], 0, 47) == 0 && tart::envelope(st->clip[0][0], 0, 60) == 624 &&
		  tart::envelope(st->clip[0][0], 0, 73) == 0);
	CHECK(st->s.track[0].value[0] == 0 && st->s.track[0].overridden == 0); // the lane's value isn't touched
	r.pad(0, 3, 100);                                                      // the next step: the edge is shared
	r.turn(0, 3);
	r.pad(0, 3, 0);
	CHECK((pointsOf(r, 0) == Pts{{48, 0}, {48, 624}, {72, 624}, {72, 936}, {96, 936}, {96, 0}}));
	// A lock with the value of its neighbour drops the point that changes nothing: 3 points instead of 4.
	r.m.edit([](tart::State& s) { s.clip[0][0].nPoints[0] = 0; });
	r.pad(0, 2, 100);
	r.turn(0, 1);
	r.pad(0, 2, 0);
	r.pad(0, 3, 100);
	r.turn(0, 1);
	r.pad(0, 3, 0);
	CHECK((pointsOf(r, 0) == Pts{{48, 0}, {48, 312}, {72, 312}, {96, 312}, {96, 0}}));
	// Shift: finer
	r.press(Shift);
	r.pad(0, 4, 100);
	r.turn(1, 31);
	r.pad(0, 4, 0);
	r.press(Shift, 0);
	CHECK((pointsOf(r, 1) == Pts{{96, 0}, {96, 312}, {120, 312}, {120, 0}}));
}

TEST(step_lock_undo_one_step) {
	using namespace push2::cc;
	Rig r;
	r.click(Layout);
	r.tap(0, 2);
	r.click(Device);
	r.pad(0, 2, 100); // a note is there: hold it, turn two lanes several times: one step
	r.turn(0, 1), r.turn(0, 1), r.turn(1, 1);
	r.pad(0, 2, 0);
	CHECK(!pointsOf(r, 0).empty() && !pointsOf(r, 1).empty());
	undoClick(r);
	CHECK(pointsOf(r, 0).empty() && pointsOf(r, 1).empty() && r.view().nNotes == 1);
	undoClick(r, true);
	CHECK(pointsOf(r, 0).size() == 4 && pointsOf(r, 1).size() == 4);
	undoClick(r);
	// An empty step: the press makes its note, the lock joins that step.
	r.pad(0, 5, 100);
	r.turn(0, 1);
	r.pad(0, 5, 0);
	CHECK(r.view().nNotes == 2 && pointsOf(r, 0).size() == 4);
	undoClick(r);
	CHECK(r.view().nNotes == 1 && pointsOf(r, 0).empty());
}

TEST(step_lock_drum_and_split) {
	using namespace push2::cc;
	Rig r;
	drumTrack(r);
	dtap(r, 3); // lane 1's first step: 72..96
	r.click(Device);
	dstep(r, 3);
	r.turn(2, 2);
	dstep(r, 3, 0);
	CHECK((pointsOf(r, 2) == Pts{{72, 0}, {72, 624}, {96, 624}, {96, 0}}) && r.view().nNotes == 1);
	undoClick(r);
	CHECK(pointsOf(r, 2).empty() && r.view().nNotes == 1);
	// The split layout: the same on its steps
	Rig q;
	split(q);
	stap(q, 1); // 24..48 with the chord of the bottom-left pad
	q.click(Device);
	sstep(q, 1);
	q.turn(0, 1);
	sstep(q, 1, 0);
	CHECK((pointsOf(q, 0) == Pts{{24, 0}, {24, 312}, {48, 312}, {48, 0}}) && q.view().nNotes == 1);
}

TEST(step_lock_reaches_slice_in_its_step) {
	using namespace push2::cc;
	SliceRig r;
	r.click(Layout);
	r.tap(0, 0); // ticks 0..24, 250 samples a tick
	r.click(Device);
	r.pad(0, 0, 100);
	for (int i = 0; i < 3; i++)
		r.turn(0, 63);
	r.pad(0, 0, 0);
	float inside = -1, outside = -1;
	for (int i = 0; i < 8000; i++) {
		r.in.play = i == 0;
		r.step();
		if (i == 1000)
			inside = r.f.track[0].cc[0];
		if (i == 7000)
			outside = r.f.track[0].cc[0];
	}
	CHECK_NEAR(inside, 189 * 312 * 10.f / 65535, 0.01);
	CHECK_NEAR(outside, 0.f, 1e-4); // tick 28: the lock is over
	CHECK(r.f.track[0].cc[1] == 0);
}

TEST(step_lock_full_popup) {
	using namespace push2::cc;
	Rig r;
	r.click(Layout);
	r.tap(0, 2);
	r.click(Device);
	auto points = [&](int n) {
		r.m.edit([=](tart::State& st) {
			tart::Clip& c = st.clip[0][0];
			c.nPoints[0] = (uint16_t)n;
			for (int i = 0; i < n; i++)
				c.points[0][i] = {(uint16_t)(200 + i), 5};
			st.s.ui.popup = tart::kNoPopup;
		});
	};
	points(253); // a lock adds 4: 257
	r.pad(0, 2, 100);
	r.turn(0, 1);
	r.pad(0, 2, 0);
	CHECK(r.view().s.ui.popup == tart::kClipFullPopup && r.state()->clip[0][0].nPoints[0] == 253);
	CHECK(r.view().nNotes == 1); // the turn was an edit: the release keeps the note
	undoClick(r);                // the failed lock left no step: this undoes the note that made the clip
	CHECK(r.state()->clip[0][0].length == 0);
	undoClick(r, true);
	points(252);
	r.pad(0, 2, 100);
	r.turn(0, 1);
	r.pad(0, 2, 0);
	CHECK(r.state()->clip[0][0].nPoints[0] == 256 && r.view().s.ui.popup == tart::kNoPopup);
}

TEST(delete_touch_clears_or_resets) {
	using namespace push2::cc;
	Rig r(false);
	fill(r, 0, 0);
	r.click(Device);
	r.m.edit([](tart::State& st) {
		tart::Track& t = st.s.track[0];
		t.value[0] = 5000, t.value[1] = 100, t.bipolar = 1 << 1;
	});
	touch(r, 0); // no Delete: just a touch
	touch(r, 0, false);
	CHECK(r.state()->s.track[0].value[0] == 5000);
	r.press(Delete);
	r.turn(0, 5); // Delete held: a turn does nothing
	CHECK(r.state()->s.track[0].value[0] == 5000);
	touch(r, 0), touch(r, 0, false);
	touch(r, 1), touch(r, 1, false);
	auto st = r.state();
	CHECK(st->s.track[0].value[0] == 0 && st->s.track[0].value[1] == 32768); // defaults: 0 V, the middle if bipolar
	r.press(Delete, 0);
	// With points: the lane's points go, each touch its own undo step, the value stays.
	setPoints(r, 2, {{0, 5}, {50, 9}});
	setPoints(r, 3, {{0, 7}});
	r.m.edit([](tart::State& st) { st.s.track[0].value[2] = 1234; });
	r.press(Delete);
	touch(r, 2), touch(r, 2, false);
	touch(r, 3), touch(r, 3, false);
	r.press(Delete, 0);
	st = r.state();
	CHECK(st->clip[0][0].nPoints[2] == 0 && st->clip[0][0].nPoints[3] == 0 && st->s.track[0].value[2] == 1234);
	undoClick(r);
	st = r.state();
	CHECK(st->clip[0][0].nPoints[3] == 1 && st->clip[0][0].nPoints[2] == 0);
	undoClick(r);
	CHECK(r.state()->clip[0][0].nPoints[2] == 2);
	// Not on another screen
	r.click(Device);
	r.press(Delete);
	touch(r, 2);
	r.press(Delete, 0);
	CHECK(r.state()->clip[0][0].nPoints[2] == 2);
}

TEST(override_until_relaunch) {
	using namespace push2::cc;
	SliceRig r(false);
	fill(r, 0, 0);
	fill(r, 0, 1);
	setPoints(r, 0, {{0, 0}, {384, 65535}}, 384);
	r.m.edit([](tart::State& st) {
		st.s.track[0].playing = 0, st.s.transport.quant = 0, st.clip[0][1].nPoints[2] = 1;
		st.clip[0][1].points[2][0] = {0, 9};
	});
	r.in.play = true;
	r.run_(3000);
	r.click(Device);
	r.turn(1, 1); // a lane the playing clip has no points for
	CHECK(r.state()->s.track[0].overridden == 0);
	auto st = r.state();
	int v = st->s.track[0].value[0];
	CHECK(v > 1500);
	r.turn(0, 1);
	st = r.state();
	CHECK(st->s.track[0].overridden == 1 && st->s.track[0].value[0] == v + 312);
	r.run_(3000); // the clip goes on, the lane doesn't
	CHECK(r.state()->s.track[0].value[0] == v + 312);
	CHECK_NEAR(r.f.track[0].cc[0], (v + 312) * 10.f / 65535, 1e-3);
	r.click(Down); // the selected clip has points on lane 2, the playing one doesn't
	r.turn(2, 1);
	CHECK(r.state()->s.track[0].overridden == 1);
	r.click(Up);
	tapSlot(r, 0, 0); // relaunched
	r.run_(600);
	st = r.state();
	CHECK(st->s.track[0].overridden == 0 && st->s.track[0].value[0] < 1000);
	st->s.track[0].overridden = 7;
	tart::sanitize(*st);
	CHECK(st->s.track[0].overridden == 0);
}

TEST(device_screen_momentary) {
	using namespace push2::cc;
	Rig r(false);
	r.in.leds = true;
	r.step();
	CHECK(r.want.cc[Device].color == push2::kWhiteDark);
	r.press(Device);
	CHECK(r.view().s.ui.screen == tart::kDeviceScreen && r.want.cc[Device].color == push2::kWhiteFull);
	r.run_(100);
	r.press(Device, 0);
	CHECK(r.view().s.ui.screen == tart::kDeviceScreen && r.want.cc[Device].color == push2::kWhiteFull); // short: stays
	r.press(Device);
	CHECK(r.view().s.ui.screen == tart::kHome);
	r.press(Device, 0);
	CHECK(r.want.cc[Device].color == push2::kWhiteDark);
	r.press(Device); // held 0.6 s: momentary
	r.run_(48000 * 6 / 10);
	r.press(Device, 0);
	CHECK(r.view().s.ui.screen == tart::kHome);
	r.click(Device); // it joins SCALE and CLIP: the last one pressed is the screen
	r.click(Clip);
	CHECK(r.view().s.ui.screen == tart::kClipScreen);
	r.click(Device);
	CHECK(r.view().s.ui.screen == tart::kDeviceScreen);
	r.press(Scale);
	r.run_(48000 * 4 / 10);
	r.press(Scale, 0);
	CHECK(r.view().s.ui.screen == tart::kHome);
	r.click(Device);
	r.in.leds = true;
	r.step();
	CHECK(r.want.cc[Track1Above].color == push2::kBlack && r.want.cc[Track8Above].color == push2::kBlack);
	CHECK(r.want.cc[Track1Below + 3].color == tart::kPaletteFull + 3);
	r.click(Track1Below + 2); // the lower buttons still select tracks
	CHECK(r.view().s.ui.track == 2);
}

TEST(home_held_step_still_edits_notes) {
	using namespace push2::cc;
	Rig r;
	r.click(Layout);
	r.tap(0, 2);
	r.pad(0, 2, 100);
	r.turn(0, 6); // Nudge on Home
	r.turn(3, 10);
	auto v = r.view();
	CHECK(v.notes[0].tick == 51 && v.notes[0].vel == 110 && v.held == 0);
	CHECK(pointsOf(r, 0).empty() && pointsOf(r, 3).empty());
	r.pad(0, 2, 0);
	r.click(Device); // on the Device screen the same encoders are CC lanes: the velocity stays
	r.pad(0, 2, 100);
	r.turn(3, 10);
	r.pad(0, 2, 0);
	v = r.view();
	CHECK(v.notes[0].vel == 110 && v.nNotes == 1);
	CHECK(pointsOf(r, 3).size() == 4);
	r.click(Device); // back on Home
	r.pad(0, 2, 100);
	r.turn(3, 5);
	r.pad(0, 2, 0);
	CHECK(r.view().notes[0].vel == 115);
}

TEST(device_view) {
	using namespace push2::cc;
	Rig r;
	r.click(Layout);
	r.tap(0, 2);
	r.click(Device);
	r.m.edit([](tart::State& st) {
		st.s.track[0].value[1] = 777;
		st.s.track[3].playing = 0;
		st.clip[3][0].length = tart::kBar, st.clip[3][0].nPoints[4] = 1; // the playing clip of another track
	});
	r.pad(0, 2, 100);
	r.turn(0, 1);
	auto v = r.view();
	CHECK(v.automated[0] == 1 && v.automated[3] == 1 << 4 && v.automated[1] == 0);
	CHECK(v.at[0] == 312 && v.at[1] == 777); // at the held step: the lock, and a lane's value
	r.pad(0, 2, 0);
	CHECK(v.lane == 255 && v.nShown == 0);
	touch(r, 0);
	v = r.view();
	CHECK(v.lane == 0 && v.nShown == 4 && v.shown[1].value == 312);
	touch(r, 3);
	touch(r, 0, false);
	v = r.view();
	CHECK(v.lane == 3 && v.nShown == 0); // the lowest touched one
	touch(r, 3, false);
	CHECK(r.view().lane == 255);
}

// ---- Automate (AUTOMATE, CC 89): recording the encoders into a take

namespace {

/** Automate on, the Device screen open and a take running on the 1-bar clip of slot 1/1 (a note at tick 0). */
void automateTake(Rig& r) {
	fill(r, 0, 0);
	r.click(push2::cc::Automate);
	r.click(push2::cc::Device);
	r.click(push2::cc::Record);
}

} // namespace

TEST(automate_records_while_touched) {
	SliceRig r;
	automateTake(r);
	runTo(r, 24);
	touch(r, 0);
	runTo(r, 30);
	CHECK(r.view().s.ui.writing == 0 && pointsOf(r, 0).empty()); // the touch alone writes nothing
	r.turn(0, 10);                                               // 3120: written from the next tick
	CHECK(r.view().s.ui.writing == 1);
	runTo(r, 40);
	r.turn(0, 10); // 6240
	runTo(r, 50);
	r.run_(20); // a lane that is being written plays its value, not the (partial) curve
	CHECK_NEAR(r.f.track[0].cc[0], 6240 * 10.f / 65535, 1e-3);
	touch(r, 0, false);
	auto v = r.view();
	CHECK(v.s.ui.writing == 0 && v.s.ui.recTrack == 0); // the touch ended the write, not the take
	Pts p = pointsOf(r, 0);
	CHECK((p == Pts{{31, 0}, {31, 3120}, {40, 3120}, {41, 6240}, {50, 6240}}));
	CHECK(pointsOf(r, 1).empty());
	r.turn(0, 10); // untouched again: no more writing
	runTo(r, 60);
	CHECK(pointsOf(r, 0) == p && r.state()->s.track[0].overridden == 1); // an ordinary turn: the lane is overridden
	r.click(push2::cc::Record);
	auto st = r.state();
	CHECK(tart::envelope(st->clip[0][0], 0, 35) == 3120 && tart::envelope(st->clip[0][0], 0, 45) == 6240);
}

TEST(automate_overwrite_keeps_outside) {
	Rig r;
	automateTake(r);
	setPoints(r, 0, {{0, 0}, {384, 38400}}); // 100 a tick
	runTo(r, 100);
	r.turn(0, 0); // not touched: nothing yet
	touch(r, 0);
	r.turn(0, 63); // +19656 from what is heard
	runTo(r, 200);
	touch(r, 0, false);
	auto st = r.state();
	const tart::Clip& c = st->clip[0][0];
	CHECK(tart::envelope(c, 0, 50) == 5000 && tart::envelope(c, 0, 90) == 9000); // before the punch-in: kept
	int in = tart::envelope(c, 0, 102), at200 = tart::envelope(c, 0, 200, true);
	CHECK(in > 25000 && at200 == in);               // replaced: the heard value plus the turn, held
	CHECK(tart::envelope(c, 0, 100, true) < 11000); // the old curve arrives at the punch-in, then jumps
	CHECK(tart::envelope(c, 0, 200) == 20000);      // punched out: back on the old line
	CHECK(tart::envelope(c, 0, 250) == 25000 && tart::envelope(c, 0, 384) == 38400); // and it goes on
	CHECK(c.nPoints[0] <= 8); // a held value is a few points, not one a tick
}

TEST(automate_wraps_loop) {
	Rig r;
	automateTake(r);
	runTo(r, 300);
	touch(r, 0);
	r.turn(0, 10); // 3120 from tick 301 to the end of the clip
	runTo(r, 384 + 20);
	r.turn(0, 10); // 6240 on the second pass, from tick 20 on
	runTo(r, 384 + 50);
	touch(r, 0, false);
	auto st = r.state();
	const tart::Clip& c = st->clip[0][0];
	CHECK(tart::envelope(c, 0, 310) == 3120 && tart::envelope(c, 0, 383) == 3120); // the first pass runs to the end
	CHECK(tart::envelope(c, 0, 10) == 3120 && tart::envelope(c, 0, 40) == 6240);   // the second pass begins at tick 0
	CHECK(tart::envelope(c, 0, 100) == 0 && tart::envelope(c, 0, 250) == 0);       // punched out: the old curve is back
	for (int i = 1; i < c.nPoints[0]; i++)
		CHECK(c.points[0][i - 1].tick <= c.points[0][i].tick);
	CHECK(r.view().s.ui.recTrack == 0);
}

TEST(automate_thinning) {
	Rig r;
	automateTake(r);
	runTo(r, 10);
	touch(r, 0);
	r.press(push2::cc::Shift); // 10 a turn
	std::vector<int> value(384);
	for (int t = 11; t < 371; t++) {
		runTo(r, t);
		value[t] = r.state()->s.track[0].value[0];
		r.turn(0, 3); // 30 a tick: a slow sweep
	}
	touch(r, 0, false);
	auto st = r.state();
	const tart::Clip& c = st->clip[0][0];
	CHECK(c.nPoints[0] >= 3 && c.nPoints[0] <= 6);
	int worst = 0;
	for (int t = 12; t < 371; t++)
		worst = std::max(worst, std::abs(tart::envelope(c, 0, t) - value[t]));
	CHECK(worst <= 512);
}

TEST(automate_needs_take_and_touch) {
	using namespace push2::cc;
	Rig r;
	fill(r, 0, 0);
	r.click(Device);
	r.click(Automate);
	touch(r, 0);
	r.turn(0, 5); // no take: just the value
	r.click(Record);
	CHECK(r.view().s.ui.writing == 0 && pointsOf(r, 0).empty());
	touch(r, 0, false);
	runTo(r, 20);
	r.turn(0, 5); // a take, but no touch
	CHECK(r.view().s.ui.writing == 0);
	r.click(Automate); // a take and a touch, but Automate is off
	touch(r, 0);
	r.turn(0, 5);
	CHECK(!r.view().s.ui.automate && r.view().s.ui.writing == 0);
	r.click(Automate);
	r.click(Track1Below + 1); // another track is selected than the one that records
	touch(r, 0, false);
	touch(r, 0);
	r.turn(0, 5);
	CHECK(r.view().s.ui.writing == 0 && r.state()->s.track[1].value[0] == 5 * 312);
	r.click(Track1Below);
	touch(r, 0, false);
	r.click(Device); // and the Home screen: the encoders aren't lanes
	touch(r, 0);
	r.turn(0, 5);
	CHECK(r.view().s.ui.writing == 0);
	r.click(Device);
	r.turn(0, 5); // all there: the first turn writes
	CHECK(r.view().s.ui.writing == 1);
	runTo(r, 40);
	touch(r, 0, false);
	CHECK(r.view().s.ui.writing == 0 && !pointsOf(r, 0).empty());
	// Without a running take, an automated lane is overridden as before
	r.click(Record);
	r.click(Automate);
	r.click(Automate);
	r.turn(0, 1);
	CHECK(r.state()->s.track[0].overridden == 1 && r.view().s.ui.writing == 0);
}

TEST(automate_take_one_undo) {
	Rig r;
	automateTake(r);
	runTo(r, 50);
	touch(r, 0);
	r.turn(0, 20);
	runTo(r, 100);
	r.turn(1, 20); // a second lane, untouched: nothing
	r.turn(0, 20);
	touch(r, 1);
	r.turn(1, 20);
	runTo(r, 150);
	hold(r, 160, 170);
	r.click(push2::cc::Record);
	auto st = r.state();
	CHECK(st->clip[0][0].nPoints[0] > 3 && st->clip[0][0].nPoints[1] > 1 && st->clip[0][0].nNotes == 2);
	CHECK(r.view().s.ui.writing == 0); // the take ended: nothing is written any more
	undoClick(r);                      // one step: the pads and both lanes
	st = r.state();
	CHECK(st->clip[0][0].nPoints[0] == 0 && st->clip[0][0].nPoints[1] == 0 && st->clip[0][0].nNotes == 1);
	undoClick(r, true);
	st = r.state();
	CHECK(st->clip[0][0].nPoints[0] > 3 && st->clip[0][0].nPoints[1] > 1 && st->clip[0][0].nNotes == 2);
}

TEST(automate_ends_with_every_take_end) {
	using namespace push2::cc;
	for (int how = 0; how < 5; how++) {
		Rig r;
		automateTake(r);
		runTo(r, 30);
		touch(r, 0);
		r.turn(0, 10);
		runTo(r, 40);
		CHECK(r.view().s.ui.writing == 1);
		if (how == 0) {
			r.click(Record);
		} else if (how == 1) {
			r.click(New);
		} else if (how == 2) {
			r.click(Play); // the transport stops
		} else if (how == 3) {
			r.click(Session);
			tapSlot(r, 0, 0); // a launch on its track
		} else {
			r.click(Session);
			r.press(Delete); // its slot is deleted
			slot(r, 0, 0, 100);
			slot(r, 0, 0, 0);
			r.press(Delete, 0);
		}
		auto v = r.view();
		CHECK(v.s.ui.recTrack == -1 && v.s.ui.writing == 0);
		r.turn(0, 1); // the touch is still down: nothing writes any more
		CHECK(r.view().s.ui.writing == 0);
		touch(r, 0, false);
	}
}

TEST(shift_automate_reenables) {
	using namespace push2::cc;
	Rig r(false);
	fill(r, 0, 0);
	setPoints(r, 0, {{0, 0}, {384, 65535}}, 384);
	r.m.edit([](tart::State& st) { st.s.track[3].overridden = 6, st.s.track[0].playing = 0; });
	r.click(Device);
	r.in.play = true;
	r.run_(1000);
	r.turn(0, 1);
	CHECK(r.state()->s.track[0].overridden == 1);
	r.press(Shift);
	r.click(Automate);
	r.press(Shift, 0);
	auto st = r.state();
	CHECK(st->s.track[0].overridden == 0 && st->s.track[3].overridden == 0 && !st->s.ui.automate);
	r.run_(1000); // the lane follows the clip again
	CHECK(r.state()->s.track[0].value[0] > 600);
	r.click(Automate);
	CHECK(r.view().s.ui.automate);
}

TEST(delete_automate_clears) {
	using namespace push2::cc;
	Rig r(false);
	fill(r, 0, 0);
	setPoints(r, 0, {{0, 5}, {50, 9}});
	setPoints(r, 3, {{10, 7}});
	setPoints(r, tart::kPan, {{10, 7}});
	r.m.edit([](tart::State& st) { st.clip[0][1].nPoints[0] = 1; }); // another clip: left alone
	r.press(Delete);
	r.click(Automate);
	r.press(Delete, 0);
	auto st = r.state();
	CHECK(!st->s.ui.automate);
	for (int j = 0; j < tart::kAutos; j++)
		CHECK(st->clip[0][0].nPoints[j] == 0);
	CHECK(st->clip[0][0].nNotes == 1 && st->clip[0][1].nPoints[0] == 1);
	undoClick(r); // one step brings every lane back
	st = r.state();
	CHECK(st->clip[0][0].nPoints[0] == 2 && st->clip[0][0].nPoints[3] == 1 && st->clip[0][0].nPoints[tart::kPan] == 1);
	r.press(Delete); // nothing to clear: no step of its own
	r.click(Automate);
	r.click(Automate);
	r.press(Delete, 0);
	r.click(Automate);
	CHECK(r.view().s.ui.automate);
}

TEST(automate_led) {
	using namespace push2::cc;
	Rig r(false);
	auto led = [&]() {
		r.in.leds = true;
		r.step();
		return r.want.cc[Automate];
	};
	CHECK(led().color == push2::kWhiteDark);
	r.press(Automate);
	CHECK(led().color == push2::kWhiteFull); // held, and it toggled
	CHECK(r.view().s.ui.automate);
	CHECK(led().color == push2::kRed && led().channel == 0);
	r.press(Automate, 0);
	CHECK(led().color == push2::kRed);
	r.click(Automate);
	CHECK(led().color == push2::kWhiteDark);
}

TEST(automate_writing_dot_and_view) {
	Rig r;
	automateTake(r);
	runTo(r, 30);
	touch(r, 0);
	r.turn(2, 1);
	auto v = r.view();
	CHECK(v.s.ui.writing == 0); // encoder 3 isn't touched
	touch(r, 2);
	r.turn(2, 1);
	v = r.view();
	CHECK(v.s.ui.writing == 1 << 2 && v.s.ui.recTrack == v.s.ui.track);
}

namespace {

uint32_t lcg(uint32_t& s) {
	return s = s * 1664525u + 1013904223u;
}

} // namespace

TEST(automate_fuzz) {
	using namespace push2::cc;
	Rig r;
	uint32_t seed = 12345;
	auto next = [&](int n) { return (int)(lcg(seed) >> 8) % n; };
	// Old curves on a few lanes of a 2-beat clip (so that the loop wraps often), some of them nearly full.
	r.m.edit([&](tart::State& st) {
		tart::Clip& c = st.clip[0][0];
		c.length = 2 * tart::kPpqn, c.nNotes = 1, c.notes[0] = {0, 24, 60, 100};
		for (int j : {0, 2, 5}) {
			c.nPoints[j] = (uint16_t)(j == 5 ? 250 : 12);
			for (int i = 0; i < c.nPoints[j]; i++)
				c.points[j][i] = {(uint16_t)(j == 5 ? i * 192 / 250 : i * 15), (uint16_t)next(65536)};
		}
	});
	setQuant(r, 0); // a take starts at once
	r.click(Automate);
	r.click(Device);
	r.click(Record);
	auto st = std::make_unique<tart::State>();
	int written = 0, grown = 0;
	for (int i = 0; i < 3000; i++) {
		int op = next(10), before = 0;
		for (int j = 0; j < tart::kAutos; j++)
			before += r.state()->clip[0][0].nPoints[j];
		if (op < 2)
			touch(r, next(4), next(5) > 0);
		else if (op < 7)
			r.turn(next(4), next(2) ? 1 + next(63) : 127 - next(63)), r.run_(next(1500));
		else if (op < 8)
			r.run_(next(3000));
		else if (op == 8)
			r.click(next(60) ? next(8) ? Record : Play : Automate);
		else
			r.press(Shift, next(2) * 127);
		r.m.save(*st);
		const tart::Ui& u = st->s.ui;
		CHECK(u.recTrack >= 0 || u.writing == 0);
		written += u.writing != 0;
		int after = 0;
		for (int j = 0; j < tart::kAutos; j++) {
			const tart::Clip& c = st->clip[0][0];
			after += c.nPoints[j];
			CHECK(c.nPoints[j] <= tart::kMaxPoints);
			for (int k = 1; k < c.nPoints[j]; k++)
				CHECK(c.points[j][k - 1].tick <= c.points[j][k].tick);
		}
		grown += after > before;
	}
	CHECK(written > 800 && grown > 250); // the run did write, with doors shutting
}

// ---- Mix, Mute, Solo, Default LEVEL (E4.4)

namespace {

/** `seconds` of audio time at 48 kHz. */
void wait(Rig& r, double seconds) {
	r.run_((int)(seconds * 48000));
}

/** The LEDs as the Push would be told at this sample. */
void ledsOf(Rig& r) {
	r.in.leds = true;
	r.step();
}

} // namespace

TEST(mix_volumes_pans) {
	using namespace push2::cc;
	Rig r(false);
	r.click(Mix);
	CHECK(r.view().s.ui.screen == tart::kMixScreen && !r.view().s.ui.mixPans);
	r.turn(0, -1); // VOLUMES: one raw unit is 312 of 65535, like a Device encoder
	r.turn(3, -2);
	auto st = r.state();
	CHECK(st->s.track[0].value[tart::kLevel] == 65535 - 312 && st->s.track[3].value[tart::kLevel] == 65535 - 624);
	CHECK(st->s.track[1].value[tart::kLevel] == 65535 && st->s.track[0].value[tart::kPan] == 32768);
	r.press(Shift); // finer
	r.turn(0, 31);
	r.press(Shift, 0);
	CHECK(r.state()->s.track[0].value[tart::kLevel] == 65535); // clamped at the top
	r.click(Track1Above + 1);                                  // PANS
	CHECK(r.view().s.ui.mixPans);
	r.turn(1, 1);
	r.turn(0, 127);
	st = r.state();
	CHECK(st->s.track[1].value[tart::kPan] == 32768 + 312 && st->s.track[0].value[tart::kPan] == 32768 - 312);
	CHECK(st->s.track[1].value[tart::kLevel] == 65535);
	CHECK_NEAR(tart::volts(st->s.track[1], tart::kPan, st->s.track[1].value[tart::kPan]), 0.0475f, 1e-3);
	// The master encoder does nothing here.
	push2::Event master = dec({0xB0, push2::cc::kMasterVolume, 5});
	r.step(&master);
	auto after = r.state();
	CHECK(std::memcmp(after->s.track, st->s.track, sizeof st->s.track) == 0 && after->s.transport.swing == 0);
	// The LEDs: the chosen box white, the other dark gray, the rest dark; MIX full.
	ledsOf(r);
	CHECK(r.want.cc[Track1Above].color == push2::kRgbDarkGray && r.want.cc[Track1Above + 1].color == push2::kRgbWhite);
	CHECK(r.want.cc[Track1Above + 2].color == push2::kBlack && r.want.cc[Track8Above].color == push2::kBlack);
	CHECK(r.want.cc[Mix].color == push2::kWhiteFull);
	r.click(Track1Above); // VOLUMES again
	ledsOf(r);
	CHECK(!r.view().s.ui.mixPans && r.want.cc[Track1Above].color == push2::kRgbWhite);
	// Lower buttons still select tracks; the upper ones do nothing on other screens.
	r.click(Track1Below + 4);
	CHECK(r.view().s.ui.track == 4);
	r.click(Mix);
	CHECK(r.view().s.ui.screen == tart::kHome);
	ledsOf(r);
	CHECK(r.want.cc[Mix].color == push2::kWhiteDark && r.want.cc[Track1Above].color == push2::kBlack);
	r.click(Track1Above + 1);
	CHECK(!r.view().s.ui.mixPans);
	r.turn(0, -1); // closed: the encoder does nothing
	CHECK(r.state()->s.track[0].value[tart::kLevel] == 65535);
	// MIX is momentary when held, as DEVICE.
	r.press(Mix);
	CHECK(r.view().s.ui.screen == tart::kMixScreen);
	wait(r, 0.4);
	r.press(Mix, 0);
	CHECK(r.view().s.ui.screen == tart::kHome);
}

TEST(mute_tap_hold_lock) {
	using namespace push2::cc;
	Rig r(false);
	auto st = [&](int t) { return r.state()->s.track[t]; };
	r.click(Mute); // a tap toggles the selected track
	CHECK(st(0).mute && !st(1).mute);
	r.click(Mute);
	CHECK(!st(0).mute);
	r.click(Track1Below + 2);
	wait(r, 0.5); // 0.5 s is still a tap
	r.press(Mute);
	wait(r, 0.5);
	r.press(Mute, 0);
	CHECK(st(2).mute && r.view().s.ui.muteSoloLock == 0);
	// A lower button while MUTE is held toggles that track and doesn't select it; the release is no tap.
	r.press(Mute);
	r.click(Track1Below + 4);
	r.click(Track1Below + 5);
	r.press(Mute, 0);
	CHECK(st(4).mute && st(5).mute && st(2).mute && r.view().s.ui.track == 2);
	CHECK(!st(0).mute);
	// A hold of 0.6 s locks the mode until the next press.
	r.press(Mute);
	wait(r, 0.7);
	r.press(Mute, 0);
	CHECK(r.view().s.ui.muteSoloLock == 1 && st(2).mute); // the release toggled nothing
	r.click(Track1Below + 5);                             // locked: lower buttons toggle without MUTE held
	r.click(Track1Below + 6);
	CHECK(!st(5).mute && st(6).mute && r.view().s.ui.track == 2);
	ledsOf(r);
	CHECK(r.want.cc[Mute].channel == push2::kPulseHalf && r.want.cc[Mute].color == tart::kPaletteYellow &&
		  r.want.cc[Mute].base == push2::kBlack);
	// Another long press locks the other one instead.
	r.press(Solo);
	wait(r, 0.7);
	r.press(Solo, 0);
	CHECK(r.view().s.ui.muteSoloLock == 2);
	r.click(Track1Below + 1);
	CHECK(st(1).solo && !st(1).mute);
	// Pressing the locked button unlocks and does nothing else.
	r.press(Solo);
	CHECK(r.view().s.ui.muteSoloLock == 0);
	r.press(Solo, 0);
	CHECK(st(2).solo == false && st(1).solo);
	r.click(Track1Below + 3); // plain selection again
	CHECK(r.view().s.ui.track == 3 && !st(3).solo);
	// MUTE and SOLO can be told apart: a locked Mute with SOLO tapped toggles the selected track's solo.
	r.press(Mute);
	wait(r, 0.7);
	r.press(Mute, 0);
	r.click(Solo);
	CHECK(st(3).solo && r.view().s.ui.muteSoloLock == 1);
	// Not saved: sanitize drops the lock.
	auto s = r.state();
	tart::sanitize(*s);
	CHECK(s->s.ui.muteSoloLock == 0 && s->s.ui.muteSoloAt == -1 && s->s.track[6].mute && s->s.track[3].solo);
}

TEST(solo_exclusive_mutes_others) {
	using namespace push2::cc;
	SliceRig r(false);
	r.run_(2);
	for (int t = 0; t < tart::kTracks; t++)
		CHECK(r.f.track[t].level == 10);
	r.click(Solo); // track 1
	r.run_(2);
	for (int t = 0; t < tart::kTracks; t++)
		CHECK(r.f.track[t].level == (t == 0 ? 10 : 0));
	r.press(Solo); // soloing track 3 un-solos track 1
	r.click(Track1Below + 2);
	r.press(Solo, 0);
	auto st = r.state();
	CHECK(!st->s.track[0].solo && st->s.track[2].solo && !st->s.track[1].solo);
	r.run_(2);
	for (int t = 0; t < tart::kTracks; t++)
		CHECK(r.f.track[t].level == (t == 2 ? 10 : 0));
	// Solo wins over nothing: a muted track that is soloed is silent; unsoloing leaves the mutes alone.
	r.press(Mute);
	r.click(Track1Below + 2);
	r.press(Mute, 0);
	r.run_(2);
	CHECK(r.f.track[2].level == 0 && r.f.track[1].level == 0);
	r.press(Solo);
	r.click(Track1Below + 2);
	r.press(Solo, 0);
	r.run_(2);
	CHECK(r.f.track[2].level == 0 && r.f.track[1].level == 10 && r.f.track[0].level == 10);
	r.press(Mute); // unmute track 3
	r.click(Track1Below + 2);
	r.press(Mute, 0);
	r.run_(2);
	CHECK(r.f.track[2].level == 10);
	// silent() is what the Push and the display use
	auto s = r.state();
	s->s.track[5].solo = true;
	CHECK(tart::silent(s->s, 0) && !tart::silent(s->s, 5));
}

TEST(mute_gates_level_playhead_runs) {
	using namespace push2::cc;
	SliceRig r;
	fill(r, 0, 0);                                      // a note on tick 0, 24 ticks long
	setPoints(r, 1, {{0, 0}, {384, 65535}});            // CC 2 ramps over the clip
	setPoints(r, tart::kLevel, {{0, 65535}, {384, 0}}); // and so does LEVEL, down
	r.m.edit([](tart::State& st) { st.s.track[0].playing = 0, st.s.transport.quant = 0; });
	r.click(Play);
	runTo(r, 4);
	r.run_(2);
	CHECK(r.f.track[0].gate[0] == 10 && r.f.track[0].level > 9.f);
	r.click(Mute);
	r.run_(2);
	CHECK(r.f.track[0].gate[0] == 0 && r.f.track[0].level == 0); // silent: LEVEL 0 V, the gate closed
	int64_t tick = r.view().s.transport.tick;
	float cc = r.f.track[0].cc[1];
	runTo(r, tick + 8);
	r.run_(2);
	auto v = r.view();
	CHECK(v.s.transport.tick > tick && r.f.track[0].cc[1] > cc);                         // the clip and its CC run on
	CHECK(v.s.track[0].value[tart::kLevel] < 65535 - 1000 && r.f.track[0].gate[0] == 0); // LEVEL's value runs on too
	CHECK(r.f.track[1].gate[0] == 0 && r.f.track[1].level == 10);                        // another track isn't touched
	r.click(Mute); // an unmute mid-note opens the gate at once (the note lasts 24 ticks)
	r.run_(2);
	CHECK(v.s.track[0].mute && !r.state()->s.track[0].mute);
	CHECK(r.f.track[0].gate[0] == 10 && r.f.track[0].level > 0.f && r.f.track[0].level < 10.f);
	// A pad played on a muted track is silent too, and sounds as it is unmuted.
	r.m.edit([](tart::State& st) { st.s.track[0].playing = tart::kNone; });
	runTo(r, 600);
	r.run_(2);
	CHECK(r.f.track[0].gate[0] == 0);
	r.pad(0, 0, 100);
	r.run_(2);
	CHECK(r.f.track[0].gate[0] == 10);
	r.click(Mute);
	r.run_(2);
	CHECK(r.f.track[0].gate[0] == 0);
	r.click(Mute);
	r.run_(2);
	CHECK(r.f.track[0].gate[0] == 10);
	r.pad(0, 0, 0);
}

TEST(mix_step_lock) {
	using namespace push2::cc;
	Rig r;
	r.click(Layout);
	r.tap(0, 2); // a 2-bar clip; step 3 is ticks 48..72
	r.click(Mix);
	r.click(Track1Above + 1); // PANS: encoder 1 is LEVEL all the same
	r.pad(0, 2, 100);
	r.turn(0, -1); // encoder 1: the selected track's LEVEL
	CHECK((pointsOf(r, tart::kLevel) == Pts{{48, 65535}, {48, 65535 - 312}, {72, 65535 - 312}, {72, 65535}}));
	r.turn(1, 1); // encoder 2: its PAN
	CHECK((pointsOf(r, tart::kPan) == Pts{{48, 32768}, {48, 32768 + 312}, {72, 32768 + 312}, {72, 32768}}));
	auto v = r.view();
	CHECK(v.at[tart::kLevel] == 65535 - 312 && v.at[tart::kPan] == 32768 + 312); // what the display shows
	r.turn(2, 3);                                                                // the other encoders do nothing
	r.turn(7, 3);
	for (int lane = 0; lane < tart::kCcs; lane++)
		CHECK(pointsOf(r, lane).empty());
	auto st = r.state();
	CHECK(st->s.track[0].value[tart::kLevel] == 65535 && st->s.track[1].value[tart::kLevel] == 65535); // no value moves
	r.turn(0, -1); // a second turn moves the same segment
	CHECK(pointsOf(r, tart::kLevel)[1].second == 65535 - 624);
	r.pad(0, 2, 0);
	CHECK(r.view().nNotes == 1); // the lock was an edit: the note stays
	undoClick(r);                // one undo step for the hold
	CHECK(pointsOf(r, tart::kLevel).empty() && pointsOf(r, tart::kPan).empty() && r.view().nNotes == 1);
	// Without a held step the same encoders are the tracks' values.
	r.turn(0, -1);
	CHECK(r.state()->s.track[0].value[tart::kPan] == 32768 - 312 && r.state()->s.track[0].value[tart::kLevel] == 65535);
}

TEST(delete_touch_level_default) {
	using namespace push2::cc;
	Rig r(false);
	fill(r, 0, 0);
	r.m.edit([](tart::State& st) { st.s.ui.defaultLevel = 5; });
	r.click(Mix);
	r.turn(2, -3);
	r.turn(3, -1);
	CHECK(r.state()->s.track[2].value[tart::kLevel] == 65535 - 936);
	r.press(Delete);
	r.turn(2, -3); // Delete held: a turn does nothing
	CHECK(r.state()->s.track[2].value[tart::kLevel] == 65535 - 936);
	touch(r, 2), touch(r, 2, false);
	auto st = r.state();
	CHECK(st->s.track[2].value[tart::kLevel] == tart::levelValue(5) &&
		  st->s.track[3].value[tart::kLevel] == 65535 - 312);
	CHECK(tart::levelValue(5) == 32768); // 5 V
	r.press(Delete, 0);
	// PANS: back to 0 V.
	r.click(Track1Above + 1);
	r.turn(1, 5);
	r.press(Delete);
	touch(r, 1), touch(r, 1, false);
	r.press(Delete, 0);
	CHECK(r.state()->s.track[1].value[tart::kPan] == 32768);
	// A track whose clip (in the selected scene) has points on the lane loses them instead, one undo step.
	r.m.edit([](tart::State& s) {
		tart::Clip& c = s.clip[1][0];
		c.length = tart::kBar, c.nPoints[tart::kPan] = 2, c.points[tart::kPan][1] = {96, 9};
		s.s.track[1].value[tart::kPan] = 1234;
	});
	r.press(Delete);
	touch(r, 1), touch(r, 1, false);
	r.press(Delete, 0);
	st = r.state();
	CHECK(st->clip[1][0].nPoints[tart::kPan] == 0 && st->s.track[1].value[tart::kPan] == 1234);
	undoClick(r);
	CHECK(r.state()->clip[1][0].nPoints[tart::kPan] == 2);
	// Not on Home.
	r.click(Mix);
	r.press(Delete);
	touch(r, 2);
	r.press(Delete, 0);
	CHECK(r.state()->clip[1][0].nPoints[tart::kPan] == 2 && r.state()->s.track[2].value[tart::kLevel] == 32768);
}

TEST(default_level_menu_sets_all) {
	using namespace push2::cc;
	SliceRig r(false);
	for (int volts = 0; volts <= 10; volts++) {
		CHECK_NEAR(tart::volts(tart::Track(), tart::kLevel, tart::levelValue(volts)), volts, 1e-3);
		r.m.edit([=](tart::State& st) { // what the menu item does
			st.s.ui.defaultLevel = (uint8_t)volts;
			for (tart::Track& tr : st.s.track)
				tr.value[tart::kLevel] = tart::levelValue(volts);
		});
		r.run_(2);
		for (int t = 0; t < tart::kTracks; t++)
			CHECK_NEAR(r.f.track[t].level, volts, 1e-3);
	}
	r.m.edit([](tart::State& st) { st.s.ui.defaultLevel = 3; });
	r.click(Mix);
	r.press(Delete);
	touch(r, 6), touch(r, 6, false);
	r.press(Delete, 0);
	r.run_(2);
	CHECK_NEAR(r.f.track[6].level, 3, 1e-3);
	CHECK(r.state()->s.track[5].value[tart::kLevel] == tart::levelValue(10)); // the others stay
	auto st = std::make_unique<tart::State>();                                // a new patch: 10 V everywhere
	CHECK(st->s.ui.defaultLevel == 10 && st->s.track[0].value[tart::kLevel] == tart::levelValue(10));
}

TEST(mute_solo_leds) {
	using namespace push2::cc;
	Rig r(false);
	ledsOf(r);
	CHECK(r.want.cc[Mute].color == push2::kWhiteDark && r.want.cc[Mute].channel == 0);
	CHECK(r.want.cc[Solo].color == push2::kWhiteDark && r.want.cc[Solo].channel == 0);
	CHECK(r.out.rgb[16] == 0xFFC800 && tart::kPaletteYellow == 116); // the 17th palette entry
	r.press(Mute);                                                   // held: yellow
	ledsOf(r);
	CHECK(r.want.cc[Mute].color == tart::kPaletteYellow && r.want.cc[Mute].channel == 0);
	CHECK(r.want.cc[Solo].color == push2::kWhiteDark);
	r.press(Mute, 0); // a tap muted track 1: yellow while it is the selected one
	ledsOf(r);
	CHECK(r.want.cc[Mute].color == tart::kPaletteYellow && r.want.cc[Mute].channel == 0);
	r.click(Track1Below + 1); // another track is selected: dim again
	ledsOf(r);
	CHECK(r.want.cc[Mute].color == push2::kWhiteDark);
	r.click(Track1Below);
	r.click(Mute); // unmuted
	ledsOf(r);
	CHECK(r.want.cc[Mute].color == push2::kWhiteDark);
	r.press(Solo);
	ledsOf(r);
	CHECK(r.want.cc[Solo].color == push2::kBlue && r.want.cc[Solo].channel == 0);
	r.press(Solo, 0);
	ledsOf(r);
	CHECK(r.want.cc[Solo].color == push2::kBlue); // track 1 is soloed
	r.click(Solo);
	ledsOf(r);
	CHECK(r.want.cc[Solo].color == push2::kWhiteDark);
	r.press(Solo); // locked: pulses between black and blue
	wait(r, 0.7);
	r.press(Solo, 0);
	ledsOf(r);
	CHECK(r.want.cc[Solo].channel == push2::kPulseHalf && r.want.cc[Solo].color == push2::kBlue &&
		  r.want.cc[Solo].base == push2::kBlack);
	CHECK(r.want.cc[Mute].color == push2::kWhiteDark);
	r.click(Solo); // unlocks
	ledsOf(r);
	CHECK(r.want.cc[Solo].channel == 0 && r.want.cc[Solo].color == push2::kWhiteDark);
}

TEST(lower_button_colors) {
	using namespace push2::cc;
	Rig r(false);
	auto lower = [&](int t) { return r.want.cc[Track1Below + t]; };
	ledsOf(r);
	CHECK(lower(0).color == push2::kRgbWhite && lower(1).color == tart::kPaletteFull + 1 &&
		  lower(7).color == tart::kPaletteFull + 7);
	r.m.edit([](tart::State& st) { st.s.track[1].mute = true; });
	ledsOf(r);
	CHECK(lower(1).color == push2::kRgbDarkGray && lower(2).color == tart::kPaletteFull + 2); // muted: dark gray
	r.click(Track1Below + 1);
	ledsOf(r);
	CHECK(lower(1).color == push2::kRgbDarkGray && lower(0).color == tart::kPaletteFull); // selected but muted, as Live
	r.m.edit([](tart::State& st) { st.s.track[1].mute = false; });
	ledsOf(r);
	CHECK(lower(1).color == push2::kRgbWhite); // selected: white
	r.m.edit([](tart::State& st) { st.s.track[4].solo = true; });
	ledsOf(r);
	CHECK(lower(4).color == push2::kBlue);                                                 // soloed: blue
	CHECK(lower(2).color == push2::kRgbDarkGray && lower(0).color == push2::kRgbDarkGray); // muted by the solo
	CHECK(lower(1).color == push2::kRgbDarkGray); // selected but muted by the solo
	CHECK(lower(7).color == push2::kRgbDarkGray);
	for (int t = 0; t < 8; t++)
		CHECK(lower(t).channel == 0);
	// The same while MUTE or SOLO is held or locked (the lower buttons are the same).
	r.press(Mute);
	ledsOf(r);
	CHECK(lower(4).color == push2::kBlue && lower(2).color == push2::kRgbDarkGray);
	r.press(Mute, 0);
	// Stop Clip held keeps its own colours: red pulses where a clip plays, nothing elsewhere.
	r.m.edit([](tart::State& st) { st.s.track[3].playing = 0; });
	r.press(StopClip);
	ledsOf(r);
	CHECK(lower(3).color == push2::kBlack && lower(3).channel == push2::kPulseHalf && lower(3).base == push2::kRed);
	CHECK(lower(4).color == push2::kBlack && lower(4).channel == 0 && lower(1).color == push2::kBlack);
	r.press(StopClip, 0);
	// The Mix screen shows the same lower buttons.
	r.click(Mix);
	ledsOf(r);
	CHECK(lower(4).color == push2::kBlue && lower(1).color == push2::kRgbDarkGray);
}

TEST(sanitize_mixer) {
	auto st = std::make_unique<tart::State>();
	st->s.track[0].mute = true, st->s.track[1].solo = true, st->s.ui.mixPans = true;
	st->s.ui.defaultLevel = 200;
	st->s.ui.screen = tart::kMixScreen;
	st->s.ui.muteSoloLock = 2, st->s.ui.muteSoloAt = 5, st->s.ui.muteSoloUsed = true;
	tart::sanitize(*st);
	CHECK(st->s.track[0].mute && st->s.track[1].solo && st->s.ui.mixPans); // saved
	CHECK(st->s.ui.defaultLevel == 10 && st->s.ui.screen == tart::kHome);
	CHECK(!st->s.track[2].mute && !st->s.track[2].solo);
	CHECK(st->s.ui.muteSoloLock == 0 && st->s.ui.muteSoloAt == -1 && !st->s.ui.muteSoloUsed);
	// The saved values survive, and an old patch (a fresh State) is unmuted at 10 V.
	st->s.ui.defaultLevel = 4;
	tart::sanitize(*st);
	CHECK(st->s.ui.defaultLevel == 4);
	const tart::Session fresh;
	CHECK(!fresh.track[0].mute && !fresh.track[0].solo && !fresh.ui.mixPans && fresh.ui.defaultLevel == 10);
}

TEST(mix_automate_writes_level) {
	using namespace push2::cc;
	SliceRig r;
	fill(r, 0, 0);
	fill(r, 1, 0);
	r.m.edit([](tart::State& st) { // track 2 plays a clip with a LEVEL curve of its own
		tart::Clip& c = st.clip[1][0];
		c.nPoints[tart::kLevel] = 2, c.points[tart::kLevel][0] = {0, 65535}, c.points[tart::kLevel][1] = {384, 0};
		st.s.track[1].playing = 0;
	});
	r.click(Automate);
	r.click(Mix);
	r.click(Record);
	runTo(r, 24);
	touch(r, 0);
	touch(r, 1);
	runTo(r, 30);
	CHECK(r.view().s.ui.writing == 0 && pointsOf(r, tart::kLevel).empty()); // the touch alone writes nothing
	r.turn(0, -10);                                                         // 65535 - 3120, written from the next tick
	CHECK(r.view().s.ui.writing == 1 << tart::kLevel);
	runTo(r, 40);
	r.run_(20);
	CHECK_NEAR(r.f.track[0].level, (65535 - 3120) * 10.f / 65535, 1e-3);
	// Another track's encoder only overrides: its curve is kept and its lane is held.
	r.turn(1, -1);
	auto st = r.state();
	CHECK(st->s.track[1].overridden == 1 << tart::kLevel && st->clip[1][0].nPoints[tart::kLevel] == 2);
	CHECK(st->s.ui.writing == 1 << tart::kLevel && r.view().s.ui.recTrack == 0);
	touch(r, 0, false);
	touch(r, 1, false);
	CHECK(r.view().s.ui.writing == 0);
	CHECK(tart::envelope(r.state()->clip[0][0], tart::kLevel, 35) == 65535 - 3120);
	for (int lane = 0; lane < tart::kCcs; lane++)
		CHECK(pointsOf(r, lane).empty());
	// PANS: the same encoder writes the PAN lane.
	r.click(Track1Above + 1);
	touch(r, 0);
	r.turn(0, 10);
	CHECK(r.view().s.ui.writing == 1 << tart::kPan);
	runTo(r, 60);
	touch(r, 0, false);
	CHECK(!pointsOf(r, tart::kPan).empty() && r.view().s.ui.writing == 0);
	CHECK(tart::envelope(r.state()->clip[0][0], tart::kPan, 55) == 32768 + 3120);
	// A touch that moves from a Device lane to Mix ends the first lane's write.
	touch(r, 2);
	r.click(Mix); // closes
	r.click(Device);
	touch(r, 2, false);
	touch(r, 3);
	r.turn(3, 5); // Device encoder 4 writes CC 4
	CHECK(r.view().s.ui.writing == 1 << 3);
	r.click(Device);
	r.click(Mix);
	r.turn(3, 5); // Mix encoder 4 belongs to track 4: an override at most, no write
	CHECK(r.view().s.ui.writing == 1 << 3);
	touch(r, 3, false);
	CHECK(r.view().s.ui.writing == 0);
}

// ---- Euclid tracks (E5.1)

namespace {

/** One gate rise of the Slice: the sample, track, channel and what the Slice read there. */
struct Rise {
	int frame, track, ch;
	float voct, vel;
};
bool operator==(const Rise& a, const Rise& b) {
	return a.frame == b.frame && a.track == b.track && a.ch == b.ch && a.voct == b.voct && a.vel == b.vel;
}

/** Steps `n` samples (`start`: from a Play or Reset on the first) and lists every gate rise, in time and channel order.
 * At 120 BPM a tick is 250 samples; a note that starts as another ends rises one sample late (the retrigger gap). */
std::vector<Rise> rises(SliceRig& r, int n, bool start = true) {
	std::vector<Rise> out;
	bool high[tart::kTracks][16] = {}; // a gate that is up at the start is not a rise
	for (int t = 0; t < tart::kTracks; t++)
		for (int c = 0; c < r.f.track[t].channels && c < 16; c++)
			high[t][c] = !start && r.f.track[t].gate[c] > 5;
	for (int i = 0; i < n; i++) {
		r.in.play = r.in.reset = start && i == 0;
		r.step();
		for (int t = 0; t < tart::kTracks; t++)
			for (int c = 0; c < 16; c++) {
				const tart::SliceTrack& s = r.f.track[t];
				bool g = c < s.channels && s.gate[c] > 5;
				if (g && !high[t][c])
					out.push_back({i, t, c, s.voct[c], s.vel[c]});
				high[t][c] = g;
			}
	}
	return out;
}

/** The ticks (frame / 250) of the rises of one channel. */
std::vector<int> ticksOf(const std::vector<Rise>& v, int track, int ch) {
	std::vector<int> out;
	for (const Rise& x : v)
		if (x.track == track && x.ch == ch)
			out.push_back(x.frame / 250);
	return out;
}

void lane(tart::Euclid& e, int l, int steps, int pulses, int rotation = 0, int prob = 100, int vel = 100) {
	e.lane[l] = {(uint8_t)steps, (uint8_t)pulses, (uint8_t)rotation, (uint8_t)prob, (uint8_t)vel};
}

/** Track `t` becomes a Euclid track with a clip of `length` ticks in slot 0 (playing it), settings filled by `f`. */
template <class F> void euclidTrack(Rig& r, int t, F f, int length = tart::kBar, bool playing = true) {
	r.m.edit([&](tart::State& st) {
		tart::Clip& c = st.clip[t][0];
		c.length = (uint16_t)length;
		f(c.euclid);
		st.s.track[t].type = tart::Euclidean, st.s.track[t].playing = playing ? 0 : tart::kNone;
	});
}

/** The pattern as the manual draws it: x for a pulse. */
std::string pat(uint32_t mask, int steps) {
	std::string s;
	for (int i = 0; i < steps; i++)
		s += mask >> i & 1 ? 'x' : '.';
	return s;
}
tart::EuclidLane mk(int steps, int pulses, int rotation = 0) {
	return {(uint8_t)steps, (uint8_t)pulses, (uint8_t)rotation, 100, 100};
}

/** The Euclid notes in [t0, t0 + length) of the clip's launch (track `track`'s octave, the Session's scale). */
std::vector<tart::Note> gen(const tart::Session& s, const tart::Euclid& e, int64_t t0 = 0, int length = tart::kBar,
							int track = 0) {
	std::vector<tart::Note> v(tart::kMaxNotes);
	v.resize((size_t)tart::euclidNotes(s, track, e, t0, length, v.data(), tart::kMaxNotes));
	return v;
}

} // namespace

TEST(euclid_bjorklund) {
	CHECK(pat(tart::euclidMask(mk(8, 3)), 8) == "x..x..x.");
	CHECK(pat(tart::euclidMask(mk(8, 5)), 8) == "x.xx.xx.");
	CHECK(pat(tart::euclidMask(mk(16, 7)), 16) == "x..x.x.x..x.x.x.");
	CHECK(pat(tart::euclidMask(mk(16, 9)), 16) == "x.xx.x.x.xx.x.x.");
	CHECK(pat(tart::euclidMask(mk(12, 5)), 12) == "x..x.x..x.x.");
	CHECK(pat(tart::euclidMask(mk(7, 4)), 7) == "x.x.x.x");
	CHECK(pat(tart::euclidMask(mk(4, 0)), 4) == "....");
	CHECK(pat(tart::euclidMask(mk(8, 3, 1)), 8) == ".x..x..x"); // ROTATE moves the pulses later
	// Every length and pulse count: k pulses, one on the first step, gaps (all of them, round the ring) differ by at
	// most 1; every rotation is the pattern shifted later by r.
	for (int n = 1; n <= 32; n++)
		for (int k = 0; k <= n; k++) {
			uint32_t m = tart::euclidMask(mk(n, k));
			CHECK(__builtin_popcount(m) == k && (k == 0 || (m & 1)) && (n == 32 || m >> n == 0));
			int lo = 99, hi = 0, last = -1, first = -1;
			for (int p = 0; p < n; p++)
				if (m >> p & 1) {
					if (last >= 0)
						lo = std::min(lo, p - last), hi = std::max(hi, p - last);
					first = first < 0 ? p : first, last = p;
				}
			if (k > 1) {
				int wrap = first + n - last;
				lo = std::min(lo, wrap), hi = std::max(hi, wrap);
				CHECK(hi - lo <= 1);
			}
			for (int r = 0; r < n; r++) {
				uint32_t t = tart::euclidMask(mk(n, k, r));
				bool same = true;
				for (int p = 0; p < n; p++)
					same &= (t >> ((p + r) % n) & 1) == (m >> p & 1);
				CHECK(same);
			}
		}
	CHECK(tart::euclidMask(mk(16, 40)) == 0xFFFF);                            // more pulses than steps: every step
	CHECK(tart::euclidMask(mk(16, 5, 20)) == tart::euclidMask(mk(16, 5, 4))); // the rotation counts modulo the steps
	CHECK(tart::euclidMask(mk(32, 32)) == ~0u && tart::euclidMask(mk(1, 1)) == 1);
}

TEST(euclid_golden) {
	auto st = std::make_unique<tart::State>();
	tart::Euclid e;
	lane(e, 0, 16, 16, 0, 50); // E(16,16) at probability 50, seed 0: which steps since the launch have a note
	auto steps = [&](int64_t t0) {
		std::vector<int> v;
		for (const tart::Note& n : gen(st->s, e, t0)) {
			CHECK(n.pitch == tart::kDrumNote && n.vel == 100 && n.len == 24 && n.tick % 24 == 0);
			v.push_back((int)((t0 + n.tick) / 24));
		}
		return v;
	};
	CHECK(steps(0) == std::vector<int>({3, 9, 10, 11, 14, 15}));
	CHECK(steps(384) == std::vector<int>({16, 17, 18, 20, 21, 22, 23, 24, 26, 27, 29}));
	// The melodic rhythm E(8,8), C Major from octave 3: the pitch of each step since the launch, CHANGE 0 and 30.
	tart::Euclid m;
	m.mode = tart::kEuclidMelodic;
	lane(m, tart::kEuclidRhythm, 8, 8);
	auto pitches = [&](const tart::Euclid& x) {
		std::vector<int> v;
		for (const tart::Note& n : gen(st->s, x))
			v.push_back(n.pitch);
		return v;
	};
	CHECK(pitches(m) == std::vector<int>({45, 45, 41, 41, 40, 40, 45, 40, 45, 45, 41, 41, 40, 40, 45, 40}));
	m.change = 30;
	CHECK(pitches(m) == std::vector<int>({45, 45, 41, 41, 40, 40, 45, 40, 45, 45, 41, 41, 41, 47, 45, 40}));
	m.seed = 1; // another seed, another melody
	CHECK(pitches(m) != std::vector<int>({45, 45, 41, 41, 40, 40, 45, 40, 45, 45, 41, 41, 41, 47, 45, 40}));
}

TEST(euclid_drum_playback) {
	SliceRig r;
	euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 16, 4), lane(e, 2, 8, 3, 0, 100, 80); });
	r.run_(2);
	CHECK(r.f.track[0].channels == 16 && r.f.track[1].channels == 1);
	auto v = rises(r, 96000);
	std::vector<int> ch0, ch2;
	for (const Rise& x : v) {
		CHECK(x.track == 0 && (x.ch == 0 || x.ch == 2)); // no other lane
		if (x.ch == 0)
			ch0.push_back(x.frame);
		else
			ch2.push_back(x.frame), CHECK(x.voct == -22.f / 12 && std::fabs(x.vel - 80 / 127.f * 10) < 1e-4);
	}
	CHECK(ch0 == std::vector<int>({0, 24000, 48000, 72000}));
	CHECK(ch2 == std::vector<int>({0, 72 * 250, 144 * 250, 192 * 250, 264 * 250, 336 * 250}));
	CHECK(v[0].voct == -2.f && std::fabs(v[0].vel - 100 / 127.f * 10) < 1e-4);
}

TEST(euclid_melodic_playback) {
	SliceRig r; // one voice
	euclidTrack(r, 0, [](tart::Euclid& e) {
		e.mode = tart::kEuclidMelodic, e.change = 30;
		lane(e, tart::kEuclidRhythm, 8, 8, 0, 100, 90);
	});
	r.run_(2);
	CHECK(r.f.track[0].channels == 1);
	auto v = rises(r, 96000);
	const int golden[16] = {45, 45, 41, 41, 40, 40, 45, 40, 45, 45, 41, 41, 41, 47, 45, 40};
	CHECK(v.size() == 16);
	for (size_t i = 0; i < v.size() && i < 16; i++)
		CHECK(v[i].ch == 0 && v[i].frame / 250 == (int)i * 24 && v[i].voct == (golden[i] - 60) / 12.f &&
			  std::fabs(v[i].vel - 90 / 127.f * 10) < 1e-4);
	r.m.edit([](tart::State& st) { st.s.track[0].voices = 3; });
	r.run_(2);
	CHECK(r.f.track[0].channels == 3);           // the layout is Melodic: the track's Voices
	push2::Event bend = dec({0xE0, 0x7F, 0x7F}); // the touch strip bends a melodic Euclid track
	r.step(&bend);
	auto st = r.state();
	auto next = gen(st->s, st->clip[0][0].euclid, 0, 24 * 18)[17]; // tick 384 passed: the next rise is step 17
	auto w = rises(r, 7000, false);
	CHECK(!w.empty() && w[0].frame / 250 > 24 * 17 - 400 &&
		  std::fabs(w[0].voct - ((next.pitch - 60) / 12.f + 1.f / 6)) < 1e-3);
}

TEST(euclid_free_running) {
	SliceRig r;
	euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 5, 3), lane(e, 1, 16, 4); });
	auto v = rises(r, 160 * 24 * 250);
	auto step = [&](int ch, int from, int to) { // the steps since the launch in [from, to) with a rise, from `from`
		std::vector<int> s;
		for (int t : ticksOf(v, 0, ch))
			if (t % 24 == 0 && t / 24 >= from && t / 24 < to)
				s.push_back(t / 24 - from);
		return s;
	};
	CHECK(step(0, 0, 26) == std::vector<int>({0, 2, 4, 5, 7, 9, 10, 12, 14, 15, 17, 19, 20, 22, 24, 25}));
	CHECK(step(0, 0, 80) == step(0, 80, 160) && step(1, 0, 80) == step(1, 80, 160)); // E(3,5) and E(4,16): 80 steps
	CHECK(step(0, 0, 16) != step(0, 16, 32));                                        // not the clip's 16 steps
	CHECK(step(1, 0, 16) == step(1, 16, 32));                                        // a 16-step lane is
	CHECK(ticksOf(v, 0, 0).size() == (size_t)(160 / 5 * 3) && ticksOf(v, 0, 1).size() == (size_t)(160 / 16 * 4));
	// A 1-beat clip changes no rise: the clip's length is not the patterns'.
	SliceRig twin;
	euclidTrack(twin, 0, [](tart::Euclid& e) { lane(e, 0, 5, 3), lane(e, 1, 16, 4); }, tart::kPpqn);
	auto w = rises(twin, 160 * 24 * 250);
	CHECK(w.size() == v.size() && std::equal(v.begin(), v.end(), w.begin()));
	// A relaunch starts the patterns again (no pulse on tick 384 otherwise: step 16 of E(3,5) is a rest), and so does
	// RESET.
	SliceRig again(false);
	euclidTrack(again, 0, [](tart::Euclid& e) { lane(e, 0, 5, 3); });
	auto a = rises(again, 380 * 250);
	CHECK(ticksOf(a, 0, 0).size() == 10 && ticksOf(a, 0, 0).back() == 15 * 24); // steps 0 .. 15
	tapSlot(again, 0, 0);                                                       // queued for tick 384
	auto b = rises(again, 400 * 250, false);
	const int gap[8] = {0, 2, 4, 5, 7, 9, 10, 12};
	CHECK(b.size() >= 8 && b[0].frame > 900 && b[0].frame < 1010); // tick 384, 380 ticks and the tap's 2 samples later
	for (int i = 0; i < 8 && i < (int)b.size(); i++)
		CHECK((b[i].frame - b[0].frame + 125) / 250 == gap[i] * 24); // (rounded: a rise may be a sample late)
	auto c = rises(again, 26 * 24 * 250);                            // RESET: from step 0 again
	std::vector<int> first, all = ticksOf(v, 0, 0);
	std::copy_if(all.begin(), all.end(), std::back_inserter(first), [](int t) { return t < 26 * 24; });
	CHECK(ticksOf(c, 0, 0) == first);
}

TEST(euclid_swing) {
	auto ticks = [](SliceRig& r, int n) { // the first n rises, in ticks
		std::vector<int> v;
		for (const Rise& x : rises(r, 24000))
			v.push_back(x.frame / 250);
		v.resize(std::min<size_t>(v.size(), (size_t)n));
		return v;
	};
	{ // E(16,16) at 1/16, swing 100: the second step of each pair is 12 ticks late. A step that starts as the note
	  // before it sounds retriggers.
		SliceRig r;
		euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 16, 16); });
		r.m.edit([](tart::State& st) { st.s.transport.swing = 100; });
		CHECK(ticks(r, 4) == std::vector<int>({0, 36, 48, 84}));
	}
	{ // launched at tick 24 (a launch at 1/16 while the transport runs): the swing is counted from the launch
		SliceRig r(false);
		setQuant(r, 11);
		euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 16, 16); }, tart::kBar, false);
		r.m.edit([](tart::State& st) { st.s.transport.swing = 100; });
		r.in.play = true;
		r.step();
		tapSlot(r, 0, 0); // two more samples
		std::vector<int> v;
		for (const Rise& x : rises(r, 48000, false))
			v.push_back((x.frame + 3) / 250);
		v.resize(4);
		CHECK(v == std::vector<int>({24, 60, 72, 108}));
	}
	{ // 1/16t: no step lands on the second sixteenth, so none moves
		SliceRig r;
		euclidTrack(r, 0, [](tart::Euclid& e) { e.grid = 5, lane(e, 0, 16, 16); });
		r.m.edit([](tart::State& st) { st.s.transport.swing = 100; });
		CHECK(ticks(r, 6) == std::vector<int>({0, 16, 32, 48, 64, 80}));
	}
}

TEST(euclid_probability_and_change) {
	auto st = std::make_unique<tart::State>();
	tart::Euclid e;
	const int length = 24 * 1024; // 1024 steps
	lane(e, 0, 16, 16, 0, 100);
	CHECK(gen(st->s, e, 0, length).size() == 1024);
	lane(e, 0, 16, 16, 0, 0);
	CHECK(gen(st->s, e, 0, length).empty());
	lane(e, 0, 16, 16, 0, 50);
	size_t half = gen(st->s, e, 0, length).size();
	CHECK(half > 410 && half < 614);
	// CHANGE: the rhythm's pitches, `back` steps (one pass of the rhythm) apart, as a share that differs.
	tart::Euclid m;
	m.mode = tart::kEuclidMelodic, m.octaves = 4;
	lane(m, tart::kEuclidRhythm, 8, 8);
	auto differ = [&](const tart::Euclid& x, int back) {
		std::vector<tart::Note> v = gen(st->s, x, 0, 24 * 2048);
		int d = 0;
		for (size_t i = (size_t)back; i < v.size(); i++)
			d += v[i].pitch != v[i - back].pitch;
		return 100.0 * d / (double)(v.size() - (size_t)back);
	};
	CHECK(differ(m, 8) == 0); // CHANGE 0: every pass is pass 0
	m.change = 30;
	CHECK(differ(m, 8) > 15 && differ(m, 8) < 40);
	m.change = 100;
	CHECK(differ(m, 8) > 70);
	// A 5-step rhythm has a pass every 5 steps, whatever the clip's length.
	lane(m, tart::kEuclidRhythm, 5, 5);
	CHECK(differ(m, 5) > 70);
	m.change = 0;
	CHECK(differ(m, 5) == 0);
}

TEST(euclid_pitches_follow_scale) {
	auto st = std::make_unique<tart::State>();
	st->s.scale.root = 2, st->s.scale.scale = 1 /*Minor*/, st->s.track[0].octave = 2; // D minor from D2 (26)
	auto inMinor = [](int pitch, int root) { return 0x5AD >> (((pitch - root) % 12 + 12) % 12) & 1; };
	for (int octaves = 1; octaves <= 4; octaves++) {
		tart::Euclid e;
		e.mode = tart::kEuclidMelodic, e.octaves = (uint8_t)octaves, e.change = 50, e.seed = 3;
		lane(e, tart::kEuclidRhythm, 16, 16);
		std::set<int> seen;
		for (const tart::Note& n : gen(st->s, e, 0, 12288)) {
			CHECK(inMinor(n.pitch, 2) && n.pitch >= 26 && n.pitch < 26 + 12 * octaves);
			seen.insert(n.pitch);
		}
		CHECK(seen.size() > (size_t)(octaves * 7 * 3 / 4)); // the notes are spread over the whole range
	}
	// A root change re-maps a playing melody at once.
	SliceRig r;
	r.m.edit([](tart::State& s) { s.s.scale.root = 2, s.s.scale.scale = 1, s.s.track[0].octave = 2; });
	euclidTrack(r, 0, [](tart::Euclid& e) {
		e.mode = tart::kEuclidMelodic, e.octaves = 2, e.change = 50;
		lane(e, tart::kEuclidRhythm, 16, 16);
	});
	auto before = rises(r, 96000);
	r.m.edit([](tart::State& s) { s.s.scale.root = 7; });
	auto after = rises(r, 96000, false);
	CHECK(before.size() == 16 && after.size() >= 15);
	for (const Rise& x : before)
		CHECK(inMinor((int)std::lround(x.voct * 12 + 60), 2));
	for (const Rise& x : after) {
		int p = (int)std::lround(x.voct * 12 + 60);
		CHECK(inMinor(p, 7) && p >= 31 && p < 31 + 24);
	}
}

namespace {

/** Track 0 a melodic Euclid track, track 1 a drum one with lanes of 5, 7 and 16 steps, both with chance in them. */
void deterministicTracks(Rig& r) {
	euclidTrack(r, 0, [](tart::Euclid& e) {
		e.mode = tart::kEuclidMelodic, e.change = 40, e.seed = 11;
		lane(e, tart::kEuclidRhythm, 16, 7, 0, 60);
	});
	euclidTrack(r, 1, [](tart::Euclid& e) {
		e.seed = 5;
		lane(e, 0, 5, 3, 0, 70), lane(e, 1, 7, 4, 2, 70), lane(e, 2, 16, 5, 0, 70, 64);
	});
}

/** The rises with their frames in ticks (a retrigger's late sample doesn't count), in time, track and channel order. */
std::vector<Rise> inTicks(std::vector<Rise> v) {
	for (Rise& x : v)
		x.frame /= 250;
	std::stable_sort(v.begin(), v.end(), [](const Rise& a, const Rise& b) {
		return a.frame != b.frame ? a.frame < b.frame : a.track != b.track ? a.track < b.track : a.ch < b.ch;
	});
	return v;
}

} // namespace

TEST(euclid_deterministic) {
	const int bars = 6 * 96000;
	SliceRig r;
	deterministicTracks(r);
	auto a = inTicks(rises(r, bars));
	CHECK(a.size() > 60);
	auto b = inTicks(rises(r, bars)); // RESET: the same again
	CHECK(a == b);
	// Saved, sanitized (a patch has no `playing`), loaded into another Rig and launched: the same again.
	auto st = r.state();
	tart::sanitize(*st);
	st->s.track[0].playing = st->s.track[1].playing = 0;
	SliceRig other;
	other.m.load(*st);
	CHECK(a == inTicks(rises(other, bars)));
	// Each pass of the clip plays exactly what euclidNotes lists for it.
	for (int pass = 0; pass < 6; pass++) {
		for (int t = 0; t < 2; t++) {
			std::vector<Rise> have;
			for (const Rise& x : a)
				if (x.track == t && x.frame / 384 == pass)
					have.push_back({x.frame - pass * 384, t, x.ch, x.voct, x.vel});
			std::vector<Rise> want;
			for (const tart::Note& n : gen(st->s, st->clip[t][0].euclid, pass * 384, 384, t))
				want.push_back({n.tick, t, t ? n.pitch - tart::kDrumNote : 0,
								t ? (float)(n.pitch - 60) / 12.f : (float)(n.pitch - 60) / 12.f, n.vel / 127.f * 10});
			CHECK(have.size() == want.size());
			for (size_t i = 0; i < have.size() && i < want.size(); i++)
				CHECK(have[i].frame == want[i].frame && have[i].ch == want[i].ch &&
					  (t || have[i].voct == want[i].voct) && std::fabs(have[i].vel - want[i].vel) < 1e-4);
		}
	}
}

TEST(euclid_layout_follows_clip) {
	SliceRig r(false);
	r.m.edit([](tart::State& st) {
		st.s.track[0].type = tart::Euclidean, st.s.track[0].voices = 2;
		for (int sc = 0; sc < 2; sc++) { // scene 1 is a Drum clip, scene 2 a Melodic one
			tart::Clip& c = st.clip[0][sc];
			c.length = tart::kBar, c.euclid.mode = (uint8_t)sc;
			lane(c.euclid, sc ? tart::kEuclidRhythm : 0, 16, 16);
			if (!sc)
				lane(c.euclid, 1, 16, 16);
		}
	});
	r.run_(2);
	CHECK(r.f.track[0].channels == 16); // stopped: the selected scene's clip decides
	r.m.edit([](tart::State& st) { st.s.ui.scene = 1; });
	r.run_(2);
	CHECK(r.f.track[0].channels == 2);
	r.m.edit([](tart::State& st) { st.s.ui.scene = 0, st.s.track[0].playing = 0; });
	r.in.play = true;
	r.run_(1);
	tapSlot(r, 0, 1); // scene 2 waits for the next bar
	r.run_(96000 - 3);
	CHECK(r.f.track[0].channels == 16 && r.f.track[0].gate[0] == 10 && r.f.track[0].gate[1] == 10); // the drum clip
	r.run_(2); // tick 384 and the retrigger's gap
	CHECK(r.f.track[0].channels == 2 && r.f.track[0].gate[0] == 10 && r.f.track[0].gate[1] == 0);
	CHECK(r.f.track[0].voct[0] != (float)(tart::kDrumNote - 60) / 12.f); // a melodic note, not lane 0's
	// The playing clip decides, not the selected one: selecting the drum clip changes nothing now.
	r.m.edit([](tart::State& st) { st.s.ui.scene = 0; });
	r.run_(2);
	CHECK(r.f.track[0].channels == 2);
	// Stopped, it follows the selection again.
	r.in.stop = true;
	r.run_(3);
	CHECK(r.f.track[0].channels == 16);
}

TEST(euclid_clip_ops) {
	using namespace push2::cc;
	auto same = [](const tart::Euclid& a, const tart::Euclid& b) { return !std::memcmp(&a, &b, sizeof a); };
	auto settings = [](tart::Euclid& e) {
		e.seed = 5, e.grid = 2, e.mode = tart::kEuclidMelodic, e.octaves = 3, e.change = 20;
		lane(e, 3, 8, 3, 1, 50, 90), lane(e, tart::kEuclidRhythm, 12, 5, 2, 70, 80);
	};
	Rig r(false);
	setQuant(r, 0);
	euclidTrack(r, 0, settings, tart::kBar, false);
	const tart::Euclid want = r.state()->clip[0][0].euclid;
	// Duplicate: the settings go along, the step size too.
	r.press(Duplicate);
	slot(r, 0, 0, 100), slot(r, 0, 0, 0);
	tapSlot(r, 1, 2);
	r.press(Duplicate, 0);
	CHECK(same(r.state()->clip[1][2].euclid, want) && r.state()->clip[1][2].euclid.grid == 2);
	// Delete leaves the defaults; Undo brings the settings back.
	r.press(Delete);
	tapSlot(r, 0, 0);
	r.press(Delete, 0);
	auto st = r.state();
	CHECK(st->clip[0][0].length == 0 && same(st->clip[0][0].euclid, tart::Euclid()));
	r.click(Undo);
	st = r.state();
	CHECK(st->clip[0][0].length == tart::kBar && same(st->clip[0][0].euclid, want));
	// A take that ends before its clip starts leaves the defaults in the slot it made.
	Rig t(false);
	t.in.play = true;
	t.step();
	t.click(Track1Below + 1);
	t.click(Record); // a clip, queued for the next bar
	CHECK(t.state()->clip[1][0].length == tart::kMaxLength);
	t.click(New);
	st = t.state();
	CHECK(st->clip[1][0].length == 0 && same(st->clip[1][0].euclid, tart::Euclid()));
	// Double Loop and a Clip-mode length change leave the Euclid notes alone: the gate trace over the next 2 bars is
	// the one of a twin that did neither. The clip's own automation is repeated as E4.1 does.
	auto trace = [&](int how) {
		SliceRig x;
		euclidTrack(x, 0, [](tart::Euclid& e) { lane(e, 0, 5, 3), lane(e, 1, 16, 5, 2, 60); });
		x.m.edit([](tart::State& s) {
			tart::Clip& c = s.clip[0][0];
			c.nPoints[0] = 2, c.points[0][0] = {0, 0}, c.points[0][1] = {96, 65535};
		});
		if (how == 1)
			x.click(DoubleLoop);
		if (how == 2)
			x.press(Clip), x.turn(0, -8), x.press(Clip, 0); // 4 beats -> 2
		auto c = x.state()->clip[0][0];
		CHECK(c.length == (how == 1 ? 2 * tart::kBar : how == 2 ? 2 * tart::kPpqn : tart::kBar));
		CHECK(how != 1 || (c.nPoints[0] == 6 && c.points[0][3].tick == tart::kBar && c.points[0][4].tick == 480));
		return inTicks(rises(x, 2 * 96000));
	};
	auto plain = trace(0);
	CHECK(plain.size() > 20 && trace(1) == plain && trace(2) == plain);
	// QUANTIZE tapped: no popup, no undo step; the same tap on a melodic track does quantize.
	Rig q;
	euclidTrack(q, 0, [](tart::Euclid& e) { lane(e, 0, 5, 3); });
	q.m.edit([](tart::State& s) { s.clip[0][0].nNotes = 1, s.clip[0][0].notes[0] = {5, 10, 60, 100}; });
	q.click(Quantize);
	CHECK(q.view().s.ui.popup == tart::kNoPopup && q.state()->clip[0][0].notes[0].tick == 5);
	q.click(Undo);
	CHECK(q.view().s.ui.popup == tart::kNoPopup);
	q.m.edit([](tart::State& s) { s.s.track[0].type = tart::Melodic; });
	q.click(Quantize);
	CHECK(q.view().s.ui.popup == tart::kQuantizedPopup && q.state()->clip[0][0].notes[0].tick == 0);
}

TEST(euclid_view_notes) {
	SliceRig r;
	euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 5, 3), lane(e, 4, 16, 7, 0, 60); }, tart::kBar, false);
	auto v = r.view();
	CHECK(!std::memcmp(&v.euclid, &r.state()->clip[0][0].euclid, sizeof v.euclid));
	auto want = gen(v.s, v.euclid, 0, tart::kBar);
	CHECK(want.size() > 8 && v.nNotes == want.size());
	for (size_t i = 0; i < want.size() && i < v.nNotes; i++)
		CHECK(v.notes[i].tick == want[i].tick && v.notes[i].pitch == want[i].pitch && v.notes[i].len == 24 &&
			  v.notes[i].pitch >= tart::kDrumNote && v.notes[i].pitch < tart::kDrumNote + tart::kLanes);
	// Playing, in the fourth pass of the clip: what plays under the playhead, not the first pass.
	r.m.edit([](tart::State& s) { s.s.track[0].playing = 0; });
	r.in.play = true;
	r.run_(1200 * 250 + 10); // tick 1200: pass 3 begins at 1152
	v = r.view();
	CHECK(v.s.transport.tick == 1200);
	want = gen(v.s, v.euclid, 1152, tart::kBar);
	auto first = gen(v.s, v.euclid, 0, tart::kBar);
	CHECK(want.size() > 8 && v.nNotes == want.size() && v.nNotes != first.size());
	for (size_t i = 0; i < want.size() && i < v.nNotes; i++)
		CHECK(v.notes[i].tick == want[i].tick && v.notes[i].pitch == want[i].pitch);
	// A melodic clip shows its pitches from the scale; a track that isn't Euclid shows the clip's notes.
	r.m.edit([](tart::State& s) {
		s.clip[0][0].euclid.mode = tart::kEuclidMelodic;
		lane(s.clip[0][0].euclid, tart::kEuclidRhythm, 8, 8);
	});
	v = r.view();
	want = gen(v.s, v.euclid, 1152, tart::kBar);
	CHECK(want.size() == 16 && v.nNotes == 16);
	for (size_t i = 0; i < want.size() && i < v.nNotes; i++)
		CHECK(v.notes[i].tick == want[i].tick && v.notes[i].pitch == want[i].pitch && v.notes[i].pitch >= 36 &&
			  v.notes[i].pitch < 48);
	r.m.edit([](tart::State& s) {
		s.s.track[0].type = tart::Melodic, s.clip[0][0].nNotes = 1, s.clip[0][0].notes[0] = {0, 5, 60, 100};
	});
	v = r.view();
	CHECK(v.nNotes == 1 && v.notes[0].pitch == 60);
}

TEST(sanitize_euclid) {
	auto st = std::make_unique<tart::State>();
	tart::Clip& c = st->clip[0][0];
	c.length = tart::kBar;
	tart::Euclid& e = c.euclid;
	e.seed = 5000, e.mode = 7, e.octaves = 9, e.change = 200, e.grid = 9, e.spare = 3;
	e.lane[0] = {0, 99, 99, 200, 0};
	e.lane[1] = {99, 5, 5, 5, 200};
	e.lane[tart::kEuclidRhythm] = {32, 32, 31, 100, 127};
	st->clip[1][1].euclid.seed = 7, st->clip[1][1].euclid.lane[3].steps = 0; // an empty slot
	st->s.track[0].type = tart::Euclidean, st->s.track[1].type = 4;
	tart::sanitize(*st);
	CHECK(e.seed == 999 && e.mode == 1 && e.octaves == 4 && e.change == 100 && e.grid == 7 && e.spare == 0);
	CHECK(e.lane[0].steps == 1 && e.lane[0].pulses == 32 && e.lane[0].rotation == 31 && e.lane[0].probability == 100 &&
		  e.lane[0].velocity == 1);
	CHECK(e.lane[1].steps == 32 && e.lane[1].probability == 5 && e.lane[1].velocity == 127);
	CHECK(e.lane[tart::kEuclidRhythm].steps == 32 && e.lane[tart::kEuclidRhythm].rotation == 31);
	e.octaves = 0;
	tart::sanitize(*st);
	CHECK(e.octaves == 1);
	CHECK(!std::memcmp(&st->clip[1][1].euclid, &static_cast<const tart::Euclid&>(tart::Euclid()), sizeof e));
	CHECK(st->s.track[0].type == tart::Euclidean && st->s.track[1].type == tart::Melodic);
}

// ---- The Euclid screen and Drum mode on the pads (E5.2)

namespace {

/** Track 0 is a Euclid track whose slot (0, 0) is empty and selected. */
void euclidEmpty(Rig& r) {
	r.m.edit([](tart::State& st) { st.s.track[0].type = tart::Euclidean; });
	r.run_(2);
}

/** Encoder `id` turned by `raw` raw units, in messages of at most 60. */
void spin(Rig& r, int id, int raw) {
	while (raw) {
		int d = std::max(-60, std::min(raw, 60));
		r.turn(id, d), raw -= d;
	}
}

tart::Euclid settingsOf(Rig& r, int track = 0, int scene = 0) {
	return r.state()->clip[track][scene].euclid;
}

/** The colour of the pad (row 0 = bottom) in the last LED refresh. */
int padColor(Rig& r, int row, int col) {
	return r.want.note[push2::padNote(row, col)].color;
}

/** The Rig runs to `tick` of a clip that plays from the start, with the LEDs refreshed. */
void ledsAt(Rig& r, int tick) {
	r.in.play = true;
	r.run_(tick * 250 + 10);
	r.in.leds = true;
	r.step();
}

} // namespace

TEST(euclid_screen_encoders) {
	for (int note = 0; note < 2; note++) { // Note and Session mode alike
		Rig r(note);
		euclidEmpty(r);
		auto lane = [&](int l = 0) { return settingsOf(r).lane[l]; };
		spin(r, 1, 8); // PULSES: 8 raw units a pulse
		CHECK(lane().pulses == 1 && lane().steps == 16);
		spin(r, 1, 8 * 40);
		CHECK(lane().pulses == 16); // clamped at STEPS
		spin(r, 1, -8 * 8);
		CHECK(lane().pulses == 8);
		spin(r, 2, 8 * 3);
		CHECK(lane().rotation == 3);
		spin(r, 2, 8 * 14);
		CHECK(lane().rotation == 1); // ROTATE wraps
		spin(r, 2, -8 * 2);
		CHECK(lane().rotation == 15);
		spin(r, 2, -8 * 15);
		CHECK(lane().rotation == 0);
		spin(r, 0, -8 * 12); // STEPS 16 -> 4: the pulses are kept but count as 4, and 16 gives the 8 back
		CHECK(lane().steps == 4 && lane().pulses == 8);
		spin(r, 1, 8); // already at STEPS: nothing changes
		CHECK(lane().pulses == 8);
		spin(r, 0, 8 * 12);
		CHECK(lane().steps == 16 && lane().pulses == 8);
		spin(r, 0, 8 * 50), spin(r, 0, -8 * 50 - 8 * 31);
		CHECK(lane().steps == 1);
		spin(r, 0, 8 * 15);
		spin(r, 3, -2 * 30), CHECK(lane().probability == 70);
		spin(r, 3, 2 * 99), CHECK(lane().probability == 100);
		spin(r, 4, 100), CHECK(lane().velocity == 127);
		spin(r, 4, -300), CHECK(lane().velocity == 1);
		spin(r, 5, 4 * 1500), CHECK(settingsOf(r).seed == 999);
		spin(r, 5, -4 * 2000), CHECK(settingsOf(r).seed == 0);
		spin(r, 5, 4 * 12), CHECK(settingsOf(r).seed == 12);
		// Drum mode: encoders 7 and 8 do nothing, and the selected lane is the one that changes.
		spin(r, 6, 300), spin(r, 7, 300);
		CHECK(settingsOf(r).octaves == 1 && settingsOf(r).change == 0);
		r.m.edit([](tart::State& st) { st.s.track[0].lane = 5; });
		spin(r, 1, 8);
		CHECK(lane(5).pulses == 1 && lane(0).pulses == 8);
		// DELETE held: the turns do nothing.
		r.press(push2::cc::Delete);
		spin(r, 1, 80), spin(r, 5, 40);
		r.press(push2::cc::Delete, 0);
		CHECK(lane(5).pulses == 1 && settingsOf(r).seed == 12);
		// Melodic mode: the rhythm, SEED, OCTAVES and CHANGE.
		r.click(push2::cc::Track1Above + 1);
		spin(r, 1, 8 * 3), CHECK(lane(tart::kEuclidRhythm).pulses == 3 && lane(5).pulses == 1);
		spin(r, 6, 30), CHECK(settingsOf(r).octaves == 2);
		spin(r, 6, 300), CHECK(settingsOf(r).octaves == 4);
		spin(r, 6, -30 * 9), CHECK(settingsOf(r).octaves == 1);
		spin(r, 7, 2 * 30), CHECK(settingsOf(r).change == 30);
		spin(r, 7, 2 * 100), CHECK(settingsOf(r).change == 100);
		spin(r, 5, 4), CHECK(settingsOf(r).seed == 13);
		CHECK(r.view().euclid.lane[tart::kEuclidRhythm].pulses == 3);
	}
}

TEST(euclid_shift_fine) {
	Rig r;
	euclidEmpty(r);
	r.press(push2::cc::Shift);
	spin(r, 1, 8);
	CHECK(settingsOf(r).lane[0].pulses == 0 && r.state()->clip[0][0].length == 0); // 32 raw units a pulse
	spin(r, 1, 24);
	CHECK(settingsOf(r).lane[0].pulses == 1);
	spin(r, 3, -8);
	CHECK(settingsOf(r).lane[0].probability == 99);
	r.press(push2::cc::Shift, 0);
	spin(r, 3, -2);
	CHECK(settingsOf(r).lane[0].probability == 98);
}

TEST(euclid_first_edit_makes_clip) {
	using namespace push2::cc;
	Rig r;
	euclidEmpty(r);
	spin(r, 3, 10); // PROB up at 100: nothing, no clip, no undo step
	auto st = r.state();
	CHECK(st->clip[0][0].length == 0 && st->s.track[0].playing == tart::kNone);
	r.click(Undo);
	CHECK(r.view().s.ui.popup == tart::kNoPopup);
	spin(r, 1, 16); // PULSES: a 2 Bar clip, playing at once, with the transport still stopped
	st = r.state();
	CHECK(st->clip[0][0].length == 2 * tart::kBar && st->s.track[0].playing == 0 && !r.view().s.transport.playing);
	CHECK(st->clip[0][0].euclid.lane[0].pulses == 2 && st->clip[0][0].nNotes == 0);
	r.click(Undo); // one step takes the clip away again
	st = r.state();
	CHECK(st->clip[0][0].length == 0 && st->s.track[0].playing == tart::kNone);
	// A scene button does the same, and so does MELODIC; DRUM on an empty slot changes nothing.
	Rig q;
	euclidEmpty(q);
	q.click(Track1Above);
	CHECK(q.state()->clip[0][0].length == 0);
	q.click(Scene8 + 2);
	CHECK(q.state()->clip[0][0].length == 2 * tart::kBar && settingsOf(q).grid == 2 &&
		  q.state()->s.track[0].playing == 0);
	q.click(Scene8 + 2); // the size it has: no step
	q.click(Undo);
	CHECK(q.state()->clip[0][0].length == 0 && q.state()->s.track[0].playing == tart::kNone);
	// While the transport plays the launch waits for the launch quantization.
	Rig p;
	euclidEmpty(p);
	p.in.play = true;
	p.run_(300);
	spin(p, 1, 8);
	auto sp = p.state();
	CHECK(sp->clip[0][0].length == 2 * tart::kBar && sp->s.track[0].playing == tart::kNone &&
		  sp->s.track[0].queued == 0);
}

TEST(euclid_undo_per_touch) {
	Rig r;
	euclidEmpty(r);
	touch(r, 1);
	r.turn(1, 8), r.turn(1, 8), r.turn(1, 8);
	touch(r, 1, false);
	CHECK(settingsOf(r).lane[0].pulses == 3);
	touch(r, 1);
	r.turn(1, 8), r.turn(1, 8);
	touch(r, 1, false);
	CHECK(settingsOf(r).lane[0].pulses == 5);
	r.click(push2::cc::Undo); // the second touch only
	CHECK(settingsOf(r).lane[0].pulses == 3 && r.state()->clip[0][0].length != 0);
	r.click(push2::cc::Undo); // the first touch, which made the clip
	CHECK(settingsOf(r).lane[0].pulses == 0 && r.state()->clip[0][0].length == 0);
	// A touch on another encoder is another step, even in the same clip.
	touch(r, 1), r.turn(1, 8), touch(r, 1, false);
	touch(r, 3), r.turn(3, -2), touch(r, 3, false);
	r.click(push2::cc::Undo);
	CHECK(settingsOf(r).lane[0].pulses == 1 && settingsOf(r).lane[0].probability == 100);
}

TEST(euclid_delete_touch_resets) {
	using namespace push2::cc;
	Rig r;
	euclidEmpty(r);
	r.press(Delete); // an empty slot: no clip, no step
	touch(r, 1), touch(r, 1, false);
	r.press(Delete, 0);
	CHECK(r.state()->clip[0][0].length == 0);
	r.m.edit([](tart::State& st) {
		tart::Clip& c = st.clip[0][0];
		c.length = tart::kBar;
		lane(c.euclid, 0, 8, 5, 3, 40, 90);
		c.euclid.seed = 77;
	});
	r.press(Delete);
	touch(r, 1), touch(r, 1, false); // PULSES -> 0
	CHECK(settingsOf(r).lane[0].pulses == 0 && settingsOf(r).lane[0].steps == 8);
	touch(r, 1), touch(r, 1, false); // already standard: nothing
	touch(r, 0), touch(r, 0, false), touch(r, 5), touch(r, 5, false);
	touch(r, 6), touch(r, 6, false); // OCTAVES in Drum mode: nothing
	r.press(Delete, 0);
	tart::Euclid e = settingsOf(r);
	CHECK(e.lane[0].steps == 16 && e.lane[0].rotation == 3 && e.lane[0].probability == 40 && e.seed == 0);
	r.click(Undo); // one step per touch: SEED, then STEPS, then PULSES
	CHECK(settingsOf(r).seed == 77 && settingsOf(r).lane[0].steps == 16);
	r.click(Undo);
	CHECK(settingsOf(r).lane[0].steps == 8 && settingsOf(r).lane[0].pulses == 0);
	r.click(Undo);
	CHECK(settingsOf(r).lane[0].pulses == 5);
	r.press(Delete); // Melodic mode: CHANGE and OCTAVES go back too
	r.m.edit(
		[](tart::State& st) { st.clip[0][0].euclid.mode = tart::kEuclidMelodic, st.clip[0][0].euclid.change = 20; });
	touch(r, 7), touch(r, 7, false);
	r.press(Delete, 0);
	CHECK(settingsOf(r).change == 0);
}

TEST(euclid_mode_buttons) {
	using namespace push2::cc;
	SliceRig r;
	euclidEmpty(r);
	auto up = [&](int i) { return r.want.cc[Track1Above + i].color; };
	r.in.leds = true;
	r.step();
	CHECK(up(0) == push2::kRgbWhite && up(1) == push2::kRgbDarkGray && r.f.track[0].channels == 16);
	r.click(Track1Above); // DRUM on an empty slot: nothing
	CHECK(r.state()->clip[0][0].length == 0);
	r.click(Track1Above + 1); // MELODIC: makes the clip, one undo step, and the layout switches to the Voices
	r.step();
	auto st = r.state();
	CHECK(st->clip[0][0].length == 2 * tart::kBar && st->clip[0][0].euclid.mode == tart::kEuclidMelodic);
	CHECK(up(0) == push2::kRgbDarkGray && up(1) == push2::kRgbWhite && r.f.track[0].channels == 1);
	r.click(Track1Above + 1); // the same again: nothing
	r.click(Undo);
	r.step();
	CHECK(r.state()->clip[0][0].length == 0 && r.f.track[0].channels == 16 && up(0) == push2::kRgbWhite);
	// A clip that exists: the switch is one step, and the notes of the other layout stop.
	r.m.edit([](tart::State& s) {
		lane(s.clip[0][0].euclid, 0, 16, 16);
		s.clip[0][0].length = tart::kBar, s.s.track[0].playing = 0;
	});
	r.in.play = true;
	r.run_(300);
	CHECK(r.f.track[0].gate[0] == 10);
	r.click(Track1Above + 1);
	r.run_(2);
	CHECK(r.f.track[0].channels == 1 && r.f.track[0].gate[0] == 0 && settingsOf(r).mode == tart::kEuclidMelodic);
	r.click(Undo);
	CHECK(settingsOf(r).mode == tart::kEuclidDrum);
	// Session mode too; but not on the Device screen, with Fixed Length held, or with Quantize's settings.
	r.m.edit([](tart::State& s) { s.s.ui.pads = tart::kSession; });
	r.click(Track1Above + 1);
	CHECK(settingsOf(r).mode == tart::kEuclidMelodic);
	r.click(Track1Above);
	CHECK(settingsOf(r).mode == tart::kEuclidDrum);
	r.click(Device);
	r.click(Track1Above + 1);
	CHECK(settingsOf(r).mode == tart::kEuclidDrum);
	r.click(Device);
	r.press(FixedLength);
	r.click(Track1Above + 1);
	CHECK(settingsOf(r).mode == tart::kEuclidDrum);
	r.press(FixedLength, 0);
	r.press(Quantize);
	r.run_(16000);
	r.click(Track1Above + 1);
	r.press(Quantize, 0);
	CHECK(settingsOf(r).mode == tart::kEuclidDrum);
	// A melodic or drum track has no such buttons.
	r.m.edit([](tart::State& s) { s.s.track[0].type = tart::Melodic; });
	r.click(Track1Above + 1);
	CHECK(settingsOf(r).mode == tart::kEuclidDrum);
}

TEST(scene_buttons_match_labels) { // the top button reads 1/32t (Push2-map: CC 43), the bottom 1/4 (CC 36)
	using namespace push2::cc;
	Rig ses(false);
	ses.click(Scene1 - 1); // Session mode: a scene button still launches (selects) its scene, scene 2 below scene 1
	CHECK(ses.view().s.ui.scene == 1);
	Rig r;
	r.click(Layout);
	r.click(Scene1);
	CHECK(r.view().s.track[0].grid == 7);
	r.click(Scene8);
	CHECK(r.view().s.track[0].grid == 0);
	r.click(Scene8 + 4); // 1/16
	r.in.leds = true;
	r.step();
	CHECK(r.want.cc[40].color == push2::kRgbWhite && r.want.cc[Scene1].color == push2::kRgbDarkGray);
	Rig q;
	euclidEmpty(q);
	q.click(Scene1);
	CHECK(settingsOf(q).grid == 7);
	q.click(Scene8);
	CHECK(settingsOf(q).grid == 0);
}

TEST(euclid_grid_per_clip) {
	using namespace push2::cc;
	SliceRig r;
	euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 16, 16); });
	r.m.edit([](tart::State& s) {
		s.clip[0][1].length = tart::kBar, lane(s.clip[0][1].euclid, 0, 16, 16);
		s.s.track[1].grid = 6, s.s.track[1].type = tart::Euclidean;
	});
	r.click(Scene8 + 2); // 1/8 on the selected clip
	auto st = r.state();
	CHECK(st->clip[0][0].euclid.grid == 2 && st->clip[0][1].euclid.grid == 4 && st->s.track[0].grid == 4);
	r.in.leds = true;
	r.step();
	for (int i = 0; i < 8; i++)
		CHECK(r.want.cc[Scene8 + i].color == (i == 2 ? push2::kRgbWhite : push2::kRgbDarkGray));
	auto v = rises(r, 96000); // E(16,16) at 1/8: a rise every 48 ticks
	std::vector<int> want;
	for (int t = 0; t < 384; t += 48)
		want.push_back(t);
	CHECK(ticksOf(v, 0, 0) == want);
	r.m.edit([](tart::State& s) { s.s.ui.scene = 1; });
	r.step();
	CHECK(r.want.cc[Scene8 + 4].color == push2::kRgbWhite && r.want.cc[Scene8 + 2].color == push2::kRgbDarkGray);
	r.click(Undo); // one step, which the scene's selection doesn't matter to
	CHECK(r.state()->clip[0][0].euclid.grid == 4);
	// Duplicate copies the size to a track with another Track::grid, and leaves that grid alone.
	r.click(Scene8 + 7);
	r.m.edit([](tart::State& s) { s.s.ui.pads = tart::kSession; });
	r.press(Duplicate);
	slot(r, 0, 1, 100), slot(r, 0, 1, 0);
	tapSlot(r, 1, 3);
	r.press(Duplicate, 0);
	st = r.state();
	CHECK(st->clip[1][3].euclid.grid == 7 && st->s.track[1].grid == 6);
}

TEST(euclid_drum_pads) {
	using namespace push2::cc;
	SliceRig r;
	euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 16, 4), lane(e, 3, 8, 5, 1, 70, 90); }, tart::kBar, false);
	r.run_(2);
	r.pad(0, 2, 100); // lane 3 plays channel 2 and is selected
	CHECK(r.f.track[0].gate[2] == 10 && r.f.track[0].voct[2] == (tart::kDrumNote + 2 - 60) / 12.f);
	CHECK(r.view().s.track[0].lane == 2);
	r.pad(0, 2, 0);
	CHECK(r.f.track[0].gate[2] == 0);
	r.press(Select);
	r.pad(1, 0, 100); // SELECT: only selects
	CHECK(r.f.track[0].gate[4] == 0 && r.view().s.track[0].lane == 4);
	r.pad(1, 0, 0);
	r.press(Select, 0);
	// DELETE + lane resets that lane: one undo step; nothing on a lane that is standard.
	r.press(Delete);
	r.pad(0, 3, 100), r.pad(0, 3, 0);
	CHECK(r.f.track[0].gate[3] == 0 && r.view().s.track[0].lane == 4);
	const tart::EuclidLane standard;
	const tart::Euclid now = settingsOf(r);
	CHECK(!std::memcmp(&now.lane[3], &standard, sizeof standard));
	r.pad(0, 1, 100), r.pad(0, 1, 0);
	r.press(Delete, 0);
	r.click(Undo);
	CHECK(settingsOf(r).lane[3].pulses == 5 && settingsOf(r).lane[0].pulses == 4);
	r.click(Undo);
	CHECK(r.view().s.ui.popup == tart::kUndoPopup && settingsOf(r).lane[3].pulses == 5);
	// The step pads and the lower right 4 x 4 add no note, make no clip and hold nothing; so do PAGE, OCTAVE, LAYOUT.
	Rig q;
	euclidEmpty(q);
	for (int row : {4, 5, 7})
		for (int col : {0, 5})
			q.pad(row, col, 100), q.pad(row, col, 0);
	q.pad(0, 5, 100), q.pad(3, 7, 100), q.pad(3, 7, 0), q.pad(0, 5, 0);
	for (int b : {(int)OctaveUp, (int)OctaveDown, (int)Layout, (int)PageLeft, (int)PageRight})
		q.click(b);
	auto st = q.state();
	CHECK(st->clip[0][0].length == 0 && st->clip[0][0].nNotes == 0 && st->s.ui.heldStep == -1);
	CHECK(st->s.track[0].octave == 3 && st->s.ui.noteLayout == 0 && st->s.ui.page == 0 && st->s.track[0].lane == 0);
	// QUANTIZE held: a lane pad does nothing.
	q.press(Quantize);
	q.pad(0, 2, 100), q.pad(0, 2, 0);
	q.press(Quantize, 0);
	CHECK(q.state()->s.track[0].lane == 0 && q.view().s.ui.popup == tart::kNoPopup);
	// A melodic track still plays its keyboard.
	q.m.edit([](tart::State& s) { s.s.track[0].type = tart::Melodic; });
	q.run_(2);
	q.pad(0, 0, 100);
	CHECK(q.view().s.ui.pad[0] == 100);
}

TEST(euclid_drum_leds) {
	using namespace push2::cc;
	Rig r;
	euclidTrack(r, 0, [](tart::Euclid& e) {
		lane(e, 0, 16, 5), lane(e, 1, 5, 2, 0, 100, 50), lane(e, 2, 3, 3), lane(e, 4, 4, 1, 1);
	});
	r.m.edit([](tart::State& s) { s.s.track[0].lane = 1; });
	const int full = tart::kPaletteFull, dim = tart::kPaletteDim;
	auto pad = [&](int k) { return padColor(r, 7 - k / 8, k % 8); }; // the step pad k of the shown pattern
	r.in.leds = true;
	r.step();
	// Lane 2 (5 steps, 2 pulses, VEL 50): x.x.. in the dim colour, dark gray rests, black from STEPS on.
	CHECK(pat(tart::euclidMask(mk(5, 2)), 5) == "x.x..");
	CHECK(pad(0) == dim && pad(1) == push2::kRgbDarkGray && pad(2) == dim && pad(3) == push2::kRgbDarkGray &&
		  pad(4) == push2::kRgbDarkGray);
	for (int k = 5; k < 32; k++)
		CHECK(pad(k) == push2::kBlack);
	// The lanes: the selected one white, with pulses the colour, without dim; the lower right, PAGE, OCTAVE dark.
	CHECK(padColor(r, 0, 1) == push2::kRgbWhite && padColor(r, 0, 0) == full && padColor(r, 0, 2) == full &&
		  padColor(r, 0, 3) == dim && padColor(r, 1, 0) == full && padColor(r, 1, 1) == dim);
	for (int row = 0; row < 4; row++)
		for (int col = 4; col < 8; col++)
			CHECK(padColor(r, row, col) == push2::kBlack);
	for (int b : {(int)PageLeft, (int)PageRight, (int)OctaveUp, (int)OctaveDown, (int)Layout})
		CHECK(r.want.cc[b].color == 0);
	for (int i = 0; i < 8; i++)
		CHECK(r.want.cc[Scene8 + i].color == (i == 4 ? push2::kRgbWhite : push2::kRgbDarkGray));
	// A 16-step lane in the full colour; a rest, and the pad past STEPS (a 3-step lane).
	r.m.edit([](tart::State& s) { s.s.track[0].lane = 0; });
	r.step();
	CHECK(pat(tart::euclidMask(mk(16, 5)), 16) == "x..x..x..x..x...");
	CHECK(pad(0) == full && pad(1) == push2::kRgbDarkGray && pad(3) == full && pad(15) == push2::kRgbDarkGray);
	r.m.edit([](tart::State& s) { s.s.track[0].lane = 1; });
	// The playhead is the lane's own step: at step 5 since the launch a 5-step lane is on step 1 again while a 16-step
	// lane is on step 5; at step 7, on 3 (pad 2) and 7. The lanes with a pulse on the step sound: green.
	ledsAt(r, 24 * 5 + 5);
	CHECK(pad(0) == push2::kGreen && pad(5) == push2::kBlack);
	CHECK(padColor(r, 0, 1) == push2::kGreen && padColor(r, 0, 2) == push2::kGreen &&
		  padColor(r, 1, 0) == push2::kGreen);
	CHECK(padColor(r, 0, 0) == full && padColor(r, 0, 3) == dim); // lane 1 has no pulse on step 5, lane 4 none at all
	r.run_(24 * 2 * 250);
	r.in.leds = true;
	r.step();
	CHECK(pad(2) == push2::kGreen && pad(0) == dim);
	r.m.edit([](tart::State& s) { s.s.track[0].lane = 0; });
	r.step();
	CHECK(pad(7) == push2::kGreen && pad(5) == push2::kRgbDarkGray); // step 7 since the launch of 16
	// A take on the slot: the playhead is red.
	r.m.edit([](tart::State& s) { s.s.ui.recTrack = 0, s.s.ui.recScene = 0; });
	r.step();
	CHECK(pad(7) == push2::kRed);
}

TEST(euclid_record_no_pads) {
	using namespace push2::cc;
	SliceRig r;
	euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 16, 4); }, tart::kBar, false);
	r.click(Automate);
	r.click(Device);
	r.click(Record); // a take on the clip: it launches and starts the transport
	runTo(r, 24);
	r.pad(0, 5 - 4, 100); // a lane pad (lane 2): played, not recorded
	CHECK(r.f.track[0].gate[1] == 10);
	runTo(r, 40);
	r.pad(0, 1, 0);
	touch(r, 0);
	r.turn(0, 10);
	runTo(r, 60);
	touch(r, 0, false);
	r.click(Record);
	auto st = r.state();
	CHECK(st->clip[0][0].nNotes == 0 && st->clip[0][0].nPoints[0] >= 2 && st->s.ui.recTrack == -1);
	r.click(Undo); // the whole take is one step
	st = r.state();
	CHECK(st->clip[0][0].nNotes == 0 && st->clip[0][0].nPoints[0] == 0 && st->clip[0][0].length == tart::kBar);
}

// ---- Melodic mode on the pads, and the fuzz (E5.3)

TEST(euclid_melodic_pads) {
	using namespace push2::cc;
	SliceRig r;
	euclidTrack(
		r, 0, [](tart::Euclid& e) { e.mode = tart::kEuclidMelodic, lane(e, tart::kEuclidRhythm, 8, 8); }, tart::kBar,
		false);
	r.run_(2);
	// The lower four rows are the keyboard of Note mode: C Major from octave 3, on the track's one voice.
	r.pad(0, 0, 100);
	CHECK(r.f.track[0].channels == 1 && r.f.track[0].gate[0] == 10 && r.f.track[0].voct[0] == -2.f);
	r.pad(0, 0, 0);
	CHECK(r.f.track[0].gate[0] == 0);
	bool root, in;
	const int p = tart::padPitch(r.view().s.scale, 3, 3, 5, root, in);
	r.pad(3, 5, 100); // also in the lower right: there is no loop selector here
	CHECK(r.f.track[0].gate[0] == 10 && r.f.track[0].voct[0] == (p - 60) / 12.f);
	r.pad(3, 5, 0);
	r.press(Select); // SELECT, DELETE: still a keyboard
	r.pad(0, 1, 100);
	CHECK(r.f.track[0].gate[0] == 10);
	r.pad(0, 1, 0);
	r.press(Select, 0);
	// The step pads show the pattern: no note, no clip change, nothing held.
	for (int row = 4; row < 8; row++)
		for (int col = 0; col < 8; col++)
			r.pad(row, col, 100), CHECK(r.f.track[0].gate[0] == 0), r.pad(row, col, 0);
	auto st = r.state();
	CHECK(st->clip[0][0].nNotes == 0 && st->s.ui.heldStep == -1 && st->s.track[0].lane == 0);
	// OCTAVE UP and DOWN set the base of the pitches, the keyboard's and the clip's, and stop at 0 and kMaxOctave.
	std::vector<int> was;
	for (const tart::Note& n : gen(st->s, st->clip[0][0].euclid))
		was.push_back(n.pitch);
	r.click(OctaveUp);
	st = r.state();
	CHECK(st->s.track[0].octave == 4 && st->s.ui.popup == tart::kNoPopup);
	auto now = gen(st->s, st->clip[0][0].euclid);
	CHECK(!was.empty() && now.size() == was.size());
	for (size_t i = 0; i < was.size() && i < now.size(); i++)
		CHECK(now[i].pitch == was[i] + 12);
	r.pad(0, 0, 100);
	CHECK(r.f.track[0].voct[0] == -1.f);
	r.pad(0, 0, 0);
	r.m.edit([](tart::State& s) { s.s.track[0].playing = 0; });
	auto v = rises(r, 3000); // the first generated note, an octave up
	CHECK(!v.empty() && v[0].voct == (now[0].pitch - 60) / 12.f);
	for (int i = 0; i < 9; i++)
		r.click(OctaveDown);
	CHECK(r.state()->s.track[0].octave == 0);
	for (int i = 0; i < 12; i++)
		r.click(OctaveUp);
	CHECK(r.state()->s.track[0].octave == tart::kMaxOctave);
	r.click(Layout); // LAYOUT and PAGE do nothing
	r.click(PageRight);
	st = r.state();
	CHECK(st->s.ui.noteLayout == 0 && st->s.ui.page == 0);
	// A new Melodic clip made from the screen has the keyboard at once; the same buttons in Drum mode do nothing.
	Rig q;
	euclidEmpty(q);
	q.click(OctaveUp);
	CHECK(q.state()->s.track[0].octave == 3);
	q.click(Track1Above + 1);
	q.pad(0, 5, 100);
	CHECK(q.view().s.ui.pad[5] == 100 && settingsOf(q).mode == tart::kEuclidMelodic);
	q.pad(0, 5, 0);
	q.click(OctaveUp);
	CHECK(q.state()->s.track[0].octave == 4);
	// The keyboard is played, never recorded: a take on the clip keeps no note, and is one undo step.
	SliceRig t;
	euclidTrack(
		t, 0, [](tart::Euclid& e) { e.mode = tart::kEuclidMelodic, lane(e, tart::kEuclidRhythm, 8, 3); }, tart::kBar,
		false);
	t.click(Record);
	runTo(t, 24);
	t.pad(0, 0, 100);
	CHECK(t.f.track[0].gate[0] == 10);
	runTo(t, 40);
	t.pad(0, 0, 0);
	t.click(OctaveUp);
	runTo(t, 60);
	t.pad(1, 2, 100), runTo(t, 70), t.pad(1, 2, 0);
	t.click(Record);
	st = t.state();
	CHECK(st->clip[0][0].nNotes == 0 && st->s.ui.recTrack == -1 && st->clip[0][0].length == tart::kBar);
	t.click(Undo);
	st = t.state();
	CHECK(st->clip[0][0].nNotes == 0 && st->clip[0][0].length == tart::kBar && st->s.track[0].octave == 4);
}

TEST(euclid_mode_switch_silences) {
	using namespace push2::cc;
	SliceRig r; // 16 Voices: the channel count is 16 in both layouts, so only the switch itself can stop the note
	euclidTrack(r, 0, [](tart::Euclid& e) { e.grid = 0, lane(e, 0, 4, 1); });
	r.m.edit([](tart::State& s) { s.s.track[0].voices = 16; });
	r.in.play = true;
	r.run_(250 * 10); // tick 10: lane 1's note sounds for a beat
	CHECK(r.f.track[0].channels == 16 && r.f.track[0].gate[0] == 10);
	r.click(Track1Above + 1); // MELODIC: the melodic rhythm has no pulse, so nothing takes the note's place
	r.run_(2);
	CHECK(r.f.track[0].channels == 16 && r.f.track[0].gate[0] == 0);
	r.run_(250 * 100); // and it does not come back as a stuck gate
	for (int c = 0; c < 16; c++)
		CHECK(r.f.track[0].gate[c] == 0);
}

TEST(euclid_melodic_leds) {
	using namespace push2::cc;
	Rig r;
	euclidTrack(r, 0, [](tart::Euclid& e) { e.mode = tart::kEuclidMelodic, lane(e, tart::kEuclidRhythm, 8, 8); });
	const int full = tart::kPaletteFull;
	auto keys = [&](int pitch) { // the keyboard pads (rows 0 to 3) of a pitch
		std::vector<std::pair<int, int>> v;
		bool root, in;
		for (int row = 0; row < 4; row++)
			for (int col = 0; col < 8; col++)
				if (tart::padPitch(r.view().s.scale, 3, row, col, root, in) == pitch)
					v.push_back({row, col});
		return v;
	};
	auto green = [&](int row, int col) { return padColor(r, row, col) == push2::kGreen; };
	// Step 0 of E(8,8) since the launch sounds the 45: green on its pads only (the others root, in scale, or dark).
	ledsAt(r, 5);
	CHECK(!keys(45).empty() && !keys(41).empty());
	for (int row = 0; row < 4; row++)
		for (int col = 0; col < 8; col++) {
			bool root, in;
			const int p = tart::padPitch(r.view().s.scale, 3, row, col, root, in);
			CHECK(green(row, col) == (p == 45));
			CHECK(p == 45 || padColor(r, row, col) == (root ? full : in ? push2::kRgbWhite : push2::kBlack));
		}
	// The rhythm: its own playhead on pad k = step k (the top row first), pulses in the track colour, black from STEPS.
	CHECK(green(7, 0) && padColor(r, 7, 1) == full && padColor(r, 7, 7) == full);
	for (int row = 4; row < 7; row++)
		for (int col = 0; col < 8; col++)
			CHECK(padColor(r, row, col) == push2::kBlack);
	for (int i = 0; i < 8; i++)
		CHECK(r.want.cc[Scene8 + i].color == (i == 4 ? push2::kRgbWhite : push2::kRgbDarkGray));
	// Step 2 sounds the 41 (E(8,8): 45 45 41 ...), and the playhead has moved on.
	ledsAt(r, 0), r.run_(24 * 2 * 250);
	r.in.leds = true;
	r.step();
	for (auto k : keys(41))
		CHECK(green(k.first, k.second));
	for (auto k : keys(45))
		CHECK(!green(k.first, k.second));
	CHECK(green(7, 2) && padColor(r, 7, 0) == full);
	// OCTAVE lit while it can move, LAYOUT, PAGE dark (LAYOUT too after a press).
	CHECK(r.want.cc[OctaveUp].color != 0 && r.want.cc[OctaveDown].color != 0);
	CHECK(r.want.cc[Layout].color == 0 && r.want.cc[PageLeft].color == 0 && r.want.cc[PageRight].color == 0);
	r.m.edit([](tart::State& s) { s.s.track[0].octave = 0; });
	r.step();
	CHECK(r.want.cc[OctaveUp].color != 0 && r.want.cc[OctaveDown].color == 0);
	r.m.edit([](tart::State& s) { s.s.track[0].octave = tart::kMaxOctave; });
	r.step();
	CHECK(r.want.cc[OctaveUp].color == 0 && r.want.cc[OctaveDown].color != 0);
	r.m.edit([](tart::State& s) { s.s.track[0].octave = 3, s.s.ui.noteLayout = 1; });
	r.step();
	CHECK(r.want.cc[Layout].color == 0 && padColor(r, 5, 0) == push2::kBlack && green(7, 2));
	r.m.edit([](tart::State& s) { s.s.ui.noteLayout = 0; });
	// A take: the sounding note and the playhead are red.
	r.m.edit([](tart::State& s) { s.s.ui.recTrack = 0, s.s.ui.recScene = 0; });
	r.step();
	for (auto k : keys(41))
		CHECK(padColor(r, k.first, k.second) == push2::kRed);
	CHECK(padColor(r, 7, 2) == push2::kRed);
	// A pad held by hand: green on every pad of its pitch, with the clip stopped.
	Rig h;
	euclidTrack(
		h, 0, [](tart::Euclid& e) { e.mode = tart::kEuclidMelodic, lane(e, tart::kEuclidRhythm, 8, 8); }, tart::kBar,
		false);
	h.run_(2);
	h.pad(0, 7, 100); // 36 + 12
	h.in.leds = true;
	h.step();
	CHECK(padColor(h, 0, 7) == push2::kGreen && padColor(h, 0, 0) == full);
	h.pad(0, 7, 0);
	h.in.leds = true;
	h.step();
	CHECK(padColor(h, 0, 7) == full);
	// The pads follow the selected clip, the outputs the playing one: a Drum clip plays, a Melodic one is selected.
	// The drum lane's channel is no note of the keyboard.
	Rig m;
	euclidTrack(m, 0, [](tart::Euclid& e) { lane(e, 0, 16, 16); });
	m.m.edit([](tart::State& s) {
		s.clip[0][1].length = tart::kBar, s.clip[0][1].euclid.mode = tart::kEuclidMelodic, s.s.ui.scene = 1;
		lane(s.clip[0][1].euclid, tart::kEuclidRhythm, 8, 8);
	});
	ledsAt(m, 5);
	CHECK(padColor(m, 0, 0) == full && padColor(m, 7, 0) == full); // not the drum channel's green, no playhead
}

TEST(euclid_pads_silent_across_modes) {
	// The selected clip and the playing clip are in different modes: the pads follow the selected one, the outputs
	// the playing one, so a pad plays nothing rather than a note on the other mode's channels.
	SliceRig r;
	euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 16, 1); }); // a Drum clip plays
	r.m.edit([](tart::State& s) {
		s.clip[0][1].length = tart::kBar, s.clip[0][1].euclid.mode = tart::kEuclidMelodic, s.s.ui.scene = 1;
	});
	r.run_(2);
	CHECK(r.f.track[0].channels == 16);
	r.pad(0, 0, 100); // the Melodic keyboard of scene 2
	for (int c = 0; c < 16; c++)
		CHECK(r.f.track[0].gate[c] == 0);
	r.pad(0, 0, 0);
	// The other way round: a Melodic clip plays, the selected Drum clip's lane pad selects but plays nothing.
	SliceRig m;
	euclidTrack(m, 0, [](tart::Euclid& e) { e.mode = tart::kEuclidMelodic; });
	m.m.edit([](tart::State& s) { s.clip[0][1].length = tart::kBar, s.s.ui.scene = 1; });
	m.run_(2);
	CHECK(m.f.track[0].channels == 1);
	m.pad(0, 2, 100);
	CHECK(m.f.track[0].gate[0] == 0 && m.view().s.track[0].lane == 2);
	m.pad(0, 2, 0);
}

namespace {

/** The same Slice frame, sample for sample (the padding bytes are not compared). */
bool sameFrame(const tart::SliceFrame& a, const tart::SliceFrame& b) {
	if (a.frame != b.frame)
		return false;
	for (int t = 0; t < tart::kTracks; t++) {
		const tart::SliceTrack &x = a.track[t], &y = b.track[t];
		if (std::memcmp(x.voct, y.voct, sizeof x.voct) || std::memcmp(x.gate, y.gate, sizeof x.gate) ||
			std::memcmp(x.vel, y.vel, sizeof x.vel) || std::memcmp(x.cc, y.cc, sizeof x.cc) || x.level != y.level ||
			x.pan != y.pan || x.channels != y.channels || x.rgb != y.rgb)
			return false;
	}
	return true;
}

struct Rng { // xorshift: the same on every platform
	uint32_t s;
	int n(int k) { return s ^= s << 13, s ^= s >> 17, s ^= s << 5, (int)(s % (uint32_t)k); }
};

/** Two identical Rigs fed the same input one sample at a time, their Slice frames compared on every sample. */
struct Twins {
	SliceRig a, b;
	int samples = 0, diverged = -1, lay[tart::kTracks] = {}; // lay: 0 not Euclid, 1 Drum layout, 2 Melodic layout
	int rises[3] = {};                                       // gate rises by layout of a Euclid track (0: none)
	bool high[tart::kTracks][16] = {};
	void step(const push2::Event* e = nullptr) {
		a.step(e), b.step(e);
		if (diverged < 0 && !sameFrame(a.f, b.f))
			diverged = samples;
		samples++;
		for (int t = 0; t < tart::kTracks; t++)
			for (int c = 0; c < 16; c++) {
				bool g = c < a.f.track[t].channels && a.f.track[t].gate[c] > 5;
				rises[lay[t]] += g && !high[t][c], high[t][c] = g;
			}
	}
	void idle(int n) {
		while (n-- > 0)
			step();
	}
	void press(int cc, int v = 127) {
		push2::Event e = dec({0xB0, (uint8_t)cc, (uint8_t)v});
		step(&e);
	}
	void click(int cc) { press(cc), press(cc, 0); }
	void turn(int id, int d) {
		push2::Event e = dec({0xB0, (uint8_t)(71 + id), (uint8_t)(d & 127)});
		step(&e);
	}
	void touch(int id, bool on) {
		push2::Event e = dec({0x90, (uint8_t)id, (uint8_t)(on ? 127 : 0)});
		step(&e);
	}
	void pad(int row, int col, int vel) {
		push2::Event e = dec({vel ? (uint8_t)0x90 : (uint8_t)0x80, push2::padNote(row, col), (uint8_t)vel});
		step(&e);
	}
	template <class F> void edit(F f) { a.m.edit(f), b.m.edit(f); }
};

} // namespace

// ---- Pattern-step locks (E5.4)

namespace {

/** The step pad of pattern step `k` (the top row is steps 1 to 8), pressed or, with vel 0, let go. */
void stepPad(Rig& r, int k, int vel = 100) {
	r.pad(7 - k / 8, k % 8, vel);
}

tart::Lock lk(int lane, int step, int target, int value) {
	return {(uint8_t)lane, (uint8_t)step, (uint8_t)target, 0, (uint16_t)value};
}
using Locks = std::vector<tart::Lock>;
Locks locksOf(const tart::State& st, int track = 0, int scene = 0) {
	const tart::Clip& c = st.clip[track][scene];
	return Locks(c.locks, c.locks + c.nLocks);
}
bool sameLocks(const Locks& a, const Locks& b) {
	return a.size() == b.size() && (a.empty() || !std::memcmp(a.data(), b.data(), a.size() * sizeof(tart::Lock)));
}
/** The locks of slot (0, 0) are `l` (sorted by key already). */
void setLocks(Rig& r, const Locks& l) {
	r.m.edit([&](tart::State& st) {
		tart::Clip& c = st.clip[0][0];
		c.nLocks = (uint16_t)l.size();
		std::copy(l.begin(), l.end(), c.locks);
	});
}
float V(int value) { // a unipolar lane's volts
	return (float)value * 10.f / 65535.f;
}

/** Runs a SliceRig (the first sample starts the transport) and returns CC `j` of track 0 `off` samples after each of
 * the ticks [from, to) began: at 120 BPM a tick is 250 samples. */
std::vector<float> ccTrace(SliceRig& r, int from, int to, int j = 0, int off = 5) {
	std::vector<float> out;
	for (int k = from; k < to; k++) {
		while (r.in.frame < 250 * k + off)
			r.in.play = r.in.frame == 0, r.step();
		out.push_back(r.f.track[0].cc[j]);
	}
	return out;
}

/** The transport starts on the first sample (the Slice tests count ticks from it: 250 samples each). */
void start(Rig& r) {
	r.in.play = true;
	r.step();
}

} // namespace

TEST(euclid_lock_make) {
	using namespace push2::cc;
	Rig r;
	euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 16, 4); }, tart::kBar, false);
	r.click(Device);
	stepPad(r, 3);
	CHECK(r.view().s.ui.heldStep == 7 * 8 + 3);
	r.turn(0, 1); // CC 1 starts from the lane's value (0) and moves one unit
	CHECK(sameLocks(locksOf(*r.state()), Locks{lk(0, 3, 0, 312)}));
	r.turn(0, 2); // more turns move the same lock
	CHECK(sameLocks(locksOf(*r.state()), Locks{lk(0, 3, 0, 936)}));
	r.turn(1, 3); // CC 2 on the same step: another lock, in order
	CHECK(sameLocks(locksOf(*r.state()), Locks{lk(0, 3, 0, 936), lk(0, 3, 1, 936)}));
	stepPad(r, 3, 0);
	auto st = r.state();
	CHECK(r.view().s.ui.heldStep == -1 && st->s.track[0].value[0] == 0 && st->clip[0][0].nPoints[0] == 0);
	r.click(Undo); // the hold is one undo step
	CHECK(locksOf(*r.state()).empty());
	r.press(Shift);
	r.click(Undo);
	r.press(Shift, 0);
	CHECK(r.state()->clip[0][0].nLocks == 2);
	r.press(Shift); // SHIFT: 312 / 31 = 10 a unit
	stepPad(r, 3);
	r.turn(0, 1);
	r.press(Shift, 0);
	stepPad(r, 3, 0);
	CHECK(sameLocks(locksOf(*r.state()), Locks{lk(0, 3, 0, 946), lk(0, 3, 1, 936)}));
	// The 257th lock: "Clip full", the clip as it was. A lock that exists still moves.
	r.m.edit([](tart::State& s) {
		tart::Clip& c = s.clip[0][0];
		c.nLocks = tart::kMaxLocks;
		for (int i = 0; i < tart::kMaxLocks; i++)
			c.locks[i] = lk(i / 16, i % 16, 0, 1000 + i);
	});
	stepPad(r, 3);
	auto before = r.state();
	r.turn(1, 5);
	auto after = r.state();
	CHECK(r.view().s.ui.popup == tart::kClipFullPopup && after->clip[0][0].nLocks == 256);
	CHECK(!std::memcmp(&before->clip[0][0], &after->clip[0][0], sizeof(tart::Clip)));
	r.turn(0, 5);
	CHECK(r.state()->clip[0][0].locks[3].value == 1003 + 5 * 312 && r.state()->clip[0][0].nLocks == 256);
	// On the Euclid screen a held step changes nothing: the encoders edit the settings.
	stepPad(r, 3, 0);
	r.click(Device); // Home
	stepPad(r, 3);
	r.turn(1, 8);
	after = r.state();
	CHECK(after->clip[0][0].euclid.lane[0].pulses == 5 && after->clip[0][0].locks[3].value == 1003 + 5 * 312);
	stepPad(r, 3, 0);
	// An empty slot, and a pad from STEPS on, hold nothing.
	Rig e;
	euclidEmpty(e);
	e.click(Device);
	stepPad(e, 3);
	CHECK(e.view().s.ui.heldStep == -1);
	Rig p;
	euclidTrack(p, 0, [](tart::Euclid& x) { lane(x, 0, 4, 1); }, tart::kBar, false);
	p.click(Device);
	stepPad(p, 4);
	CHECK(p.view().s.ui.heldStep == -1);
	stepPad(p, 4, 0);
	stepPad(p, 3);
	CHECK(p.view().s.ui.heldStep == 7 * 8 + 3);
	// The lock belongs to the lane the pads show: the selected drum lane, or the Melodic rhythm.
	stepPad(p, 3, 0);
	p.pad(0, 3, 100), p.pad(0, 3, 0); // lane 3
	stepPad(p, 1);
	p.turn(0, 1);
	CHECK(sameLocks(locksOf(*p.state()), Locks{lk(3, 1, 0, 312)}));
	stepPad(p, 1, 0);
	p.m.edit([](tart::State& s) { s.clip[0][0].euclid.mode = tart::kEuclidMelodic; });
	stepPad(p, 5);
	p.turn(0, 1);
	CHECK(sameLocks(locksOf(*p.state()), Locks{lk(3, 1, 0, 312), lk(16, 5, 0, 312)}));
}

TEST(euclid_lock_playback) {
	SliceRig r(false); // lane 0 is all rests; CC 1 is 5 V on step 3 of 4 (ticks 48..71), every 96 ticks
	euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 4, 0); });
	setLocks(r, {lk(0, 2, 0, 32768)});
	auto cc = ccTrace(r, 0, 300);
	for (int k = 0; k < 300; k++) { // at the first sample of a tick: 0 V, the ramps over ticks 47 and 71, 5 V between
		const int c = k % 96;
		const float want = c >= 48 && c < 71 ? 5.f : c == 71 ? 4.9f : c == 47 ? 0.1f : 0.f;
		CHECK_NEAR(cc[(size_t)k], want, c == 71 || c == 47 ? 0.06f : 0.01f);
	}
	// Halfway through tick 47 and 71 the lane is half way.
	SliceRig h(false);
	euclidTrack(h, 0, [](tart::Euclid& e) { lane(e, 0, 4, 0); });
	setLocks(h, {lk(0, 2, 0, 32768)});
	const float up = ccTrace(h, 47, 48, 0, 125)[0], down = ccTrace(h, 71, 72, 0, 125)[0];
	CHECK(up > 2.2f && up < 2.8f && down > 2.2f && down < 2.8f);
	CHECK_NEAR(r.f.track[0].cc[1], 0.f, 1e-6); // the other lanes don't move
	// A relaunch starts the cycle again from there.
	setQuant(r, 0);
	ccTrace(r, 300, 1000);
	tapSlot(r, 0, 0);
	r.run_(500);
	const int s = (int)r.state()->s.track[0].startTick;
	CHECK(s >= 1000 && s < 1004);
	auto again = ccTrace(r, s + 40, s + 80);
	for (int k = s + 40; k < s + 80; k++) {
		const int c = k - s;
		if (c != 47 && c != 71)
			CHECK_NEAR(again[(size_t)(k - s - 40)], c >= 48 && c < 71 ? 5.f : 0.f, 0.01f);
	}
}

TEST(euclid_lock_rest_and_dropped) {
	SliceRig r;
	// Lane 0 is all rests, lane 1's pulses are all dropped by PROB 0: each lock applies on its step all the same.
	euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 4, 0), lane(e, 1, 4, 4, 0, 0); });
	setLocks(r, {lk(0, 1, 0, 32768), lk(1, 2, 1, 32768)});
	std::vector<float> a, b;
	bool gate = false;
	for (int k = 0; k < 200; k++) {
		while (r.in.frame < 250 * k + 5) {
			r.in.play = r.in.frame == 0, r.step();
			for (int c = 0; c < 16; c++)
				gate |= r.f.track[0].gate[c] > 5;
		}
		a.push_back(r.f.track[0].cc[0]), b.push_back(r.f.track[0].cc[1]);
	}
	for (int k = 0; k < 200; k++) {
		const int c = k % 96; // lock 1 holds ticks 24..47, lock 2 ticks 48..71 (the ramps at 23, 47 and 71 are skipped)
		if (c != 23 && c != 47 && c != 71) {
			CHECK_NEAR(a[(size_t)k], c >= 24 && c < 47 ? 5.f : 0.f, 0.01f);
			CHECK_NEAR(b[(size_t)k], c >= 48 && c < 71 ? 5.f : 0.f, 0.01f);
		}
	}
	CHECK(!gate); // nothing sounded: no pulse on the rest, none passed PROB 0
}

TEST(euclid_lock_priority) {
	SliceRig r;
	euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 8, 0), lane(e, 3, 8, 0); });
	// Lanes 0 and 3 are locked on CC 1 over the same step; lane 3 alone on step 6.
	setLocks(r, {lk(0, 2, 0, 10000), lk(3, 2, 0, 40000), lk(3, 5, 0, 30000)});
	auto cc = ccTrace(r, 0, 192);
	for (int k : {50, 60, 70})
		CHECK_NEAR(cc[(size_t)k], V(10000), 0.01f); // the lowest lane
	for (int k : {122, 130, 140})
		CHECK_NEAR(cc[(size_t)k], V(30000), 0.01f); // lane 3 on its own
	CHECK_NEAR(cc[10], 0.f, 0.01f);
	r.pad(0, 3, 100), r.pad(0, 3, 0); // lane 3 selected: nothing changes
	CHECK(r.view().s.track[0].lane == 3);
	auto next = ccTrace(r, 192, 384);
	for (int k : {50, 60, 70})
		CHECK_NEAR(next[(size_t)k], V(10000), 0.01f);
	for (int k : {122, 140})
		CHECK_NEAR(next[(size_t)k], V(30000), 0.01f);
}

TEST(euclid_lock_rotate_and_steps) {
	using namespace push2::cc;
	SliceRig r;
	euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 8, 2); });
	start(r);
	r.click(Device);
	stepPad(r, 2); // step 3 of the pattern, as it is now (not rotated)
	r.turn(0, 60);
	stepPad(r, 2, 0);
	CHECK(sameLocks(locksOf(*r.state()), Locks{lk(0, 2, 0, 18720)}));
	r.click(Device); // Home
	r.turn(2, 8);    // ROTATE +1: the pulses move one step later, the lock with them
	CHECK(r.state()->clip[0][0].euclid.lane[0].rotation == 1);
	CHECK(sameLocks(locksOf(*r.state()), Locks{lk(0, 2, 0, 18720)}));
	auto cc = ccTrace(r, 0, 120);
	CHECK_NEAR(cc[50], 0.f, 0.01f); // step 3 no longer
	CHECK_NEAR(cc[80], V(18720), 0.01f);
	CHECK_NEAR(cc[90], V(18720), 0.01f); // step 4 (ticks 72..95)
	r.click(Device);                     // the pad of step 4 is the lock's now
	stepPad(r, 3);
	CHECK(r.view().locked == 1 && r.view().at[0] == 18720);
	stepPad(r, 3, 0);
	stepPad(r, 2);
	CHECK(r.view().locked == 0);
	stepPad(r, 2, 0);
	// STEPS 4 -> 2 silences a lock on step 4; back to 4 it plays again, and the pad of step 4 holds nothing meanwhile.
	r.m.edit([](tart::State& s) { s.clip[0][0].euclid.lane[0].steps = 4, s.clip[0][0].euclid.lane[0].rotation = 0; });
	setLocks(r, {lk(0, 3, 0, 31200)});
	r.click(Device); // Home
	r.turn(0, -16);
	CHECK(r.state()->clip[0][0].euclid.lane[0].steps == 2 && r.state()->clip[0][0].nLocks == 1);
	const int a = 200;
	auto quiet = ccTrace(r, a, a + 200);
	for (float v : quiet)
		CHECK(v < 0.5f);
	r.click(Device);
	stepPad(r, 3);
	CHECK(r.view().s.ui.heldStep == -1);
	stepPad(r, 3, 0);
	r.click(Device);
	r.turn(0, 16);
	CHECK(r.state()->clip[0][0].euclid.lane[0].steps == 4);
	auto loud = ccTrace(r, 420, 620);
	for (int k = 420; k < 620; k++) { // step 4 of 4 since the launch: ticks 72..95 of every 96
		const int c = k % 96;
		if (c >= 73 && c < 95)
			CHECK_NEAR(loud[(size_t)(k - 420)], V(31200), 0.01f);
		if (c < 71)
			CHECK_NEAR(loud[(size_t)(k - 420)], 0.f, 0.01f);
	}
	// Drum -> Melodic -> Drum keeps the drum locks; Melodic plays only the rhythm's.
	setLocks(r, {lk(0, 1, 0, 20000), lk(16, 3, 0, 50000)});
	r.m.edit([](tart::State& s) { s.clip[0][0].euclid.lane[0].steps = 4; });
	r.click(Track1Above + 1); // MELODIC
	auto mel = ccTrace(r, 700, 900);
	for (int k = 700; k < 900; k++) {
		const int c = k % 384; // the rhythm has 16 steps of 24 ticks: step 4 is ticks 72..95
		if (c % 384 >= 73 && c % 384 < 95)
			CHECK_NEAR(mel[(size_t)(k - 700)], V(50000), 0.01f);
		else if (c % 384 < 71 || c % 384 > 96)
			CHECK_NEAR(mel[(size_t)(k - 700)], 0.f, 0.01f);
	}
	r.click(Track1Above); // DRUM
	CHECK(r.state()->clip[0][0].nLocks == 2);
	auto drum = ccTrace(r, 1000, 1200);
	for (int k = 1000; k < 1200; k++) {
		const int c = k % 96; // lane 0 has 4 steps: step 2 is ticks 24..47
		if (c >= 25 && c < 47)
			CHECK_NEAR(drum[(size_t)(k - 1000)], V(20000), 0.01f);
		else if (c >= 49 || c < 23)
			CHECK_NEAR(drum[(size_t)(k - 1000)], 0.f, 0.01f);
	}
}

TEST(euclid_lock_with_e4) {
	using namespace push2::cc;
	SliceRig r;
	euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 4, 0); });
	r.m.edit([](tart::State& s) { // CC 1 has points (a flat 10000) and a lock on step 3; CC 2 only the lock
		tart::Clip& c = s.clip[0][0];
		c.nPoints[0] = 2, c.points[0][0] = {0, 10000}, c.points[0][1] = {384, 10000};
		c.nLocks = 2, c.locks[0] = lk(0, 2, 0, 40000), c.locks[1] = lk(0, 2, 1, 40000);
	});
	start(r);
	r.click(Device);
	auto cc = ccTrace(r, 0, 100);
	CHECK_NEAR(cc[20], V(10000), 0.01f); // the points elsewhere, the lock on its step
	CHECK_NEAR(cc[60], V(40000), 0.01f);
	CHECK_NEAR(cc[80], V(10000), 0.01f);
	auto st = r.state(); // a lock never writes the lane's value
	CHECK(st->s.track[0].value[0] == 10000 && st->s.track[0].value[1] == 0 && st->s.track[0].overridden == 0);
	// Turning CC 1 overrides the points and the lock until a relaunch; so does a lane that only has locks.
	r.turn(0, 1);
	r.turn(1, 1);
	CHECK(r.state()->s.track[0].overridden == 3);
	auto over = ccTrace(r, 150, 160);
	for (float v : over)
		CHECK_NEAR(v, V(10312), 0.01f);
	CHECK_NEAR(ccTrace(r, 160, 161, 1)[0], V(312), 0.01f);
	r.press(Shift); // SHIFT + AUTOMATE: the clips are followed again
	r.click(Automate);
	r.press(Shift, 0);
	CHECK(r.state()->s.track[0].overridden == 0);
	auto back = ccTrace(r, 200, 260);
	CHECK_NEAR(back[10], V(10000), 0.01f); // tick 210 of 384: 18 into a cycle
	CHECK_NEAR(back[50], V(40000), 0.01f); // tick 250: 58 into the cycle
	// A lane that Automate writes plays the encoder and ignores its locks.
	r.click(Automate);
	r.click(Record); // a take on the playing clip
	touch(r, 0);
	r.turn(0, 10);
	const int written = r.state()->s.track[0].value[0];
	CHECK(r.state()->s.ui.writing == 1 && written != 10000);
	auto w = ccTrace(r, 336, 380); // ticks 48..71 of a cycle: the lock's step
	for (float v : w)
		CHECK_NEAR(v, V(written), 0.01f);
	touch(r, 0, false);
	r.click(Record);
	// LEVEL and PAN locks from the Mix screen's held step.
	SliceRig m;
	euclidTrack(m, 0, [](tart::Euclid& e) { lane(e, 0, 4, 0); });
	start(m);
	m.click(Mix);
	stepPad(m, 2);
	m.turn(0, -60);
	m.turn(1, 60);
	stepPad(m, 2, 0);
	CHECK(sameLocks(locksOf(*m.state()),
					Locks{lk(0, 2, tart::kLevel, 65535 - 18720), lk(0, 2, tart::kPan, 32768 + 18720)}));
	while (m.in.frame < 250 * 20 + 5) // tick 20: no lock; tick 60: the step's
		m.step();
	const float level0 = m.f.track[0].level, pan0 = m.f.track[0].pan;
	while (m.in.frame < 250 * 60 + 5)
		m.step();
	const float level = m.f.track[0].level, pan = m.f.track[0].pan;
	const tart::Track t;
	CHECK_NEAR(level0, 10.f, 0.01f);
	CHECK_NEAR(pan0, 0.f, 0.01f);
	CHECK_NEAR(level, tart::volts(t, tart::kLevel, 65535 - 18720), 0.01f);
	CHECK_NEAR(pan, tart::volts(t, tart::kPan, 32768 + 18720), 0.01f);
}

TEST(euclid_lock_delete) {
	using namespace push2::cc;
	auto make = [](Rig& r) {
		euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 16, 4), lane(e, 1, 8, 2); }, tart::kBar, false);
		r.m.edit([](tart::State& s) {
			tart::Clip& c = s.clip[0][0];
			c.nLocks = 4, c.locks[0] = lk(0, 3, 0, 100), c.locks[1] = lk(0, 3, 1, 200), c.locks[2] = lk(0, 4, 0, 300),
			c.locks[3] = lk(1, 3, 0, 400);
			c.nPoints[0] = 2, c.points[0][0] = {0, 5}, c.points[0][1] = {96, 6};
			s.s.track[0].value[2] = 5000;
		});
		r.click(Device);
	};
	auto undone = [](Rig& r, const tart::State& was) { // one Undo brings the clip back
		r.click(Undo);
		return !std::memcmp(&r.state()->clip[0][0], &was.clip[0][0], sizeof(tart::Clip));
	};
	{ // DELETE + a step pad: every lock of that step of the shown pattern (lane 0)
		Rig r;
		make(r);
		auto was = r.state();
		r.press(Delete);
		stepPad(r, 3), stepPad(r, 3, 0);
		r.press(Delete, 0);
		CHECK(sameLocks(locksOf(*r.state()), Locks{lk(0, 4, 0, 300), lk(1, 3, 0, 400)}) &&
			  r.view().s.ui.heldStep == -1);
		CHECK(undone(r, *was));
	}
	{ // a step without locks: nothing, not even an undo step
		Rig r;
		make(r);
		r.press(Delete);
		stepPad(r, 7), stepPad(r, 7, 0);
		stepPad(r, 20), stepPad(r, 20, 0); // (the pads past STEPS also)
		r.press(Delete, 0);
		r.click(Undo);
		CHECK(r.view().s.ui.popup != tart::kUndoPopup && r.state()->clip[0][0].nLocks == 4);
	}
	{ // DELETE + touch with a step held: that target's lock on the step; the others stay
		Rig r;
		make(r);
		auto was = r.state();
		stepPad(r, 3);
		r.press(Delete);
		touch(r, 0), touch(r, 0, false);
		touch(r, 5), touch(r, 5, false); // no lock: nothing
		r.press(Delete, 0);
		stepPad(r, 3, 0);
		CHECK(sameLocks(locksOf(*r.state()), Locks{lk(0, 3, 1, 200), lk(0, 4, 0, 300), lk(1, 3, 0, 400)}));
		CHECK(r.state()->clip[0][0].nPoints[0] == 2); // the points stay
		CHECK(undone(r, *was));
	}
	{ // DELETE + touch with nothing held: the lane's points and every lock on that target; else the value resets
		Rig r;
		make(r);
		auto was = r.state();
		r.press(Delete);
		touch(r, 0), touch(r, 0, false);
		auto st = r.state();
		CHECK(sameLocks(locksOf(*st), Locks{lk(0, 3, 1, 200)}) && st->clip[0][0].nPoints[0] == 0);
		touch(r, 2), touch(r, 2, false); // no points, no locks: the value goes back
		CHECK(r.state()->s.track[0].value[2] == 0);
		r.press(Delete, 0);
		CHECK(undone(r, *was));
		// With only locks the same touch takes them (and makes one undo step)
		r.m.edit([](tart::State& s) { s.clip[0][0].nPoints[0] = 0; });
		auto was2 = r.state();
		r.press(Delete);
		touch(r, 0), touch(r, 0, false);
		r.press(Delete, 0);
		CHECK(locksOf(*r.state()).size() == 1 && undone(r, *was2));
	}
	{ // DELETE + AUTOMATE: the points and all the locks
		Rig r;
		make(r);
		auto was = r.state();
		r.press(Delete);
		r.click(Automate);
		r.press(Delete, 0);
		auto st = r.state();
		CHECK(st->clip[0][0].nLocks == 0 && st->clip[0][0].nPoints[0] == 0);
		CHECK(undone(r, *was));
		r.m.edit([](tart::State& s) { s.clip[0][0].nPoints[0] = 0, s.clip[0][0].nLocks = 0; });
		r.press(Delete);
		r.click(Automate); // nothing there: nothing happens
		r.press(Delete, 0);
		r.m.edit([](tart::State& s) { s.s.ui.popup = tart::kNoPopup; });
		r.click(Undo);
		CHECK(r.view().s.ui.popup != tart::kUndoPopup);
	}
	{ // DELETE + a lane pad: the lane's settings and its locks
		Rig r;
		make(r);
		auto was = r.state();
		r.press(Delete);
		r.pad(0, 1, 100), r.pad(0, 1, 0);
		r.press(Delete, 0);
		auto st = r.state();
		CHECK(sameLocks(locksOf(*st), Locks{lk(0, 3, 0, 100), lk(0, 3, 1, 200), lk(0, 4, 0, 300)}));
		const tart::EuclidLane standard;
		CHECK(!std::memcmp(&st->clip[0][0].euclid.lane[1], &standard, sizeof standard));
		CHECK(undone(r, *was));
		// a lane with standard settings but locks: the locks go, one undo step
		r.m.edit([](tart::State& s) { s.clip[0][0].euclid.lane[1] = tart::EuclidLane(); });
		auto was2 = r.state();
		r.press(Delete);
		r.pad(0, 1, 100), r.pad(0, 1, 0);
		r.press(Delete, 0);
		CHECK(r.state()->clip[0][0].nLocks == 3 && undone(r, *was2));
		// holding a step lets go when a lane pad is pressed
		stepPad(r, 3);
		CHECK(r.view().s.ui.heldStep >= 0);
		r.pad(0, 2, 100), r.pad(0, 2, 0);
		CHECK(r.view().s.ui.heldStep == -1);
	}
}

TEST(euclid_lock_ops) {
	using namespace push2::cc;
	Rig r(false);
	setQuant(r, 0);
	euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 16, 4); }, tart::kBar, false);
	setLocks(r, {lk(0, 3, 0, 100), lk(0, 4, 1, 200), lk(5, 9, 8, 300)});
	const Locks want = locksOf(*r.state());
	r.press(Duplicate); // Duplicate copies the locks along
	slot(r, 0, 0, 100), slot(r, 0, 0, 0);
	tapSlot(r, 1, 2);
	r.press(Duplicate, 0);
	CHECK(sameLocks(locksOf(*r.state(), 1, 2), want));
	r.press(Delete); // Delete empties the slot's locks; Undo brings them back
	tapSlot(r, 0, 0);
	r.press(Delete, 0);
	auto st = r.state();
	CHECK(st->clip[0][0].nLocks == 0 && st->clip[0][0].locks[0].value == 0 && st->clip[0][0].locks[2].value == 0);
	r.click(Undo);
	CHECK(sameLocks(locksOf(*r.state()), want));
	// sanitize
	auto s = std::make_unique<tart::State>();
	tart::Clip& c = s->clip[0][0];
	c.length = tart::kBar;
	const Locks bad = {lk(3, 5, 1, 7),  lk(0, 2, 0, 100), lk(17, 0, 0, 1),
					   lk(0, 32, 0, 1), lk(0, 1, 10, 1),  lk(0, 2, 0, 200)};
	std::copy(bad.begin(), bad.end(), c.locks);
	c.locks[0].spare = 9;
	c.nLocks = (uint16_t)bad.size();
	tart::sanitize(*s);
	CHECK(sameLocks(locksOf(*s),
					Locks{lk(0, 2, 0, 100), lk(3, 5, 1, 7)})); // dropped, sorted, the first of a key, spare 0
	CHECK(c.locks[2].value == 0 && c.locks[5].lane == 0);
	for (int i = 0; i < tart::kMaxLocks; i++)
		c.locks[i] = lk(i / 16, i % 16, 0, i + 1);
	c.nLocks = 999; // past the ceiling
	tart::sanitize(*s);
	CHECK(c.nLocks == tart::kMaxLocks && c.locks[255].value == 256);
	for (int i = 0; i < tart::kMaxLocks; i++)
		c.locks[i] = lk(15 - i / 16, 15 - i % 16, 0, i + 1); // backwards
	tart::sanitize(*s);
	CHECK(c.nLocks == tart::kMaxLocks && c.locks[0].lane == 0 && c.locks[0].step == 0 && c.locks[255].lane == 15);
	s->clip[0][0].length = 0; // an empty slot has none
	tart::sanitize(*s);
	CHECK(c.nLocks == 0 && c.locks[0].value == 0 && c.locks[255].value == 0);
}

TEST(euclid_lock_view_leds) {
	using namespace push2::cc;
	Rig r;
	euclidTrack(r, 0, [](tart::Euclid& e) { lane(e, 0, 8, 3, 1); }, tart::kBar, false);
	setLocks(r, {lk(0, 0, 0, 1000), lk(0, 2, tart::kLevel, 500)}); // rotated by 1: pads 2 and 4 (k = 1 and 3)
	r.click(Device);
	stepPad(r, 1);
	tart::View v = r.view();
	CHECK(v.at[0] == 1000 && v.locked == 1 && v.at[1] == 0);
	CHECK(v.automated[0] == (1 | 1 << tart::kLevel)); // the dots count locks
	r.in.leds = true;
	r.step();
	const uint32_t mask = tart::euclidMask(mk(8, 3, 1));
	auto led = [&](int k) { return r.want.note[push2::padNote(7 - k / 8, k % 8)]; };
	CHECK(led(1).color == push2::kRgbWhite && led(1).channel == push2::kStatic); // the held step: white
	const uint8_t base3 = mask >> 3 & 1 ? tart::kPaletteFull : push2::kRgbDarkGray;
	CHECK(led(3).color == push2::kRgbWhite && led(3).channel == push2::kPulseHalf && led(3).base == base3);
	CHECK(led(5).channel == push2::kStatic && led(5).color != push2::kRgbWhite);
	stepPad(r, 1, 0);
	stepPad(r, 3);
	v = r.view();
	CHECK(v.locked == 1 << tart::kLevel && v.at[tart::kLevel] == 500 && v.at[0] == 0);
	stepPad(r, 3, 0);
	r.click(Mix);
	stepPad(r, 3);
	CHECK(r.view().at[tart::kLevel] == 500);
	stepPad(r, 3, 0);
	// The playhead is green over a locked step and over the held one.
	Rig p;
	euclidTrack(p, 0, [](tart::Euclid& e) { lane(e, 0, 8, 3, 1); });
	setLocks(p, {lk(0, 2, 0, 1000)});
	ledsAt(p, 24 * 3 + 5);
	auto pled = [&](int k) { return p.want.note[push2::padNote(7 - k / 8, k % 8)]; };
	CHECK(pled(3).color == push2::kGreen && pled(3).channel == push2::kStatic);
	p.click(Device);
	stepPad(p, 3);
	p.step();
	CHECK(pled(3).color == push2::kGreen && pled(3).channel == push2::kStatic);
	// A locked step in Melodic mode: the rhythm's pattern is shown.
	p.m.edit([](tart::State& s) { s.clip[0][0].euclid.mode = tart::kEuclidMelodic; });
	setLocks(p, {lk(16, 6, 0, 5)});
	stepPad(p, 3, 0);
	p.step();
	CHECK(pled(6).channel == push2::kPulseHalf && pled(2).channel == push2::kStatic);
}

TEST(euclid_lock_model) {
	// 50 random clips, 4 bars each: the Slice's CC, LEVEL and PAN at the first samples of every tick against a naive
	// reference written from the design: a lock on step s of the unrotated pattern of lane l plays on pattern step
	// (s + rotation) % steps while the lane is active (Drum: lanes 0 to 15, Melodic: the rhythm), the lowest lane wins,
	// and a lock on a step from STEPS on is hidden.
	static const int kTicksPerStep[8] = {96, 64, 48, 32, 24, 16, 12, 8};
	Rng g{20250611};
	struct Ref {
		int lane, step, target, value;
	};
	long checked = 0, locked = 0, contested = 0;
	for (int run = 0; run < 50; run++) {
		const int gridIdx = g.n(8), grid = kTicksPerStep[gridIdx];
		const bool melodic = g.n(3) == 0;
		const int lanes[5] = {0, 1, 2, 3, tart::kEuclidRhythm};
		int steps[17] = {}, rotation[17] = {};
		SliceRig r(false);
		euclidTrack(r, 0, [&](tart::Euclid& e) {
			e.mode = melodic ? tart::kEuclidMelodic : tart::kEuclidDrum, e.grid = (uint8_t)gridIdx;
			for (int l : lanes) {
				steps[l] = 1 + g.n(32), rotation[l] = g.n(32);
				lane(e, l, steps[l], g.n(steps[l] + 1), rotation[l]);
			}
		});
		std::map<std::tuple<int, int, int>, int> keys;
		for (int i = 0; i < 60; i++) {
			const int l = lanes[g.n(5)], s = g.n(32), t = g.n(10), v = g.n(65536);
			keys.insert({{l, s, t}, v}); // the first of a key stays
		}
		std::vector<Ref> ref;
		Locks locks;
		for (const auto& k : keys)
			ref.push_back({std::get<0>(k.first), std::get<1>(k.first), std::get<2>(k.first), k.second}),
				locks.push_back(lk(std::get<0>(k.first), std::get<1>(k.first), std::get<2>(k.first), k.second));
		setLocks(r, locks);
		const tart::Track base = r.state()->s.track[0];
		for (int k = 0; k < 4 * tart::kBar; k++) {
			while (r.in.frame < 250 * k + 2)
				r.in.play = r.in.frame == 0, r.step();
			for (int j = 0; j < tart::kAutos; j++) {
				int want = -1, lowest = 99, hits = 0;
				for (const Ref& x : ref) {
					if (x.target != j || (melodic ? x.lane != tart::kEuclidRhythm : x.lane == tart::kEuclidRhythm))
						continue;
					const int n = steps[x.lane];
					if (x.step >= n || (k / grid) % n != (x.step + rotation[x.lane] % n) % n)
						continue;
					hits++;
					if (x.lane < lowest)
						lowest = x.lane, want = x.value;
				}
				const float expected = tart::volts(base, j, want >= 0 ? want : base.value[j]);
				const tart::SliceTrack& o = r.f.track[0];
				const float got = j < tart::kCcs ? o.cc[j] : j == tart::kLevel ? o.level : o.pan;
				CHECK_NEAR(got, expected, 0.15f);
				checked++, locked += want >= 0, contested += hits > 1;
			}
		}
	}
	std::printf("euclid_lock_model: %ld lane-ticks checked, %ld on a lock, %ld with two lanes locked\n", checked,
				locked, contested);
	CHECK(locked > 20000 && contested > 100);
}

TEST(euclid_fuzz) {
	using namespace push2::cc;
	Twins w;
	auto view = std::make_unique<tart::View>();
	auto copy = std::make_unique<tart::State>();
	static tart::Euclid prev[tart::kTracks][tart::kScenes];
	// Three Euclid tracks with four clips each (modes, step sizes, lanes, seeds all different), a Melodic and a Drum
	// track; no launch quantization, so launches and relaunches land at once.
	// A clip with settings of every kind (as a patch would hold): `fill` makes one in each empty slot of tracks 1 to 3.
	auto fill = [](tart::State& st, uint32_t seed) {
		Rng g{seed};
		for (int t = 0; t < 3; t++)
			for (int sc = 0; sc < 4; sc++) {
				tart::Clip& c = st.clip[t][sc];
				if (c.length)
					continue;
				c.length = (uint16_t)(tart::kBar * (1 + g.n(2)));
				tart::Euclid& e = c.euclid;
				e.mode = (uint8_t)((t + sc) % 3 != 0), e.grid = (uint8_t)g.n(8), e.seed = (uint16_t)g.n(1000);
				e.octaves = (uint8_t)(1 + g.n(4)), e.change = (uint8_t)g.n(60);
				for (int l = 0; l <= tart::kLanes; l++) {
					const int n = 1 + g.n(32);
					lane(e, l, n, g.n(n + 1), g.n(n), 40 + g.n(61), 30 + g.n(98));
				}
				std::map<std::tuple<int, int, int>, int> keys; // some pattern-step locks, in order
				for (int i = g.n(40); i-- > 0;)
					keys.insert({{g.n(2) ? g.n(4) : tart::kEuclidRhythm, g.n(32), g.n(tart::kAutos)}, g.n(65536)});
				for (const auto& k : keys)
					c.locks[c.nLocks++] =
						lk(std::get<0>(k.first), std::get<1>(k.first), std::get<2>(k.first), k.second);
			}
	};
	w.edit([&](tart::State& st) {
		st.s.transport.quant = 0, st.s.ui.pads = tart::kNote;
		for (int t = 0; t < tart::kTracks; t++) {
			st.s.track[t].type = t < 3 ? tart::Euclidean : t == 3 ? tart::Melodic : tart::Drum;
			st.s.track[t].voices = (uint8_t)(t < 3 ? t + 1 : 1), st.s.track[t].playing = t < 3 ? 0 : tart::kNone;
		}
		fill(st, 20240611);
		st.clip[3][0].length = tart::kBar, st.clip[3][0].nNotes = 1, st.clip[3][0].notes[0] = {0, 24, 60, 100};
	});
	Rng g{12345};
	long nLocks = 0;
	int counts[22] = {}, heldSteps = 0, switches = 0, relaunches = 0, resets = 0, keys = 0, edited = 0, stateTypes = 0,
		held[3] = {};
	const int ops = 3000;
	w.a.in.play = w.b.in.play = true;
	for (int op = 0; op < ops; op++) {
		w.a.m.view(*view);
		const tart::Session& s = view->s;
		const int t = s.ui.track;
		const bool euclid = s.track[t].type == tart::Euclidean;
		const int k = g.n(100);
		int kind = 0;
		for (int b : {(int)Shift, (int)Delete, (int)Select}) // a held button doesn't stay for long
			if (s.ui.button[b] && g.n(3) == 0)
				w.press(b, 0);
		if (op % 30 == 0) { // something plays now and then: the transport runs, every Euclid track has a clip going
			if (!s.transport.playing)
				w.click(Play);
			w.click(Session);
			for (int tr = 0; tr < 3; tr++)
				if (s.track[tr].playing < 0 && view->length[tr][tr])
					w.pad(7 - tr, tr, 100), w.pad(7 - tr, tr, 0);
			w.click(Note);
		}
		if (op % 40 == 0) // a patch loads into the empty slots
			w.edit([=](tart::State& st) { fill(st, (uint32_t)op * 7919 + 1); });
		if (k < 15) { // an encoder turn, in a touch or alone
			const int id = g.n(8), d = (g.n(2) ? 1 : -1) * (1 + g.n(20));
			const bool touch = g.n(2);
			kind = 1;
			if (touch)
				w.touch(id, true);
			w.turn(id, d);
			if (touch)
				w.touch(id, false);
		} else if (k < 19) { // a touch alone: a DELETE + touch resets
			const int id = g.n(8);
			w.touch(id, true), w.touch(id, false), kind = 2;
		} else if (k < 24) { // SHIFT, DELETE and SELECT, held until the next time
			const int b = k % 3 == 0 ? Shift : k % 3 == 1 ? Delete : Select;
			w.press(b, s.ui.button[b] ? 0 : 127), kind = 3;
		} else if (k < 29) {                                // DRUM and MELODIC
			w.click(Track1Above + (g.n(3) != 0)), kind = 4; // MELODIC more often than DRUM
		} else if (k < 33) {                                // the step size
			w.click(Scene8 + g.n(8)), kind = 5;
		} else if (k < 43) {                                        // a pad: lane, keyboard or step
			const int row = g.n(3) ? g.n(4) : g.n(8), col = g.n(8); // the keyboard rows more often
			keys += euclid && s.ui.pads == tart::kNote && row < 4 && !s.ui.button[Delete] &&
					view->euclid.mode == tart::kEuclidMelodic;
			w.pad(row, col, 20 + g.n(100)), w.idle(g.n(30)), w.pad(row, col, 0), kind = 6;
		} else if (k < 47) {                                                         // another track
			w.click(Track1Below + (g.n(4) ? g.n(4) : g.n(tart::kTracks))), kind = 7; // mostly the Euclid ones
		} else if (k < 51) {                                                         // NOTE or SESSION
			w.click(g.n(3) ? Note : Session), kind = 8;
		} else if (k < 56) { // UNDO, redo with SHIFT held
			w.click(Undo), kind = 9;
		} else if (k < 59) { // PLAY (a stop only now and then)
			if (!s.transport.playing || g.n(4) == 0)
				w.click(Play);
			kind = 10;
		} else if (k < 62) {
			w.a.in.reset = w.b.in.reset = true, w.step(), resets++, kind = 11;
		} else if (k < 70) { // launch a clip, often the one that plays: a relaunch
			const int track = g.n(3),
					  scene = g.n(3) ? (s.track[track].playing >= 0 ? s.track[track].playing : 0) : g.n(4);
			const int64_t start = s.track[track].startTick;
			const bool same = s.track[track].playing == scene;
			w.click(Session);
			w.pad(7 - scene, track, 100), w.pad(7 - scene, track, 0);
			if (g.n(2))
				w.click(Note);
			w.idle(300); // the launch is at the next tick
			w.a.m.view(*view);
			relaunches += same && view->s.track[track].startTick != start;
			kind = 12;
		} else if (k < 74) { // the length of the selected clip in Clip mode, DOUBLE LOOP
			w.click(Clip), w.turn(0, g.n(2) ? 8 : -8), w.click(Clip);
			if (g.n(3) == 0)
				w.click(DoubleLoop);
			kind = 13;
		} else if (k < 78) { // a type change by an edit, as Browse does (a held step is let go)
			const int track = g.n(3), type = g.n(10) < 7 ? 2 : g.n(2);
			w.edit([=](tart::State& st) { st.s.track[track].type = (uint8_t)type, st.s.ui.heldStep = -1; });
			stateTypes++, kind = 14;
		} else if (k < 81) { // the Device and Mix screens and the Scale screen
			w.click(g.n(3) == 0 ? Mix : g.n(2) ? Device : Scale), kind = 15;
		} else if (k < 85) { // OCTAVE, LAYOUT, PAGE
			const int b[5] = {OctaveUp, OctaveDown, Layout, PageLeft, PageRight};
			w.click(b[g.n(5)]), kind = 16;
		} else if (k < 88) { // a take, Automate, the next empty slot
			const int b[3] = {Record, Automate, New};
			w.click(b[g.n(3)]), kind = 17;
		} else if (k < 90) { // the Voices of a track, the launch quantization
			const int track = g.n(tart::kTracks), v = 1 + g.n(4), q = g.n(3) ? 0 : g.n(6);
			w.edit([=](tart::State& st) { st.s.track[track].voices = (uint8_t)v, st.s.transport.quant = (uint8_t)q; });
			kind = 18;
		} else if (k < 92) { // STOP CLIP and a track
			w.press(StopClip), w.click(Track1Below + g.n(3)), w.press(StopClip, 0), kind = 19;
		} else if (k < 93) { // MUTE, SOLO
			w.click(g.n(2) ? Mute : Solo), kind = 20;
		} else if (k <
				   97) { // locks: a step pad held on the Device or Mix screen, turns, a DELETE variant, let go or not
			const int row = 4 + g.n(4), col = g.n(8), screen = g.n(3) ? tart::kDeviceScreen : tart::kMixScreen;
			const uint8_t pads = s.ui.pads;
			w.edit([=](tart::State& st) { st.s.ui.screen = (uint8_t)screen, st.s.ui.pads = tart::kNote; });
			w.pad(row, col, 100);
			for (int i = g.n(4); i-- > 0;)
				w.turn(g.n(3), (g.n(2) ? 1 : -1) * (1 + g.n(60)));
			if (g.n(3) == 0)
				w.press(Delete), w.touch(g.n(3), true), w.touch(g.n(3), false), w.press(Delete, 0);
			if (g.n(4) != 0)
				w.pad(row, col, 0);
			if (g.n(4) == 0)
				w.press(Delete), w.pad(row, col, 100), w.pad(row, col, 0), w.press(Delete, 0);
			w.edit([=](tart::State& st) { st.s.ui.screen = tart::kHome, st.s.ui.pads = pads; });
			kind = 21;
		} else
			kind = 0;
		counts[kind]++;
		w.idle(g.n(7) == 0 ? 300 + g.n(6000) : 1 + g.n(30)); // time passes: a step is 6000 samples at 1/16
		w.idle(2);                                           // so a layout change is in the Slice's frame
		// The invariants.
		bool sane = true;
		int want[tart::kTracks], lay[tart::kTracks];
		w.a.m.edit([&](tart::State& st) {
			*copy = st;
			tart::sanitize(*copy);
			for (int tr = 0; tr < tart::kTracks; tr++) {
				for (int sc = 0; sc < tart::kScenes; sc++) {
					sane &= !std::memcmp(&copy->clip[tr][sc].euclid, &st.clip[tr][sc].euclid, sizeof(tart::Euclid));
					sane &= copy->clip[tr][sc].nLocks == st.clip[tr][sc].nLocks && // sorted, unique, in range, <= 256
							!std::memcmp(copy->clip[tr][sc].locks, st.clip[tr][sc].locks, sizeof st.clip[tr][sc].locks);
					nLocks += st.clip[tr][sc].nLocks;
					edited += !!std::memcmp(&prev[tr][sc], &st.clip[tr][sc].euclid, sizeof(tart::Euclid));
					prev[tr][sc] = st.clip[tr][sc].euclid;
				}
				const tart::Track& x = st.s.track[tr];
				const tart::Euclid& e = st.clip[tr][x.playing != tart::kNone ? x.playing : st.s.ui.scene].euclid;
				const bool drum = x.type == tart::Drum || (x.type == tart::Euclidean && e.mode == tart::kEuclidDrum);
				want[tr] = drum ? tart::kLanes : std::max<int>(1, std::min<int>(x.voices, 16));
				lay[tr] = x.type != tart::Euclidean ? 0 : drum ? 1 : 2;
			}
		});
		CHECK(sane);
		for (int tr = 0; tr < tart::kTracks; tr++) {
			const tart::SliceTrack& o = w.a.f.track[tr];
			CHECK(o.channels == want[tr]);
			switches += lay[tr] && w.lay[tr] && lay[tr] != w.lay[tr];
			w.lay[tr] = lay[tr];
			for (int c = 0; c < o.channels; c++)
				CHECK((o.gate[c] == 0 || o.gate[c] == 10) && std::isfinite(o.voct[c]) && std::isfinite(o.vel[c]));
			for (int c = 0; c < tart::kCcs; c++)
				CHECK(std::isfinite(o.cc[c]));
		}
		w.a.m.view(*view); // the selected slot's picture, sorted and within its room; a held step is a step pad
		CHECK(view->nNotes <= tart::kMaxNotes);
		for (int i = 1; i < view->nNotes; i++)
			CHECK(view->notes[i - 1].tick <= view->notes[i].tick);
		CHECK(view->s.ui.heldStep == -1 || view->s.ui.heldStep >= 32); // a held step is a step pad
		heldSteps += view->s.ui.heldStep >= 0;
		held[view->s.track[view->s.ui.track].type == tart::Euclidean ? 1 : 0]++;
		CHECK(w.diverged < 0);
	}
	// Everything let go and stopped: the Slice goes quiet on both.
	w.edit([](tart::State& st) { st.s.ui.recTrack = -1; });
	for (int b : {(int)Shift, (int)Delete, (int)Select})
		w.press(b, 0);
	w.a.in.stop = w.b.in.stop = true;
	w.idle(2000);
	for (int tr = 0; tr < tart::kTracks; tr++)
		for (int c = 0; c < w.a.f.track[tr].channels; c++)
			CHECK(w.a.f.track[tr].gate[c] == 0);
	CHECK(w.diverged < 0);
	std::printf("euclid_fuzz: %d samples, rises drum %d melodic %d, layout switches %d, relaunches %d, resets %d, "
				"keyboard pads %d, Euclid edits %d, type edits %d, ops on a Euclid track %d, other %d\n",
				w.samples, w.rises[1], w.rises[2], switches, relaunches, resets, keys, edited, stateTypes, held[1],
				held[0]);
	CHECK(w.rises[1] > 100 && w.rises[2] > 50); // both modes sounded
	CHECK(switches > 20 && relaunches > 10 && resets > 20 && keys > 15 && edited > 100 && stateTypes > 20);
	CHECK(held[1] > ops / 4 && held[0] > ops / 4);
	for (int kind = 1; kind <= 21; kind++)
		CHECK(counts[kind] > 0);
	std::printf("euclid_fuzz: locks held at the end of an op %d times, %ld locks seen\n", heldSteps, nLocks);
	CHECK(heldSteps > 10 && nLocks > 5000);
}
