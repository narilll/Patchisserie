#include "tart.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace tart {

namespace {

constexpr double kEps = 1e-9;  // internal clock: absorbs float error so tick k lands on the exact sample
constexpr int kTempoUnits = 1; // raw encoder deltas per detent: 1 BPM
// ponytail: the track encoders send about 210 steps per turn (the manual); 8 per scale and 30 per layout choice are
// guesses, calibrate on the hardware.
constexpr int kScaleUnits = 8, kLayoutUnits = 30;
// ponytail: raw encoder deltas per beat of a clip's length on the Clip screen; a guess, calibrate on the hardware.
constexpr int kLengthUnits = 4;
// ponytail: 2 raw deltas per percent, from the manual's "about 210 steps per turn"; calibrate on the hardware.
constexpr int kSwingUnits = 2;
// ponytail: raw encoder deltas per change of a held step's Nudge, Length (a step), Length fine (a tick) and Velocity
// (1); guesses like the ones above, calibrate on the hardware.
constexpr int kNoteUnits[4] = {2, 8, 2, 1};
// ponytail: raw encoder deltas per change of the Euclid screen's STEPS, PULSES, ROTATE, PROB, VEL, SEED, OCTAVES and
// CHANGE, and SHIFT makes it 4 times as many; guesses, calibrate on the hardware.
constexpr int kEuclidUnits[8] = {8, 8, 8, 2, 1, 4, 30, 2};
// The ticks of a step by resolution (the scene buttons from the top): 1/4, 1/4t, 1/8, 1/8t, 1/16, 1/16t, 1/32, 1/32t.
constexpr int kGrid[8] = {96, 64, 48, 32, 24, 16, 12, 8};
// The steps of a drum track's page, and what one pad of its loop selector covers at a step size: a page, up to a bar.
constexpr int kDrumSteps = 32;
constexpr int loopTicks(int grid) {
	return std::min(kDrumSteps * grid, kBar);
}
// ponytail: a Device encoder's raw delta is 312 of the 65535 a lane spans (about 210 steps per turn, as the manual
// says), and SHIFT divides it by 31; guesses, calibrate on the hardware.
constexpr int kValueUnits = 312, kFineDivisor = 31;
// ponytail: the writer drops a recorded point when the line through its neighbours stays within 256 of 65535 (0.4% of
// the range; the 256 points of a lane are then about a minute of a slow sweep); a guess, lower it if sweeps look
// angular.
constexpr int kDoorTolerance = 256;
// The grids of Quantize To and Record Quantize (kQuantToNames): the straight step and the triplet one (0: none). "+T"
// takes the nearer of the two.
constexpr int kQuantTo[8][2] = {{96, 0}, {48, 0}, {32, 0}, {48, 32}, {24, 0}, {16, 0}, {24, 16}, {12, 0}};
// ponytail: raw encoder deltas per change of Quantize To and Record Quantize; a guess, calibrate on the hardware.
constexpr int kQuantUnits = 8;
// The launch quantization in ticks, by Transport::quant.
constexpr int kQuant[14] = {0, 3072, 1536, 768, 384, 192, 128, 96, 64, 48, 32, 24, 16, 12};

/** The pitch of a sequencer row (0 = bottom): the next step of the scale above the octave's root (the next semitone in
 * Chromatic), or -1 above 127. */
int seqPitch(const Session& s, int row, bool& root, bool& inScale) {
	Scale sq = s.scale;
	sq.layout = 1; // "right" with the row as the column: step = row
	return padPitch(sq, s.track[s.ui.track].octave, row, 0, root, inScale);
}

/** The pitch and the start tick of a sequencer pad of the selected track: a melodic row and column of the page of 8
 * steps, or a step of the page of 32 on a drum lane or in the split layout (the top row is steps 1..8). The pitch is -1
 * for a pad that is no step or is above 127; a split step has none (a chord), it is 0. */
int stepOf(const Session& s, int pad, int& start) {
	const Ui& u = s.ui;
	const Track& tr = s.track[u.track];
	const bool split = tr.type != Drum && u.noteLayout == 2;
	if (tr.type != Drum && !split) {
		bool root, inScale;
		start = (u.page * 8 + pad % 8) * kGrid[tr.grid];
		return seqPitch(s, pad / 8, root, inScale);
	}
	start = (u.page * kDrumSteps + (7 - pad / 8) * 8 + pad % 8) * kGrid[tr.grid];
	return pad / 8 < 4 ? -1 : split ? 0 : kDrumNote + tr.lane;
}

/** The notes that start on the step at `start`, whatever their pitch: the first `max` indices go to `idx`; returns how
 * many there are. */
int stepNotes(const Clip& c, int start, int grid, int* idx, int max) {
	int k = 0;
	for (int i = 0; i < c.nNotes; i++)
		if (c.notes[i].tick >= start - grid / 2 && c.notes[i].tick < start + grid / 2) {
			if (k < max)
				idx[k] = i;
			k++;
		}
	return k;
}

/** Whether the split layout's chord has `pitch`: the pads held on its lower half, by default the track's bottom-left
 * pad. */
bool chordHas(const Session& s, int pitch) {
	const Ui& u = s.ui;
	if (u.chord[0] | u.chord[1])
		return u.chord[pitch >> 6] >> (pitch & 63) & 1;
	bool root, inScale;
	return pitch == padPitch(s.scale, s.track[u.track].octave, 0, 0, root, inScale);
}

/** The loop pad (counted from the clip's start) under a pad of the lower right 4 x 4: the pads show the group of 16
 * that holds the viewed page. */
int loopIndex(const Session& s, int pad) {
	int grid = kGrid[s.track[s.ui.track].grid];
	return s.ui.page * kDrumSteps * grid / loopTicks(grid) / 16 * 16 + (3 - pad / 8) * 4 + pad % 8 - 4;
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

/** Sorts the first `n` notes and drops the repeats of a tick and pitch: the clip's notes again. In place, as a sort
 * that allocates would not do on the audio thread. */
void settle(Clip& c, int n) {
	std::sort(c.notes, c.notes + n, before);
	c.nNotes = (uint16_t)(std::unique(c.notes, c.notes + n,
									  [](const Note& a, const Note& b) { return !before(a, b) && !before(b, a); }) -
						  c.notes);
}

/** Where a note that starts at `tick` goes in a clip of `length`: `amount` percent of the way to the nearest line of
 * the grid `to` (of both grids, the nearer line). A line at the end of the clip is its start. */
int quantized(int tick, int to, int amount, int length) {
	int target = 0, best = 1 << 30;
	for (int g : kQuantTo[to]) {
		if (!g)
			continue;
		int line = (tick + g / 2) / g * g;
		if (std::abs(line - tick) < best)
			best = std::abs(line - tick), target = line;
	}
	int moved = tick + (target - tick) * amount / 100;
	return moved >= length ? moved - length : moved;
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

/** Takes off the notes that start in [t0, t1) with a pitch in [p0, p1]. */
void eraseNotes(Clip& c, int t0, int t1, int p0, int p1) {
	auto in = [=](const Note& n) { return n.tick >= t0 && n.tick < t1 && n.pitch >= p0 && n.pitch <= p1; };
	c.nNotes = (uint16_t)(std::remove_if(c.notes, c.notes + c.nNotes, in) - c.notes);
}

/** The value a lane returns to (Delete + touch): 0 V, the middle of a bipolar range (also PAN), Default LEVEL for
 * LEVEL. */
uint16_t defaultValue(const Ui& u, const Track& t, int lane) {
	return lane == kLevel ? levelValue(u.defaultLevel) : lane == kPan || (t.bipolar >> lane & 1) ? 32768 : 0;
}

/** Replaces the points of `lane` with a tick in [t0, t1] by the `n` sorted ones in `add`. False, and nothing changed,
 * when the lane would hold too many; `dry` only asks. */
bool replacePoints(Clip& c, int lane, int t0, int t1, const Point* add, int n, bool dry = false) {
	Point *p = c.points[lane], *end = p + c.nPoints[lane];
	Point* lo = std::lower_bound(p, end, t0, [](const Point& a, int t) { return a.tick < t; });
	Point* hi = std::upper_bound(lo, end, t1, [](int t, const Point& a) { return t < a.tick; });
	int count = c.nPoints[lane] - (int)(hi - lo) + n;
	if (count > kMaxPoints)
		return false;
	if (!dry) {
		std::memmove(lo + n, hi, (size_t)(end - hi) * sizeof(Point));
		std::copy(add, add + n, lo);
		c.nPoints[lane] = (uint16_t)count;
	}
	return true;
}

/** The last point of `lane` with a tick in [t0, t1], or null. */
const Point* lastIn(const Clip& c, int lane, int t0, int t1) {
	const Point *p = c.points[lane], *end = p + c.nPoints[lane];
	const Point* hi = std::upper_bound(p, end, t1, [](int t, const Point& a) { return t < a.tick; });
	return hi != p && hi[-1].tick >= t0 ? hi - 1 : nullptr;
}

/** The last page of the clip at this step size, `perPage` steps to a page. */
int lastPage(const Clip& c, int grid, int perPage) {
	return c.length ? std::max(((c.length + grid - 1) / grid + perPage - 1) / perPage - 1, 0) : 0;
}

/** The steps on a page of the selected track. */
int pageSteps(const Track& tr, const Ui& u) {
	return tr.type == Drum || u.noteLayout == 2 ? kDrumSteps : 8;
}

/** Selects a slot: a held step belongs to the clip it was pressed on. */
void select(Ui& ui, int track, int scene) {
	ui.heldStep = -1;
	if (track != ui.track || scene != ui.scene)
		ui.track = (uint8_t)track, ui.scene = (uint8_t)scene, ui.page = 0;
}

/** The ticks the held step covers, [start, end), cut at the end of the clip. */
void stepSpan(const Session& s, const Clip& c, int& start, int& end) {
	stepOf(s, s.ui.heldStep, start);
	end = std::min(start + kGrid[s.track[s.ui.track].grid], (int)c.length);
}

/** The tick a launch or stop happens at: the next multiple of the launch quantization after `tick`. */
int64_t launchAt(const Transport& t) {
	int q = kQuant[t.quant];
	return q ? (t.tick / q + 1) * q : t.tick + 1;
}

/** Where an open take ends if it ends now: the next whole unit (the launch quantization, at least a beat) counted from
 * the clip's start, at most kMaxLength long. */
int64_t takeEnd(const Session& s, const Track& tr) {
	int64_t unit = std::max(kQuant[s.transport.quant], kPpqn);
	int64_t elapsed = std::max<int64_t>(s.transport.tick - tr.startTick, 0);
	return tr.startTick + std::min((elapsed / unit + 1) * unit, (int64_t)kMaxLength);
}

/** What a sounding pad or the playhead is lit with: green, red while a take records on the selected track (the
 * playhead: in the selected slot). */
uint8_t hot(const Ui& u, bool head = false) {
	return u.recTrack == u.track && (!head || u.recScene == u.scene) ? push2::kRed : push2::kGreen;
}

// MUTE and SOLO: a press shorter than this is a tap, a longer one locks the mode (Ableton's value).
constexpr double kMuteSoloSeconds = 0.6;

/** What the lower buttons toggle: 1 mute, 2 solo (the button held, else the mode a long press locked), 0 select. */
int muteSoloMode(const Ui& u) {
	return u.button[push2::cc::Mute] ? 1 : u.button[push2::cc::Solo] ? 2 : u.muteSoloLock;
}

/** Toggles the mute (mode 1) or the solo (mode 2) of a track; soloing it clears every other solo. */
void toggleMuteSolo(Session& s, int mode, int track) {
	Track& t = s.track[track];
	if (mode == 1) {
		t.mute = !t.mute;
		return;
	}
	bool on = !t.solo;
	for (Track& k : s.track)
		k.solo = false;
	t.solo = on;
}

/** Whole detents of an encoder: `delta` raw units added to the remainder `acc`, `units` per detent. */
int steps(int& acc, int delta, int units) {
	acc += delta;
	int n = acc / units;
	acc -= n * units;
	return n;
}

// ---- Euclid (E5): generated live from the clip's settings and the steps since the launch, never written to notes

/** A pure integer hash, the same on every platform (no std::rand): the draw of (seed, what, a, b). */
uint32_t draw(uint32_t seed, uint32_t what, uint32_t a, uint32_t b) {
	uint32_t x = seed * 0x9E3779B1u ^ what * 0x85EBCA77u ^ a * 0xC2B2AE3Du ^ b * 0x27D4EB2Fu;
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x; // draw(0,1,0,0) == 0xc4a0e45c, draw(7,17,3,2) == 0x676a73d9
}

/** Bjorklund's pattern of `k` pulses in `n` steps (Toussaint's group pairing, in the canonical form that starts with a
 * pulse): bit p is step p. n 1..32, k 0..n. */
uint32_t bjorklund(int n, int k) {
	if (k <= 0)
		return 0;
	if (k >= n)
		return n >= 32 ? ~0u : (1u << n) - 1;
	struct G {
		uint32_t bits;
		int len;
	} a[32], b[32], rest[32];
	int na = k, nb = n - k;
	for (int i = 0; i < na; i++)
		a[i] = {1, 1};
	for (int i = 0; i < nb; i++)
		b[i] = {0, 1};
	while (nb > 1) {
		int m = std::min(na, nb), nr = 0;
		for (int i = m; i < (na > m ? na : nb); i++)
			rest[nr++] = na > m ? a[i] : b[i];
		for (int i = 0; i < m; i++)
			a[i] = {a[i].bits | b[i].bits << a[i].len, a[i].len + b[i].len};
		na = m, nb = nr, std::copy(rest, rest + nr, b);
	}
	uint32_t bits = 0;
	int pos = 0;
	for (int i = 0; i < na; i++)
		bits |= a[i].bits << pos, pos += a[i].len;
	for (int i = 0; i < nb; i++)
		bits |= b[i].bits << pos, pos += b[i].len;
	return bits;
}

// ponytail: a melodic note that changed more than this many passes of its rhythm ago falls back to the base melody.
constexpr int kMutationScan = 256;

int laneSteps(const EuclidLane& l) {
	return std::min(std::max<int>(l.steps, 1), kMaxSteps);
}

/** The scale degree of pulse `k` (its index in the unrotated pattern) on pass `pass` of the melodic rhythm, of `nd`
 * degrees. CHANGE accumulates: each pass gives the pulse a new pitch with that chance, and a changed one keeps it until
 * it changes again, so the pitch comes from the last pass at or before this one that changed it (0: the base). */
int degree(const Euclid& e, int k, int64_t pass, int nd) {
	int64_t from = 0;
	for (int64_t l = pass; e.change && l >= std::max<int64_t>(pass - kMutationScan, 1); l--)
		if (draw(e.seed, 32, (uint32_t)k, (uint32_t)l) % 100 < e.change) {
			from = l;
			break;
		}
	return (int)((uint64_t)draw(e.seed, 33, (uint32_t)k, (uint32_t)from) * (uint32_t)nd >> 32);
}

/** The patterns of the clip's playing lanes (Drum: lanes 0..15, Melodic: lane kEuclidRhythm), the others 0. */
void euclidMasks(const Euclid& e, uint32_t* mask /*[kLanes + 1]*/) {
	std::fill(mask, mask + kLanes + 1, 0u);
	if (e.mode == kEuclidDrum)
		for (int l = 0; l < kLanes; l++)
			mask[l] = euclidMask(e.lane[l]);
	else
		mask[kEuclidRhythm] = euclidMask(e.lane[kEuclidRhythm]);
}

/** The notes of step `n` since the launch (tick and length are the caller's): Drum, a pulse of each lane that passes
 * its probability, on its lane's note; Melodic, at most one, pitched from the scale. */
int euclidAt(const Session& s, int track, const Euclid& e, const uint32_t* mask, int64_t n, Note* out) {
	const uint32_t step = (uint32_t)n; // ponytail: wraps after 2^32 steps (years)
	int count = 0;
	if (e.mode == kEuclidDrum) {
		for (int l = 0; l < kLanes; l++) {
			const EuclidLane& x = e.lane[l];
			if (mask[l] >> (n % laneSteps(x)) & 1 && draw(e.seed, 1 + l, step, 0) % 100 < (uint32_t)x.probability)
				out[count++] = {0, 0, (uint8_t)(kDrumNote + l), x.velocity};
		}
		return count;
	}
	const EuclidLane& x = e.lane[kEuclidRhythm];
	const int steps = laneSteps(x), p = (int)(n % steps);
	if (!(mask[kEuclidRhythm] >> p & 1) || draw(e.seed, 1 + kEuclidRhythm, step, 0) % 100 >= (uint32_t)x.probability)
		return 0;
	// A pulse is keyed by its index k in the unrotated pattern, so ROTATE moves the phrase with the rhythm.
	const int q = (p - x.rotation % steps + steps) % steps;
	const int k = __builtin_popcount(bjorklund(steps, std::min<int>(x.pulses, steps)) & ((1u << q) - 1));
	const int nd = __builtin_popcount(kScales[s.scale.scale % kScaleCount].mask) * e.octaves;
	Scale sq = s.scale; // the degrees of the scale from the root in the track's octave, as seqPitch does
	sq.layout = 1, sq.inKey = true, sq.fixed = false;
	bool root, inScale;
	int pitch = padPitch(sq, s.track[track].octave, degree(e, k, n / steps, nd), 0, root, inScale);
	if (pitch < 0)
		return 0;
	out[0] = {0, 0, (uint8_t)pitch, x.velocity};
	return 1;
}

/** Field `f` of the Euclid screen (the encoder: 0 STEPS, 1 PULSES, 2 ROTATE, 3 PROB, 4 VEL, 5 SEED, 6 OCTAVES, 7
 * CHANGE) of `e`, the lane `lane` for the first five: its stored value, which `set` (when not -1) replaces. */
int euclidField(Euclid& e, int lane, int f, int set = -1) {
	EuclidLane& x = e.lane[lane];
	int was = f == 5 ? e.seed : 0;
	uint8_t* b[8] = {&x.steps, &x.pulses, &x.rotation, &x.probability, &x.velocity, nullptr, &e.octaves, &e.change};
	if (f == 5) {
		if (set >= 0)
			e.seed = (uint16_t)set;
		return was;
	}
	was = *b[f];
	if (set >= 0)
		*b[f] = (uint8_t)set;
	return was;
}

} // namespace

const char* const kTypeNames[kBrowseTypes] = {"Melodic", "Drum", "Euclid"};

const char* const kQuantToNames[8] = {"1/4", "1/8", "1/8T", "1/8+T", "1/16", "1/16T", "1/16+T", "1/32"};

const char* const kQuantNames[14] = {"None", "8 Bars", "4 Bars", "2 Bars", "1 Bar", "1/2",   "1/2T",
									 "1/4",  "1/4T",   "1/8",    "1/8T",   "1/16",  "1/16T", "1/32"};

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

void setText(char* dst, size_t size, const char* text) {
	size_t n = std::min(std::strlen(text), size - 1);
	while (n && (text[n] & 0xC0) == 0x80) // inside a character: leave it out
		n--;
	std::memmove(dst, text, n);
	std::memset(dst + n, 0, size - n);
}

void setName(Track& t, const char* name) {
	setText(t.name, kNameBytes, name);
}

float volts(const Track& t, int lane, int v) {
	bool bipolar = lane == kPan || (lane < kCcs && t.bipolar >> lane & 1);
	return bipolar ? (float)(v - 32768) * 5.f / 32768.f : (float)v * 10.f / 65535.f;
}

uint16_t levelValue(int volts) {
	return (uint16_t)((65535 * std::min(std::max(volts, 0), 10) + 5) / 10);
}

int envelope(const Clip& c, int lane, int tick, bool arriving) {
	const Point *p = c.points[lane], *e = p + c.nPoints[lane];
	if (p == e)
		return -1;
	auto lower = [](const Point& a, int t) { return a.tick < t; };
	auto upper = [](int t, const Point& a) { return t < a.tick; };
	const Point* b = arriving ? std::lower_bound(p, e, tick, lower) : std::upper_bound(p, e, tick, upper);
	if (b == p)
		return p->value;
	if (b == e)
		return e[-1].value;
	const Point* a = b - 1; // a.tick < b.tick: a jump is never between them
	return a->value + (int)((int64_t)(b->value - a->value) * (tick - a->tick) / (b->tick - a->tick));
}

State::State() {
	for (int i = 0; i < kTracks; i++) {
		std::snprintf(s.track[i].name, kNameBytes, "Track %d", i + 1);
		s.track[i].color = kDefaultColors[i];
		s.track[i].value[kLevel] = 65535, s.track[i].value[kPan] = 32768; // 10 V, 0 V
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
	u.noteLayout = std::min<uint8_t>(u.noteLayout, 2);
	u.screen = kHome; // a patch opens on Home, as the Scale screen is a dialog
	u.track = std::min<uint8_t>(u.track, kTracks - 1);
	u.scene = std::min<uint8_t>(u.scene, kScenes - 1);
	u.fixedBars = std::min<uint8_t>(u.fixedBars, 7);
	u.fixedOn = u.fixedOn != 0;
	u.quantTo = std::min<uint8_t>(u.quantTo, 7), u.quantAmount = std::min<uint8_t>(u.quantAmount, 100);
	u.recQuant = std::min<uint8_t>(u.recQuant, 7), u.recQuantOn = u.recQuantOn != 0;
	u.quantizeAt = -1, u.quantizeUsed = false;
	u.chord[0] = u.chord[1] = 0, u.page = 0, u.heldStep = -1, u.heldHad = u.heldEdited = false, u.fixedAt = -1,
	u.bend = 0;
	std::fill(u.pad, u.pad + 64, 0);
	std::fill(u.button, u.button + 128, false);
	u.touched = 0, u.popup = kNoPopup, u.popupAt = u.now = 0, u.screenAt = u.padsAt = -1;
	u.prevPads = kSession, u.dup = -1, u.browse = 0;
	u.recTrack = -1, u.recScene = 0, u.recFresh = u.recOpen = false, u.recEnd = -1, u.writing = 0;
	u.automate = u.automate != 0, u.mixPans = u.mixPans != 0, u.defaultLevel = std::min<uint8_t>(u.defaultLevel, 10);
	u.muteSoloLock = 0, u.muteSoloAt = -1, u.muteSoloUsed = false;
	for (auto& row : st.clip)
		for (Clip& c : row) { // `playing` is transient, so a clip needs no more than sorted notes of sound values
			c.length = std::min<uint16_t>(c.length, kMaxLength);
			c.nNotes = c.length ? std::min<uint16_t>(c.nNotes, kMaxNotes) : 0;
			Euclid& e = c.euclid;
			if (!c.length) {
				e = Euclid(); // an empty slot holds the defaults, so a clip made in it starts from them
			} else {
				e.seed = std::min<uint16_t>(e.seed, kMaxSeed), e.mode = std::min<uint8_t>(e.mode, kEuclidMelodic);
				e.octaves = std::min<uint8_t>(std::max<uint8_t>(e.octaves, 1), kMaxOctaves);
				e.change = std::min<uint8_t>(e.change, 100), e.grid = std::min<uint8_t>(e.grid, 7), e.spare = 0;
				for (EuclidLane& l : e.lane) {
					l.steps = std::min<uint8_t>(std::max<uint8_t>(l.steps, 1), kMaxSteps);
					l.pulses = std::min<uint8_t>(l.pulses, kMaxSteps),
					l.rotation = std::min<uint8_t>(l.rotation, kMaxSteps - 1);
					l.probability = std::min<uint8_t>(l.probability, 100);
					l.velocity = std::min<uint8_t>(std::max<uint8_t>(l.velocity, 1), 127);
				}
			}
			int n = 0;
			for (int i = 0; i < c.nNotes; i++) {
				Note x = c.notes[i];
				if (x.pitch > 127 || x.tick >= kMaxLength)
					continue;
				x.vel = std::min<uint8_t>(std::max<uint8_t>(x.vel, 1), 127);
				x.len = std::min<uint16_t>(std::max<uint16_t>(x.len, 1), kMaxLength);
				c.notes[n++] = x;
			}
			settle(c, n);
			for (int j = 0; j < kAutos; j++) { // ponytail: a jump's order is lost only in a hand-edited patch
				Point* p = c.points[j];
				int m = 0;
				for (int i = 0, k = c.length ? std::min<int>(c.nPoints[j], kMaxPoints) : 0; i < k; i++)
					if (p[i].tick < kMaxLength)
						p[m++] = p[i];
				c.nPoints[j] = (uint16_t)m;
				auto by = [](const Point& a, const Point& b) { return a.tick < b.tick; };
				if (!std::is_sorted(p, p + m, by))
					std::sort(p, p + m, by);
			}
		}
	for (Track& t : st.s.track) {
		char name[kNameBytes + 1] = {};
		std::memcpy(name, t.name, kNameBytes); // not necessarily terminated
		setName(t, name);
		for (char (&cc)[kCcNameBytes] : t.ccName) {
			char text[kCcNameBytes + 1] = {};
			std::memcpy(text, cc, kCcNameBytes);
			setText(cc, kCcNameBytes, text);
		}
		t.color = std::min<uint8_t>(t.color, kColors - 1);
		if (t.type > Euclidean)
			t.type = Melodic;
		t.lane = std::min<uint8_t>(t.lane, kLanes - 1);
		t.voices = std::min<uint8_t>(std::max<uint8_t>(t.voices, 1), 16);
		t.grid = std::min<uint8_t>(t.grid, 7);
		t.octave = (int8_t)std::min<int>(std::max<int>(t.octave, 0), kMaxOctave);
		t.playing = t.queued = kNone;
		t.launchTick = t.startTick = 0;
		t.overridden = 0;
		t.mute = t.mute != 0, t.solo = t.solo != 0;
	}
}

void lengthName(char* buf, size_t size, int length) {
	if (length % kPpqn)
		std::snprintf(buf, size, "%d ticks", length);
	else if (length % kBar)
		std::snprintf(buf, size, "%d Beats", length / kPpqn);
	else
		std::snprintf(buf, size, length == kBar ? "1 Bar" : "%d Bars", length / kBar);
}

void popupText(const View& v, char* buf, size_t size) {
	const Ui& ui = v.s.ui;
	const Transport& t = v.s.transport;
	char name[24];
	switch (ui.popup) {
	case kUndoPopup:
		std::snprintf(buf, size, "Undo: Reverted last action"); // Live's texts
		break;
	case kRedoPopup:
		std::snprintf(buf, size, "Redo: Re-performed last undone action");
		break;
	case kQuantizedPopup:
		std::snprintf(buf, size, "Quantized to: %s, %d%%", kQuantToNames[std::min<int>(ui.quantTo, 7)], ui.quantAmount);
		break;
	case kLoopLengthPopup:
		lengthName(name, sizeof name, v.length[ui.track][ui.scene]);
		std::snprintf(buf, size, "New loop length: %s", name);
		break;
	case kLoopTooLongPopup:
		std::snprintf(buf, size, "Loop is 32 Bars at most");
		break;
	case kClipFullPopup:
		std::snprintf(buf, size, "Clip full");
		break;
	case kTempoPopup:
		std::snprintf(buf, size, t.external ? "Tempo %.1f BPM EXT" : "Tempo %.1f BPM", t.external ? t.shownBpm : t.bpm);
		break;
	default:
		std::snprintf(buf, size, "Swing %d %%", t.swing);
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

/** The Browse screen's buttons: UP and DOWN move the cursor, lower 8 is LOAD (the type goes to the selected track) and
 * upper 1 is CLOSE; LEFT, RIGHT and the other buttons above and below the display do nothing. */
bool Machine::browseButton(State& st, int button) {
	using namespace push2::cc;
	Ui& ui = st.s.ui;
	if (button == Up || button == Down) {
		ui.browse = (uint8_t)std::min(std::max(ui.browse + (button == Down ? 1 : -1), 0), kBrowseTypes - 1);
	} else if (button == Track8Below) {
		st.s.track[ui.track].type = ui.browse, ui.heldStep = -1, ui.page = 0, ui.screen = kHome;
	} else if (button == Track1Above) {
		ui.screen = kHome;
	} else if (button < Left || button > Down) {
		return (button >= Track1Below && button <= Track8Below) || (button >= Track1Above && button <= Track8Above);
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
	bool drum = s.track[ui.track].type == Drum;
	if ((e.type == Event::Button || e.type == Event::Pad) && e.value)
		gesture_++; // a press starts a gesture; releases and encoder turns belong to the one before
	switch (e.type) {
	case Event::Button:
		ui.button[e.index & 127] = e.value != 0;
		if (!e.value) {
			if ((e.index == push2::cc::Scale || e.index == push2::cc::Clip || e.index == push2::cc::Device ||
				 e.index == push2::cc::Mix) &&
				ui.screenAt >= 0) {
				if (ui.now - ui.screenAt >= kHoldSeconds)
					ui.screen = kHome; // held: the screen was only there for as long as the button
				ui.screenAt = -1;
			}
			if (e.index == push2::cc::Session || e.index == push2::cc::Note) {
				if (ui.padsAt >= 0 && ui.now - ui.padsAt >= kHoldSeconds)
					ui.pads = ui.prevPads; // held: the mode was only there for as long as the button
				ui.padsAt = -1;
			}
			if (e.index == push2::cc::Quantize) { // a tap quantizes; a hold, or a turn or a pad meanwhile, doesn't
				if (ui.quantizeAt >= 0 && !ui.quantizeUsed && ui.now - ui.quantizeAt < kHoldSeconds)
					quantizeClip(st, 0, 127);
				ui.quantizeAt = -1;
			}
			if ((e.index == push2::cc::Mute || e.index == push2::cc::Solo) && ui.muteSoloAt >= 0) {
				// a tap toggles the selected track (unless a lower button was used); a hold locks the mode
				int mode = e.index == push2::cc::Mute ? 1 : 2;
				if (ui.now - ui.muteSoloAt >= kMuteSoloSeconds)
					ui.muteSoloLock = (uint8_t)mode;
				else if (!ui.muteSoloUsed)
					toggleMuteSolo(s, mode, ui.track);
				ui.muteSoloAt = -1;
			}
			if (e.index == push2::cc::Duplicate)
				ui.dup = -1;
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
		else if (e.index == push2::cc::Undo) {
			bool redo = ui.button[push2::cc::Shift];
			if (undo(st, redo))
				show(redo ? kRedoPopup : kUndoPopup);
		} else if (e.index == push2::cc::Record) {
			record(st);
		} else if (e.index == push2::cc::Automate) { // Shift: automation follows the clips again; Delete: no automation
			Clip& c = st.clip[ui.track][ui.scene];
			if (ui.button[push2::cc::Shift]) {
				for (Track& tr : s.track)
					tr.overridden = 0;
			} else if (ui.button[push2::cc::Delete]) {
				if (std::any_of(c.nPoints, c.nPoints + kAutos, [](uint16_t n) { return n != 0; }))
					checkpoint(ui.track, ui.scene), std::fill(c.nPoints, c.nPoints + kAutos, 0);
			} else {
				ui.automate = !ui.automate;
			}
		} else if (e.index == push2::cc::Mute || e.index == push2::cc::Solo) {
			int mode = e.index == push2::cc::Mute ? 1 : 2;
			if (ui.muteSoloLock == mode) // the locked button: unlock, and its release does nothing
				ui.muteSoloLock = 0, ui.muteSoloAt = -1;
			else
				ui.muteSoloAt = ui.now, ui.muteSoloUsed = false;
		} else if (e.index == push2::cc::Note || e.index == push2::cc::Session) {
			ui.prevPads = ui.pads, ui.pads = e.index == push2::cc::Note ? kNote : kSession;
			ui.padsAt = ui.now, ui.heldStep = -1;
		} else if (e.index == push2::cc::FixedLength)
			ui.fixedAt = ui.now;
		else if (ui.button[push2::cc::FixedLength] && fixedLengthButton(ui, e.index)) {
		} else if (e.index == push2::cc::Quantize) {
			ui.quantizeAt = ui.now, ui.quantizeUsed = false;
		} else if (e.index == push2::cc::DoubleLoop) {
			doubleLoop(st);
		} else if (ui.button[push2::cc::Quantize] && e.index == push2::cc::Track1Above + 4) { // Record Quantize
			ui.recQuantOn = !ui.recQuantOn, ui.quantizeUsed = true;
		} else if (e.index == push2::cc::Scale || e.index == push2::cc::Clip || e.index == push2::cc::Device ||
				   e.index == push2::cc::Mix) {
			uint8_t to = e.index == push2::cc::Scale  ? kScaleScreen
						 : e.index == push2::cc::Clip ? kClipScreen
						 : e.index == push2::cc::Mix  ? kMixScreen
													  : kDeviceScreen;
			ui.screen = ui.screen == to ? (uint8_t)kHome : to;
			ui.screenAt = ui.screen == to ? ui.now : -1;
		} else if (e.index == push2::cc::Browse) {
			ui.screen = ui.screen == kBrowseScreen ? kHome : kBrowseScreen;
			ui.browse = std::min<uint8_t>(s.track[ui.track].type, kBrowseTypes - 1);
		} else if (ui.screen == kScaleScreen && scaleButton(s.scale, e.index)) {
		} else if (ui.screen == kBrowseScreen && browseButton(st, e.index)) {
		} else if (ui.screen == kMixScreen &&
				   (e.index == push2::cc::Track1Above || e.index == push2::cc::Track1Above + 1)) {
			ui.mixPans = e.index != push2::cc::Track1Above; // VOLUMES, PANS
		} else if (euclidHome(s) && (e.index == push2::cc::Track1Above || e.index == push2::cc::Track1Above + 1)) {
			euclidModeButton(st, e.index - push2::cc::Track1Above); // DRUM, MELODIC
		} else if (e.index == push2::cc::StopClip) {
			if (ui.button[push2::cc::Shift])
				for (int k = 0; k < kTracks; k++)
					stopTrack(s, k, false);
		} else if (e.index >= push2::cc::Track1Below && e.index <= push2::cc::Track8Below) {
			int n = e.index - push2::cc::Track1Below;
			if (ui.button[push2::cc::StopClip])
				stopTrack(s, n, false);
			else if (int mode = muteSoloMode(ui))
				toggleMuteSolo(s, mode, n), ui.muteSoloUsed = true;
			else
				select(ui, n, ui.scene);
		} else if (e.index >= push2::cc::Left && e.index <= push2::cc::Down) { // no scrolling: all 64 slots fit
			int d = e.index == push2::cc::Left ? -1 : e.index == push2::cc::Right ? 1 : 0;
			int v = e.index == push2::cc::Up ? -1 : e.index == push2::cc::Down ? 1 : 0;
			select(ui, std::min(std::max(ui.track + d, 0), kTracks - 1),
				   std::min(std::max(ui.scene + v, 0), kScenes - 1));
		} else if (e.index == push2::cc::New) { // ends a take, stops the track and moves to the next empty slot
			endTake();
			stopTrack(s, ui.track, false);
			int sc = ui.scene;
			while (sc < kScenes && st.clip[ui.track][sc].length)
				sc++;
			if (sc < kScenes)
				select(ui, ui.track, sc);
		} else if (ui.pads == kSession) {
			if (e.index >= push2::cc::Scene8 && e.index <= push2::cc::Scene1)
				sceneButton(st, push2::cc::Scene1 - e.index);
		} else if (s.track[ui.track].type == Euclidean) {
			// ponytail: OCTAVE, LAYOUT and PAGE do nothing on a Euclid track until E5.3 (OCTAVE); the scene buttons are
			// the clip's step size, and this keeps the sequencer branch from setting Track::grid for it.
			if (e.index >= push2::cc::Scene8 && e.index <= push2::cc::Scene1)
				euclidGridButton(st, push2::cc::Scene1 - e.index);
		} else if (drum && (e.index == push2::cc::OctaveUp || e.index == push2::cc::OctaveDown ||
							e.index == push2::cc::Layout)) {
			// drum pads have no octave and no sequencer layout
		} else if (e.index == push2::cc::OctaveUp || e.index == push2::cc::OctaveDown) {
			int8_t& o = s.track[ui.track].octave;
			o = (int8_t)std::min(std::max(o + (e.index == push2::cc::OctaveUp ? 1 : -1), 0), kMaxOctave);
		} else if (e.index == push2::cc::Layout)
			ui.noteLayout = (uint8_t)((ui.noteLayout + 1) % 3), ui.heldStep = -1, ui.page = 0;
		else if ((ui.noteLayout || drum) &&
				 ui.heldStep < 0) { // the sequencers: pages and step sizes (not while a step is held)
			Track& tr = s.track[ui.track];
			int last = lastPage(st.clip[ui.track][ui.scene], kGrid[tr.grid], pageSteps(tr, ui));
			if (e.index == push2::cc::PageLeft || e.index == push2::cc::PageRight)
				ui.page = (uint8_t)std::min(std::max(ui.page + (e.index == push2::cc::PageRight ? 1 : -1), 0), last);
			else if (e.index >= push2::cc::Scene8 && e.index <= push2::cc::Scene1) {
				tr.grid = (uint8_t)(push2::cc::Scene1 - e.index);
				ui.page = (uint8_t)std::min<int>(
					ui.page, lastPage(st.clip[ui.track][ui.scene], kGrid[tr.grid], pageSteps(tr, ui)));
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
		if (ui.button[push2::cc::Quantize])
			ui.quantizeUsed = true;
		if (e.index < 8 && ui.button[push2::cc::Quantize]) { // the Quantize settings
			quantizeEncoder(st, e.index, e.value);
		} else if (e.index == push2::kTempoId) {
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
		} else if (e.index < 7 && ui.screen == kBrowseScreen) { // the Browse screen: the cursor
			int n = steps(accBrowse_, e.value, kScaleUnits);
			ui.browse = (uint8_t)std::min(std::max(ui.browse + n, 0), kBrowseTypes - 1);
		} else if (e.index == 0 && ui.screen == kClipScreen) { // the Clip screen: the length of the selected clip
			int n = steps(accClip_, e.value, kLengthUnits);
			if (n)
				setLength(st, (st.clip[ui.track][ui.scene].length / kPpqn + n) * kPpqn);
		} else if (e.index < 8 && ui.screen == kDeviceScreen) { // the Device screen: CC 1 to 8, or a held step's lock
			valueEncoder(st, e.index, ui.track, e.index, e.value);
		} else if (e.index < 8 && ui.screen == kMixScreen) { // the Mix screen: LEVEL or PAN of each track, or a held
			if (ui.heldStep < 0) // step's lock on the selected track (encoder 1 LEVEL, 2 PAN)
				valueEncoder(st, e.index, e.index, ui.mixPans ? kPan : kLevel, e.value);
			else if (e.index < 2)
				valueEncoder(st, e.index, ui.track, e.index ? kPan : kLevel, e.value);
		} else if (e.index < 8 && euclidHome(s)) { // the Euclid screen: the settings of the selected clip
			euclidEncoder(st, e.index, e.value);
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
		if (e.value && e.index < 8 && euclidHome(s)) { // a touch is a gesture: one undo step for its turns
			gesture_++;
			if (ui.button[push2::cc::Delete])
				euclidReset(st, e.index);
		}
		if (e.value && e.index < 8 && ui.button[push2::cc::Delete]) {
			if (ui.screen == kDeviceScreen)
				deleteLane(st, ui.track, e.index);
			else if (ui.screen == kMixScreen)
				deleteLane(st, e.index, ui.mixPans ? kPan : kLevel);
		}
		if (!e.value && e.index < 8 && writeBy_[e.index] >= 0) // the touch ends what its turns wrote
			endWrite(st, writeBy_[e.index]), writeBy_[e.index] = -1;
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

/** An encoder turned while Quantize is held: 1 Swing, 2 Quantize To, 3 Amount, 5 Record Quantize. */
void Machine::quantizeEncoder(State& st, int encoder, int delta) {
	Ui& ui = st.s.ui;
	auto pick = [&](uint8_t& v, int& acc, int units, int max) {
		v = (uint8_t)std::min(std::max(v + steps(acc, delta, units), 0), max);
	};
	if (encoder == 0)
		pick(st.s.transport.swing, accSwing_, kSwingUnits, 100);
	else if (encoder == 1)
		pick(ui.quantTo, accQuantTo_, kQuantUnits, 7);
	else if (encoder == 2)
		pick(ui.quantAmount, accAmount_, kSwingUnits, 100);
	else if (encoder == 4)
		pick(ui.recQuant, accRecQuant_, kQuantUnits, 7);
}

/** Quantize: moves the starts of the selected clip's notes with a pitch in [p0, p1] toward the grid (Quantize To,
 * Amount), one undo step. The ends stay where they are, as on Live's Push. Two notes that land on the same tick and
 * pitch become one. */
void Machine::quantizeClip(State& st, int p0, int p1) {
	Ui& ui = st.s.ui;
	Clip& c = st.clip[ui.track][ui.scene];
	if (!c.length || st.s.track[ui.track].type == Euclidean) // a Euclid track plays no notes of the clip's
		return;
	checkpoint(ui.track, ui.scene);
	for (int i = 0; i < c.nNotes; i++)
		if (c.notes[i].pitch >= p0 && c.notes[i].pitch <= p1 && c.notes[i].tick < c.length)
			c.notes[i].tick = (uint16_t)quantized(c.notes[i].tick, ui.quantTo, ui.quantAmount, c.length);
	settle(c, c.nNotes);
	ui.heldStep = -1;
	ui.popup = kQuantizedPopup, ui.popupAt = ui.now;
}

/** Double Loop: the notes from the clip's end on are dropped, the clip is copied after its end and its length doubles.
 * Not past 32 bars, not when the notes would not fit, not while an open take sets the length. */
void Machine::doubleLoop(State& st) {
	Ui& ui = st.s.ui;
	Clip& c = st.clip[ui.track][ui.scene];
	if (!c.length || (ui.recOpen && ui.recTrack == ui.track && ui.recScene == ui.scene))
		return;
	ui.popupAt = ui.now;
	if (c.length * 2 > kMaxLength) {
		ui.popup = kLoopTooLongPopup;
		return;
	}
	int keep = 0; // the notes are sorted: those inside the clip come first
	while (keep < c.nNotes && c.notes[keep].tick < c.length)
		keep++;
	if (keep * 2 > kMaxNotes) {
		ui.popup = kClipFullPopup;
		return;
	}
	// A lane with points is framed (0, start) ... (length, end) and then repeated: two more points a half.
	for (int j = 0; j < kAutos; j++) {
		int inside = 0;
		for (int i = 0; i < c.nPoints[j]; i++)
			inside += c.points[j][i].tick > 0 && c.points[j][i].tick < c.length;
		if (c.nPoints[j] && 2 * (inside + 2) > kMaxPoints) {
			ui.popup = kClipFullPopup;
			return;
		}
	}
	checkpoint(ui.track, ui.scene);
	for (int j = 0; j < kAutos; j++) {
		if (!c.nPoints[j])
			continue;
		Point half[kMaxPoints / 2];
		int n = 0;
		half[n++] = {0, (uint16_t)envelope(c, j, 0)};
		for (int i = 0; i < c.nPoints[j]; i++)
			if (c.points[j][i].tick > 0 && c.points[j][i].tick < c.length)
				half[n++] = c.points[j][i];
		half[n++] = {c.length, (uint16_t)envelope(c, j, c.length, true)};
		for (int i = 0; i < n; i++)
			c.points[j][i] = half[i], c.points[j][n + i] = {(uint16_t)(half[i].tick + c.length), half[i].value};
		c.nPoints[j] = (uint16_t)(n * 2);
	}
	for (int i = 0; i < keep; i++)
		c.notes[keep + i] = {(uint16_t)(c.notes[i].tick + c.length), c.notes[i].len, c.notes[i].pitch, c.notes[i].vel};
	c.nNotes = (uint16_t)(keep * 2), c.length = (uint16_t)(c.length * 2);
	ui.heldStep = -1;
	ui.popup = kLoopLengthPopup;
}

/** Sets the length of the selected clip to `ticks` (1 beat to kMaxLength); an empty slot stays empty. Notes
 * beyond the new end are kept but don't play. A clip that plays on is cut at once: when its position is past the new
 * end, the loop wraps and the notes that began in the cut part end. */
void Machine::setLength(State& st, int ticks) {
	Ui& ui = st.s.ui;
	Clip& c = st.clip[ui.track][ui.scene];
	if (!c.length || (ui.recOpen && ui.recTrack == ui.track && ui.recScene == ui.scene)) // an open take sets it
		return;
	checkpoint(ui.track, ui.scene);
	int old = c.length;
	c.length = (uint16_t)std::min(std::max(ticks, kPpqn), kMaxLength);
	Track& tr = st.s.track[ui.track];
	if (tr.playing == ui.scene && (st.s.transport.tick - tr.startTick) % old >= c.length) {
		for (int i = 0; i < nActive_[ui.track]; i++)
			voice_[ui.track].noteOff(active_[ui.track][i].id);
		nActive_[ui.track] = 0;
	}
}

/** A pad pressed (velocity 1..127) or released (0): the note goes to the track selected now, and stops on that track.
 * In the sequencer, the pads are the steps of the selected clip instead. */
void Machine::pad(State& st, int i, int vel) {
	Session& s = st.s;
	Live& l = live_[i];
	if (l.id) {
		voice_[l.track].noteOff(l.id);
		if (l.rec >= 0)
			recordNote(st, i);
		l = Live();
	}
	s.ui.pad[i] = 0;
	if (vel && s.ui.button[push2::cc::Quantize]) {
		s.ui.quantizeUsed = true; // a pad while Quantize is held: no tap
		// Only a drum lane does something with Quantize held (that lane is quantized): other pads don't launch or edit.
		if (s.ui.pads == kSession || s.track[s.ui.track].type != Drum || i / 8 >= 4 || i % 8 >= 4)
			return;
	}
	if (s.ui.pads == kSession) {
		if (vel)
			session(st, i);
		return;
	}
	if (s.track[s.ui.track].type == Euclidean) {
		euclidPad(st, i, vel);
		return;
	}
	if (s.track[s.ui.track].type == Drum) {
		drumPad(st, i, vel);
		return;
	}
	if (s.ui.noteLayout == 2) {
		splitPad(st, i, vel);
		return;
	}
	if (s.ui.noteLayout) {
		sequencer(st, i, vel);
		return;
	}
	if (!vel)
		return;
	bool root, inScale;
	int p = padPitch(s.scale, s.track[s.ui.track].octave, i / 8, i % 8, root, inScale);
	if (p >= 0)
		play(s, i, p, vel);
}

/** Starts the note of pad `i` on the selected track. A drum track plays it on the voice channel of its lane. */
void Machine::play(Session& s, int i, int pitch, int vel) {
	Live& l = live_[i];
	l = {++nextId_, s.ui.track, (uint8_t)pitch, (uint8_t)vel};
	s.ui.pad[i] = (uint8_t)vel;
	const Ui& u = s.ui;
	int length = state_->clip[l.track][u.recScene].length;
	if (u.recTrack == l.track && s.track[l.track].playing == u.recScene && length && s.track[l.track].type != Euclidean)
		l.rec = u.recScene, l.at = atTicks_,
		l.pos = (uint16_t)(std::max<int64_t>(atTicks_ - s.track[l.track].startTick, 0) % length);
	int midichan = layout_[l.track] == Drum ? pitch - kDrumNote + 1 : 0;
	voice_[l.track].noteOn(l.id, (float)pitch, (float)vel / 127.f, midichan);
}

/** A pad of a drum track: the lower left 4 x 4 are the lanes (row * 4 + col, from the bottom left), which a press
 * selects and plays (with Select held it only selects, with Delete held it takes the lane's notes off the clip), the
 * four rows above are the steps of the selected lane, and the lower right 4 x 4 the loop selector. */
void Machine::drumPad(State& st, int i, int vel) {
	Session& s = st.s;
	if (i / 8 > 3) {
		sequencer(st, i, vel);
	} else if (i % 8 > 3) {
		loopPad(st, i, vel);
	} else if (vel) {
		int lane = i / 8 * 4 + i % 8;
		if (s.ui.button[push2::cc::Delete]) {
			checkpoint(s.ui.track, s.ui.scene);
			eraseNotes(st.clip[s.ui.track][s.ui.scene], 0, kMaxLength, kDrumNote + lane, kDrumNote + lane);
			return;
		}
		if (s.ui.button[push2::cc::Quantize]) {
			quantizeClip(st, kDrumNote + lane, kDrumNote + lane);
			return;
		}
		s.track[s.ui.track].lane = (uint8_t)lane;
		if (!s.ui.button[push2::cc::Select])
			play(s, i, kDrumNote + lane, vel);
	}
}

/** A Euclid edit begins: keeps the selected clip for Undo, and an empty slot first gets a clip of the Fixed Length
 * setting, launched as one a sequencer makes (at once when stopped, without starting the transport). Returns the
 * clip's settings. */
Euclid& Machine::euclidEdit(State& st) {
	Ui& ui = st.s.ui;
	Clip& c = st.clip[ui.track][ui.scene];
	checkpoint(ui.track, ui.scene);
	if (!c.length)
		c.length = (uint16_t)(kPpqn << ui.fixedBars), launch(st.s, ui.track, ui.scene);
	return c.euclid;
}

/** A pad of a Euclid track in Note mode: Drum mode's lane pads (lower left 4 x 4) play their channel and select the
 * lane (SELECT: only select; DELETE: the lane's settings go back to the standard ones). The step pads show the pattern
 * and the rest does nothing, as Melodic mode's keyboard until E5.3. */
void Machine::euclidPad(State& st, int i, int vel) {
	Session& s = st.s;
	Ui& ui = s.ui;
	Clip& c = st.clip[ui.track][ui.scene];
	if (!vel || i / 8 > 3 || i % 8 > 3 || c.euclid.mode != kEuclidDrum)
		return;
	const int lane = i / 8 * 4 + i % 8;
	if (ui.button[push2::cc::Delete]) {
		const EuclidLane standard;
		if (c.length && std::memcmp(&c.euclid.lane[lane], &standard, sizeof standard))
			checkpoint(ui.track, ui.scene), c.euclid.lane[lane] = standard;
		return;
	}
	s.track[ui.track].lane = (uint8_t)lane; // (pad() lets no pad through while QUANTIZE is held)
	if (!ui.button[push2::cc::Select])
		play(s, i, kDrumNote + lane, vel);
}

/** Encoder `encoder` turned on the Euclid screen: Drum mode edits the selected lane (encoders 1 to 5) and SEED, Melodic
 * mode the rhythm, SEED, OCTAVES and CHANGE. The value moves from the effective one (PULSES and ROTATE count against
 * STEPS), and a turn that changes nothing makes no clip and no undo step. */
void Machine::euclidEncoder(State& st, int encoder, int delta) {
	Ui& ui = st.s.ui;
	Euclid& e = st.clip[ui.track][ui.scene].euclid;
	const bool melodic = e.mode == kEuclidMelodic;
	if (ui.button[push2::cc::Delete] || (encoder > 5 && !melodic))
		return;
	const int lane = melodic ? kEuclidRhythm : st.s.track[ui.track].lane, n = laneSteps(e.lane[lane]);
	int v = euclidField(e, lane, encoder);
	if (encoder == 1)
		v = std::min(v, n);
	else if (encoder == 2)
		v %= n;
	static const int kLow[8] = {1, 0, 0, 0, 1, 0, 1, 0};
	const int high[8] = {kMaxSteps, n, n - 1, 100, 127, kMaxSeed, kMaxOctaves, 100};
	const int d = steps(accEuclid_[encoder], delta, kEuclidUnits[encoder] * (ui.button[push2::cc::Shift] ? 4 : 1));
	const int to = encoder == 2 ? ((v + d) % n + n) % n : std::min(std::max(v + d, kLow[encoder]), high[encoder]);
	if (to == v)
		return;
	euclidField(euclidEdit(st), lane, encoder, to);
}

/** DELETE + touch of encoder `encoder` on the Euclid screen: the field goes back to its standard value (one undo step
 * for the touch; an empty slot and a field that is standard stay as they are). */
void Machine::euclidReset(State& st, int encoder) {
	Ui& ui = st.s.ui;
	Clip& c = st.clip[ui.track][ui.scene];
	const bool melodic = c.euclid.mode == kEuclidMelodic;
	if (!c.length || (encoder > 5 && !melodic))
		return;
	Euclid standard;
	const int lane = melodic ? kEuclidRhythm : st.s.track[ui.track].lane, want = euclidField(standard, lane, encoder);
	if (euclidField(c.euclid, lane, encoder) != want)
		checkpoint(ui.track, ui.scene), euclidField(c.euclid, lane, encoder, want);
}

/** DRUM (upper button 1) or MELODIC (2) on the Euclid screen: the mode of the selected clip. */
void Machine::euclidModeButton(State& st, int mode) {
	if (st.clip[st.s.ui.track][st.s.ui.scene].euclid.mode != mode)
		euclidEdit(st).mode = (uint8_t)mode;
}

/** A scene button in Note mode on a Euclid track: the step size of the selected clip. */
void Machine::euclidGridButton(State& st, int grid) {
	if (st.clip[st.s.ui.track][st.s.ui.scene].euclid.grid != grid)
		euclidEdit(st).grid = (uint8_t)grid;
}

/** A loop pad covers loopTicks of the clip (a page of steps, up to a bar). A press inside the clip views its page, one
 * outside makes the clip end with it (an empty slot gets a clip up to there, in whole bars, and launches it), holding
 * one pad and pressing another ends the clip with the later one, and with Delete held it takes the pad's notes off. */
void Machine::loopPad(State& st, int i, int vel) {
	Session& s = st.s;
	Ui& ui = s.ui;
	ui.pad[i] = (uint8_t)vel;
	Clip& c = st.clip[ui.track][ui.scene];
	int grid = kGrid[s.track[ui.track].grid], len = loopTicks(grid), p = loopIndex(s, i);
	if (!vel || p * len >= kMaxLength)
		return;
	int end = (p + 1) * len, held = -1; // the later of the other pads that are held
	for (int j = 0; j < 32; j++)
		if (j / 8 < 4 && j % 8 > 3 && j != i && ui.pad[j])
			held = std::max(held, loopIndex(s, j));
	if (ui.button[push2::cc::Delete]) {
		checkpoint(ui.track, ui.scene);
		eraseNotes(c, p * len, end, 0, 127);
	} else if (held >= 0 && c.length) {
		setLength(st, (std::max(p, held) + 1) * len);
	} else if (p * len < c.length) {
		ui.page = (uint8_t)(p * len / (kDrumSteps * grid)), ui.heldStep = -1;
	} else if (c.length) {
		setLength(st, end);
	} else {
		checkpoint(ui.track, ui.scene);
		c.length = (uint16_t)std::min((end + kBar - 1) / kBar * kBar, kMaxLength);
		launch(s, ui.track, ui.scene);
	}
	ui.page = (uint8_t)std::min<int>(ui.page, lastPage(c, grid, kDrumSteps));
}

/** Starts the clip of `scene` on `track` at the launch quantization, at once when the transport is stopped. */
void Machine::launch(Session& s, int track, int scene) {
	if (s.ui.recTrack == track)
		endTake();
	Track& tr = s.track[track];
	if (!clock_.started()) {
		tr.playing = (int8_t)scene, tr.queued = kNone, tr.startTick = 0, tr.overridden = 0;
		return;
	}
	tr.queued = (int8_t)scene;
	tr.launchTick = launchAt(s.transport);
}

/** Stops the track's clip at the launch quantization (at once when `now` or the transport is stopped) and cancels a
 * launch that is waiting. */
void Machine::stopTrack(Session& s, int track, bool now) {
	if (s.ui.recTrack == track)
		endTake();
	Track& tr = s.track[track];
	if (!now && clock_.started()) {
		if (tr.playing != kNone || tr.queued != kNone)
			tr.queued = kStopQueued, tr.launchTick = launchAt(s.transport);
		return;
	}
	for (int i = 0; i < nActive_[track]; i++)
		voice_[track].noteOff(active_[track][i].id);
	nActive_[track] = 0;
	tr.playing = tr.queued = kNone;
}

/** The transport stopped: every clip stops (as in Live) and its notes end. Pads you hold go on sounding. */
void Machine::stopClips(Session& s) {
	for (int t = 0; t < kTracks; t++)
		stopTrack(s, t, true);
}

/** A slot is empty: a clip that was playing there stops at once, a launch that was waiting is dropped. */
void Machine::vacate(State& st, int track, int scene) {
	Session& s = st.s;
	if (s.track[track].playing == scene)
		stopTrack(s, track, true);
	if (s.track[track].queued == scene)
		s.track[track].queued = kNone;
	if (track == s.ui.track && scene == s.ui.scene)
		s.ui.heldStep = -1;
}

/** Empties a slot. */
void Machine::clear(State& st, int track, int scene) {
	if (st.s.ui.recTrack == track && st.s.ui.recScene == scene)
		endTake(true);
	if (st.clip[track][scene].length)
		checkpoint(track, scene);
	vacate(st, track, scene);
	std::memset(static_cast<void*>(&st.clip[track][scene]), 0, sizeof(Clip)); // all zero is the empty clip...
	st.clip[track][scene].euclid = Euclid();                                  // ...with the Euclid defaults
}

/** Copies a slot (an empty one empties the destination). */
void Machine::copy(State& st, int track, int scene, int toTrack, int toScene) {
	if (st.clip[track][scene].length) {
		if (st.s.ui.recTrack == toTrack && st.s.ui.recScene == toScene)
			endTake(true);
		checkpoint(toTrack, toScene);
		st.clip[toTrack][toScene] = st.clip[track][scene];
	} else
		clear(st, toTrack, toScene);
}

/** A Session pad: launches its clip (an empty slot stops the track); Select, Delete and Duplicate make it select,
 * delete or copy instead. */
void Machine::session(State& st, int i) {
	Session& s = st.s;
	Ui& ui = s.ui;
	int t = i % 8, sc = 7 - i / 8;
	bool full = st.clip[t][sc].length != 0;
	if (ui.button[push2::cc::Select]) {
		select(ui, t, sc);
	} else if (ui.button[push2::cc::Delete]) {
		clear(st, t, sc);
	} else if (ui.button[push2::cc::Duplicate]) { // the first slot is the source, every one after it a destination
		if (ui.dup < 0 && full)
			ui.dup = (int8_t)(t * 8 + sc);
		else if (ui.dup >= 0 && ui.dup != t * 8 + sc)
			copy(st, ui.dup / 8, ui.dup % 8, t, sc);
		if (ui.dup >= 0)
			select(ui, t, sc);
	} else {
		select(ui, t, sc);
		if (full) {
			launch(s, t, sc);
			if (!clock_.started())
				request_ = Request::Play;
		} else {
			stopTrack(s, t, false);
		}
	}
}

/** A Session scene button: launches the row (the tracks with an empty slot stop); Select, Delete and Duplicate make it
 * select the scene, empty the row or copy it into the next scene. */
void Machine::sceneButton(State& st, int scene) {
	Session& s = st.s;
	if (s.ui.button[push2::cc::Select]) {
		select(s.ui, s.ui.track, scene);
		return;
	}
	bool launched = false;
	for (int t = 0; t < kTracks; t++) {
		if (s.ui.button[push2::cc::Delete]) {
			clear(st, t, scene);
		} else if (s.ui.button[push2::cc::Duplicate]) {
			if (scene + 1 < kScenes)
				copy(st, t, scene, t, scene + 1);
		} else if (st.clip[t][scene].length) {
			launch(s, t, scene);
			launched = true;
		} else {
			stopTrack(s, t, false);
		}
	}
	if (!s.ui.button[push2::cc::Delete] && !s.ui.button[push2::cc::Duplicate]) {
		select(s.ui, s.ui.track, scene);
		if (launched && !clock_.started())
			request_ = Request::Play;
	}
}

/** A sequencer pad: tap a step to put a note on it (the first one makes the clip) or to take it off, hold it and turn
 * an encoder to edit it, or hold it and press a later step of its row (a drum lane: of the page) for a long note. */
void Machine::sequencer(State& st, int i, int vel) {
	Session& s = st.s;
	Ui& ui = s.ui;
	Clip& c = st.clip[ui.track][ui.scene];
	int grid = kGrid[s.track[ui.track].grid], start;
	int pitch = stepOf(s, i, start);
	if (!vel) { // a tap on a note that was there takes it off, unless the hold edited it
		if (i == ui.heldStep) {
			int n = findNote(c, pitch, start, grid);
			if (ui.heldHad && !ui.heldEdited && n >= 0)
				checkpoint(ui.track, ui.scene), eraseNote(c, n);
			ui.heldStep = -1;
		}
		return;
	}
	bool created = !c.length;                                 // the first note makes the clip and launches it
	int length = c.length ? c.length : kPpqn << ui.fixedBars; // the Fixed Length setting, on or off
	if (pitch < 0 || start >= length)
		return;
	int n = findNote(c, pitch, start, grid);
	if (ui.button[push2::cc::Delete]) {
		if (n >= 0)
			checkpoint(ui.track, ui.scene), eraseNote(c, n);
	} else if (ui.heldStep >= 0) { // ponytail: one held step; a press on a later step of its row stretches its note
		int heldStart, heldPitch = stepOf(s, ui.heldStep, heldStart), h = findNote(c, heldPitch, heldStart, grid);
		if (pitch == heldPitch && start > heldStart && h >= 0) {
			checkpoint(ui.track, ui.scene);
			c.notes[h].len = (uint16_t)(std::min(start + grid, length) - c.notes[h].tick), ui.heldEdited = true;
		}
	} else if (n >= 0) {
		ui.heldStep = (int8_t)i, ui.heldHad = true, ui.heldEdited = false;
	} else {
		if (c.nNotes >= kMaxNotes) {
			ui.popup = kClipFullPopup, ui.popupAt = ui.now;
			return;
		}
		checkpoint(ui.track, ui.scene);
		Note note = {(uint16_t)start, (uint16_t)std::min(grid, length - start), (uint8_t)pitch, 100};
		insertNote(c, note); // there is room, and findNote found no note there
		ui.heldStep = (int8_t)i, ui.heldHad = ui.heldEdited = false;
		c.length = (uint16_t)length;
		if (created)
			launch(s, ui.track, ui.scene);
	}
}

/** A pad of the split layout (Sequencer + 32 Notes). The lower 4 x 4 play like Note mode; pads held together there are
 * the chord (a lone press replaces it), and with a step held a press toggles its pitch in that step. The four rows
 * above are 32 steps: a tap on an empty one adds the chord's notes, a tap on one with notes takes them all off, hold
 * one and turn an encoder to edit all its notes, or hold it and press a later step for long notes. */
void Machine::splitPad(State& st, int i, int vel) {
	Session& s = st.s;
	Ui& ui = s.ui;
	Clip& c = st.clip[ui.track][ui.scene];
	int grid = kGrid[s.track[ui.track].grid], start;
	stepOf(s, i, start);
	if (i / 8 < 4) {
		if (!vel)
			return;
		bool root, inScale;
		int p = padPitch(s.scale, s.track[ui.track].octave, i / 8, i % 8, root, inScale);
		if (p < 0)
			return;
		if (ui.heldStep >= 0) {
			int hs, n;
			stepOf(s, ui.heldStep, hs);
			n = findNote(c, p, hs, grid);
			if (n >= 0) {
				checkpoint(ui.track, ui.scene), eraseNote(c, n);
			} else if (c.nNotes >= kMaxNotes) {
				ui.popup = kClipFullPopup, ui.popupAt = ui.now;
			} else {
				checkpoint(ui.track, ui.scene);
				insertNote(c, {(uint16_t)hs, (uint16_t)std::min(grid, c.length - hs), (uint8_t)p, 100});
			}
			ui.heldEdited = true;
		} else {
			bool alone = true;
			for (int j = 0; j < 32; j++)
				alone &= j == i || !ui.pad[j];
			if (alone)
				ui.chord[0] = ui.chord[1] = 0;
			ui.chord[p >> 6] |= 1ull << (p & 63);
		}
		play(s, i, p, vel);
		return;
	}
	int t0 = start - grid / 2, t1 = start + grid / 2;
	if (!vel) { // a tap on a step that had notes takes them off, unless the hold edited it
		if (i == ui.heldStep) {
			if (ui.heldHad && !ui.heldEdited)
				checkpoint(ui.track, ui.scene), eraseNotes(c, t0, t1, 0, 127);
			ui.heldStep = -1;
		}
		return;
	}
	bool created = !c.length;
	int length = c.length ? c.length : kPpqn << ui.fixedBars;
	if (start >= length)
		return;
	bool has = stepNotes(c, start, grid, nullptr, 0) > 0;
	if (ui.button[push2::cc::Delete]) {
		if (has)
			checkpoint(ui.track, ui.scene), eraseNotes(c, t0, t1, 0, 127);
	} else if (ui.heldStep >= 0) { // a press on a later step stretches every note of the held one
		int hs;
		stepOf(s, ui.heldStep, hs);
		if (start > hs && stepNotes(c, hs, grid, nullptr, 0)) {
			checkpoint(ui.track, ui.scene);
			for (int k = 0; k < c.nNotes; k++)
				if (c.notes[k].tick >= hs - grid / 2 && c.notes[k].tick < hs + grid / 2)
					c.notes[k].len = (uint16_t)(std::min(start + grid, length) - c.notes[k].tick);
			ui.heldEdited = true;
		}
	} else if (has) {
		ui.heldStep = (int8_t)i, ui.heldHad = true, ui.heldEdited = false;
	} else {
		int count = 0;
		for (int p = 0; p < 128; p++)
			count += chordHas(s, p);
		if (!count)
			return;
		if (c.nNotes + count > kMaxNotes) {
			ui.popup = kClipFullPopup, ui.popupAt = ui.now;
			return;
		}
		checkpoint(ui.track, ui.scene);
		for (int p = 0; p < 128; p++)
			if (chordHas(s, p))
				insertNote(c, {(uint16_t)start, (uint16_t)std::min(grid, length - start), (uint8_t)p, 100});
		ui.heldStep = (int8_t)i, ui.heldHad = ui.heldEdited = false;
		c.length = (uint16_t)length;
		if (created)
			launch(s, ui.track, ui.scene);
	}
}

/** An encoder turned while a step is held: 0 Nudge (ticks, within half a step), 1 Length (steps), 2 Length fine
 * (ticks), 3 Velocity. */
void Machine::noteEncoder(State& st, int encoder, int delta) {
	Ui& ui = st.s.ui;
	Clip& c = st.clip[ui.track][ui.scene];
	int grid = kGrid[st.s.track[ui.track].grid], start;
	int pitch = stepOf(st.s, ui.heldStep, start);
	// The notes to edit: the one on the step, or in the split layout every one (ponytail: at most 16 at a time).
	Note sel[16];
	int cnt = 0, idx[16];
	if (st.s.track[ui.track].type != Drum && ui.noteLayout == 2) {
		cnt = std::min(stepNotes(c, start, grid, idx, 16), 16);
		for (int k = 0; k < cnt; k++)
			sel[k] = c.notes[idx[k]];
	} else if (int h = findNote(c, pitch, start, grid); h >= 0) {
		sel[cnt++] = c.notes[h];
	}
	int n = steps(accNote_[encoder], delta, kNoteUnits[encoder]);
	if (!cnt || !n)
		return;
	checkpoint(ui.track, ui.scene);
	ui.heldEdited = true;
	for (int k = 0; k < cnt; k++) {
		const Note note = sel[k];
		int h = (int)(std::lower_bound(c.notes, c.notes + c.nNotes, note, before) - c.notes);
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
}

/** An encoder turned on the Device or Mix screen: `lane` of `track` moves (DELETE held: nothing, the touch cleared it);
 * with a step held, a step lock on the selected clip instead. While Automate is on, the take's clip plays on this
 * track and the encoder is touched, the lane is written from the first turn until the touch ends (see write).
 * Otherwise a lane that the track's playing clip has points for is overridden until the clip launches again. */
void Machine::valueEncoder(State& st, int encoder, int track, int lane, int delta) {
	Ui& ui = st.s.ui;
	if (ui.button[push2::cc::Delete])
		return;
	Track& tr = st.s.track[track];
	int d = delta * kValueUnits / (ui.button[push2::cc::Shift] ? kFineDivisor : 1);
	if (ui.heldStep >= 0) {
		stepLock(st, lane, d);
		return;
	}
	const uint16_t was = tr.value[lane];
	tr.value[lane] = (uint16_t)std::min(std::max(tr.value[lane] + d, 0), 65535);
	if (ui.automate && ui.recTrack == track && tr.playing == ui.recScene && ui.touched >> encoder & 1) {
		if (writeBy_[encoder] >= 0 && writeBy_[encoder] != lane) // the screen changed under a touch: that lane ends
			endWrite(st, writeBy_[encoder]);
		if (!(ui.writing >> lane & 1))
			door_[lane].on = false, door_[lane].pre = was;
		ui.writing |= (uint16_t)(1 << lane), tr.overridden &= (uint16_t)~(1 << lane), writeBy_[encoder] = (int8_t)lane;
	} else if (tr.playing != kNone && st.clip[track][tr.playing].nPoints[lane]) {
		tr.overridden |= (uint16_t)(1 << lane);
	}
}

/** The swinging-door writer: the take's clip takes the value of `lane` at tick `pos` of its loop; called every tick
 * for the lanes in Ui::writing. A pass begins at its first tick (and again at tick 0 after a wrap): the old curve
 * arrives at `pos`, then jumps to the value. Each tick the old points the head has passed are taken off, and a point
 * is kept only when the line from the last one can no longer stay within kDoorTolerance of the samples since, so a slow
 * sweep takes few. endWrite ends the pass. The stretch between punch-in and punch-out is replaced on every pass. A lane
 * that is within 3 points of its limit stops writing ("Clip full"): the two points that endWrite adds always fit. */
void Machine::write(State& st, int lane, int pos) {
	Ui& u = st.s.ui;
	Clip& c = st.clip[u.recTrack][u.recScene];
	Door& d = door_[lane];
	const int v = st.s.track[u.recTrack].value[lane];
	auto full = [&](int extra) {
		if (c.nPoints[lane] + extra + 2 <= kMaxPoints)
			return false;
		endWrite(st, lane);
		u.popup = kClipFullPopup, u.popupAt = u.now;
		return true;
	};
	if (d.on && pos <= d.prev)
		endWrite(st, lane, false); // the loop wrapped: this pass is over, the next begins here
	if (!d.on) {
		int arriving = envelope(c, lane, pos, true);
		Point add[2];
		int n = 0;
		if (full(2))
			return;
		d.old = arriving >= 0;
		d.oldLeft = {(uint16_t)pos, (uint16_t)std::max(arriving, 0)};
		if (const Point* l = lastIn(c, lane, pos, pos))
			d.oldLeft = *l;
		if (arriving < 0)
			arriving = d.pre;
		if (pos > 0 && arriving != v)
			add[n++] = {(uint16_t)pos, (uint16_t)arriving};
		add[n++] = {(uint16_t)pos, (uint16_t)v};
		replacePoints(c, lane, pos, pos, add, n);
		d.on = true, d.prev = d.t0 = pos, d.v0 = d.vl = (uint16_t)v, d.lo = -1e30f, d.hi = 1e30f;
		return;
	}
	float lo = std::max(d.lo, (float)(v - kDoorTolerance - d.v0) / (float)(pos - d.t0)),
		  hi = std::min(d.hi, (float)(v + kDoorTolerance - d.v0) / (float)(pos - d.t0));
	if (lo > hi) { // the door is shut: the last sample is a point, and the next line starts from it
		if (full(1))
			return;
		const Point p = {(uint16_t)d.prev, d.vl};
		replacePoints(c, lane, d.prev + 1, d.prev, &p, 1);
		d.t0 = d.prev, d.v0 = d.vl;
		lo = (float)(v - kDoorTolerance - d.v0) / (float)(pos - d.t0);
		hi = (float)(v + kDoorTolerance - d.v0) / (float)(pos - d.t0);
	}
	if (const Point* l = lastIn(c, lane, d.prev + 1, pos)) // the head passes the old points: they go
		d.oldLeft = *l;
	replacePoints(c, lane, d.prev + 1, pos, nullptr, 0);
	d.lo = lo, d.hi = hi, d.prev = pos, d.vl = (uint16_t)v;
}

/** Ends the pass of `lane`: the last value becomes a point, then the old curve comes back at once (a jump to what it
 * was at that tick, unless the clip ends there or the lane had no curve). `stop`: the lane is no longer written (else
 * the touch goes on and the next pass follows). */
void Machine::endWrite(State& st, int lane, bool stop) {
	Ui& u = st.s.ui;
	Door& d = door_[lane];
	if (d.on && u.writing >> lane & 1) { // (a patch that was loaded mid-pass leaves `on` behind)
		Clip& c = st.clip[u.recTrack][u.recScene];
		Point add[2];
		int n = 0;
		if (d.prev > d.t0)
			add[n++] = {(uint16_t)d.prev, d.vl};
		if (d.old && d.prev + 1 < c.length) {
			const Point *p = c.points[lane], *end = p + c.nPoints[lane];
			const Point* r = std::upper_bound(p, end, d.prev, [](int t, const Point& a) { return t < a.tick; });
			int was = d.oldLeft.value;
			if (r != end)
				was += (int)((int64_t)(r->value - was) * (d.prev - d.oldLeft.tick) / (r->tick - d.oldLeft.tick));
			if (was != d.vl)
				add[n++] = {(uint16_t)d.prev, (uint16_t)was};
		}
		replacePoints(c, lane, d.prev + 1, d.prev, add, n);
	}
	d.on = false;
	if (stop)
		u.writing &= (uint16_t)~(1 << lane);
}

/** A step lock: the held step of the selected clip gets a flat segment of the lane over its length, at the envelope in
 * its middle (the lane's value when it has no points) moved by `delta`. The points are (start, line before) (start, v)
 * (end, v) (end, line after), the line being what it was on either side, without those that change nothing. A step
 * next to a lock shares its edge. One undo step per hold. */
void Machine::stepLock(State& st, int lane, int delta) {
	Ui& ui = st.s.ui;
	Clip& c = st.clip[ui.track][ui.scene];
	int start, end;
	ui.heldEdited = true; // a turn is an edit, even when it changes nothing: the release keeps the note
	stepSpan(st.s, c, start, end);
	int value = st.s.track[ui.track].value[lane];
	int now = envelope(c, lane, (start + end) / 2);
	now = now < 0 ? value : now;
	int v = std::min(std::max(now + delta, 0), 65535);
	if (v == now)
		return;
	int before = envelope(c, lane, start, true), after = envelope(c, lane, end);
	if (before < 0)
		before = after = value;
	Point add[4];
	int n = 0;
	if (before != v)
		add[n++] = {(uint16_t)start, (uint16_t)before};
	add[n++] = {(uint16_t)start, (uint16_t)v};
	add[n++] = {(uint16_t)end, (uint16_t)v};
	if (after != v)
		add[n++] = {(uint16_t)end, (uint16_t)after};
	if (!replacePoints(c, lane, start, end, add, n, true)) {
		ui.popup = kClipFullPopup, ui.popupAt = ui.now;
		return;
	}
	checkpoint(ui.track, ui.scene);
	replacePoints(c, lane, start, end, add, n);
}

/** Delete + touch of an encoder on the Device or Mix screen: the points of the lane in the track's clip of the selected
 * scene go, one undo step; with none, the lane's value goes back to its default. */
void Machine::deleteLane(State& st, int track, int lane) {
	Ui& ui = st.s.ui;
	Clip& c = st.clip[track][ui.scene];
	Track& tr = st.s.track[track];
	if (c.nPoints[lane]) {
		gesture_++; // the touch is a gesture of its own
		checkpoint(track, ui.scene);
		c.nPoints[lane] = 0;
	} else {
		tr.value[lane] = defaultValue(ui, tr, lane);
	}
}

/** The Session's LEDs: the pads in the track's color, pulsing on the clip that plays, blinking green on one that is
 * queued (the playing clip blinks to dark when a stop is queued); the scene buttons green where the scene has clips,
 * blinking where one is queued. */
static void sessionLeds(const State& st, push2::Leds& out) {
	const Session& s = st.s;
	for (int sc = 0; sc < kScenes; sc++) {
		bool clips = false, queued = false;
		for (int t = 0; t < kTracks; t++) {
			const Track& tr = s.track[t];
			uint8_t full = (uint8_t)(kPaletteFull + t);
			push2::Led led = {push2::kBlack, push2::kStatic};
			if (st.clip[t][sc].length) {
				led = {full, push2::kStatic};
				if (tr.playing == sc)
					led = tr.queued == kStopQueued ? push2::Led{push2::kBlack, push2::kBlinkQuarter, full}
												   : push2::Led{(uint8_t)(kPaletteDim + t), push2::kPulseHalf, full};
				if (tr.queued == sc)
					led = {push2::kGreen, push2::kBlinkQuarter, full};
				if (s.ui.recTrack == t && s.ui.recScene == sc) // the take: red
					led = tr.playing == sc ? push2::Led{push2::kRed, push2::kPulseHalf, full}
										   : push2::Led{push2::kBlack, push2::kBlinkQuarter, push2::kRed};
			}
			out.note[push2::padNote(7 - sc, t)] = led;
			clips |= st.clip[t][sc].length != 0, queued |= tr.queued == sc;
		}
		out.cc[push2::cc::Scene1 - sc] = queued ? push2::Led{push2::kBlack, push2::kBlinkQuarter, push2::kGreen}
												: push2::Led{clips ? push2::kGreen : push2::kBlack, push2::kStatic};
	}
}

/** The eight scene buttons as step sizes: `grid` white, the others dark gray. */
static void gridLeds(int grid, push2::Leds& out) {
	for (int i = 0; i < 8; i++)
		out.cc[push2::cc::Scene1 - i] = {i == grid ? push2::kRgbWhite : push2::kRgbDarkGray, push2::kStatic};
}

/** The page buttons (lit where there is a page to go to) and the step sizes on the scene buttons, for both sequencers.
 */
static void pageLeds(const State& st, push2::Leds& out) {
	const Ui& u = st.s.ui;
	const Track& tr = st.s.track[u.track];
	int last = lastPage(st.clip[u.track][u.scene], kGrid[tr.grid], pageSteps(tr, u));
	auto white = [&](int b, bool full) { out.cc[b] = {full ? push2::kWhiteFull : push2::kWhiteDark, push2::kStatic}; };
	if (u.page > 0)
		white(push2::cc::PageLeft, u.button[push2::cc::PageLeft]);
	if (u.page < last)
		white(push2::cc::PageRight, u.button[push2::cc::PageRight]);
	gridLeds(tr.grid, out);
}

/** The sequencer's LEDs: notes in the track's color (dim for soft ones), the empty steps dark gray (white in a root
 * row), nothing past the end of the clip, the playhead green (white on a note), the held step white; page, step size
 * and Delete buttons. */
static void seqLeds(const State& st, push2::Leds& out) {
	const Session& s = st.s;
	const Ui& u = s.ui;
	const Track& tr = s.track[u.track];
	const Clip& c = st.clip[u.track][u.scene];
	int grid = kGrid[tr.grid];
	pageLeds(st, out);

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
				color = step == head   ? hot(u, true)
						: root[row]    ? push2::kRgbWhite
						: inScale[row] ? push2::kRgbDarkGray
									   : push2::kBlack;
			}
			out.note[push2::padNote(row, col)] = {color, push2::kStatic};
		}
}

/** The 32 steps of the viewed page (the top four rows) for the drum lane and the split layout: the playhead green, a
 * held one white, a note in the track's colour (dim for soft ones; `vel` is the velocity on each step, 0: none), an
 * empty one dark gray, nothing past the end of the clip. Returns where the clip is (-1: not playing). */
static int stepLeds(const State& st, const uint8_t* vel, push2::Leds& out) {
	const Session& s = st.s;
	const Ui& u = s.ui;
	const Track& tr = s.track[u.track];
	const Clip& c = st.clip[u.track][u.scene];
	int grid = kGrid[tr.grid];
	int pos = -1; // where the clip is
	if (s.transport.playing && tr.playing == u.scene && c.length)
		pos = (int)(std::max<int64_t>(s.transport.tick - tr.startTick, 0) % c.length);
	for (int k = 0; k < kDrumSteps; k++) {
		int step = u.page * kDrumSteps + k, pad = (7 - k / 8) * 8 + k % 8;
		uint8_t color = push2::kRgbDarkGray;
		if (c.length && step * grid >= c.length)
			color = push2::kBlack;
		else if (pad == u.heldStep)
			color = push2::kRgbWhite;
		else if (pos >= 0 && pos / grid == step)
			color = hot(u, true);
		else if (vel[k])
			color = vel[k] < 64 ? (uint8_t)(kPaletteDim + u.track) : (uint8_t)(kPaletteFull + u.track);
		out.note[push2::padNote(pad / 8, pad % 8)] = {color, push2::kStatic};
	}
	return pos;
}

/** The 16 lane pads (lower left 4 x 4) of a drum track and of a Euclid track in Drum mode: the selected lane white, a
 * sounding one green, else the track's colour, dim when `has[lane]` is false. */
static void laneLeds(const State& st, const filo::VoiceAllocator& voice, const bool* has, push2::Leds& out) {
	const Ui& u = st.s.ui;
	const Track& tr = st.s.track[u.track];
	for (int lane = 0; lane < kLanes; lane++)
		out.note[push2::padNote(lane / 4, lane % 4)] = {voice.gate(lane)  ? hot(u)
														: lane == tr.lane ? push2::kRgbWhite
														: has[lane]       ? (uint8_t)(kPaletteFull + u.track)
																		  : (uint8_t)(kPaletteDim + u.track),
														push2::kStatic};
}

/** The 32 step pads of a Euclid track: the pattern of `lane` of the selected clip in its own time (pad k is step k, the
 * top row steps 1 to 8): a pulse in the track's colour (dim below VEL 64), a rest dark gray, black from STEPS on, and
 * the lane's own current step since the launch green (red in a take), whatever the clip's length. */
static void euclidStepLeds(const State& st, int lane, push2::Leds& out) {
	const Session& s = st.s;
	const Ui& u = s.ui;
	const Track& tr = s.track[u.track];
	const Clip& c = st.clip[u.track][u.scene];
	const EuclidLane& x = c.euclid.lane[lane];
	const int n = laneSteps(x), grid = kGrid[std::min<int>(c.euclid.grid, 7)];
	const uint32_t mask = euclidMask(x);
	const int head = s.transport.playing && tr.playing == u.scene && c.length
						 ? (int)(std::max<int64_t>(s.transport.tick - tr.startTick, 0) / grid % n)
						 : -1;
	for (int k = 0; k < kMaxSteps; k++)
		out.note[push2::padNote(7 - k / 8, k % 8)] = {k >= n             ? push2::kBlack
													  : k == head        ? hot(u, true)
													  : !(mask >> k & 1) ? push2::kRgbDarkGray
													  : x.velocity < 64  ? (uint8_t)(kPaletteDim + u.track)
																		 : (uint8_t)(kPaletteFull + u.track),
													  push2::kStatic};
}

/** A drum track's LEDs. Lanes: the selected one white, a sounding one green, else the track's colour, dim when the
 * clip has no note on it. Steps of the selected lane: the playhead green, a held one white, a note in the track's
 * colour (dim for soft ones), an empty one dark gray, nothing past the end of the clip. Loop pads: the playing one
 * green, those of the viewed page white, the rest of the clip light gray, black past its end. */
static void drumLeds(const State& st, const filo::VoiceAllocator& voice, push2::Leds& out) {
	const Session& s = st.s;
	const Ui& u = s.ui;
	const Track& tr = s.track[u.track];
	const Clip& c = st.clip[u.track][u.scene];
	int grid = kGrid[tr.grid], len = loopTicks(grid);
	pageLeds(st, out);
	bool has[kLanes] = {};
	uint8_t vel[kDrumSteps] = {}; // the velocity of the selected lane's note on each step of the page, 0: none
	for (int i = 0; i < c.nNotes; i++) {
		const Note& n = c.notes[i];
		if (n.pitch >= kDrumNote && n.pitch < kDrumNote + kLanes)
			has[n.pitch - kDrumNote] = true;
		int step = (n.tick + grid / 2) / grid - u.page * kDrumSteps;
		if (n.pitch == kDrumNote + tr.lane && step >= 0 && step < kDrumSteps)
			vel[step] = n.vel;
	}
	laneLeds(st, voice, has, out);
	int pos = stepLeds(st, vel, out);
	int page = kDrumSteps * grid;
	for (int row = 0; row < 4; row++)
		for (int col = 4; col < 8; col++) {
			int p = loopIndex(s, row * 8 + col);
			bool inside = p * len < c.length, viewed = p * len < (u.page + 1) * page && (p + 1) * len > u.page * page;
			out.note[push2::padNote(row, col)] = {pos >= 0 && pos / len == p ? hot(u, true)
												  : inside && viewed         ? push2::kRgbWhite
												  : inside                   ? push2::kRgbLightGray
																			 : push2::kBlack,
												  push2::kStatic};
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
	// Metronome: solid white off, pulsing between white and dim as fast as the Push goes on
	out.cc[Metronome] = s.transport.metronome ? push2::Led{push2::kWhiteDark, push2::kPulse24th, push2::kWhiteFull}
											  : push2::Led{push2::kWhiteFull, push2::kStatic};
	white(TapTempo, u.button[TapTempo]);
	white(Shift, u.button[Shift]);
	white(push2::cc::Note, u.pads == kNote);
	white(push2::cc::Session, u.pads == kSession);
	for (int b : {push2::cc::Select, push2::cc::Delete, push2::cc::Duplicate, push2::cc::New, push2::cc::StopClip})
		white(b, u.button[b]);
	white(push2::cc::Scale, u.screen == kScaleScreen || u.button[push2::cc::Scale]);
	white(push2::cc::Clip, u.screen == kClipScreen || u.button[push2::cc::Clip]);
	white(push2::cc::Device, u.screen == kDeviceScreen || u.button[push2::cc::Device]);
	white(push2::cc::Mix, u.screen == kMixScreen || u.button[push2::cc::Mix]);
	// MUTE and SOLO: the mode locked pulses, a held button or a muted (soloed) selected track is lit, else dim.
	for (int mode = 1; mode <= 2; mode++) {
		int b = mode == 1 ? push2::cc::Mute : push2::cc::Solo;
		uint8_t color = mode == 1 ? kPaletteYellow : push2::kBlue;
		out.cc[b] = u.muteSoloLock == mode ? push2::Led{color, push2::kPulseHalf, push2::kBlack}
					: u.button[b] || (mode == 1 ? tr.mute : tr.solo) ? push2::Led{color, push2::kStatic}
																	 : push2::Led{push2::kWhiteDark, push2::kStatic};
	}
	if (u.screen == kMixScreen) // VOLUMES and PANS: the chosen one white, the other dark gray
		for (int i = 0; i < 2; i++)
			out.cc[push2::cc::Track1Above + i] = {i == u.mixPans ? push2::kRgbWhite : push2::kRgbDarkGray,
												  push2::kStatic};
	if (euclidHome(s)) // DRUM and MELODIC: the clip's mode white, the other dark gray
		for (int i = 0; i < 2; i++)
			out.cc[push2::cc::Track1Above + i] = {
				i == st.clip[u.track][u.scene].euclid.mode ? push2::kRgbWhite : push2::kRgbDarkGray, push2::kStatic};
	white(push2::cc::FixedLength, u.fixedOn || u.button[push2::cc::FixedLength]);
	if (u.button[push2::cc::Undo] || (u.button[Shift] ? undoTop_ != undoEnd_ : undoTop_ != undoBottom_))
		white(push2::cc::Undo, u.button[push2::cc::Undo]); // dim: there is something to undo (Shift: to redo)
	if (u.recTrack >= 0) {                                 // a take: red, blinking while it waits to start or to stop
		bool waiting = s.track[u.recTrack].playing != u.recScene || u.recEnd >= 0;
		out.cc[push2::cc::Record] = waiting ? push2::Led{push2::kBlack, push2::kBlinkHalf, push2::kRed}
											: push2::Led{push2::kRed, push2::kStatic};
	} else {
		white(push2::cc::Record, u.button[push2::cc::Record]);
	}
	out.cc[push2::cc::Automate] =
		u.automate ? push2::Led{push2::kRed, push2::kStatic}
				   : push2::Led{u.button[push2::cc::Automate] ? push2::kWhiteFull : push2::kWhiteDark, push2::kStatic};
	white(push2::cc::Quantize, u.button[push2::cc::Quantize]);
	if (st.clip[u.track][u.scene].length || u.button[push2::cc::DoubleLoop])
		white(push2::cc::DoubleLoop, u.button[push2::cc::DoubleLoop]);
	white(push2::cc::Browse, u.screen == kBrowseScreen || u.button[push2::cc::Browse]);
	if (u.screen == kBrowseScreen) { // LOAD and CLOSE; the other buttons stay dark
		out.cc[push2::cc::Track8Below] = out.cc[push2::cc::Track1Above] = {push2::kRgbWhite, push2::kStatic};
	} else if (u.screen == kScaleScreen) { // the roots (the selected one white), In Key (always on) and Fixed
		const Scale& sc = s.scale;
		auto option = [&](int c, bool on) {
			out.cc[c] = {on ? push2::kRgbWhite : push2::kRgbDarkGray, push2::kStatic};
		};
		for (int i = 0; i < 12; i++)
			option(i < 6 ? push2::cc::Track1Above + 1 + i : push2::cc::Track1Below - 5 + i, kRoots[i] == sc.root);
		option(push2::cc::Track1Below, true);
		option(push2::cc::Track8Below, sc.fixed);
	} else if (quantizeSettings(u)) { // Record Quantize on the fifth upper button
		out.cc[push2::cc::Track1Above + 4] = {u.recQuantOn ? push2::kRgbWhite : push2::kRgbDarkGray, push2::kStatic};
	} else if (u.button[push2::cc::FixedLength]) { // the lengths (the chosen one white) and the switch
		for (int i = 0; i < 8; i++)
			out.cc[push2::cc::Track1Below + i] = {i == u.fixedBars ? push2::kRgbWhite : push2::kRgbDarkGray,
												  push2::kStatic};
		out.cc[push2::cc::Track1Above] = {u.fixedOn ? push2::kRgbWhite : push2::kRgbDarkGray, push2::kStatic};
	} else if (u.button[push2::cc::StopClip]) { // red: pulsing where a clip plays, blinking where a stop is queued
		for (int t = 0; t < kTracks; t++) {
			const Track& k = s.track[t];
			if (k.queued == kStopQueued)
				out.cc[push2::cc::Track1Below + t] = {push2::kBlack, push2::kBlinkQuarter, push2::kRed};
			else if (k.playing != kNone)
				out.cc[push2::cc::Track1Below + t] = {push2::kBlack, push2::kPulseHalf, push2::kRed};
		}
	} else {
		for (int t = 0; t < kTracks; t++)
			out.cc[push2::cc::Track1Below + t] = {s.track[t].solo
													  ? push2::kBlue // as Live: selected white unless silent
												  : silent(s, t) ? push2::kRgbDarkGray
												  : t == u.track ? push2::kRgbWhite
																 : (uint8_t)(kPaletteFull + t),
												  push2::kStatic};
	}
	if (u.pads == kSession) {
		sessionLeds(st, out);
		return;
	}
	if (tr.type == Drum) { // no octave, no layout
		drumLeds(st, voice_[u.track], out);
		return;
	}
	if (tr.type ==
		Euclidean) { // the step size, the pattern of the lane, and in Drum mode the lanes; OCTAVE (until E5.3),
		const Euclid& e = st.clip[u.track][u.scene].euclid; // LAYOUT, PAGE and the other pads stay dark
		const bool drum = e.mode == kEuclidDrum;
		bool has[kLanes];
		for (int l = 0; l < kLanes; l++)
			has[l] = e.lane[l].pulses > 0;
		gridLeds(e.grid, out);
		euclidStepLeds(st, drum ? tr.lane : kEuclidRhythm, out);
		if (drum)
			laneLeds(st, voice_[u.track], has, out);
		return;
	}
	if (tr.octave > 0)
		white(push2::cc::OctaveDown, u.button[push2::cc::OctaveDown]);
	if (tr.octave < kMaxOctave)
		white(push2::cc::OctaveUp, u.button[push2::cc::OctaveUp]);
	white(push2::cc::Layout, u.noteLayout || u.button[push2::cc::Layout]);
	if (u.noteLayout == 1) {
		seqLeds(st, out);
		return;
	}
	bool held[128] = {}; // pitches sounding from the pads: lit on every pad with that pitch
	for (int i = 0; i < 64; i++)
		held[live_[i].pitch & 127] |= u.pad[i] != 0;
	// The split layout: the steps above, and the chord (a held step's notes while one is held) in dim colour below.
	bool mark[128] = {};
	if (u.noteLayout == 2) {
		const Clip& c = st.clip[u.track][u.scene];
		int grid = kGrid[tr.grid], heldAt = -1;
		uint8_t vel[kDrumSteps] = {};
		if (u.heldStep >= 0)
			heldAt = u.page * kDrumSteps + (7 - u.heldStep / 8) * 8 + u.heldStep % 8;
		for (int i = 0; i < c.nNotes; i++) {
			int step = (c.notes[i].tick + grid / 2) / grid;
			if (step - u.page * kDrumSteps >= 0 && step - u.page * kDrumSteps < kDrumSteps)
				vel[step - u.page * kDrumSteps] = std::max(vel[step - u.page * kDrumSteps], c.notes[i].vel);
			if (step == heldAt)
				mark[c.notes[i].pitch & 127] = true;
		}
		for (int p = 0; p < 128 && heldAt < 0; p++)
			mark[p] = chordHas(s, p);
		pageLeds(st, out);
		stepLeds(st, vel, out);
	}
	for (int row = 0; row < (u.noteLayout == 2 ? 4 : 8); row++)
		for (int col = 0; col < 8; col++) {
			bool root, inScale;
			int p = padPitch(s.scale, tr.octave, row, col, root, inScale);
			if (p < 0)
				continue; // above 127: dark and silent
			uint8_t c = held[p]   ? hot(u)
						: mark[p] ? (uint8_t)(kPaletteDim + u.track)
						: root    ? (uint8_t)(kPaletteFull + u.track)
						: inScale ? push2::kRgbWhite
								  : push2::kBlack;
			out.note[push2::padNote(row, col)] = {c, push2::kStatic};
		}
}

/** The voices of track `t` follow what it plays: 16 channels, one for each lane, for a drum track and for a Euclid
 * track whose clip (the playing one, else the one in the selected scene) is in Drum mode; else its Voices, rotating. A
 * change of type or of layout silences the track. */
void Machine::layout(const State& st, int t) {
	const Track& tr = st.s.track[t];
	const Euclid& e = st.clip[t][tr.playing != kNone ? tr.playing : st.s.ui.scene].euclid;
	const uint8_t want = tr.type == Drum || (tr.type == Euclidean && e.mode == kEuclidDrum) ? Drum : Melodic;
	if (tr.type == type_[t] && want == layout_[t])
		return;
	type_[t] = tr.type, layout_[t] = want;
	voice_[t].allOff(), nActive_[t] = 0;
	voice_[t].setMode(want == Drum ? filo::VoiceAllocator::MPE : filo::VoiceAllocator::Rotate);
	voice_[t].setChannels(want == Drum ? kLanes : tr.voices);
}

uint32_t euclidMask(const EuclidLane& l) {
	const int n = laneSteps(l), r = l.rotation % n;
	const uint32_t m = bjorklund(n, std::min<int>(l.pulses, n)), full = n >= 32 ? ~0u : (1u << n) - 1;
	return r ? ((m << r) | (m >> (n - r))) & full : m; // ROTATE moves the pulses later
}

int euclidNotes(const Session& s, int track, const Euclid& e, int64_t t0, int length, Note* out, int max) {
	uint32_t mask[kLanes + 1];
	euclidMasks(e, mask);
	const int grid = kGrid[std::min<int>(e.grid, 7)];
	int count = 0;
	// ponytail: a melodic note's degree is scanned for each note (up to kMutationScan draws); memoize per pulse and
	// pass if the frame time shows it.
	for (int64_t n = (t0 + grid - 1) / grid; n * grid < t0 + length && count < max; n++) {
		Note at[kLanes];
		for (int i = 0, c = euclidAt(s, track, e, mask, n, at); i < c && count < max; i++, count++)
			out[count] = {(uint16_t)(n * grid - t0), (uint16_t)grid, at[i].pitch, at[i].vel};
	}
	return count;
}

/** Tick `k` of a Euclid track: the notes of the step that starts on it (steps are counted from the launch, whatever the
 * clip's length), swung as the clip's notes are. */
void Machine::euclidTick(State& st, int t, int64_t k) {
	const Track& tr = st.s.track[t];
	const Euclid& e = st.clip[t][tr.playing].euclid;
	const int grid = kGrid[std::min<int>(e.grid, 7)], d = (st.s.transport.swing * 12 + 50) / 100;
	for (int pass = 0; pass <= (d > 0); pass++) {
		const int64_t rel = k - tr.startTick - (pass ? d : 0);
		if (rel < 0 || rel % grid || (d > 0 && (rel % 48 == 24) != (pass == 1)))
			continue;
		uint32_t mask[kLanes + 1];
		euclidMasks(e, mask);
		Note at[kLanes];
		for (int i = 0, c = euclidAt(st.s, t, e, mask, rel / grid, at); i < c; i++) {
			if (nActive_[t] == kActive)
				break;
			const uint32_t id = ++nextId_;
			voice_[t].noteOn(id, (float)at[i].pitch, (float)at[i].vel / 127.f,
							 layout_[t] == Drum ? at[i].pitch - kDrumNote + 1 : 0);
			active_[t][nActive_[t]++] = {id, k + grid};
		}
	}
}

/** Tick `k`: the launches that are due, the notes that end, then the notes that start in each track's clip. A note
 * that ends on the tick another starts is released first, so the voice retriggers. */
void Machine::tick(State& st, int64_t k, Outputs& out) {
	Session& s = st.s;
	Ui& u = s.ui;
	if (u.recOpen && u.recTrack >= 0 &&
		s.track[u.recTrack].playing == u.recScene) { // an open take ends, or hits 32 bars
		int64_t end = u.recEnd >= 0 ? u.recEnd : s.track[u.recTrack].startTick + kMaxLength;
		if (k >= end)
			u.recEnd = end, endTake();
	}
	for (int t = 0; t < kTracks; t++) {
		Track& tr = s.track[t];
		filo::VoiceAllocator& v = voice_[t];
		if (tr.queued != kNone && k >= tr.launchTick) {
			if (tr.queued == kStopQueued)
				stopTrack(s, t, true);
			else
				tr.playing = tr.queued, tr.startTick = k, tr.overridden = 0;
			tr.queued = kNone;
		}
		layout(st, t); // after a launch, before its first notes: the new clip may want other channels
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
		for (int j = 0; j < kAutos; j++) {               // a lane with points plays them, the others keep their value
			if (t == u.recTrack && u.writing >> j & 1) { // a lane that is being written plays its value
				write(st, j, pos);
				continue;
			}
			int e = envelope(c, j, pos);
			if (e >= 0 && !(tr.overridden >> j & 1))
				tr.value[j] = (uint16_t)e;
		}
		if (tr.type == Euclidean) {
			euclidTick(st, t, k);
			continue;
		}
		bool drum = tr.type == Drum;
		// Swing delays the notes exactly on an even sixteenth (tick 24 of 48) by up to 12 ticks, where they start late.
		int d = (s.transport.swing * 12 + 50) / 100;
		for (int pass = 0; pass <= (d > 0); pass++) {
			int at = pass ? ((pos - d) % c.length + c.length) % c.length : pos;
			Note key = {(uint16_t)at, 0, 0, 0};
			for (const Note* n = std::lower_bound(c.notes, c.notes + c.nNotes, key, before);
				 n != c.notes + c.nNotes && n->tick == at; n++) {
				if (d > 0 && (n->tick % 48 == 24) != (pass == 1))
					continue;
				int ch = drum ? n->pitch - kDrumNote + 1 : 0; // a drum lane plays on the voice channel of its number
				if (drum && (ch < 1 || ch > kLanes))
					continue; // outside the lanes: kept, silent

				if (nActive_[t] == kActive)
					break;
				uint32_t id = ++nextId_;
				v.noteOn(id, (float)n->pitch, (float)n->vel / 127.f, ch);
				active_[t][nActive_[t]++] = {id, k + n->len};
			}
		}
	}
	if (s.transport.metronome && k % kPpqn == 0)
		out.metro = true, out.metroV = k % kBar == 0 ? 10.f : 5.f;
}

/** RECORD: starts a take on the selected slot, or ends the one that is running. A slot without a clip gets one first:
 * of the Fixed Length when that is on, else an open one (kMaxLength until the take ends). The clip is launched when it
 * isn't playing (at once when the transport is stopped, which then starts), and Fixed Length or not, the take
 * overdubs every pass until RECORD. An open take ends at the next whole unit from its start (see takeEnd). */
void Machine::record(State& st) {
	Session& s = st.s;
	Ui& ui = s.ui;
	if (ui.recTrack >= 0) {
		const Track& tr = s.track[ui.recTrack];
		if (!ui.recOpen || tr.playing != ui.recScene)
			endTake();
		else if (ui.recEnd < 0)
			ui.recEnd = takeEnd(s, tr);
		return;
	}
	int t = ui.track, sc = ui.scene;
	Clip& c = st.clip[t][sc];
	checkpoint(t, sc); // the take is one step: no more for this slot until it ends
	bool fresh = !c.length;
	if (fresh)
		c.length = (uint16_t)(ui.fixedOn ? kPpqn << ui.fixedBars : kMaxLength);
	if (s.track[t].playing != sc && s.track[t].queued != sc) {
		launch(s, t, sc);
		if (!clock_.started())
			request_ = Request::Play;
	}
	ui.recTrack = (int8_t)t, ui.recScene = (int8_t)sc, ui.recFresh = fresh, ui.recOpen = fresh && !ui.fixedOn;
	ui.recEnd = -1;
}

/** Ends the take, whichever way: RECORD, NEW, Stop, a launch or a stop of its track, Delete or a copy over its slot,
 * Undo, or 32 bars. An open length becomes what the take has run (a take that never began leaves no clip behind), and
 * a launch that was waiting for it is dropped. A pad that is still held is recorded when it is let go, unless
 * `discard`. */
void Machine::endTake(bool discard) {
	State& st = *state_; // every caller runs under the lock on this state
	Ui& u = st.s.ui;
	if (u.recTrack < 0)
		return;
	Track& tr = st.s.track[u.recTrack];
	Clip& c = st.clip[u.recTrack][u.recScene];
	for (int j = 0; j < kAutos; j++) // every lane that is written ends here
		endWrite(st, j);
	std::fill(writeBy_, writeBy_ + 8, -1);
	if (tr.playing != u.recScene) {
		if (tr.queued == u.recScene)
			tr.queued = kNone;
		if (u.recFresh)
			std::memset(static_cast<void*>(&c), 0, sizeof(Clip)), c.euclid = Euclid();
	} else if (u.recOpen) {
		c.length = (uint16_t)((u.recEnd >= 0 ? u.recEnd : takeEnd(st.s, tr)) - tr.startTick);
	}
	if (discard)
		for (Live& l : live_)
			l.rec = -1;
	u.recTrack = -1, u.recFresh = u.recOpen = false, u.recEnd = -1;
}

/** A pad that was recorded is let go: its note goes into the clip of the take, from where it began to now (at least a
 * tick, at most the clip). A full clip says so. */
void Machine::recordNote(State& st, int pad) {
	const Live& l = live_[pad];
	Clip& c = st.clip[l.track][l.rec];
	const Ui& ui = st.s.ui;
	int pos = l.pos >= c.length || !ui.recQuantOn ? l.pos : quantized(l.pos, ui.recQuant, 100, c.length);
	if (pos >= c.length)
		return;
	if (c.nNotes >= kMaxNotes) {
		st.s.ui.popup = kClipFullPopup, st.s.ui.popupAt = st.s.ui.now;
		return;
	}
	int len = (int)std::min<int64_t>(std::max<int64_t>(atTicks_ - l.at, 1), c.length);
	insertNote(
		c, {(uint16_t)pos, (uint16_t)len, l.pitch, l.vel}); // the same tick and pitch as a note there: left as it was
}

/** Keeps the slot's clip as it is now, before an edit. One entry for a slot and a gesture (hold + encoder turns); a new
 * one drops the redo entries, and a full ring its oldest. */
void Machine::checkpoint(int track, int scene) {
	if (track == state_->s.ui.recTrack && scene == state_->s.ui.recScene)
		return; // the take's own step covers it
	if (undoTop_ != undoBottom_) {
		const Snapshot& top = undo_[(undoTop_ - 1) % kUndo];
		if (top.step == gesture_ && top.track == track && top.scene == scene)
			return;
	}
	if (undoTop_ - undoBottom_ == kUndo)
		undoBottom_++;
	Snapshot& e = undo_[undoTop_++ % kUndo];
	undoEnd_ = undoTop_;
	e.step = gesture_, e.track = (uint8_t)track, e.scene = (uint8_t)scene;
	e.clip = state_->clip[track][scene];
}

/** Undoes (or redoes) every entry of the step at the top: each swaps with its slot, so the other direction is the same
 * walk. A slot that ends up empty is vacated. False when there is nothing to do. */
bool Machine::undo(State& st, bool redo) {
	if (redo ? undoTop_ == undoEnd_ : undoTop_ == undoBottom_)
		return false;
	endTake(true); // the history is about to change under it
	Ui& ui = st.s.ui;
	uint32_t step = undo_[(redo ? undoTop_ : undoTop_ - 1) % kUndo].step;
	while (redo ? undoTop_ != undoEnd_ : undoTop_ != undoBottom_) {
		Snapshot& e = undo_[(redo ? undoTop_ : undoTop_ - 1) % kUndo];
		if (e.step != step)
			break;
		redo ? undoTop_++ : undoTop_--;
		Clip& c = st.clip[e.track][e.scene];
		std::swap_ranges((uint8_t*)&e.clip, (uint8_t*)&e.clip + sizeof(Clip), (uint8_t*)&c); // no 33 KB temporary
		if (!c.length)
			vacate(st, e.track, e.scene);
		if (e.track == ui.track && e.scene == ui.scene) {
			const Track& tr = st.s.track[e.track];
			ui.heldStep = -1, ui.page = (uint8_t)std::min<int>(ui.page, lastPage(c, kGrid[tr.grid], pageSteps(tr, ui)));
		}
	}
	return true;
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
	atTicks_ = std::max<int64_t>((int64_t)std::llround(at * kPpqn), 0);
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
		for (int i = 0; i < kTracks;
			 i++) // a track changed type (Browse or the menu) or layout: silence it, switch its voices
			layout(*state_, i);
		// ponytail: at most 8 ticks per sample, the rest follow on the next samples; only an external clock at 1 or 2
		// pulses per beat jumps that far.
		for (int n = 0; n < 8 && clock_.started() && nextTick_ <= due; n++)
			tick(*state_, nextTick_++, out);
		Transport& t = s.transport;
		bpm_ = t.bpm;
		t.playing = clock_.started(), t.external = in.clockConnected;
		t.tick = std::max<int64_t>(nextTick_ - 1, 0);
		// What the Slice interpolates: each lane from its value at the last tick towards the next tick's.
		lastTick_ = nextTick_ - 1;
		for (int i = 0; i < kTracks; i++) {
			const Track& tr = s.track[i];
			const Clip* c = tr.playing != kNone ? &state_->clip[i][tr.playing] : nullptr;
			int pos = c && c->length ? (int)(std::max<int64_t>(lastTick_ - tr.startTick, 0) % c->length) : 0;
			for (int j = 0; j < kAutos; j++) {
				from_[i][j] = volts(tr, j, tr.value[j]);
				bool plays = !(tr.overridden >> j & 1) && !(i == s.ui.recTrack && s.ui.writing >> j & 1);
				int e = c && c->length && plays ? envelope(*c, j, pos + 1) : -1;
				to_[i][j] = e >= 0 ? volts(tr, j, e) : from_[i][j];
			}
		}
		t.shownBpm = (in.clockConnected ? (float)clock_.cps() : cps_) * 60.f;
		for (int i = 0; i < kTracks; i++)
			muted_[i] = silent(s, i);
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
		out.rgb[2 * kTracks] = 0xFFC800; // ponytail: calibrate on the hardware
		leds(*state_, want);
		out.ledsDone = true;
		unlock();
	}

	// The voices: the bend is on the selected track only. CC, LEVEL and PAN run between the last tick's value and the
	// next one's; a busy lock keeps the last ones.
	float frac = clock_.started() ? (float)std::min(std::max(at * kPpqn - (double)lastTick_, 0.), 1.) : 0.f;
	for (int t = 0; t < kTracks; t++) {
		filo::VoiceAllocator& v = voice_[t];
		bool drum = layout_[t] == Drum; // 16 channels, one for each lane, whatever Voices says
		v.setChannels(drum ? kLanes : voices_[t]);
		if (slice) {
			SliceTrack& o = slice->track[t];
			float bend = t == selected_ && !drum ? bend_ : 0.f;
			o.channels = (uint8_t)v.channels();
			for (int c = 0; c < o.channels; c++)
				o.voct[c] = (drum ? (float)(kDrumNote + c - 60) / 12.f : v.pitchV(c)) + bend,
				o.gate[c] = v.gate(c) && !muted_[t] ? 10.f : 0.f, o.vel[c] = v.velV(c);
			auto lane = [&](int j) { return from_[t][j] + (to_[t][j] - from_[t][j]) * frac; };
			for (int c = 0; c < kCcs; c++)
				o.cc[c] = lane(c);
			o.level = muted_[t] ? 0.f : lane(kLevel), o.pan = lane(kPan), o.rgb = rgb_[t];
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
	const Clip& clip = state_->clip[u.track][u.scene];
	for (int t = 0; t < kTracks; t++) {
		for (int c = 0; c < kScenes; c++)
			v.length[t][c] = state_->clip[t][c].length;
		const int8_t playing = v.s.track[t].playing;
		const Clip* lanes = t == u.track ? &clip : playing != kNone ? &state_->clip[t][playing] : nullptr;
		v.automated[t] = 0;
		for (int j = 0; lanes && j < kAutos; j++)
			v.automated[t] |= (uint16_t)((lanes->nPoints[j] != 0) << j);
	}
	for (int j = 0; j < kAutos; j++)
		v.at[j] = v.s.track[u.track].value[j];
	v.lane = 255, v.nShown = 0;
	for (int b = 0; b < kCcs && u.screen == kDeviceScreen && v.lane == 255; b++)
		if (u.touched >> b & 1)
			v.lane = (uint8_t)b, v.nShown = clip.nPoints[b],
			std::copy(clip.points[b], clip.points[b] + v.nShown, v.shown);
	const bool euclid = v.s.track[u.track].type == Euclidean;
	v.euclid = clip.euclid;
	v.nNotes = euclid ? 0 : clip.nNotes; // a Euclid slot's notes are generated below
	if (!euclid)
		std::copy(clip.notes, clip.notes + clip.nNotes, v.notes);
	v.held = -1;
	v.heldCount = 0;
	if (u.pads == kNote && (u.noteLayout || v.s.track[u.track].type == Drum) && u.heldStep >= 0) {
		int start, end, pitch = stepOf(v.s, u.heldStep, start), grid = kGrid[v.s.track[u.track].grid];
		stepSpan(v.s, clip, start, end);
		for (int j = 0; j < kAutos; j++)
			if (int e = envelope(clip, j, (start + end) / 2); e >= 0)
				v.at[j] = (uint16_t)e;
		if (u.noteLayout == 2 && v.s.track[u.track].type != Drum) { // the split layout: the first of its notes
			int first = -1;
			v.heldCount = (uint8_t)std::min(stepNotes(clip, start, grid, &first, 1), 255);
			v.held = (int16_t)first;
		} else {
			v.held = (int16_t)findNote(clip, pitch, start, grid);
		}
	}
	unlock();
	if (euclid) { // the Euclid notes of the pass of the clip the playhead is in, so the box shows what plays under it
		const Track& tr = v.s.track[u.track];
		const int length = v.length[u.track][u.scene];
		const int64_t pass = v.s.transport.playing && tr.playing == u.scene && length
								 ? std::max<int64_t>(v.s.transport.tick - tr.startTick, 0) / length
								 : 0;
		v.nNotes =
			(uint16_t)(length ? euclidNotes(v.s, u.track, v.euclid, pass * length, length, v.notes, kMaxNotes) : 0);
	}
}

void Machine::save(State& st) {
	lock();
	st = *state_;
	unlock();
}

void Machine::load(const State& st) {
	lock();
	*state_ = st;
	undoBottom_ = undoTop_ = undoEnd_ = 0; // the clips are not the ones the history is of
	unlock();
	silence_.store(true, std::memory_order_relaxed);
	dirty_.store(true, std::memory_order_release);
}

} // namespace tart
