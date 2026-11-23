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
// ponytail: raw encoder deltas per change of a held step's Nudge, Length (a step), Length fine (a tick) and Velocity
// (1); guesses like the ones above, calibrate on the hardware.
constexpr int kNoteUnits[4] = {2, 8, 2, 1};
// The ticks of a step by resolution (the scene buttons from the top): 1/4, 1/4t, 1/8, 1/8t, 1/16, 1/16t, 1/32, 1/32t.
constexpr int kGrid[8] = {96, 64, 48, 32, 24, 16, 12, 8};
// The launch quantization in ticks, by Transport::quant.
constexpr int kQuant[14] = {0, 3072, 1536, 768, 384, 192, 128, 96, 64, 48, 32, 24, 16, 12};

/** The pitch of a sequencer row (0 = bottom): the next step of the scale above the octave's root (the next semitone in
 * Chromatic), or -1 above 127. */
int seqPitch(const Session& s, int row, bool& root, bool& inScale) {
	Scale sq = s.scale;
	sq.layout = 1; // "right" with the row as the column: step = row
	return padPitch(sq, s.track[s.ui.track].octave, row, 0, root, inScale);
}

/** The note of `pitch` on the step that starts at `start` (its tick within half a step), or -1. ponytail: a scan, as it
 * only runs on a pad press. */
int findNote(const Clip& c, int pitch, int start, int grid) {
	for (int i = 0; i < c.nNotes; i++)
		if (c.notes[i].pitch == pitch && (c.notes[i].tick + grid / 2) / grid * grid == start)
			return i;
	return -1;
}

bool before(const Note& a, const Note& b) {
	return a.tick != b.tick ? a.tick < b.tick : a.pitch < b.pitch;
}

/** Puts `n` in its place in the sorted notes: its index, or -1 when the clip is full or has that tick and pitch. */
int insertNote(Clip& c, const Note& n) {
	if (c.nNotes >= kMaxNotes)
		return -1;
	Note* end = c.notes + c.nNotes;
	Note* p = std::lower_bound(c.notes, end, n, before);
	if (p != end && !before(n, *p))
		return -1;
	std::memmove(p + 1, p, (size_t)(end - p) * sizeof(Note));
	*p = n;
	c.nNotes++;
	return (int)(p - c.notes);
}

void eraseNote(Clip& c, int i) {
	std::memmove(c.notes + i, c.notes + i + 1, (size_t)(c.nNotes - i - 1) * sizeof(Note));
	c.nNotes--;
}

/** The last page of the clip at this step size. */
int lastPage(const Clip& c, int grid) {
	return c.length ? std::max(((c.length + grid - 1) / grid + 7) / 8 - 1, 0) : 0;
}

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
	u.fixedBars = std::min<uint8_t>(u.fixedBars, 7);
	u.fixedOn = u.fixedOn != 0;
	u.page = 0, u.heldStep = -1, u.heldHad = u.heldEdited = false, u.fixedAt = -1, u.bend = 0;
	std::fill(u.pad, u.pad + 64, 0);
	std::fill(u.button, u.button + 128, false);
	u.touched = 0, u.popup = kNoPopup, u.popupAt = u.now = 0, u.scaleAt = -1;
	for (auto& row : st.clip)
		for (Clip& c : row) { // `playing` is transient, so a clip needs no more than sorted notes of sound values
			c.length = std::min<uint16_t>(c.length, kMaxLength);
			c.nNotes = c.length ? std::min<uint16_t>(c.nNotes, kMaxNotes) : 0;
			int n = 0;
			for (int i = 0; i < c.nNotes; i++) {
				Note x = c.notes[i];
				if (x.pitch > 127 || x.tick >= kMaxLength)
					continue;
				x.vel = std::min<uint8_t>(std::max<uint8_t>(x.vel, 1), 127);
				x.len = std::min<uint16_t>(std::max<uint16_t>(x.len, 1), kMaxLength);
				c.notes[n++] = x;
			}
			std::sort(c.notes, c.notes + n, before);
			c.nNotes =
				(uint16_t)(std::unique(c.notes, c.notes + n,
									   [](const Note& a, const Note& b) { return !before(a, b) && !before(b, a); }) -
						   c.notes);
		}
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

/** While Fixed Length is held: lower buttons 1..8 choose the length, upper button 1 switches it on or off. */
static bool fixedLengthButton(Ui& ui, int button) {
	if (button >= push2::cc::Track1Below && button <= push2::cc::Track8Below)
		ui.fixedBars = (uint8_t)(button - push2::cc::Track1Below);
	else if (button == push2::cc::Track1Above)
		ui.fixedOn = !ui.fixedOn;
	else
		return false;
	ui.fixedAt = -1;
	return true;
}

