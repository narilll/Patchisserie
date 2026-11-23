// Tart's display: what push2::Screen shows on the Push (960x160: 8 columns of 120 px, the top band over the encoders
// and upper buttons, the bottom band over the lower buttons) and in the panel's mirror.
#include <algorithm>
#include <cstdio>

#include <nanovg.h>

#include "tart.hpp"

namespace tart {

namespace {

const char* const kLayoutNames[] = {"4ths", "3rds", "Sequential"};
// What the Scale screen's root buttons show, in kRoots order. ASCII flats: the fonts may lack the flat sign.
const char* const kRootNames[] = {"C", "G", "D", "A", "E", "B", "F", "Bb", "Eb", "Ab", "Db", "Gb"};

const char* const kQuantNames[] = {"None", "8 Bars", "4 Bars", "2 Bars", "1 Bar", "1/2",   "1/2T",
								   "1/4",  "1/4T",   "1/8",    "1/8T",   "1/16",  "1/16T", "1/32"};

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

		// Middle: where the clips will be.
		std::snprintf(buf, sizeof buf, "Track %d \xC2\xB7 Scene %d", ui.track + 1, ui.scene + 1);
		text(480, 90, buf, 28, NVG_ALIGN_CENTER, white, sans);
		if (status)
			text(8, 130, status, 14, NVG_ALIGN_LEFT, dim, sans);

		// Bottom band: the tracks; the selected one inverted.
		for (int i = 0; i < kTracks; i++) {
			const uint8_t* c = kLiveColors[v.s.track[i].color % kColors];
			NVGcolor color = nvgRGB(c[0], c[1], c[2]);
			bool selected = i == ui.track;
			if (selected)
				rect(120.f * i + 3, 138, 114, 20, color);
			text(120.f * i + 60, 154, v.s.track[i].name, 15, NVG_ALIGN_CENTER, selected ? black : color, sans);
		}
	}

	// Popup: while the encoder is touched and for a second after a turn.
	bool tempo = ui.popup == kTempoPopup;
	if ((tempo || ui.popup == kSwingPopup) &&
		(ui.touched >> (tempo ? push2::kTempoId : push2::kSwingId) & 1 || ui.now - ui.popupAt < 1)) {
		if (tempo)
			std::snprintf(buf, sizeof buf, t.external ? "Tempo %.1f BPM EXT" : "Tempo %.1f BPM",
						  t.external ? t.shownBpm : t.bpm);
		else
			std::snprintf(buf, sizeof buf, "Swing %d %%", t.swing);
		rect(270, 45, 420, 70, nvgRGB(0x30, 0x32, 0x36));
		text(480, 94, buf, 36, NVG_ALIGN_CENTER, white, sans);
	}
}

} // namespace tart
