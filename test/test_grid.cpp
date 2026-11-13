// editor::GridEdit (src/editor/grid.hpp): cursor, edits, clipboard, drag and undo over a stopped lattice::Machine.
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "editor/grid.hpp"
#include "editor/theme.hpp"

using editor::GridEdit;

namespace {

/** A stopped 6x4 machine (it only changes by edits) and an editor on it. */
struct Fix {
	lattice::Machine m;
	GridEdit g;
	explicit Fix(const std::string& text = "abcdef\nghijkl\nmnopqr\nstuvwx\n") {
		m.load(text);
		g.machine = &m;
		g.refresh();
	}
	std::string rows() { return m.text(); }
};

bool at(const GridEdit& g, int x, int y, int dx, int dy) {
	return g.x == x && g.y == y && g.dx == dx && g.dy == dy;
}

} // namespace

TEST(grid_cursor_clamps) {
	Fix f;
	f.g.move(-1, -1);
	CHECK(at(f.g, 0, 0, 0, 0));
	f.g.move(8, 8);
	CHECK(at(f.g, 5, 3, 0, 0));
	f.g.place(2, 1);
	f.g.scale(2, 1);
	CHECK(at(f.g, 2, 1, 2, 1));
	f.g.scale(9, 9);
	CHECK(at(f.g, 2, 1, 3, 2));
	f.g.scale(-9, -9);
	CHECK(at(f.g, 2, 1, -2, -1));
	CHECK(f.g.selection().x == 0 && f.g.selection().y == 0 && f.g.selection().w == 3 && f.g.selection().h == 2);
	f.g.place(-4, 99);
	CHECK(at(f.g, 0, 3, 0, 0));
	f.g.place(1, 1);
	f.g.extendTo(99, -5);
	CHECK(at(f.g, 1, 1, 4, -1));
	f.g.extendTo(0, 2);
	CHECK(at(f.g, 1, 1, -1, 1));
}

TEST(grid_move_keeps_extent) {
	Fix f;
	f.g.place(1, 1);
	f.g.scale(2, 1);
	f.g.move(1, 1);
	CHECK(at(f.g, 2, 2, 2, 1));
	f.g.move(1, 1); // far corner would leave: the extent shrinks
	CHECK(at(f.g, 3, 3, 2, 0));
	f.g.move(-8, -8);
	CHECK(at(f.g, 0, 0, 2, 0));
}

TEST(grid_select_all_and_collapse) {
	Fix f;
	CHECK(!f.g.collapse());
	f.g.place(3, 2);
	f.g.selectAll();
	CHECK(at(f.g, 0, 0, 5, 3));
	CHECK(f.g.collapse());
	CHECK(at(f.g, 0, 0, 0, 0));
	CHECK(!f.g.collapse());
}

TEST(grid_type) {
	Fix f;
	f.g.place(1, 1);
	f.g.scale(1, 1);
	f.g.type('Z');
	CHECK(f.g.grid.at(1, 1) == 'Z' && f.g.grid.at(2, 1) == 'i' && f.g.grid.at(2, 2) == 'o'); // origin only
	CHECK(at(f.g, 1, 1, 1, 1));
	f.g.type(' ');
	f.g.type('\n');
	f.g.type(char(200));
	CHECK(f.g.grid.at(1, 1) == 'Z');
	f.g.undo();
	CHECK(f.g.grid.at(1, 1) == 'h'); // the ignored keys made no steps
	f.g.insert = true;
	f.g.place(4, 0);
	f.g.type('1');
	CHECK(at(f.g, 5, 0, 0, 0));
	f.g.type('2');
	f.g.type('3');
	CHECK(at(f.g, 5, 0, 0, 0));
	CHECK(f.g.grid.at(4, 0) == '1' && f.g.grid.at(5, 0) == '3');
	f.g.place(0, 3);
	f.g.scale(1, 0);
	f.g.type('9'); // insert keeps the extent
	CHECK(at(f.g, 1, 3, 1, 0));
}