void Machine::handle(State& st, const push2::Event& e) {
	using push2::Event;
	Session& s = st.s;
	Transport& t = s.transport;
	Ui& ui = s.ui;
	auto show = [&](Popup p) { ui.popup = p, ui.popupAt = ui.now; };
	auto select = [&](int track) { // a held step belongs to the clip it was pressed on
		ui.heldStep = -1;
		if (track != ui.track)
			ui.track = (uint8_t)track, ui.page = 0;
	};
	switch (e.type) {
	case Event::Button:
		ui.button[e.index & 127] = e.value != 0;
		if (!e.value) {
			if (e.index == push2::cc::Scale && ui.scaleAt >= 0 && ui.now - ui.scaleAt >= kHoldSeconds)
				ui.screen = kHome; // held: the screen was only there for as long as the button
			if (e.index == push2::cc::Scale)
				ui.scaleAt = -1;
			if (e.index == push2::cc::FixedLength) { // a tap toggles; a hold, or choosing a length, doesn't
				if (ui.fixedAt >= 0 && ui.now - ui.fixedAt < kHoldSeconds)
					ui.fixedOn = !ui.fixedOn;
				ui.fixedAt = -1;
			}
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
		else if (e.index == push2::cc::FixedLength)
			ui.fixedAt = ui.now;
		else if (ui.button[push2::cc::FixedLength] && fixedLengthButton(ui, e.index)) {
		} else if (e.index == push2::cc::Scale) {
			ui.screen = ui.screen == kScaleScreen ? kHome : kScaleScreen;
			ui.scaleAt = ui.screen == kScaleScreen ? ui.now : -1;
		} else if (ui.screen == kScaleScreen && scaleButton(s.scale, e.index)) {
		} else if (e.index >= push2::cc::Track1Below && e.index <= push2::cc::Track8Below)
			select(e.index - push2::cc::Track1Below);
		else if (ui.pads != kNote)
			break;
		else if (e.index == push2::cc::Left || e.index == push2::cc::Right)
			select(std::min(std::max(ui.track + (e.index == push2::cc::Right ? 1 : -1), 0), kTracks - 1));
		else if (e.index == push2::cc::OctaveUp || e.index == push2::cc::OctaveDown) {
			int8_t& o = s.track[ui.track].octave;
			o = (int8_t)std::min(std::max(o + (e.index == push2::cc::OctaveUp ? 1 : -1), 0), kMaxOctave);
		} else if (e.index == push2::cc::Layout)
			ui.noteLayout ^= 1, ui.heldStep = -1, ui.page = 0;
		else if (ui.noteLayout && ui.heldStep < 0) { // the sequencer: pages and step sizes (not while a step is held)
			Track& tr = s.track[ui.track];
			int last = lastPage(st.clip[ui.track][ui.scene], kGrid[tr.grid]);
			if (e.index == push2::cc::PageLeft || e.index == push2::cc::PageRight)
				ui.page = (uint8_t)std::min(std::max(ui.page + (e.index == push2::cc::PageRight ? 1 : -1), 0), last);
			else if (e.index >= push2::cc::Scene8 && e.index <= push2::cc::Scene1) {
				tr.grid = (uint8_t)(push2::cc::Scene1 - e.index);
				ui.page = (uint8_t)std::min<int>(ui.page, lastPage(st.clip[ui.track][ui.scene], kGrid[tr.grid]));
			}
		}
		break;
	case Event::Pad:
		pad(st, e.index & 63, e.value);
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
		} else if (e.index < 4 && ui.heldStep >= 0) { // a held step: Nudge, Length, Length fine, Velocity
			noteEncoder(st, e.index, e.value);
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
 * In the sequencer, the pads are the steps of the selected clip instead. */
void Machine::pad(State& st, int i, int vel) {
	Session& s = st.s;
	Live& l = live_[i];
	if (l.id)
		voice_[l.track].noteOff(l.id), l = Live(), s.ui.pad[i] = 0;
	if (s.ui.pads == kNote && s.ui.noteLayout) {
		sequencer(st, i, vel);
		return;
	}
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

/** Starts the clip of `scene` on `track` at the launch quantization, at once when the transport is stopped. */
void Machine::launch(Session& s, int track, int scene) {
	Track& tr = s.track[track];
	if (!clock_.started()) {
		tr.playing = (int8_t)scene, tr.queued = kNone, tr.startTick = 0;
		return;
	}
	int q = kQuant[s.transport.quant];
	tr.queued = (int8_t)scene;
	tr.launchTick = q ? (s.transport.tick / q + 1) * q : s.transport.tick + 1;
}

/** The transport stopped: every clip stops (as in Live) and its notes end. Pads you hold go on sounding. */
void Machine::stopClips(Session& s) {
	for (int t = 0; t < kTracks; t++) {
		for (int i = 0; i < nActive_[t]; i++)
			voice_[t].noteOff(active_[t][i].id);
		nActive_[t] = 0;
		s.track[t].playing = s.track[t].queued = kNone;
	}
}

/** A sequencer pad: tap a step to put a note on it (the first one makes the clip) or to take it off, hold it and turn
 * an encoder to edit it, or hold it and press a later step of its row for a long note. */
void Machine::sequencer(State& st, int i, int vel) {
	Session& s = st.s;
	Ui& ui = s.ui;
	Track& tr = s.track[ui.track];
	Clip& c = st.clip[ui.track][ui.scene];
	int grid = kGrid[tr.grid], start = (ui.page * 8 + i % 8) * grid;
	bool root, inScale;
	int pitch = seqPitch(s, i / 8, root, inScale);
	if (!vel) { // a tap on a note that was there takes it off, unless the hold edited it
		if (i == ui.heldStep) {
			int n = findNote(c, pitch, start, grid);
			if (ui.heldHad && !ui.heldEdited && n >= 0)
				eraseNote(c, n);
			ui.heldStep = -1;
		}
		return;
	}
	int length = c.length ? c.length : ui.fixedOn ? kPpqn << ui.fixedBars : kBar;
	if (pitch < 0 || start >= length)
		return;
	int n = findNote(c, pitch, start, grid);
	if (ui.button[push2::cc::Delete]) {
		if (n >= 0)
			eraseNote(c, n);
	} else if (ui.heldStep >= 0) { // ponytail: one held step; a press in its row to the right stretches its note
		int h = findNote(c, pitch, (ui.page * 8 + ui.heldStep % 8) * grid, grid);
		if (i / 8 == ui.heldStep / 8 && i % 8 > ui.heldStep % 8 && h >= 0)
			c.notes[h].len = (uint16_t)(std::min(start + grid, length) - c.notes[h].tick), ui.heldEdited = true;
	} else if (n >= 0) {
		ui.heldStep = (int8_t)i, ui.heldHad = true, ui.heldEdited = false;
	} else {
		Note note = {(uint16_t)start, (uint16_t)std::min(grid, length - start), (uint8_t)pitch, 100};
		if (insertNote(c, note) < 0)
			return;
		ui.heldStep = (int8_t)i, ui.heldHad = ui.heldEdited = false;
		c.length = (uint16_t)length;
		// ponytail: until Session mode (E2.5) is there to launch it, a clip you add notes to plays when its track has
		// none.
		if (tr.playing == kNone && tr.queued == kNone)
			launch(s, ui.track, ui.scene);
	}
}

/** An encoder turned while a step is held: 0 Nudge (ticks, within half a step), 1 Length (steps), 2 Length fine
 * (ticks), 3 Velocity. */
void Machine::noteEncoder(State& st, int encoder, int delta) {
	Ui& ui = st.s.ui;
	Clip& c = st.clip[ui.track][ui.scene];
	int grid = kGrid[st.s.track[ui.track].grid], start = (ui.page * 8 + ui.heldStep % 8) * grid;
	bool root, inScale;
	int h = findNote(c, seqPitch(st.s, ui.heldStep / 8, root, inScale), start, grid);
	int n = steps(accNote_[encoder], delta, kNoteUnits[encoder]);
	if (h < 0 || !n)
		return;
	Note note = c.notes[h];
	ui.heldEdited = true;
	if (encoder == 0) { // moves it in the order of the notes
		int tick = std::min(std::max(note.tick + n, std::max(start - grid / 2, 0)),
							std::min(start + grid / 2 - 1, c.length - 1));
		eraseNote(c, h);
		Note moved = note;
		moved.tick = (uint16_t)tick;
		if (insertNote(c, moved) < 0)
			insertNote(c, note); // that tick and pitch is taken
	} else if (encoder == 3) {
		c.notes[h].vel = (uint8_t)std::min(std::max(note.vel + n, 1), 127);
	} else {
		int len = note.len + n * (encoder == 1 ? grid : 1);
		c.notes[h].len = (uint16_t)std::min(std::max(len, 1), c.length - note.tick);
	}
}

/** The sequencer's LEDs: notes in the track's color (dim for soft ones), the empty steps dark gray (white in a root
 * row), nothing past the end of the clip, the playhead green (white on a note), the held step white; page, step size
 * and Delete buttons. */
static void seqLeds(const State& st, push2::Leds& out) {
	const Session& s = st.s;
	const Ui& u = s.ui;
	const Track& tr = s.track[u.track];
	const Clip& c = st.clip[u.track][u.scene];
	int grid = kGrid[tr.grid], last = lastPage(c, grid);
	auto white = [&](int b, bool full) { out.cc[b] = {full ? push2::kWhiteFull : push2::kWhiteDark, push2::kStatic}; };
	if (u.page > 0)
		white(push2::cc::PageLeft, u.button[push2::cc::PageLeft]);
	if (u.page < last)
		white(push2::cc::PageRight, u.button[push2::cc::PageRight]);
	white(push2::cc::Delete, u.button[push2::cc::Delete]);
	for (int i = 0; i < 8; i++)
		out.cc[push2::cc::Scene1 - i] = {i == tr.grid ? push2::kRgbWhite : push2::kRgbDarkGray, push2::kStatic};

	int pitch[8];
	bool root[8], inScale[8];
	for (int row = 0; row < 8; row++)
		pitch[row] = seqPitch(s, row, root[row], inScale[row]);
	uint8_t vel[8][8] = {}; // the velocity of the note on each pad, 0: none
	for (int i = 0; i < c.nNotes; i++) {
		int col = (c.notes[i].tick + grid / 2) / grid - u.page * 8;
		for (int row = 0; row < 8 && col >= 0 && col < 8; row++)
			if (pitch[row] == c.notes[i].pitch)
				vel[row][col] = c.notes[i].vel;
	}
	int head = -1; // the step the clip is at
	if (s.transport.playing && tr.playing == u.scene && c.length)
		head = (int)(std::max<int64_t>(s.transport.tick - tr.startTick, 0) % c.length) / grid;
	for (int row = 0; row < 8; row++)
		for (int col = 0; col < 8; col++) {
			int step = u.page * 8 + col;
			uint8_t color = push2::kBlack;
			if (pitch[row] < 0 || (c.length && step * grid >= c.length)) {
			} else if (row * 8 + col == u.heldStep) {
				color = push2::kRgbWhite;
			} else if (vel[row][col]) {
				color = step == head         ? push2::kRgbWhite
						: vel[row][col] < 64 ? (uint8_t)(kPaletteDim + u.track)
											 : (uint8_t)(kPaletteFull + u.track);
			} else {
				color = step == head   ? push2::kGreen
						: root[row]    ? push2::kRgbWhite
						: inScale[row] ? push2::kRgbDarkGray
									   : push2::kBlack;
			}
			out.note[push2::padNote(row, col)] = {color, push2::kStatic};
		}
}

void Machine::leds(const State& st, push2::Leds& out) const {
	using push2::cc::Metronome, push2::cc::Play, push2::cc::Shift, push2::cc::TapTempo;
	const Session& s = st.s;
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
	white(push2::cc::FixedLength, u.fixedOn || u.button[push2::cc::FixedLength]);
	if (u.screen == kScaleScreen) { // the roots (the selected one white), In Key (always on) and Fixed
		const Scale& sc = s.scale;
		auto option = [&](int c, bool on) {
			out.cc[c] = {on ? push2::kRgbWhite : push2::kRgbDarkGray, push2::kStatic};
		};
		for (int i = 0; i < 12; i++)
			option(i < 6 ? push2::cc::Track1Above + 1 + i : push2::cc::Track1Below - 5 + i, kRoots[i] == sc.root);
		option(push2::cc::Track1Below, true);
		option(push2::cc::Track8Below, sc.fixed);
	} else if (u.button[push2::cc::FixedLength]) { // the lengths (the chosen one white) and the switch
		for (int i = 0; i < 8; i++)
			out.cc[push2::cc::Track1Below + i] = {i == u.fixedBars ? push2::kRgbWhite : push2::kRgbDarkGray,
												  push2::kStatic};
		out.cc[push2::cc::Track1Above] = {u.fixedOn ? push2::kRgbWhite : push2::kRgbDarkGray, push2::kStatic};
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
	white(push2::cc::Layout, u.noteLayout || u.button[push2::cc::Layout]);
	if (u.noteLayout) {
		seqLeds(st, out);
		return;
	}
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

/** Tick `k`: the launches that are due, the notes that end, then the notes that start in each track's clip. A note
 * that ends on the tick another starts is released first, so the voice retriggers. */
void Machine::tick(State& st, int64_t k, Outputs& out) {
	Session& s = st.s;
	for (int t = 0; t < kTracks; t++) {
		Track& tr = s.track[t];
		filo::VoiceAllocator& v = voice_[t];
		if (tr.queued != kNone && k >= tr.launchTick) {
			if (tr.queued == kStopQueued)
				tr.playing = kNone;
			else
				tr.playing = tr.queued, tr.startTick = k;
			tr.queued = kNone;
		}
		for (int i = 0; i < nActive_[t];) {
			if (active_[t][i].off <= k)
				v.noteOff(active_[t][i].id), active_[t][i] = active_[t][--nActive_[t]];
			else
				i++;
		}
		if (tr.playing == kNone)
			continue;
		const Clip& c = st.clip[t][tr.playing];
		if (!c.length)
			continue;
		int pos = (int)((k - tr.startTick) % c.length);
		// Swing delays the notes exactly on an even sixteenth (tick 24 of 48) by up to 12 ticks, where they start late.
		int d = (s.transport.swing * 12 + 50) / 100;
		for (int pass = 0; pass <= (d > 0); pass++) {
			int at = pass ? ((pos - d) % c.length + c.length) % c.length : pos;
			Note key = {(uint16_t)at, 0, 0, 0};
			for (const Note* n = std::lower_bound(c.notes, c.notes + c.nNotes, key, before);
				 n != c.notes + c.nNotes && n->tick == at; n++) {
				if (d > 0 && (n->tick % 48 == 24) != (pass == 1))
					continue;
				if (nActive_[t] == kActive)
					break;
				uint32_t id = ++nextId_;
				v.noteOn(id, (float)n->pitch, (float)n->vel / 127.f, 0);
				active_[t][nActive_[t]++] = {id, k + n->len};
			}
		}
	}
	if (s.transport.metronome && k % kPpqn == 0)
		out.metro = true, out.metroV = k % kBar == 0 ? 10.f : 5.f;
}

void Machine::process(const Inputs& in, Outputs& out, push2::Leds& want, SliceFrame* slice) {
	out = Outputs();
	if (silence_.load(std::memory_order_relaxed) && silence_.exchange(false)) { // a patch was loaded
		for (filo::VoiceAllocator& v : voice_)
			v.allOff();
		std::fill(nActive_, nActive_ + kTracks, 0);
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
		clock_.stop(), stopClips_ = sync_ = true;

	// Tempo: setCps re-anchors the clock, so only on change.
	float cps = std::min(std::max(bpm_ * std::exp2(in.tempoCv), 20.f), 300.f) / 60.f;
	if (cps != cps_)
		cps_ = cps, clock_.setCps(cps);
	clock_.setPulsesPerCycle(in.ppqnIn);
	clk_.setDivision(in.ppqnOut);
	clock_.step(in.clockConnected, in.clockEdge, reset);
	if (reset)
		nextTick_ = 0, lastAt_ = -1, out.reset = sync_ = restartClips_ = true;
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
		if (stopClips_)
			stopClips(s), stopClips_ = false;
		if (restartClips_) { // clips begin again, the launches still waiting go now, and notes from before end
			for (int t = 0; t < kTracks; t++) {
				for (int i = 0; i < nActive_[t]; i++)
					voice_[t].noteOff(active_[t][i].id);
				nActive_[t] = 0;
				s.track[t].startTick = s.track[t].launchTick = 0;
			}
			restartClips_ = false;
		}
		for (int i = 0; i < in.nEvents; i++)
			handle(*state_, in.events[i]);
		out.consumed = in.nEvents > 0;
		// ponytail: at most 8 ticks per sample, the rest follow on the next samples; only an external clock at 1 or 2
		// pulses per beat jumps that far.
		for (int n = 0; n < 8 && clock_.started() && nextTick_ <= due; n++)
			tick(*state_, nextTick_++, out);
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
		leds(*state_, want);
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
	v.held = -1;
	if (u.pads == kNote && u.noteLayout && u.heldStep >= 0) {
		int grid = kGrid[v.s.track[u.track].grid];
		bool root, inScale;
		v.held = (int16_t)findNote(clip, seqPitch(v.s, u.heldStep / 8, root, inScale),
								   (u.page * 8 + u.heldStep % 8) * grid, grid);
	}
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
