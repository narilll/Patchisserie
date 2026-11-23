// Tart: the Push 2 decoder, LED diff, display frame and the echo state.
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <thread>
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
		push2::Event ev[13];
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
		tart::SliceFrame frame;
		in.play = true;
		for (int i = 0; i < 20000; i++) {
			in.events = ev, in.nEvents = i % 7 == 0 ? 13 : 0, in.frame = i;
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
			st.s.track[1].type = (uint8_t)(n++ / 7 % 2);
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
}

TEST(state_layout) {
	CHECK(std::is_trivially_copyable<tart::State>::value);
	CHECK(tart::kMaxNotes == 4096);
	CHECK(sizeof(tart::State) < 3 * 1024 * 1024); // heap only: ~2.1 MB
	CHECK(sizeof(tart::Machine) < 16 * 1024);
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
	CHECK(r.f.track[2].rgb == (0xEDu << 16 | 0xDAu << 8 | 0x3Cu) && r.f.hop == 0);
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
	tart::sanitize(*st); // Euclid is E5: a Melodic track until then
	CHECK(st->s.track[3].type == tart::Drum && st->s.track[3].lane == tart::kLanes - 1);
	CHECK(st->s.track[4].type == tart::Melodic && st->s.ui.browse == 0 && st->s.ui.screen == tart::kHome);
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
	r.click(push2::cc::Scene1 - 2); // 1/8: the third scene button from the top
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
	r.click(push2::cc::Scene1 - 7); // 1/32t: 8 ticks, 48 steps in the bar, 6 pages
	r.click(push2::cc::PageRight);
	r.click(push2::cc::PageRight);
	CHECK(r.view().s.ui.page == 2);
	r.click(push2::cc::Scene1 - 4); // 1/16: 16 steps, 2 pages: the page is cut back
	CHECK(r.view().s.ui.page == 1);
	r.in.leds = true;
	r.step();
	auto led = [&](int row, int col) { return r.want.note[push2::padNote(row, col)].color; };
	CHECK(led(0, 0) == push2::kRgbWhite && led(1, 0) == push2::kRgbDarkGray); // the root row, the others
	CHECK(r.want.cc[push2::cc::Scene1 - 4].color == push2::kRgbWhite &&
		  r.want.cc[push2::cc::Scene1].color == push2::kRgbDarkGray);
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
	r.click(push2::cc::Layout);
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
	CHECK(r.view().s.ui.browse == 1); // two types: stops at the end
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
	r.click(push2::cc::Scene1 - 2); // 1/8
	CHECK(r.view().s.track[0].grid == 2);
	dtap(r, 1);
	v = r.view();
	CHECK(v.nNotes == 3 && v.notes[1].tick == 48 && v.notes[1].len == 48 && v.notes[1].pitch == 41);
	// Pages: 4 Bars is 64 steps of 1/16.
	r.m.edit([](tart::State& st) { st.s.ui.fixedBars = 4; });
	r.click(push2::cc::Scene1 - 4);
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
	r.click(push2::cc::Scene1 - 7); // 1/32t: 8 ticks, a page of 32 steps is 256 ticks
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
	CHECK(r.want.cc[push2::cc::Scene1 - 4].color == push2::kRgbWhite && r.want.cc[push2::cc::PageRight].color == 0);
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