TEST(grid_erase_copy_cut) {
	Fix f;
	f.g.place(1, 1);
	f.g.scale(2, 1);
	CHECK(f.g.copy() == "hij\nnop");
	f.g.erase();
	CHECK(f.rows() == "abcdef\ng...kl\nm...qr\nstuvwx\n");
	f.g.undo();
	CHECK(f.rows() == "abcdef\nghijkl\nmnopqr\nstuvwx\n");
	f.g.place(1, 1);
	f.g.scale(2, 1);
	CHECK(f.g.cut() == "hij\nnop");
	CHECK(f.rows() == "abcdef\ng...kl\nm...qr\nstuvwx\n");
	f.g.place(5, 3);
	CHECK(f.g.copy() == "x");
}

TEST(grid_paste) {
	Fix f;
	f.g.place(1, 1);
	f.g.paste("XY\nZ\n");
	CHECK(f.rows() == "abcdef\ngXYjkl\nmZopqr\nstuvwx\n"); // the short line keeps what is past its end
	CHECK(at(f.g, 1, 1, 1, 1));
	f.g.undo();
	CHECK(f.rows() == "abcdef\nghijkl\nmnopqr\nstuvwx\n");
	f.g.paste("1 3\r\n456"); // CRLF, a space becomes '.'
	CHECK(f.rows() == "abcdef\ng1.3kl\nm456qr\nstuvwx\n");
	CHECK(at(f.g, 1, 1, 2, 1));
	f.g.place(4, 2);
	f.g.paste("abc\ndef\nghi\njkl"); // clipped right and bottom
	CHECK(f.rows() == "abcdef\ng1.3kl\nm456ab\nstuvde\n");
	CHECK(at(f.g, 4, 2, 1, 1));
	f.g.paste(""); // nothing
	f.g.paste("\n");
	CHECK(f.rows() == "abcdef\ng1.3kl\nm456ab\nstuvde\n");
	f.g.place(0, 0); // 'Q' alone, then an empty line keeps its row
	f.g.paste("Q\n\nR");
	CHECK(f.rows() == "Qbcdef\ng1.3kl\nR456ab\nstuvde\n");
}

TEST(grid_drag) {
	Fix f;
	f.g.scale(1, 0); // "ab"
	f.g.drag(1, 0);  // overlaps itself
	CHECK(f.rows() == ".abdef\nghijkl\nmnopqr\nstuvwx\n");
	CHECK(at(f.g, 1, 0, 1, 0));
	f.g.drag(0, 1); // the content moves, what was under it is lost
	CHECK(f.rows() == "...def\ngabjkl\nmnopqr\nstuvwx\n");
	CHECK(at(f.g, 1, 1, 1, 0));
	f.g.undo();
	CHECK(f.rows() == ".abdef\nghijkl\nmnopqr\nstuvwx\n");
	f.g.undo();
	CHECK(f.rows() == "abcdef\nghijkl\nmnopqr\nstuvwx\n");
	f.g.redo();
	f.g.redo();
	CHECK(f.rows() == "...def\ngabjkl\nmnopqr\nstuvwx\n");
}

TEST(grid_drag_blocked_at_edges) {
	Fix f;
	f.g.place(4, 0);
	f.g.scale(1, 0); // "ef" at the right edge
	f.g.drag(1, 0);
	f.g.drag(0, -1);
	CHECK(at(f.g, 4, 0, 1, 0));
	CHECK(f.rows() == "abcdef\nghijkl\nmnopqr\nstuvwx\n");
	f.g.undo(); // no step was made
	CHECK(f.rows() == "abcdef\nghijkl\nmnopqr\nstuvwx\n");
	f.g.place(0, 3);
	f.g.scale(0, -1); // "m", "s": the selection normalizes upward
	f.g.drag(-1, 1);
	CHECK(at(f.g, 0, 3, 0, -1));
	f.g.drag(0, -1);
	CHECK(f.rows() == "abcdef\nmhijkl\nsnopqr\n.tuvwx\n");
	CHECK(at(f.g, 0, 2, 0, -1));
}

