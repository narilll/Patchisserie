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
// ponytail: the track encoders send about 210 steps per turn (the manual); 8 per scale and 30 per layout choice are
// guesses, calibrate on the hardware.
constexpr int kScaleUnits = 8, kLayoutUnits = 30;
constexpr double kHoldSeconds = 0.5; // a button held this long is momentary
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

// The Push's screen colours for Live's 70 clip colours (Ableton's Push 2 script, colors.py), 14 to a row as Live shows
// them. ponytail: the Push palette has few distinct colours, so many indices repeat.
const uint8_t kLiveColors[kColors][3] = {
	{0xED, 0x59, 0x38}, {0xFF, 0x64, 0x00}, {0x80, 0x47, 0x13}, {0xED, 0xDA, 0x3C}, {0x94, 0xFF, 0x18},
	{0x00, 0x9D, 0x32}, {0x00, 0xB9, 0x55}, {0x00, 0xCC, 0x89}, {0x00, 0x71, 0xA4}, {0x49, 0x32, 0xB3},
	{0x52, 0x60, 0xDD}, {0xE1, 0x57, 0xE3}, {0xFF, 0x1E, 0x32}, {0xED, 0xDA, 0x3C}, {0xD1, 0x17, 0x0A},
	{0xFF, 0x32, 0x00}, {0x58, 0x23, 0x07}, {0xE4, 0xC2, 0x00}, {0x00, 0xE6, 0x31}, {0x33, 0x9E, 0x13},
	{0x00, 0x71, 0x4E}, {0x00, 0xBB, 0xAD}, {0x00, 0x6A, 0xCA}, {0x00, 0x5A, 0x62}, {0xAB, 0x50, 0xFF},
	{0x88, 0x42, 0x5B}, {0xFF, 0x4A, 0x96}, {0x80, 0x47, 0x13}, {0xED, 0x59, 0x38}, {0xFF, 0x64, 0x00},
	{0x80, 0x47, 0x13}, {0xED, 0xDA, 0x3C}, {0x94, 0xFF, 0x18}, {0x00, 0x9D, 0x32}, {0x00, 0xB9, 0x55},
	{0x00, 0xCC, 0x89}, {0x00, 0x71, 0xA4}, {0x49, 0x32, 0xB3}, {0x52, 0x60, 0xDD}, {0xAB, 0x50, 0xFF},
	{0xFF, 0x1E, 0x32}, {0x00, 0x71, 0xA4}, {0xED, 0x59, 0x38}, {0xFF, 0x64, 0x00}, {0x80, 0x47, 0x13},
	{0xED, 0xDA, 0x3C}, {0x94, 0xFF, 0x18}, {0x00, 0x9D, 0x32}, {0x00, 0xB9, 0x55}, {0x00, 0xCC, 0x89},
	{0x00, 0x71, 0xA4}, {0x49, 0x32, 0xB3}, {0x52, 0x60, 0xDD}, {0xE1, 0x57, 0xE3}, {0xFF, 0x1E, 0x32},
	{0x52, 0x60, 0xDD}, {0xD1, 0x17, 0x0A}, {0xFF, 0x32, 0x00}, {0x58, 0x23, 0x07}, {0xE4, 0xC2, 0x00},
	{0x00, 0xE6, 0x31}, {0x33, 0x9E, 0x13}, {0x00, 0x71, 0x4E}, {0x00, 0x5A, 0x62}, {0x49, 0x32, 0xB3},
	{0x00, 0x6A, 0xCA}, {0xAB, 0x50, 0xFF}, {0xE1, 0x57, 0xE3}, {0xFF, 0x4A, 0x96}, {0x58, 0x23, 0x07},
};

// The 8 colours new tracks start with: indices into kLiveColors, all different.
static const uint8_t kDefaultColors[kTracks] = {12, 1, 3, 6, 21, 8, 24, 26};

