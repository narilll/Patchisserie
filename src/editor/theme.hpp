#pragma once
// Lattice's grid colours: Hundred Rabbits' theme format (nine colours, an .svg with each as the fill of the element
// of that id; github.com/hundredrabbits/Themes) and the cell styles of Orca JS's client. Pure logic, no Rack or
// NanoVG: the grid view maps slots to colours; test/test_grid.cpp tests it.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <string>

#include "lattice.hpp"

namespace editor {

enum Slot : int8_t { kBg, kFHigh, kFMed, kFLow, kFInv, kBHigh, kBMed, kBLow, kBInv, kSlots };
inline const char* const kSlotNames[kSlots] = {"background", "f_high", "f_med", "f_low", "f_inv",
											   "b_high",     "b_med",  "b_low", "b_inv"};

struct Theme {
	uint32_t c[kSlots]; // 0xRRGGBB
	bool operator==(const Theme& o) const { return std::memcmp(c, o.c, sizeof c) == 0; }
	bool operator!=(const Theme& o) const { return !(*this == o); }
};

/** "#rgb" or "#rrggbb", any case; anything else is false and leaves rgb alone. */
inline bool fromHex(const std::string& s, uint32_t& rgb) {
	if ((s.size() != 4 && s.size() != 7) || s[0] != '#')
		return false;
	for (size_t i = 1; i < s.size(); i++)
		if (!std::isxdigit((unsigned char)s[i]))
			return false;
	uint32_t v = (uint32_t)std::strtoul(s.c_str() + 1, nullptr, 16);
	if (s.size() == 4) // #rgb: each digit doubled
		v = (v >> 8 & 15) * 0x110000 + (v >> 4 & 15) * 0x1100 + (v & 15) * 0x11;
	rgb = v;
	return true;
}

inline std::string hex(uint32_t rgb) {
	char s[8];
	std::snprintf(s, sizeof s, "#%06x", (unsigned)(rgb & 0xffffff));
	return s;
}

/** A Hundred Rabbits theme .svg: all nine slots, or false and `t` untouched. */
inline bool parseTheme(const std::string& svg, Theme& t) {
	Theme out;
	for (int i = 0; i < kSlots; i++) {
		// ponytail: a regex over the text, not an XML parser: the themes are flat; a nested or escaped tag fools it.
		std::smatch tag, fill;
		std::regex id(std::string("<[^>]*\\bid\\s*=\\s*([\"'])") + kSlotNames[i] + "\\1[^>]*>");
		if (!std::regex_search(svg, tag, id))
			return false;
		std::string el = tag.str();
		static const std::regex kFill("\\bfill\\s*=\\s*([\"'])([^\"']*)\\1");
		if (!std::regex_search(el, fill, kFill) || !fromHex(fill[2].str(), out.c[i]))
			return false;
	}
	t = out;
	return true;
}

struct Preset {
	const char* name;
	Theme theme;
};

inline const Preset kPresets[] = {
	{"Patchisserie", {{0x1b1c1e, 0xeeeee8, 0x76787e, 0x4e5056, 0x1b1c1e, 0xc6c7c2, 0x8fa6ff, 0x2f3a66, 0xffb07a}}},
	{"Orca", {{0x000000, 0xffffff, 0x777777, 0x444444, 0x000000, 0xdddddd, 0x72dec2, 0x222222, 0xffb545}}},
	{"Orca light", {{0xeeeeee, 0x0a0a0a, 0x4a4a4a, 0x6a6a6a, 0x111111, 0xa1a1a1, 0xc1c1c1, 0xffffff, 0xffb545}}},
};

// Orca's operators: A-Z and these (orca-c: no `$`); '*' is the bang, drawn apart.
inline bool isOperator(char c) {
	return (c >= 'A' && c <= 'Z') || (c && std::strchr("*#:%!?=;", c));
}

/** The operator guide's entries, in orca-c's order: copied from push_opers_guide_msg() in vendor/orca-c/tui_main.c
(where `%` is commented out and `*` "self" is left out); test_grid pins it. `;` and `=` Lattice ignores. */
struct OperatorInfo {
	char glyph;
	const char* name;
	const char* desc;
	bool supported;
};

inline const OperatorInfo kOperators[] = {
	{'A', "add", "Outputs sum of inputs.", true},
	{'B', "subtract", "Outputs difference of inputs.", true},
	{'C', "clock", "Outputs modulo of frame.", true},
	{'D', "delay", "Bangs on modulo of frame.", true},
	{'E', "east", "Moves eastward, or bangs.", true},
	{'F', "if", "Bangs if inputs are equal.", true},
	{'G', "generator", "Writes operands with offset.", true},
	{'H', "halt", "Halts southward operand.", true},
	{'I', "increment", "Increments southward operand.", true},
	{'J', "jumper", "Outputs northward operand.", true},
	{'K', "konkat", "Reads multiple variables.", true},
	{'L', "lesser", "Outputs smallest input.", true},
	{'M', "multiply", "Outputs product of inputs.", true},
	{'N', "north", "Moves Northward, or bangs.", true},
	{'O', "read", "Reads operand with offset.", true},
	{'P', "push", "Writes eastward operand.", true},
	{'Q', "query", "Reads operands with offset.", true},
	{'R', "random", "Outputs random value.", true},
	{'S', "south", "Moves southward, or bangs.", true},
	{'T', "track", "Reads eastward operand.", true},
	{'U', "uclid", "Bangs on Euclidean rhythm.", true},
	{'V', "variable", "Reads and writes variable.", true},
	{'W', "west", "Moves westward, or bangs.", true},
	{'X', "write", "Writes operand with offset.", true},
	{'Y', "jymper", "Outputs westward operand.", true},
	{'Z', "lerp", "Transitions operand to target.", true},
	{'*', "bang", "Bangs neighboring operands.", true},
	{'#', "comment", "Halts line.", true},
	{':', "midi", "Sends MIDI note.", true},
	{'!', "cc", "Sends MIDI control change.", true},
	{'?', "pb", "Sends MIDI pitch bend.", true},
	{'%', "mono", "Sends MIDI monophonic note.", true},
	{'=', "osc", "Sends OSC message.", false},
	{';', "udp", "Sends UDP message.", false},
};

struct Cell {
	char glyph;    // 0: nothing drawn
	int8_t fg, bg; // Slots; -1: none
};

/** One cell as Orca JS styles it (client.js makeStyle), by orca-c's mark flags; `reading` is the raw glyph under the
cursor; `dots` draws the dim dot an empty cell would hide. (ponytail: Orca JS's "locals" dots near the cursor are
skipped.) */
inline Cell cellStyle(char c, uint8_t mark, bool selected, bool cursor, bool marker, char reading, bool paused,
					  bool dots = false) {
	bool lock = mark & lattice::kLock, port = mark & (lattice::kInput | lattice::kOutput | lattice::kHasteInput);
	char glyph = c;
	if (c == '.')
		glyph = cursor ? (paused ? '~' : '@') : marker ? '+' : selected || port || lock || dots ? '.' : 0;
	if (selected)
		return {glyph, kFInv, kBInv};
	if (c != '.' && c == reading && !lock)
		return {glyph, kBInv, -1};
	if (c == '*' && !lock)
		return {glyph, kBHigh, -1};
	if (mark & lattice::kOutput)
		return {glyph, kFLow, kBHigh};
	if (mark & lattice::kHasteInput)
		return {glyph, kBMed, -1};
	if (mark & lattice::kInput)
		return {glyph, kBHigh, -1};
	if (isOperator(c) && !lock)
		return {glyph, kFLow, kBMed};
	return {glyph, lock ? kFMed : kFLow, -1};
}

} // namespace editor