TEST(grid_undo_redo) {
	Fix f;
	f.g.place(1, 1);
	f.g.type('A');
	f.g.place(3, 3);
	f.g.type('B');
	f.g.undo();
	CHECK(f.g.grid.at(3, 3) == 'v' && f.g.grid.at(1, 1) == 'A');
	CHECK(at(f.g, 3, 3, 0, 0));
	f.g.undo();
	CHECK(f.rows() == "abcdef\nghijkl\nmnopqr\nstuvwx\n");
	CHECK(at(f.g, 1, 1, 0, 0));
	f.g.undo(); // nothing left
	CHECK(f.rows() == "abcdef\nghijkl\nmnopqr\nstuvwx\n");
	f.g.redo();
	f.g.redo();
	CHECK(f.rows() == "abcdef\ngAijkl\nmnopqr\nstuBwx\n");
	f.g.redo(); // nothing left
	f.g.undo();
	f.g.type('C'); // a new edit clears redo
	f.g.redo();
	CHECK(f.g.grid.at(3, 3) == 'C');
}

TEST(grid_undo_leaves_other_cells) {
	Fix f;
	f.g.place(1, 1);
	f.g.scale(1, 0);
	f.g.erase();
	f.m.write({2, 1, 2, 1}, "XY"); // (2,1) is in the rect, (3,1) is not
	f.g.undo();
	CHECK(f.m.text() == "abcdef\nghiYkl\nmnopqr\nstuvwx\n");
}

TEST(grid_undo_cap) {
	Fix f;
	f.g.place(0, 0);
	f.g.type('Z');
	f.g.place(1, 0);
	for (int i = 0; i < 1000; i++)
		f.g.type(char('a' + i % 26));
	for (int i = 0; i < 1000; i++)
		f.g.undo();
	CHECK(f.g.grid.at(1, 0) == 'b'); // the original
	CHECK(f.g.grid.at(0, 0) == 'Z');
	f.g.undo(); // the oldest step was dropped
	CHECK(f.g.grid.at(0, 0) == 'Z');
	CHECK(f.g.grid.at(1, 0) == 'b');
}

TEST(grid_undo_byte_cap) {
	// Pasting a full 256x256 grid holds 128 KB per step: far fewer than 1000 steps fit in kMaxUndoBytes.
	Fix f("a\n");
	f.g.resize(256, 256);
	const int steps = 400;
	for (int i = 0; i < steps; i++) {
		std::string row(256, char('a' + i % 26));
		std::string text;
		for (int y = 0; y < 256; y++)
			text += row + '\n';
		f.g.place(0, 0);
		f.g.paste(text);
	}
	int undone = 0; // consecutive steps differ at (0,0): each undo that has a step to undo changes it
	for (int i = 0; i < steps; i++) {
		char before = f.g.grid.at(0, 0);
		f.g.undo();
		undone += f.g.grid.at(0, 0) != before;
	}
	CHECK(undone > 10); // the newest steps work
	CHECK((size_t)undone <= GridEdit::kMaxUndoBytes / (2 * 256 * 256) + 1);
}

TEST(grid_resize_load_clear_history) {
	Fix f;
	f.g.place(5, 3);
	f.g.type('Z');
	f.g.resize(3, 2);
	CHECK(f.g.grid.w == 3 && f.g.grid.h == 2);
	CHECK(at(f.g, 2, 1, 0, 0));
	f.g.undo();
	CHECK(f.m.width() == 3 && f.g.grid.at(0, 0) == 'a');
	f.g.place(2, 1);
	f.g.type('Q');
	f.g.load("ab\ncd\nef\nxy\nzw\n");
	CHECK(f.g.grid.w == 2 && f.g.grid.h == 5);
	CHECK(at(f.g, 1, 1, 0, 0));
	f.g.undo();
	CHECK(f.g.grid.at(1, 1) == 'd');
	f.g.place(1, 1);
	f.g.scale(0, 3);
	f.g.load("ab\ncd\n");
	CHECK(at(f.g, 1, 1, 0, 0));
	f.g.redo();
	CHECK(f.g.grid.at(1, 1) == 'd');
}

