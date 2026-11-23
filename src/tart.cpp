#include "tart.hpp"

#include <thread>

namespace tart {

namespace {

// Which buttons have RGB LEDs (the rest are white): the track buttons, scene buttons, Mute/Solo, Play and Record.
// ponytail: from the manual's examples (Mute RGB; Master, Tempo, Undo white) plus Live's behaviour; a wrong guess only
// shows a different shade in the echo test.
bool isRgb(int c) {
	return (c >= 20 && c <= 27) || (c >= 36 && c <= 43) || c == push2::cc::Mute || c == push2::cc::Solo ||
		   c == push2::cc::Play || c == push2::cc::Record || (c >= 102 && c <= 109);
}

uint8_t buttonColor(int c) {
	return isRgb(c) ? push2::kGreen : push2::kWhiteFull;
}

uint8_t padColor(int velocity) {
	return velocity < 43 ? push2::kBlue : velocity < 86 ? push2::kGreen : push2::kRed;
}

} // namespace

void handle(State& s, const push2::Event& e) {
	using push2::Event;
	switch (e.type) {
	case Event::Pad:
		if (e.index < 64)
			s.pad[e.index] = (uint8_t)e.value;
		break;
	case Event::Button:
		if (e.index < 128)
			s.button[e.index] = e.value != 0;
		break;
	case Event::Encoder:
		if (e.index < push2::kEncoders)
			s.encoder[e.index] += e.value;
		break;
	case Event::Touch:
		if (e.index <= push2::kStrip)
			s.touched = e.value ? s.touched | 1u << e.index : s.touched & ~(1u << e.index);
		break;
	case Event::Bend:
		s.bend = e.value;
		break;
	case Event::None:
		return;
	}
	for (int i = 0; i < 3; i++)
		s.last[i] = e.raw[i];
	s.lastSize = e.size;
	s.events++;
}

void leds(const State& s, push2::Leds& out) {
	out = push2::Leds();
	for (int i = 0; i < 64; i++)
		if (s.pad[i])
			out.note[push2::kPadFirst + i] = {padColor(s.pad[i]), push2::kStatic};
	for (int c = 0; c < 128; c++)
		if (s.button[c])
			out.cc[c] = {buttonColor(c), push2::kStatic};
	if (!s.button[push2::cc::Play])
		out.cc[push2::cc::Play] = {push2::kGreen, push2::kBlinkQuarter};
}

void Machine::lock() {
	while (!tryLock())
		while (busy_.load(std::memory_order_relaxed))
			std::this_thread::yield();
}

bool Machine::apply(const push2::Event* ev, int n, push2::Leds& want) {
	if (!tryLock())
		return false;
	for (int i = 0; i < n; i++)
		handle(state_, ev[i]);
	leds(state_, want);
	unlock();
	return true;
}

State Machine::snapshot() {
	lock();
	State s = state_;
	unlock();
	return s;
}

} // namespace tart
