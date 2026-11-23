// Tart: the Push 2 decoder, LED diff, display frame and the echo state.
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
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

	Rig() { in.sampleRate = 48000; }
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
};

/** A Rig with a Slice on its right: `f` is the frame a Slice would read. */
struct SliceRig : Rig {
	tart::SliceFrame f = {};
	SliceRig() { slice = &f; }
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
	CHECK(r.want.cc[push2::cc::Undo].color == 0 && r.want.cc[push2::cc::Session].color == 0);
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
		push2::Event ev[4];
		ev[0] = dec({0xB0, push2::cc::kTempo, 1});
		ev[1] = dec({0xB0, push2::cc::Metronome, 127});
		ev[2] = dec({0x90, 36, 100}); // a pad and the touch strip
		ev[3] = dec({0xE0, 0, 0x60});
		tart::SliceFrame frame;
		in.play = true;
		for (int i = 0; i < 20000; i++) {
			in.events = ev, in.nEvents = i % 7 == 0 ? 4 : 0, in.frame = i;
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
			st.s.transport.swing = (uint8_t)(n % 100), st.s.track[0].voices = (uint8_t)(n++ % 16 + 1);
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
	CHECK(sizeof(tart::State) < 2 * 1024 * 1024); // heap only: ~1.3 MB
	CHECK(sizeof(tart::Machine) < 16 * 1024);
	CHECK(sizeof(tart::SliceFrame) < 2048);
	CHECK(sizeof(tart::State) > 1000000);
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
	CHECK(r.want.cc[push2::cc::Note].color == push2::kWhiteFull && r.want.cc[push2::cc::Session].color == 0);
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
	CHECK(t.playing == tart::kNone && t.queued == tart::kNone);
	CHECK(st->s.track[2].voices == 16 && st->s.track[2].octave == 0);
	CHECK(st->s.scale.root == 11 && st->s.scale.scale == 0 && st->s.scale.layout == 5);
	CHECK(st->s.ui.pads == tart::kNote && st->s.ui.fixedBars == 5 && st->s.ui.scene == 7 && st->s.ui.bend == 0 &&
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
	// Held but under 0.5 s: stays.
	r.press(Scale);
	r.run_(48000 * 4 / 10);
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