// --- editor/theme.hpp ---

namespace {

// github.com/hundredrabbits/Themes (MIT), verbatim: quote styles and extra ids differ.
const std::string kApollo =
	R"svg(<svg width="96px" height="64px" xmlns="http://www.w3.org/2000/svg" baseProfile="full" version="1.1">
  <rect width='96' height='64'  id='background' fill='#29272b'></rect>
  <!-- Foreground -->
  <circle cx='24' cy='24' r='8' id='f_high' fill='#ffffff'></circle>
  <circle cx='40' cy='24' r='8' id='f_med' fill='#e47464'></circle>
  <circle cx='56' cy='24' r='8' id='f_low' fill='#66606b'></circle>
  <circle cx='72' cy='24' r='8' id='f_inv' fill='#000000'></circle>
  <!-- Background -->
  <circle cx='24' cy='40' r='8' id='b_high' fill='#000000'></circle>
  <circle cx='40' cy='40' r='8' id='b_med' fill='#201e21'></circle>
  <circle cx='56' cy='40' r='8' id='b_low' fill='#322e33'></circle>
  <circle cx='72' cy='40' r='8' id='b_inv' fill='#e47464'></circle>
</svg>)svg";
const std::string kSolarisedLight = R"svg(<svg width="96" height="64" xmlns="http://www.w3.org/2000/svg">
 <g>
  <title>background</title>
  <rect fill="none" id="canvas_background" height="66" width="98" y="-1" x="-1"/>
 </g>
 <g>
  <title>Layer 1</title>
  <!-- Background -->
  <rect fill="#eee8d5" id="background" height="64" width="96"/> <!-- Base 2 -->
  <circle fill="#002b36" id="b_high" r="8" cy="24" cx="24"/>    <!-- Base 03 -->
  <circle fill="#073642" id="b_med" r="8" cy="24" cx="40"/>     <!-- Base 02 -->
  <circle fill="#fdf6e3" id="b_low" r="8" cy="24" cx="56"/>     <!-- Base 3 -->
  <circle fill="#cb4b16" id="b_inv" r="8" cy="24" cx="72"/>     <!-- Orange (Compilment) -->
  <!-- Foreground -->
  <circle fill="#586e75" id="f_high" r="8" cy="40" cx="24"/>    <!-- Base 01-->
  <circle fill="#6c71c4" id="f_med" r="8" cy="40" cx="40"/>     <!-- Violet (Analagous) -->
  <circle fill="#93a1a1" id="f_low" r="8" cy="40" cx="56"/>     <!-- Base 1 -->
  <circle fill="#fdf6e3" id="f_inv" r="8" cy="40" cx="72"/>     <!-- Base 3 -->
  <!-- Tape -->
  <desc id='tape_invert' fill='false'></desc>
  <desc id='tape_unstarted' fill='#dc322f'></desc>
  <desc id='tape_progressing' fill='#cb4b16'></desc>
  <desc id='tape_submitted' fill='#b58900'></desc>
  <desc id='tape_approved' fill='#268bd2'></desc>
  <desc id='tape_done' fill='#2aa198'></desc>
 </g>
<!--
base03:    #002b36  bg (dark)
base02:    #073642
base01:    #586e75  content
base00:    #657b83
base0:     #839496
base1:     #93a1a1
base2:     #eee8d5  bg (light)
base3:     #fdf6e3
yellow:    #b58900  split compliment
orange:    #cb4b16  compliment
red:       #dc322f  triad
magenta:   #d33682  tetrad
violet:    #6c71c4  analagous
blue:      #268bd2  monotone
cyan:      #2aa198  analagous
green:     #859900  tetrad
-->
</svg>)svg";

