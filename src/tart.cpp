#include "tart.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

namespace tart {

namespace {

constexpr double kEps = 1e-9;  // internal clock: absorbs float error so tick k lands on the exact sample
constexpr int kTempoUnits = 1; // raw encoder deltas per detent: 1 BPM
// ponytail: 2 raw deltas per percent, from the manual's "about 210 steps per turn"; calibrate on the hardware.
constexpr int kSwingUnits = 2;

/** Whole detents of an encoder: `delta` raw units added to the remainder `acc`, `units` per detent. */
int steps(int& acc, int delta, int units) {
	acc += delta;
	int n = acc / units;
	acc -= n * units;
	return n;
}

void leds(const Session& s, push2::Leds& out) {
	using push2::cc::Metronome, push2::cc::Play, push2::cc::Shift, push2::cc::TapTempo;
	auto white = [&](int c, bool full) { out.cc[c] = {full ? push2::kWhiteFull : push2::kWhiteDark, push2::kStatic}; };
	out = push2::Leds();
	out.cc[Play] = {s.transport.playing ? push2::kGreen : push2::kRgbWhite, push2::kStatic};
	// Metronome: solid white off, pulsing between white and dim as fast as the Push goes on (the static white is sent
	// first)
	out.cc[Metronome] = s.transport.metronome ? push2::Led{push2::kWhiteDark, push2::kPulse24th}
											  : push2::Led{push2::kWhiteFull, push2::kStatic};
	white(TapTempo, s.ui.button[TapTempo]);
	white(Shift, s.ui.button[Shift]);
}

} // namespace

State::State() {
	for (int i = 0; i < kTracks; i++) {
		std::snprintf(s.track[i].name, kNameBytes, "Track %d", i + 1);
		s.track[i].color = (uint8_t)i;
	}
}

void sanitize(State& st) {
	Transport& t = st.s.transport;
	t.bpm = std::isfinite(t.bpm) ? std::min(std::max(t.bpm, 20.f), 300.f) : 120.f;
	t.swing = std::min<uint8_t>(t.swing, 100);
	t.quant = std::min<uint8_t>(t.quant, 13);
	t.playing = t.external = false;
	t.tick = 0;
	t.shownBpm = t.bpm;
	Ui& u = st.s.ui;
	u.screen = 0; // ponytail: Home is the only screen until E2.3
	u.track = std::min<uint8_t>(u.track, kTracks - 1);
	u.scene = std::min<uint8_t>(u.scene, kScenes - 1);
	std::fill(u.button, u.button + 128, false);
	u.touched = 0, u.popup = kNoPopup, u.popupAt = u.now = 0;
}

void Machine::lock() {
	while (!tryLock())
		while (busy_.load(std::memory_order_relaxed))
			std::this_thread::yield();
}

// ---- Audio thread

/** Average of the last 4 intervals between taps; a gap above 2 s starts a new series. */
void Machine::tap(Transport& t) {
	if (nTaps_ && (double)(frame_ - taps_[nTaps_ - 1]) > 2.0 * sr_)
		nTaps_ = 0;
	if (nTaps_ == 5)
		std::copy(taps_ + 1, taps_ + 5, taps_), nTaps_ = 4;
	taps_[nTaps_++] = frame_;
	if (nTaps_ >= 2) {
		double beat = (double)(taps_[nTaps_ - 1] - taps_[0]) / (nTaps_ - 1) / sr_;
		t.bpm = std::min(std::max((float)std::round(600.0 / beat) / 10.f, 20.f), 300.f);
	}
}

void Machine::handle(Session& s, const push2::Event& e) {
	using push2::Event;
	Transport& t = s.transport;
	Ui& ui = s.ui;
	auto show = [&](Popup p) { ui.popup = p, ui.popupAt = ui.now; };
	switch (e.type) {
	case Event::Button:
		ui.button[e.index & 127] = e.value != 0;
		if (!e.value)
			break;
		if (e.index == push2::cc::Play)
			request_ = clock_.started() ? Request::Stop : Request::Play;
		else if (e.index == push2::cc::Metronome)
			t.metronome = !t.metronome;
		else if (e.index == push2::cc::TapTempo)
			tap(t), show(kTempoPopup); // never starts the transport
		break;
	case Event::Encoder:
		if (e.index == push2::kTempoId) {
			float step = ui.button[push2::cc::Shift] ? 0.1f : 1.f;
			float bpm = t.bpm + (float)steps(accTempo_, e.value, kTempoUnits) * step;
			t.bpm = std::min(std::max(std::round(bpm * 10.f) / 10.f, 20.f), 300.f);
			show(kTempoPopup);
		} else if (e.index == push2::kSwingId) {
			t.swing = (uint8_t)std::min(std::max(t.swing + steps(accSwing_, e.value, kSwingUnits), 0), 100);
			show(kSwingPopup);
		}
		break;
	case Event::Touch:
		if (e.index < 32)
			ui.touched = e.value ? ui.touched | 1u << e.index : ui.touched & ~(1u << e.index);
		if (e.value && e.index == push2::kTempoId)
			show(kTempoPopup);
		else if (e.value && e.index == push2::kSwingId)
			show(kSwingPopup);
		break;
	default:
		break;
	}
}

void Machine::tick(const Session& s, int64_t k, Outputs& out) {
	if (s.transport.metronome && k % kPpqn == 0)
		out.metro = true, out.metroV = k % kBar == 0 ? 10.f : 5.f;
}

void Machine::process(const Inputs& in, Outputs& out, push2::Leds& want) {
	out = Outputs();
	frame_ = in.frame;
	if (in.sampleRate != sr_)
		sr_ = in.sampleRate, clock_.setSampleRate(sr_);
	sync_ |= dirty_.load(std::memory_order_acquire);

	// Transport: the jacks and the Push's Play (handled last sample).
	Request r = request_;
	request_ = Request::None;
	bool reset = in.reset;
	if ((in.play || r == Request::Play) && !clock_.started()) {
		clock_.start(), reset = true, out.midiStart = true, sync_ = true; // from cycle 0
	}
	if ((in.stop || r == Request::Stop) && clock_.started())
		clock_.stop(), sync_ = true;

	// Tempo: setCps re-anchors the clock, so only on change.
	float cps = std::min(std::max(bpm_ * std::exp2(in.tempoCv), 20.f), 300.f) / 60.f;
	if (cps != cps_)
		cps_ = cps, clock_.setCps(cps);
	clock_.setPulsesPerCycle(in.ppqnIn);
	clk_.setDivision(in.ppqnOut);
	clock_.step(in.clockConnected, in.clockEdge, reset);
	if (reset)
		nextTick_ = 0, lastAt_ = -1, out.reset = sync_ = true;
	// External: no epsilon, the PLL holds just below the next pulse and an epsilon would fire ticks early.
	double at = clock_.cycle() + (in.clockConnected ? 0 : kEps);
	int64_t due = (int64_t)std::floor(at * kPpqn);
	if (in.nEvents || in.leds || (clock_.started() && nextTick_ <= due))
		sync_ = true;

	if (sync_ && tryLock()) { // busy: the UI thread is copying; try again next sample
		sync_ = false;
		dirty_.store(false, std::memory_order_relaxed);
		Session& s = state_->s;
		s.ui.now = (double)frame_ / sr_;
		for (int i = 0; i < in.nEvents; i++)
			handle(s, in.events[i]);
		out.consumed = in.nEvents > 0;
		// ponytail: at most 8 ticks per sample, the rest follow on the next samples; only an external clock at 1 or 2
		// pulses per beat jumps that far.
		for (int n = 0; n < 8 && clock_.started() && nextTick_ <= due; n++)
			tick(s, nextTick_++, out);
		Transport& t = s.transport;
		bpm_ = t.bpm;
		t.playing = clock_.started(), t.external = in.clockConnected;
		t.tick = std::max<int64_t>(nextTick_ - 1, 0);
		t.shownBpm = (in.clockConnected ? (float)clock_.cps() : cps_) * 60.f;
		leds(s, want);
		out.ledsDone = true;
		unlock();
	}

	out.run = clock_.started();
	if (out.run) {
		out.clk = clk_.clk(lastAt_, at);
		out.midiClock = push_.clk(lastAt_, at);
		lastAt_ = at;
	} else if ((freePhase_ += bpm_ / 60.f * 24.f / sr_) >= 1.f) { // the Push keeps animating while stopped
		freePhase_ -= 1.f;
		out.midiClock = true;
	}
}

// ---- UI thread

void Machine::view(View& v) {
	lock();
	v.s = state_->s;
	unlock();
}

void Machine::save(State& st) {
	lock();
	st = *state_;
	unlock();
}

void Machine::load(const State& st) {
	lock();
	*state_ = st;
	unlock();
	dirty_.store(true, std::memory_order_release);
}

} // namespace tart
