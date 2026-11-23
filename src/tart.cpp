#include "tart.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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

} // namespace

const uint8_t kTrackRgb[8][3] = {{255, 94, 94},  {255, 165, 40}, {240, 220, 60},  {90, 220, 90},
								 {60, 210, 210}, {80, 140, 255}, {170, 110, 255}, {255, 110, 200}};

// In Key scales as bitmasks of the semitones above the root. ponytail: Major only until E2.3's scale list.
constexpr uint16_t kScaleMasks[] = {0xAB5};
constexpr int kScaleCount = sizeof kScaleMasks / sizeof *kScaleMasks;

int padPitch(const Scale& sc, int octave, int row, int col, bool& root, bool& inScale) {
	if (sc.layout & 1)
		std::swap(row, col); // right: rows and columns trade places
	// What a row adds to the column, by layout (4ths, 3rds, Sequential), In Key in scale steps, Chromatic in semitones.
	static const int kRise[2][3] = {{3, 2, 8}, {5, 4, 8}};
	int step = col + kRise[!sc.inKey][sc.layout / 2] * row;
	// ponytail: Fixed with In Key walks the scale from C instead of the root; E2.3 gives it Live's meaning.
	int p = 12 * octave + (sc.fixed ? 0 : sc.root);
	uint16_t mask = kScaleMasks[sc.scale % kScaleCount];
	if (sc.inKey) {
		int n = __builtin_popcount(mask), semis = 0;
		for (int seen = -1; !(mask >> semis & 1 && ++seen == step % n); semis++) {
		}
		p += 12 * (step / n) + semis;
	} else {
		p += step;
	}
	int rel = ((p - sc.root) % 12 + 12) % 12;
	root = rel == 0, inScale = mask >> rel & 1;
	return p > 127 ? -1 : p;
}

void setName(Track& t, const char* name) {
	size_t n = std::min(std::strlen(name), (size_t)kNameBytes - 1);
	while (n && (name[n] & 0xC0) == 0x80) // inside a character: leave it out
		n--;
	std::memmove(t.name, name, n);
	std::memset(t.name + n, 0, kNameBytes - n);
}

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
	Scale& sc = st.s.scale;
	sc.root = std::min<uint8_t>(sc.root, 11);
	sc.scale = sc.scale < kScaleCount ? sc.scale : 0;
	sc.layout = std::min<uint8_t>(sc.layout, 5);
	sc.inKey = sc.inKey != 0, sc.fixed = sc.fixed != 0;
	Ui& u = st.s.ui;
	u.pads = std::min<uint8_t>(u.pads, kNote);
	u.noteLayout = std::min<uint8_t>(u.noteLayout, 1);
	u.screen = 0; // ponytail: Home is the only screen until E2.3
	u.track = std::min<uint8_t>(u.track, kTracks - 1);
	u.scene = std::min<uint8_t>(u.scene, kScenes - 1);
	u.fixedBars = std::min<uint8_t>(u.fixedBars, 5);
	u.fixedOn = u.fixedOn != 0;
	u.page = 0, u.heldStep = -1, u.bend = 0;
	std::fill(u.pad, u.pad + 64, 0);
	std::fill(u.button, u.button + 128, false);
	u.touched = 0, u.popup = kNoPopup, u.popupAt = u.now = 0;
	// ponytail: clips are empty until E2.4, which sanitizes them (sorted notes, lengths, playing slots).
	for (Track& t : st.s.track) {
		char name[kNameBytes + 1] = {};
		std::memcpy(name, t.name, kNameBytes); // not necessarily terminated
		setName(t, name);
		t.color = std::min<uint8_t>(t.color, kColors - 1);
		t.type = Melodic; // ponytail: the other types come with E3 and E5
		t.voices = std::min<uint8_t>(std::max<uint8_t>(t.voices, 1), 16);
		t.grid = std::min<uint8_t>(t.grid, 7);
		t.octave = (int8_t)std::min<int>(std::max<int>(t.octave, 0), kMaxOctave);
		t.playing = t.queued = kNone;
		t.launchTick = t.startTick = 0;
	}
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
		else if (e.index == push2::cc::Note)
			ui.pads = kNote;
		else if (e.index >= push2::cc::Track1Below && e.index <= push2::cc::Track8Below)
			ui.track = (uint8_t)(e.index - push2::cc::Track1Below);
		else if (ui.pads != kNote)
			break;
		else if (e.index == push2::cc::Left || e.index == push2::cc::Right)
			ui.track = (uint8_t)std::min(std::max(ui.track + (e.index == push2::cc::Right ? 1 : -1), 0), kTracks - 1);
		else if (e.index == push2::cc::OctaveUp || e.index == push2::cc::OctaveDown) {
			int8_t& o = s.track[ui.track].octave;
			o = (int8_t)std::min(std::max(o + (e.index == push2::cc::OctaveUp ? 1 : -1), 0), kMaxOctave);
		}
		break;
	case Event::Pad:
		pad(s, e.index & 63, e.value);
		break;
	case Event::Bend:
		ui.bend = std::min(std::max((float)(e.value - 8192) / 8192.f, -1.f), 1.f);
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
		if (!e.value && e.index == push2::kStrip)
			ui.bend = 0; // the strip springs back; don't wait for the Push's own message
		if (e.value && e.index == push2::kTempoId)
			show(kTempoPopup);
		else if (e.value && e.index == push2::kSwingId)
			show(kSwingPopup);
		break;
	default:
		break;
	}
}