using editor::Cell;
using editor::cellStyle;

/** cellStyle with the common defaults: playing, no cursor under it, reading 'x'. */
Cell style(char c, uint8_t mark = 0, bool selected = false, bool cursor = false, bool marker = false,
		   char reading = 'x', bool paused = false) {
	return cellStyle(c, mark, selected, cursor, marker, reading, paused);
}

bool is(Cell c, char glyph, int fg, int bg) {
	return c.glyph == glyph && c.fg == fg && c.bg == bg;
}

} // namespace

TEST(theme_from_hex) {
	uint32_t v = 7;
	CHECK(editor::fromHex("#abc", v) && v == 0xaabbcc);
	CHECK(editor::fromHex("#AbCdEf", v) && v == 0xabcdef);
	v = 7;
	for (const char* bad : {"none", "rgb(1,2,3)", "#abcd", "abc", "#abcdeg", "", "#"})
		CHECK(!editor::fromHex(bad, v));
	CHECK(v == 7);
	CHECK(editor::hex(0x00ff10) == "#00ff10");
}

TEST(theme_parse_files) {
	editor::Theme t;
	CHECK(editor::parseTheme(kApollo, t));
	const uint32_t apollo[] = {0x29272b, 0xffffff, 0xe47464, 0x66606b, 0x000000,
							   0x000000, 0x201e21, 0x322e33, 0xe47464};
	for (int i = 0; i < editor::kSlots; i++)
		CHECK(t.c[i] == apollo[i]);
	CHECK(editor::parseTheme(kSolarisedLight, t));
	const uint32_t solarised[] = {0xeee8d5, 0x586e75, 0x6c71c4, 0x93a1a1, 0xfdf6e3,
								  0x002b36, 0x073642, 0xfdf6e3, 0xcb4b16};
	for (int i = 0; i < editor::kSlots; i++)
		CHECK(t.c[i] == solarised[i]);
}

TEST(theme_parse_rejects) {
	editor::Theme t = editor::kPresets[1].theme;
	size_t at = kApollo.find("<circle cx='72' cy='40'"); // b_inv's
	std::string missing = kApollo;
	missing.replace(at, kApollo.find('\n', at) - at, "");
	CHECK(!editor::parseTheme(missing, t));
	std::string none = kApollo;
	none.replace(none.find("fill='#e47464'></circle>\n</svg>"), 14, "fill='none'");
	CHECK(!editor::parseTheme(none, t));
	std::string canvas = kSolarisedLight; // only canvas_background (fill none) left of the background
	canvas.replace(canvas.find("id=\"background\""), 15, "id=\"other\"");
	CHECK(!editor::parseTheme(canvas, t));
	CHECK(t == editor::kPresets[1].theme);
}

TEST(theme_presets) {
	CHECK(std::string(editor::kPresets[0].name) == "Patchisserie");
	for (const editor::Preset& p : editor::kPresets)
		for (int i = 0; i < editor::kSlots; i++) {
			uint32_t v = 0;
			CHECK(editor::fromHex(editor::hex(p.theme.c[i]), v) && v == p.theme.c[i]);
		}
}

