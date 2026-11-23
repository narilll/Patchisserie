// Tart: the Push 2 decoder, LED diff, display frame and the echo state.
#include <atomic>
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

TEST(tart_echo) {
	tart::State s;
	push2::Leds l;
	tart::handle(s, dec({0x90, 38, 100}));
	tart::leds(s, l);
	CHECK(s.pad[2] == 100 && l.note[38].color == push2::kRed && l.note[38].channel == 0);
	tart::handle(s, dec({0x90, 38, 0}));
	tart::leds(s, l);
	CHECK(s.pad[2] == 0 && l.note[38].color == 0);

	CHECK(l.cc[push2::cc::Play].channel == push2::kBlinkQuarter);
	tart::handle(s, dec({0xB0, push2::cc::Play, 127}));
	tart::leds(s, l);
	CHECK(l.cc[push2::cc::Play].channel == 0 && l.cc[push2::cc::Play].color != 0);
	tart::handle(s, dec({0xB0, push2::cc::Undo, 127}));
	tart::leds(s, l);
	CHECK(l.cc[push2::cc::Undo].color == push2::kWhiteFull);

	tart::handle(s, dec({0xB0, 71, 3}));
	tart::handle(s, dec({0xB0, 71, 127}));
	CHECK(s.encoder[0] == 2);
	tart::handle(s, dec({0x90, 12, 127}));
	CHECK(s.touched == 1u << push2::kStrip);
	tart::handle(s, dec({0x90, 12, 0}));
	CHECK(s.touched == 0);
	tart::handle(s, dec({0xE0, 1, 2}));
	CHECK(s.bend == ((2 << 7) | 1) && s.lastSize == 3 && s.last[0] == 0xE0);
	uint32_t n = s.events;
	tart::handle(s, dec({0xA0, 1, 2}));
	CHECK(s.events == n);
}

TEST(tart_machine_threads) {
	tart::Machine m;
	constexpr int kApplies = 5000;
	std::atomic<bool> done{false};
	std::thread audio([&] {
		push2::Event ev[2];
		ev[0].type = Event::Encoder, ev[0].index = 0, ev[0].value = 1;
		ev[1].type = Event::Button, ev[1].index = push2::cc::Play, ev[1].value = 1;
		push2::Leds want;
		for (int i = 0; i < kApplies; i++)
			while (!m.apply(ev, 2, want))
				std::this_thread::yield();
		done = true;
	});
	bool consistent = true;
	while (!done) {
		tart::State s = m.snapshot();
		consistent = consistent && s.events == (uint32_t)s.encoder[0] * 2;
	}
	audio.join();
	tart::State s = m.snapshot();
	CHECK(consistent);
	CHECK(s.encoder[0] == kApplies && s.events == 2u * kApplies && s.button[push2::cc::Play]);
}
