// Tart: the Push 2 decoder, LED diff, display frame and the echo state.
#include <atomic>
#include <cmath>
#include <memory>
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

	Rig() { in.sampleRate = 48000; }
	void step(const push2::Event* ev = nullptr, int n = 1) {
		in.events = ev, in.nEvents = ev ? n : 0;
		m.process(in, out, want);
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
		uint8_t cc = id == push2::kTempoId ? push2::cc::kTempo : push2::cc::kSwing;
		push2::Event e = dec({0xB0, cc, (uint8_t)(delta & 127)});
		step(&e);
	}
	tart::View view() {
		tart::View v;
		m.view(v);
		return v;
	}
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
	CHECK(r.want.cc[push2::cc::Undo].color == 0 && r.want.note[36].color == 0);
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
		push2::Event ev[2];
		ev[0] = dec({0xB0, push2::cc::kTempo, 1});
		ev[1] = dec({0xB0, push2::cc::Metronome, 127});
		in.play = true;
		for (int i = 0; i < 20000; i++) {
			in.events = ev, in.nEvents = i % 7 == 0 ? 2 : 0, in.frame = i;
			m.process(in, out, want);
			in.play = false;
		}
		done = true;
	});
	int n = 0;
	auto v = std::make_unique<tart::View>();
	while (!done) {
		m.view(*v);
		m.edit([&](tart::State& st) { st.s.transport.swing = (uint8_t)(n++ % 100); });
	}
	audio.join();
	m.view(*v);
	CHECK(v->s.transport.playing && v->s.transport.bpm > 120);
}
