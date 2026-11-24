// Tart's display: what push2::Screen shows on the Push (960x160: 8 columns of 120 px, the top band over the encoders
// and upper buttons, the bottom band over the lower buttons) and in the panel's mirror.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include <nanovg.h>

#include "tart.hpp"

namespace tart {

namespace {

const char* const kLayoutNames[] = {"4ths", "3rds", "Sequential"};
// What the Scale screen's root buttons show, in kRoots order. ASCII flats: the fonts may lack the flat sign.
const char* const kRootNames[] = {"C", "G", "D", "A", "E", "B", "F", "Bb", "Eb", "Ab", "Db", "Gb"};

const char* const kFixedNames[] = {"1 BEAT", "2 BEATS", "1 BAR", "2 BARS", "4 BARS", "8 BARS", "16 BARS", "32 BARS"};
const int kGrids[8] = {96, 64, 48, 32, 24, 16, 12, 8}; // ticks of a step, as in tart.cpp

} // namespace

void paint(NVGcontext* vg, const View& v, int sans, int mono, const char* status, const char* message) {
	const NVGcolor white = nvgRGB(255, 255, 255), dim = nvgRGB(0x90, 0x92, 0x8c), black = nvgRGB(0, 0, 0);
	auto text = [&](float x, float y, const char* s, float size, int align, NVGcolor c, int font) {
		nvgFontFaceId(vg, font);
		nvgFontSize(vg, size);
		nvgFillColor(vg, c);
		nvgTextAlign(vg, align | NVG_ALIGN_BASELINE);
		nvgText(vg, x, y, s, nullptr);
	};
	auto rect = [&](float x, float y, float w, float h, NVGcolor c) {
		nvgBeginPath(vg);
		nvgRoundedRect(vg, x, y, w, h, 3);
		nvgFillColor(vg, c);
		nvgFill(vg);
	};
	if (message) {
		text(480, 90, message, 40, NVG_ALIGN_CENTER, white, sans);
		return;
	}
	const Transport& t = v.s.transport;
	const Ui& ui = v.s.ui;
	char buf[48];

	// Top band: the transport.
	auto cell = [&](int column, float width, const char* label, const char* value, float y = 17) {
		text(120.f * column + 6, y, label, 12, NVG_ALIGN_LEFT, dim, sans);
		text(120.f * (column + width) - 6, y, value, 14, NVG_ALIGN_RIGHT, white, mono);
	};
	auto setting = [&](int column, const char* label, const char* value) { // a bigger value under its label
		text(120.f * column + 6, 40, label, 12, NVG_ALIGN_LEFT, dim, sans);
		text(120.f * (column + 1) - 6, 60, value, 18, NVG_ALIGN_RIGHT, white, mono);
	};
	const bool euclid = euclidHome(v.s);
	if (ui.screen == kScaleScreen) {
		const Scale& sc = v.s.scale;
		// Top: the layout (encoder 1), the roots C G D A E B (upper buttons 2..7), the direction (encoder 8).
		cell(0, 1, "LAYOUT", kLayoutNames[sc.layout / 2]);
		cell(7, 1, "DIRECTION", sc.layout % 2 ? "Right" : "Up");
		// Bottom: In Key or Chromatic (lower button 1), the roots F Bb Eb Ab Db Gb, Fixed (lower button 8).
		cell(0, 1, "NOTES", sc.inKey ? "In Key" : "Chromatic", 154);
		cell(7, 1, "FIXED", sc.fixed ? "Yes" : "No", 154);
		for (int k = 0; k < 12; k++) {
			bool selected = kRoots[k] == sc.root;
			float x = 120.f * (k % 6 + 1), y = k < 6 ? 2 : 138;
			if (selected)
				rect(x + 3, y, 114, 20, white);
			text(x + 60, y + 16, kRootNames[k], 15, NVG_ALIGN_CENTER, selected ? black : white, sans);
		}
		// Middle: the scale list in columns of four under encoders 2..7, the selected scale in view.
		int columns = (kScaleCount + 3) / 4, first = std::min(std::max(sc.scale / 4 - 2, 0), std::max(columns - 6, 0));
		for (int c = 0; c < 6; c++)
			for (int r = 0; r < 4; r++) {
				int k = (first + c) * 4 + r;
				if (k >= kScaleCount)
					continue;
				float x = 120.f * (c + 1), y = 28.f + 27 * r;
				if (k == sc.scale)
					rect(x + 3, y, 114, 24, white);
				text(x + 60, y + 18, kScales[k].name, 14, NVG_ALIGN_CENTER, k == sc.scale ? black : white, sans);
			}
	} else if (ui.screen == kBrowseScreen) { // CLOSE over upper 1, TYPE over encoder 1, the list, LOAD over lower 8
		const Track& tr = v.s.track[ui.track];
		rect(3, 2, 114, 20, nvgRGB(0x30, 0x32, 0x36));
		text(60, 18, "CLOSE", 15, NVG_ALIGN_CENTER, white, sans);
		cell(0, 1, "TYPE", kTypeNames[std::min<int>(tr.type, kBrowseTypes - 1)], 40);
		for (int k = 0; k < kBrowseTypes; k++) {
			float y = 54.f + 27 * k;
			bool cursor = k == ui.browse;
			if (cursor)
				rect(123, y, 234, 24, white);
			if (k == tr.type) { // the type the track has now
				nvgBeginPath(vg);
				nvgCircle(vg, 138, y + 12, 4);
				nvgFillColor(vg, cursor ? black : dim);
				nvgFill(vg);
			}
			text(150, y + 18, kTypeNames[k], 14, NVG_ALIGN_LEFT, cursor ? black : white, sans);
		}
		rect(120.f * 7 + 3, 138, 114, 20, white);
		text(120.f * 7 + 60, 154, "LOAD", 15, NVG_ALIGN_CENTER, black, sans);
	} else if (ui.screen == kSetupScreen) { // DISPLAY LIGHT and LED BRIGHTNESS over encoders 2 and 3, in percent
		std::snprintf(buf, sizeof buf, "%d %%", (v.s.setup.display * 100 + 127) / 255);
		setting(1, "DISPLAY LIGHT", buf);
		std::snprintf(buf, sizeof buf, "%d %%", (v.s.setup.led * 100 + 63) / 127);
		setting(2, "LED BRIGHTNESS", buf);
	} else if (ui.screen == kClipScreen) { // the length of the selected clip, as bars.beats.ticks
		int length = v.length[ui.track][ui.scene];
		if (length) {
			std::snprintf(buf, sizeof buf, "%d.%d.%d", length / kBar, length / kPpqn % 4, length % kPpqn);
			cell(0, 1, "LENGTH", buf);
		}
	} else if (quantizeSettings(ui)) { // SWING, QUANTIZE TO, AMOUNT and REC QUANT over their encoders, the switch above
		std::snprintf(buf, sizeof buf, "%d %%", t.swing);
		setting(0, "SWING", buf);
		setting(1, "QUANTIZE TO", kQuantToNames[std::min<int>(ui.quantTo, 7)]);
		std::snprintf(buf, sizeof buf, "%d %%", ui.quantAmount);
		setting(2, "AMOUNT", buf);
		setting(4, "REC QUANT", kQuantToNames[std::min<int>(ui.recQuant, 7)]);
		rect(120.f * 4 + 3, 2, 114, 20, ui.recQuantOn ? white : nvgRGB(0x30, 0x32, 0x36));
		text(120.f * 4 + 60, 18, ui.recQuantOn ? "REC QUANT ON" : "REC QUANT OFF", 15, NVG_ALIGN_CENTER,
			 ui.recQuantOn ? black : white, sans);
	} else if (ui.button[push2::cc::FixedLength]) {
		cell(0, 1, "FIXED LENGTH", ui.fixedOn ? "On" : "Off");
	} else if (euclid) { // DRUM and MELODIC over upper 1 and 2, then the settings of the lane (Drum) or the rhythm
		const Euclid& e = v.euclid;
		const bool melodic = e.mode == kEuclidMelodic;
		for (int k = 0; k < 2; k++) {
			rect(120.f * k + 3, 2, 114, 20, k == e.mode ? white : nvgRGB(0x30, 0x32, 0x36));
			text(120.f * k + 60, 18, k ? "MELODIC" : "DRUM", 15, NVG_ALIGN_CENTER, k == e.mode ? black : white, sans);
		}
		const EuclidLane& x = e.lane[melodic ? kEuclidRhythm : v.s.track[ui.track].lane];
		const int n = std::min(std::max<int>(x.steps, 1), kMaxSteps);
		auto number = [&](int column, const char* label, int value, const char* unit = "") {
			std::snprintf(buf, sizeof buf, "%d%s", value, unit);
			setting(column, label, buf);
		};
		number(0, "STEPS", n);
		number(1, "PULSES", std::min<int>(x.pulses, n));
		number(2, "ROTATE", x.rotation % n);
		number(3, "PROB", x.probability, " %");
		number(4, "VEL", x.velocity);
		number(5, "SEED", e.seed);
		if (melodic)
			number(6, "OCTAVES", e.octaves), number(7, "CHANGE", e.change, " %");
	} else if (ui.screen == kDeviceScreen) { // CC 1 to 8: the name and a dot (white: automated, gray: overridden) over
		const Track& tr = v.s.track[ui.track]; // the value, which is the one at the held step while one is held
		const bool lockHeld = ui.heldStep >= 0 && tr.type == tart::Euclidean;
		for (int j = 0; j < kCcs; j++) {
			float x = 120.f * j;
			bool over = tr.overridden >> j & 1, lit = over || v.automated[ui.track] >> j & 1;
			if (lockHeld) // a held Euclid step: a white dot where it has a lock, none elsewhere
				over = false, lit = v.locked >> j & 1;
			bool written = ui.recTrack == ui.track && ui.writing >> j & 1; // red: the take is writing it
			if (lit || written) {
				nvgBeginPath(vg);
				nvgCircle(vg, x + 10, 8, 3.5f);
				nvgFillColor(vg, written ? nvgRGB(0xff, 0x30, 0x30) : over ? nvgRGB(0x70, 0x72, 0x6c) : white);
				nvgFill(vg);
			}
			std::snprintf(buf, sizeof buf, "CC %d", j + 1);
			text(x + 18, 11, tr.ccName[j][0] ? tr.ccName[j] : buf, 11, NVG_ALIGN_LEFT, dim, sans);
			float volts = tart::volts(tr, j, ui.heldStep >= 0 ? v.at[j] : tr.value[j]);
			std::snprintf(buf, sizeof buf, tr.bipolar >> j & 1 ? "%+.2f V" : "%.2f V", volts);
			text(x + 114, 27, buf, 14, NVG_ALIGN_RIGHT, white, mono);
		}
	} else if (ui.screen == kMixScreen) { // VOLUMES, PANS, SEND A, SEND B over upper 1 to 4; a column a track: its lane
		// (a held step: the selected track's LEVEL, PAN, SEND A and SEND B in columns 1 to 4) as a dot (as on Device),
		// M and S badges, the value and a bar
		const NVGcolor grey = nvgRGB(0x30, 0x32, 0x36);
		static const char* const kMixNames[4] = {"VOLUMES", "PANS", "SEND A", "SEND B"};
		for (int k = 0; k < 4; k++) {
			rect(120.f * k + 3, 2, 114, 20, k == ui.mixLane ? white : grey);
			text(120.f * k + 60, 18, kMixNames[k], 15, NVG_ALIGN_CENTER, k == ui.mixLane ? black : white, sans);
		}
		const bool held = ui.heldStep >= 0;
		for (int j = 0; j < (held ? 4 : kTracks); j++) {
			const int trk = held ? ui.track : j, lane = kMixLane[held ? j : ui.mixLane];
			const Track& tr = v.s.track[trk];
			const float x = 120.f * j;
			const bool lockHeld = held && tr.type == tart::Euclidean; // its locks are the dots, as on Device
			const bool over = !lockHeld && tr.overridden >> lane & 1,
					   lit = lockHeld ? v.locked >> lane & 1 : over || v.automated[trk] >> lane & 1;
			const bool written = ui.recTrack == trk && ui.writing >> lane & 1;
			if (lit || written) {
				nvgBeginPath(vg);
				nvgCircle(vg, x + 10, 33, 3.5f);
				nvgFillColor(vg, written ? nvgRGB(0xff, 0x30, 0x30) : over ? nvgRGB(0x70, 0x72, 0x6c) : white);
				nvgFill(vg);
			}
			if (held)
				text(x + 18, 37,
					 lane == kPan     ? "PAN"
					 : lane == kLevel ? "LEVEL"
					 : lane == kSendA ? "SEND A"
									  : "SEND B",
					 11, NVG_ALIGN_LEFT, dim, sans);
			if (tr.mute && !held) {
				rect(x + 76, 26, 18, 14, nvgRGB(255, 200, 0));
				text(x + 85, 37, "M", 12, NVG_ALIGN_CENTER, black, sans);
			}
			if (tr.solo && !held) {
				rect(x + 96, 26, 18, 14, nvgRGB(0, 0xa0, 0xff));
				text(x + 105, 37, "S", 12, NVG_ALIGN_CENTER, black, sans);
			}
			const int value = held ? v.at[lane] : tr.value[lane];
			std::snprintf(buf, sizeof buf, lane == kPan ? "%+.2f V" : "%.2f V", tart::volts(tr, lane, value));
			text(x + 114, 58, buf, 14, NVG_ALIGN_RIGHT, white, mono);
			const uint8_t* c = kLiveColors[tr.color % kColors];
			const float y0 = 66, h = 64, mid = y0 + h / 2; // the bar: LEVEL from the bottom, PAN from the middle
			rect(x + 44, y0, 32, h, nvgRGB(0x20, 0x22, 0x26));
			const float base = lane == kPan ? mid : y0 + h;
			const float edge = lane == kPan ? mid - h / 2 * (value - 32768) / 32768.f : y0 + h - h * value / 65535.f;
			rect(x + 44, std::min(edge, base), 32, std::max(std::abs(base - edge), 1.f), nvgRGB(c[0], c[1], c[2]));
		}
	} else if (v.held >= 0) { // a held step: its note
		const Note& n = v.notes[v.held];
		int grid = kGrids[v.s.track[ui.track].grid];
		std::snprintf(buf, sizeof buf, "%+d", n.tick - (n.tick + grid / 2) / grid * grid);
		cell(0, 1, "NUDGE", buf);
		std::snprintf(buf, sizeof buf, "%.2f st", (float)n.len / grid);
		cell(1, 1, "LENGTH", buf);
		std::snprintf(buf, sizeof buf, "%d t", n.len);
		cell(2, 1, "FINE", buf);
		std::snprintf(buf, sizeof buf, "%d", n.vel);
		cell(3, 1, "VELOCITY", buf);
		if (v.heldCount) {
			std::snprintf(buf, sizeof buf, "%d", v.heldCount);
			cell(4, 1, "NOTES", buf);
		}
	} else {
		std::snprintf(buf, sizeof buf, t.external ? "%.1f EXT" : "%.1f", t.shownBpm);
		cell(0, 1, "TEMPO", buf);
		std::snprintf(buf, sizeof buf, "%d %%", t.swing);
		cell(1, 1, "SWING", buf);
		cell(2, 1, "QUANT", kQuantNames[t.quant]);
		cell(3, 1, "METRO", t.metronome ? "On" : "Off");
		std::snprintf(buf, sizeof buf, "%d.%d.%d", (int)(t.tick / kBar) + 1, (int)(t.tick / kPpqn % 4) + 1,
					  (int)(t.tick / (kPpqn / 4) % 4) + 1);
		cell(4, 2, "POSITION", buf);
		nvgBeginPath(vg); // play triangle or stop square
		if (t.playing) {
			nvgMoveTo(vg, 920, 5);
			nvgLineTo(vg, 920, 19);
			nvgLineTo(vg, 932, 12);
			nvgClosePath(vg);
		} else {
			nvgRect(vg, 920, 6, 12, 12);
		}
		nvgFillColor(vg, t.playing ? nvgRGB(0x40, 0xd0, 0x60) : dim);
		nvgFill(vg);
	}
	if (ui.screen != kScaleScreen && ui.screen != kBrowseScreen) {
		// Middle: the selected slot, its notes over time and the playhead.
		const Track& tr = v.s.track[ui.track];
		const uint8_t* tc = kLiveColors[tr.color % kColors];
		const bool drum = tr.type == Drum || (tr.type == Euclidean && v.euclid.mode == kEuclidDrum);
		if (ui.screen != kMixScreen && ui.screen != kSetupScreen) { // these screens have no clip overview
			char lane[16] = "";                                     // the Euclid screen's lane
			if (euclid && !v.euclid.mode)
				std::snprintf(lane, sizeof lane, " \xC2\xB7 Lane %d", tr.lane + 1);
			std::snprintf(buf, sizeof buf, "Track %d \xC2\xB7 Scene %d%s%s", ui.track + 1, ui.scene + 1,
						  tr.type == Drum        ? " \xC2\xB7 Drum"
						  : tr.type == Euclidean ? " \xC2\xB7 Euclid"
												 : "",
						  lane);
			// The Euclid screen has its settings in the middle too: a smaller header and box.
			const float hy = euclid ? 80 : 46, hs = euclid ? 16 : 18;
			text(8, hy, buf, hs, NVG_ALIGN_LEFT, white, sans);
			int length = v.length[ui.track][ui.scene];
			const bool rec = ui.recTrack == ui.track && ui.recScene == ui.scene; // a take runs here
			const float x0 = 8, w = 944, y0 = euclid ? 86 : 54,
						h = euclid                     ? 38
							: ui.screen == kClipScreen ? 70
													   : 62; // larger on the Clip screen
			if (!length) {
				text(480, euclid ? 112 : 96, "Empty slot", 24, NVG_ALIGN_CENTER, dim, sans);
			} else {
				lengthName(buf, sizeof buf, length);
				const NVGcolor red = nvgRGB(0xff, 0x30, 0x30);
				if (rec && ui.recOpen) { // the length is not known yet
					text(952, hy, "Recording", hs, NVG_ALIGN_RIGHT, red, sans);
				} else {
					text(952, hy, buf, hs, NVG_ALIGN_RIGHT, dim, sans);
					if (rec)
						text(942 - nvgTextBounds(vg, 0, 0, buf, nullptr, nullptr), hy, "REC", hs, NVG_ALIGN_RIGHT, red,
							 sans);
				}
				rect(x0, y0, w, h, nvgRGB(0x20, 0x22, 0x26));
				int lo = 127, hi = 0;
				for (int i = 0; i < v.nNotes; i++)
					lo = std::min<int>(lo, v.notes[i].pitch), hi = std::max<int>(hi, v.notes[i].pitch);
				int span = std::max(hi - lo + 1, 12); // semitones the box shows, from the lowest note
				if (drum)
					lo = kDrumNote, span = kLanes; // the lanes
				float nh = std::max(h / span, 2.f);
				for (int i = 0; i < v.nNotes; i++) {
					const Note& n = v.notes[i];
					if (n.tick >= length || (drum && (n.pitch < lo || n.pitch >= lo + span)))
						continue;
					float x = x0 + w * n.tick / length,
						  nw = std::max(w * std::min<int>(n.len, length - n.tick) / length, 2.f);
					nvgBeginPath(vg);
					nvgRect(vg, x, y0 + h - nh * (n.pitch - lo + 1), nw, nh - 1);
					nvgFillColor(vg, nvgRGB(tc[0], tc[1], tc[2]));
					nvgFill(vg);
				}
				if (v.nShown) { // the touched CC's envelope: a step at every jump
					auto y = [&](int value) { return y0 + h - 1 - (h - 2) * (float)value / 65535.f; };
					nvgBeginPath(vg);
					nvgMoveTo(vg, x0, y(v.shown[0].value));
					for (int i = 0; i < v.nShown; i++)
						nvgLineTo(vg, x0 + w * std::min<int>(v.shown[i].tick, length) / length, y(v.shown[i].value));
					nvgLineTo(vg, x0 + w, y(v.shown[v.nShown - 1].value));
					nvgStrokeColor(vg, white);
					nvgStrokeWidth(vg, 2);
					nvgStroke(vg);
				}
				if (t.playing && tr.playing == ui.scene) {
					float x = x0 + w * (float)(std::max<int64_t>(t.tick - tr.startTick, 0) % length) / length;
					nvgBeginPath(vg);
					nvgRect(vg, x, y0, 2, h);
					nvgFillColor(vg, nvgRGB(0x40, 0xd0, 0x60));
					nvgFill(vg);
				}
			}
			if (status)
				text(8, 130, status, 14, NVG_ALIGN_LEFT, dim, sans);
		}

		// Bottom band: the tracks, the selected one inverted; with Fixed Length held, its lengths.
		for (int i = 0; i < kTracks; i++) {
			if (ui.button[push2::cc::FixedLength]) {
				bool chosen = i == ui.fixedBars;
				if (chosen)
					rect(120.f * i + 3, 138, 114, 20, white);
				text(120.f * i + 60, 154, kFixedNames[i], 15, NVG_ALIGN_CENTER, chosen ? black : white, sans);
				continue;
			}
			const uint8_t* c = kLiveColors[v.s.track[i].color % kColors];
			NVGcolor color = silent(v.s, i) ? nvgRGB(0x58, 0x5a, 0x56) : nvgRGB(c[0], c[1], c[2]); // muted: dark gray
			bool selected = i == ui.track;
			if (selected)
				rect(120.f * i + 3, 138, 114, 20, color);
			text(120.f * i + 60, 154, v.s.track[i].name, 15, NVG_ALIGN_CENTER, selected ? black : color, sans);
			// A triangle where a clip plays, a blinking square where its stop is queued.
			const Track& k = v.s.track[i];
			bool stopQueued = k.queued == kStopQueued;
			if (stopQueued ? std::fmod(ui.now, 0.5) < 0.25 : k.playing != kNone) {
				float x = 120.f * i + 8;
				nvgBeginPath(vg);
				if (stopQueued) {
					nvgRect(vg, x, 142, 10, 10);
				} else {
					nvgMoveTo(vg, x, 141);
					nvgLineTo(vg, x, 153);
					nvgLineTo(vg, x + 11, 147);
					nvgClosePath(vg);
				}
				nvgFillColor(vg, selected ? black : color);
				nvgFill(vg);
			}
		}
	}

	// Popup: while the encoder is touched and for a second after a turn.
	const bool undo = ui.popup >= kUndoPopup, full = ui.popup >= kClipFullPopup;
	const int knob = ui.popup == kTempoPopup    ? push2::kTempoId
					 : ui.popup == kSwingPopup  ? push2::kSwingId
					 : ui.popup == kMasterPopup ? push2::kMasterId
												: -1;
	if (full ? ui.now - ui.popupAt < 1 : knob >= 0 && (ui.touched >> knob & 1 || ui.now - ui.popupAt < 1)) {
		popupText(v, buf, sizeof buf);
		rect(undo ? 150 : 270, 45, undo ? 660 : 420, 70, nvgRGB(0x30, 0x32, 0x36));
		text(480, 94, buf, undo ? 28 : 36, NVG_ALIGN_CENTER, white, sans);
	}
}

} // namespace tart