// Live's scales (the Live 11 Push list). ponytail: append-only (the saved index), and it may be missing the scales Live
// 12 added; Live's own list is not in the remote script, so this is from memory. The masks are the semitones above the
// root.
const ScaleDef kScales[] = {
	{"Major", 0xAB5},
	{"Minor", 0x5AD},
	{"Dorian", 0x6AD},
	{"Mixolydian", 0x6B5},
	{"Lydian", 0xAD5},
	{"Phrygian", 0x5AB},
	{"Locrian", 0x56B},
	{"Whole Tone", 0x555},
	{"Half-whole Dim.", 0x6DB},
	{"Whole-half Dim.", 0xB6D},
	{"Minor Blues", 0x4E9},
	{"Minor Pentatonic", 0x4A9},
	{"Major Pentatonic", 0x295},
	{"Harmonic Minor", 0x9AD},
	{"Harmonic Major", 0x9B5},
	{"Dorian #4", 0x6CD},
	{"Phrygian Dominant", 0x5B3},
	{"Melodic Minor", 0xAAD},
	{"Lydian Augmented", 0xB55},
	{"Lydian Dominant", 0x6D5},
	{"Super Locrian", 0x55B},
	{"8-Tone Spanish", 0x57B},
	{"Bhairav", 0x9B3},
	{"Hungarian Minor", 0x9CD},
	{"Hirajoshi", 0x18D},
	{"In-Sen", 0x4A3},
	{"Iwato", 0x463},
	{"Kumoi", 0x28D},
	{"Pelog Selisir", 0x18B},
	{"Pelog Tembung", 0x1A3},
	{"Messiaen 1", 0x555},
	{"Messiaen 2", 0x6DB},
	{"Messiaen 3", 0xDDD},
	{"Messiaen 4", 0x9E7},
	{"Messiaen 5", 0x8E3},
	{"Messiaen 6", 0xD75},
	{"Messiaen 7", 0xBEF},
};
const int kScaleCount = sizeof kScales / sizeof *kScales;

