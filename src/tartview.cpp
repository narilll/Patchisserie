// Tart's display: what push2::Screen shows on the Push (960x160: 8 columns of 120 px, the top band over the encoders
// and upper buttons, the bottom band over the lower buttons) and in the panel's mirror.
#include <cstdio>

#include <nanovg.h>

#include "tart.hpp"

namespace tart {

namespace {

// ponytail: 8 fixed colours, as many as the tracks, until E2.3 brings Live's 70 and the colour menu.
const uint8_t kTrackRgb[8][3] = {{255, 94, 94},  {255, 165, 40}, {240, 220, 60},  {90, 220, 90},
								 {60, 210, 210}, {80, 140, 255}, {170, 110, 255}, {255, 110, 200}};
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
	auto cell = [&](int column, float width, const char* label, const char* value) {
		text(120.f * column + 6, 17, label, 12, NVG_ALIGN_LEFT, dim, sans);
		text(120.f * (column + width) - 6, 17, value, 14, NVG_ALIGN_RIGHT, white, mono);
	};
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
		const uint8_t* c = kTrackRgb[v.s.track[i].color % 8];
		NVGcolor color = nvgRGB(c[0], c[1], c[2]);
		bool selected = i == ui.track;
		if (selected)
			rect(120.f * i + 3, 138, 114, 20, color);
		text(120.f * i + 60, 154, v.s.track[i].name, 15, NVG_ALIGN_CENTER, selected ? black : color, sans);
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