/** A pad pressed (velocity 1..127) or released (0): the note goes to the track selected now, and stops on that track.
 */
void Machine::pad(Session& s, int i, int vel) {
	Live& l = live_[i];
	if (l.id)
		voice_[l.track].noteOff(l.id), l = Live(), s.ui.pad[i] = 0;
	if (!vel || s.ui.pads != kNote)
		return;
	bool root, inScale;
	int p = padPitch(s.scale, s.track[s.ui.track].octave, i / 8, i % 8, root, inScale);
	if (p < 0)
		return;
	l = {++nextId_, s.ui.track, (uint8_t)p};
	s.ui.pad[i] = (uint8_t)vel;
	voice_[l.track].noteOn(l.id, (float)p, (float)vel / 127.f, 0);
}

void Machine::leds(const Session& s, push2::Leds& out) const {
	using push2::cc::Metronome, push2::cc::Play, push2::cc::Shift, push2::cc::TapTempo;
	const Ui& u = s.ui;
	const Track& tr = s.track[u.track];
	auto white = [&](int c, bool full) { out.cc[c] = {full ? push2::kWhiteFull : push2::kWhiteDark, push2::kStatic}; };
	out = push2::Leds();
	out.cc[Play] = {s.transport.playing ? push2::kGreen : push2::kRgbWhite, push2::kStatic};
	// Metronome: solid white off, pulsing between white and dim as fast as the Push goes on (the static white is sent
	// first)
	out.cc[Metronome] = s.transport.metronome ? push2::Led{push2::kWhiteDark, push2::kPulse24th}
											  : push2::Led{push2::kWhiteFull, push2::kStatic};
	white(TapTempo, u.button[TapTempo]);
	white(Shift, u.button[Shift]);
	white(push2::cc::Note, u.pads == kNote);
	for (int t = 0; t < kTracks; t++) // ponytail: white and dark gray until E2.3's track colours
		out.cc[push2::cc::Track1Below + t] = {t == u.track ? push2::kRgbWhite : push2::kRgbDarkGray, push2::kStatic};
	if (u.pads != kNote)
		return;
	if (tr.octave > 0)
		white(push2::cc::OctaveDown, u.button[push2::cc::OctaveDown]);
	if (tr.octave < kMaxOctave)
		white(push2::cc::OctaveUp, u.button[push2::cc::OctaveUp]);
	bool held[128] = {}; // pitches sounding from the pads: lit on every pad with that pitch
	for (int i = 0; i < 64; i++)
		held[live_[i].pitch & 127] |= u.pad[i] != 0;
	for (int row = 0; row < 8; row++)
		for (int col = 0; col < 8; col++) {
			bool root, inScale;
			int p = padPitch(s.scale, tr.octave, row, col, root, inScale);
			if (p < 0)
				continue; // above 127: dark and silent
			// ponytail: the root is blue, not the track colour, until E2.3's palette.
			uint8_t c = held[p] ? push2::kGreen : root ? push2::kBlue : inScale ? push2::kRgbWhite : push2::kBlack;
			out.note[push2::padNote(row, col)] = {c, push2::kStatic};
		}
}

void Machine::tick(const Session& s, int64_t k, Outputs& out) {
	if (s.transport.metronome && k % kPpqn == 0)
		out.metro = true, out.metroV = k % kBar == 0 ? 10.f : 5.f;
}

void Machine::process(const Inputs& in, Outputs& out, push2::Leds& want, SliceFrame* slice) {
	out = Outputs();
	if (silence_.load(std::memory_order_relaxed) && silence_.exchange(false)) { // a patch was loaded
		for (filo::VoiceAllocator& v : voice_)
			v.allOff();
		std::fill(live_, live_ + 64, Live());
	}
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
		for (int i = 0; i < kTracks; i++) {
			voices_[i] = s.track[i].voices;
			const uint8_t* c = kTrackRgb[s.track[i].color % 8];
			rgb_[i] = (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
		}
		selected_ = s.ui.track, bend_ = s.ui.bend * 2.f / 12.f;
		leds(s, want);
		out.ledsDone = true;
		unlock();
	}

	// The voices: the bend is on the selected track only.
	for (int t = 0; t < kTracks; t++) {
		filo::VoiceAllocator& v = voice_[t];
		v.setChannels(voices_[t]);
		if (slice) {
			SliceTrack& o = slice->track[t];
			float bend = t == selected_ ? bend_ : 0.f;
			o.channels = (uint8_t)v.channels();
			for (int c = 0; c < o.channels; c++)
				o.voct[c] = v.pitchV(c) + bend, o.gate[c] = v.gate(c) ? 10.f : 0.f, o.vel[c] = v.velV(c);
			// ponytail: CC, LEVEL and PAN stay at their defaults until E4 gives the clips automation and a mixer.
			std::fill(o.cc, o.cc + kCcs, 0.f);
			o.level = 10.f, o.pan = 0.f, o.rgb = rgb_[t];
		}
		v.tick();
	}
	if (slice)
		slice->frame = (uint32_t)in.frame, slice->hop = 0;

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
	const Ui& u = v.s.ui;
	for (int t = 0; t < kTracks; t++)
		for (int c = 0; c < kScenes; c++)
			v.length[t][c] = state_->clip[t][c].length;
	const Clip& clip = state_->clip[u.track][u.scene];
	v.nNotes = clip.nNotes;
	std::copy(clip.notes, clip.notes + clip.nNotes, v.notes);
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
	silence_.store(true, std::memory_order_relaxed);
	dirty_.store(true, std::memory_order_release);
}

} // namespace tart