TEST(theme_cell_style) {
	using namespace editor;
	using lattice::kInput, lattice::kOutput, lattice::kHasteInput, lattice::kLock;
	CHECK(is(style('a', kInput, true), 'a', kFInv, kBInv));               // selected beats everything,
	CHECK(is(style('a', 0, true, false, false, 'a'), 'a', kFInv, kBInv)); // the reader,
	CHECK(is(style('*', 0, true), '*', kFInv, kBInv));
	CHECK(is(style('a', 0, false, false, false, 'a'), 'a', kBInv, -1)); // the reader,
	CHECK(is(style('*', 0, false, false, false, '*'), '*', kBInv, -1)); // beats '*'
	CHECK(is(style('a', 0, false, false, false, 'a', false), 'a', kBInv, -1));
	CHECK(is(style('a', kLock, false, false, false, 'a'), 'a', kFMed, -1));    // but not locked
	CHECK(is(style('.', 0, false, false, false, '.'), 0, kFLow, -1));          // nor a dot
	CHECK(is(style('*', kOutput | kInput), '*', kBHigh, -1));                  // '*' beats the ports
	CHECK(is(style('*', kLock), '*', kFMed, -1));                              // locked: not the bang
	CHECK(is(style('a', kOutput | kHasteInput | kInput), 'a', kFLow, kBHigh)); // output beats haste
	CHECK(is(style('a', kHasteInput | kInput), 'a', kBMed, -1));               // beats input
	CHECK(is(style('a', kInput), 'a', kBHigh, -1));
	CHECK(is(style('a', kInput | kLock), 'a', kBHigh, -1)); // ports beat the lock
	CHECK(is(style('A'), 'A', kFLow, kBMed));               // an operator
	CHECK(is(style('A', kLock), 'A', kFMed, -1));           // locked: plain lock
	CHECK(is(style('a', kLock), 'a', kFMed, -1));
	CHECK(is(style('a'), 'a', kFLow, -1));
	CHECK(is(style('.'), 0, kFLow, -1));                                       // invisible
	CHECK(is(style('.', 0, false, false, true), '+', kFLow, -1));              // a marker
	CHECK(is(style('.', kLock), '.', kFMed, -1));                              // locked
	CHECK(is(style('.', kInput), '.', kBHigh, -1));                            // a port
	CHECK(is(style('.', 0, true), '.', kFInv, kBInv));                         // selected
	CHECK(is(style('.', 0, true, true), '@', kFInv, kBInv));                   // the cursor, playing
	CHECK(is(style('.', 0, true, true, false, '.', true), '~', kFInv, kBInv)); // stopped
	// dots: only an invisible '.' changes
	auto dotted = [](char c, uint8_t mark = 0, bool selected = false, bool cursor = false, bool marker = false) {
		return cellStyle(c, mark, selected, cursor, marker, 'x', false, true);
	};
	CHECK(is(dotted('.'), '.', kFLow, -1));
	CHECK(is(dotted('.', 0, false, false, true), '+', kFLow, -1));
	CHECK(is(dotted('.', kLock), '.', kFMed, -1));
	CHECK(is(dotted('.', kInput), '.', kBHigh, -1));
	CHECK(is(dotted('.', 0, true), '.', kFInv, kBInv));
	CHECK(is(dotted('.', 0, true, true), '@', kFInv, kBInv));
	CHECK(is(dotted('a'), 'a', kFLow, -1));
}

TEST(operator_guide_matches_orca_c) {
	std::ifstream f("vendor/orca-c/tui_main.c"); // from the repo root, like make test
	std::stringstream ss;
	ss << f.rdbuf();
	std::string src = ss.str();
	size_t at = src.find("Guide_item items[]");
	CHECK(at != std::string::npos);
	size_t end = src.find("};", at);
	std::string body = src.substr(at, end - at);
	// {'X', "name", "desc"} entries, a commented-out one included, except the `// {'*', "self", ...}` line.
	static const std::regex kItem("\\{'(.)', \"([^\"]*)\", \"([^\"]*)\"\\}");
	std::vector<std::string> want;
	for (std::sregex_iterator it(body.begin(), body.end(), kItem), e; it != e; ++it)
		if ((*it)[2] != "self")
			want.push_back((*it)[1].str() + "|" + (*it)[2].str() + "|" + (*it)[3].str());
	std::vector<std::string> have;
	for (const auto& o : editor::kOperators)
		have.push_back(std::string(1, o.glyph) + "|" + o.name + "|" + o.desc);
	CHECK(want.size() == 34);
	CHECK(have == want);
}