int padPitch(const Scale& sc, int octave, int row, int col, bool& root, bool& inScale) {
	if (sc.layout & 1)
		std::swap(row, col); // right: rows and columns trade places
	// What a row adds to the column, by layout (4ths, 3rds, Sequential), In Key in scale steps, Chromatic in semitones.
	static const int kRise[2][3] = {{3, 2, 8}, {5, 4, 8}};
	int step = col + kRise[!sc.inKey][sc.layout / 2] * row;
	// Fixed: the bottom-left pad stays C and the root only moves the lit pads, so In Key walks the scale's notes up
	// from C (the first note of the scale at or above it), not from the root.
	int p = 12 * octave + (sc.fixed ? 0 : sc.root);
	uint16_t mask = kScales[sc.scale % kScaleCount].mask;
	uint16_t walk = sc.fixed ? (uint16_t)((mask << sc.root | mask >> (12 - sc.root)) & 0xFFF) : mask; // from the pad
	if (sc.inKey) {
		int n = __builtin_popcount(walk), semis = 0;
		for (int seen = -1; !(walk >> semis & 1 && ++seen == step % n); semis++) {
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
		s.track[i].color = kDefaultColors[i];
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
	u.screen = kHome; // a patch opens on Home, as the Scale screen is a dialog
	u.track = std::min<uint8_t>(u.track, kTracks - 1);
	u.scene = std::min<uint8_t>(u.scene, kScenes - 1);
	u.fixedBars = std::min<uint8_t>(u.fixedBars, 5);
	u.fixedOn = u.fixedOn != 0;
	u.page = 0, u.heldStep = -1, u.bend = 0;
	std::fill(u.pad, u.pad + 64, 0);
	std::fill(u.button, u.button + 128, false);
	u.touched = 0, u.popup = kNoPopup, u.popupAt = u.now = 0, u.scaleAt = -1;
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

/** The Scale screen's buttons: the roots (upper 2..7, lower 2..7), In Key (lower 1), Fixed (lower 8) and the arrows,
 * which move through the scale list (up and down by one, left and right by a column of four). */
bool Machine::scaleButton(Scale& sc, int button) {
	using namespace push2::cc;
	int i = button - Track1Above - 1, j = button - Track1Below - 1;
	if (i >= 0 && i < 6)
		sc.root = kRoots[i];
	else if (j >= 0 && j < 6)
		sc.root = kRoots[6 + j];
	else if (button == Track1Below)
		sc.inKey = !sc.inKey;
	else if (button == Track8Below)
		sc.fixed = !sc.fixed;
	else if (button >= Left && button <= Down) {
		int by = button == Left ? -4 : button == Right ? 4 : button == Up ? -1 : 1;
		sc.scale = (uint8_t)std::min(std::max(sc.scale + by, 0), kScaleCount - 1);
	} else {
		return false;
	}
	return true;
}

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
		if (!e.value) {
			if (e.index == push2::cc::Scale && ui.scaleAt >= 0 && ui.now - ui.scaleAt >= kHoldSeconds)
				ui.screen = kHome; // held: the screen was only there for as long as the button
			if (e.index == push2::cc::Scale)
				ui.scaleAt = -1;
			break;
		}
		if (e.index == push2::cc::Play)
			request_ = clock_.started() ? Request::Stop : Request::Play;
		else if (e.index == push2::cc::Metronome)
			t.metronome = !t.metronome;
		else if (e.index == push2::cc::TapTempo)
			tap(t), show(kTempoPopup); // never starts the transport
		else if (e.index == push2::cc::Note)
			ui.pads = kNote;
		else if (e.index == push2::cc::Scale) {
			ui.screen = ui.screen == kScaleScreen ? kHome : kScaleScreen;
			ui.scaleAt = ui.screen == kScaleScreen ? ui.now : -1;
		} else if (ui.screen == kScaleScreen && scaleButton(s.scale, e.index)) {
		} else if (e.index >= push2::cc::Track1Below && e.index <= push2::cc::Track8Below)
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
		} else if (e.index < 8 && ui.screen == kScaleScreen) { // the Scale screen: layout, scale list, direction
			Scale& sc = s.scale;
			if (e.index == 0 || e.index == 7) {
				int n = steps(accScale_[e.index], e.value, kLayoutUnits);
				if (e.index == 0)
					sc.layout = (uint8_t)(std::min(std::max(sc.layout / 2 + n, 0), 2) * 2 + sc.layout % 2);
				else if (n)
					sc.layout = (uint8_t)(sc.layout / 2 * 2 + (n > 0));
			} else {
				int n = steps(accScale_[e.index], e.value, kScaleUnits);
				sc.scale = (uint8_t)std::min(std::max(sc.scale + n, 0), kScaleCount - 1);
			}
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
	white(push2::cc::Scale, u.screen == kScaleScreen || u.button[push2::cc::Scale]);
	if (u.screen == kScaleScreen) { // the roots (the selected one white), In Key (always on) and Fixed
		const Scale& sc = s.scale;
		auto option = [&](int c, bool on) {
			out.cc[c] = {on ? push2::kRgbWhite : push2::kRgbDarkGray, push2::kStatic};
		};
		for (int i = 0; i < 12; i++)
			option(i < 6 ? push2::cc::Track1Above + 1 + i : push2::cc::Track1Below - 5 + i, kRoots[i] == sc.root);
		option(push2::cc::Track1Below, true);
		option(push2::cc::Track8Below, sc.fixed);
	} else {
		for (int t = 0; t < kTracks; t++)
			out.cc[push2::cc::Track1Below + t] = {t == u.track ? push2::kRgbWhite : (uint8_t)(kPaletteFull + t),
												  push2::kStatic};
	}
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
			uint8_t c = held[p]   ? push2::kGreen
						: root    ? (uint8_t)(kPaletteFull + u.track)
						: inScale ? push2::kRgbWhite
								  : push2::kBlack;
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
			const uint8_t* c = kLiveColors[s.track[i].color % kColors];
			rgb_[i] = (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
			// ponytail: raw RGB; if the LEDs look washed out on the hardware, apply a gamma here.
			out.rgb[i] = rgb_[i];
			out.rgb[kTracks + i] = (uint32_t)(c[0] / 4) << 16 | (uint32_t)(c[1] / 4) << 8 | (c[2] / 4);
			if (s.track[i].color != sentColor_[i])
				out.palette = true;
		}
		if (out.palette)
			for (int i = 0; i < kTracks; i++)
				sentColor_[i] = s.track[i].color;
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
