// editor::Doc (src/editor/doc.hpp): editing, UTF-8, undo, words, lines, tokens, and where the playing spans are; and
// the engine's bounded highlight store.
#include <string>

#include "check.hpp"
#include "editor/doc.hpp"
#include "engine.hpp"

using editor::Doc;
using editor::Token;

namespace {

/** The text between UTF-16 offsets [start, end) of code `id`, where it is now; "-" if it is gone. */
std::string at(const Doc& d, uint32_t id, int start, int end) {
	size_t b = 0, e = 0;
	return d.locate(id, start, end, b, e) ? d.text().substr(b, e - b) : "-";
}

/** Tokens as "kind:text" joined by spaces (kinds: C S N K F L O). */
std::string tokens(const std::string& code) {
	std::string out;
	for (const Token& t : editor::tokenize(code))
		out += std::string(out.empty() ? "" : " ") + "CSNKFLO"[t.kind] + ":" + code.substr(t.begin, t.end - t.begin);
	return out;
}

} // namespace

TEST(doc_insert_delete_replace_selection) {
	Doc d("note(\"c e\")");
	d.setCursor(9);
	d.type(" g");
	CHECK(d.text() == "note(\"c e g\")" && d.cursor() == 11);
	d.select(6, 7); // "c"
	CHECK(d.selection() == "c");
	d.type("d");
	CHECK(d.text() == "note(\"d e g\")" && !d.hasSelection() && d.cursor() == 7);
	d.select(5, 12);
	d.backspace();
	CHECK(d.text() == "note()" && d.cursor() == 5);
	d.insert("\"a\nb\"");
	CHECK(d.text() == "note(\"a\nb\")" && d.cursor() == 10);
	d.setCursor(0);
	d.del();
	CHECK(d.text() == "ote(\"a\nb\")");
	d.selectAll();
	d.del();
	CHECK(d.text().empty() && d.cursor() == 0);
	d.backspace(); // nothing to delete: no undo step
	d.del();
	CHECK(d.undo() && d.text() == "ote(\"a\nb\")");
	// Shift-selection: the anchor stays, left/right without Shift collapse to the selection's ends.
	d.setCursor(3);
	d.right(true);
	d.right(true);
	CHECK(d.anchor() == 3 && d.cursor() == 5 && d.selection() == "(\"");
	d.left(false);
	CHECK(d.cursor() == 3 && !d.hasSelection());
	d.select(3, 5);
	d.right(false);
	CHECK(d.cursor() == 5 && !d.hasSelection());
	CHECK(editor::clean("a\r\nb\rc\td") == "a\nb\nc  d");
}

TEST(doc_utf8_never_splits_a_codepoint) {
	Doc d("aé🎵b"); // a, é (2 bytes), 🎵 (4 bytes), b
	CHECK(d.text().size() == 8);
	d.setCursor(0);
	d.right(false);
	CHECK(d.cursor() == 1);
	d.right(false);
	CHECK(d.cursor() == 3);
	d.right(false);
	CHECK(d.cursor() == 7);
	d.left(false);
	CHECK(d.cursor() == 3);
	d.setCursor(5); // inside 🎵: back to its start
	CHECK(d.cursor() == 3);
	d.setCursor(7);
	d.backspace(); // one codepoint, all 4 bytes
	CHECK(d.text() == "aéb" && d.cursor() == 3);
	d.backspace();
	CHECK(d.text() == "ab" && d.cursor() == 1);
	d.type(editor::utf8(0x10d)); // č
	d.type(editor::utf8(0x1f3b5));
	CHECK(d.text() == "ač🎵b" && d.cursor() == 7);
	d.setCursor(1);
	d.del();
	CHECK(d.text() == "a🎵b");
	CHECK(d.column(5) == 2 && d.at(0, 2) == 5 && d.at(0, 99) == 6);
}

TEST(doc_undo_redo_coalescing_and_cursor) {
	Doc d("s(\"bd\")");
	d.setCursor(5);
	for (char c : std::string(" sd hh"))
		d.type(std::string(1, c));
	CHECK(d.text() == "s(\"bd sd hh\")");
	CHECK(d.undo()); // the whole run of typing at once
	CHECK(d.text() == "s(\"bd\")" && d.cursor() == 5);
	CHECK(d.redo() && d.text() == "s(\"bd sd hh\")" && d.cursor() == 11);
	// A cursor move ends the run.
	d.type("x");
	d.left(false);
	d.right(false);
	d.type("y");
	CHECK(d.undo() && d.text() == "s(\"bd sd hhx\")");
	CHECK(d.undo() && d.text() == "s(\"bd sd hh\")");
	// Backspaces in a row coalesce, and undo brings back the selection that was typed over.
	d.setCursor(11);
	d.backspace();
	d.backspace();
	d.backspace();
	CHECK(d.text() == "s(\"bd sd\")");
	CHECK(d.undo() && d.text() == "s(\"bd sd hh\")" && d.cursor() == 11);
	d.select(6, 8);
	d.type("c");
	d.type("p");
	CHECK(d.text() == "s(\"bd cp hh\")");
	CHECK(d.undo() && d.text() == "s(\"bd sd hh\")" && d.anchor() == 6 && d.cursor() == 8);
	// Deletes forward coalesce too; a new edit drops the redo history.
	d.setCursor(3);
	d.del();
	d.del();
	d.del();
	CHECK(d.text() == "s(\"sd hh\")");
	CHECK(d.undo() && d.text() == "s(\"bd sd hh\")" && d.cursor() == 3);
	d.insert("x");
	CHECK(!d.redo());
	// Paste and Enter are steps of their own.
	d.setCursor(0);
	d.insert("a");
	d.insert("b");
	CHECK(d.undo() && d.text().compare(0, 3, "as(") == 0);
	// setText (a patch loads) clears the history.
	d.setText("new");
	CHECK(!d.undo() && !d.redo() && d.cursor() == 0);
}

TEST(doc_words_and_double_click) {
	Doc d("$: s(\"bd sd\").fast(2)  // héllo");
	d.selectWord(7); // in "bd"
	CHECK(d.selection() == "bd");
	d.selectWord(6); // at its first char
	CHECK(d.selection() == "bd");
	d.selectWord(13); // punctuation: that char
	CHECK(d.selection() == ".");
	d.selectWord(21); // the run of spaces
	CHECK(d.selection() == "  ");
	d.selectWord(28); // é counts as a letter
	CHECK(d.selection() == "héllo");
	d.selectWord(d.text().size()); // past the end: the word before
	CHECK(d.selection() == "héllo");
	d.setCursor(0);
	d.wordRight(false);
	CHECK(d.cursor() == 1); // "$"
	d.wordRight(false);
	CHECK(d.cursor() == 4); // "s"
	d.wordRight(true);
	CHECK(d.cursor() == 8 && d.selection() == "(\"bd");
	d.setCursor(19);
	d.wordLeft(false);
	CHECK(d.cursor() == 14); // "fast"
	d.wordLeft(false);
	CHECK(d.cursor() == 9); // "sd"
	d.setCursor(11);
	d.backspace(true);
	CHECK(d.text() == "$: s(\"bd \").fast(2)  // héllo");
	d.del(true);
	CHECK(d.text() == "$: s(\"bd (2)  // héllo");
	Doc e("a\n\nb");
	e.selectWord(2); // an empty line: nothing
	CHECK(!e.hasSelection() && e.cursor() == 2);
}

TEST(doc_select_line_for_triple_click) {
	Doc d("ab\ncd\nef");
	d.selectLine(1); // a middle line: from its start to the next line's start, newline included
	CHECK(d.anchor() == 3 && d.cursor() == 6 && d.selection() == "cd\n");
	d.selectLine(2); // the last line, no newline: to the end of the document
	CHECK(d.anchor() == 6 && d.cursor() == 8 && d.selection() == "ef");
	d.setText("a\n\nb");
	d.selectLine(1); // an empty line: just its newline
	CHECK(d.anchor() == 2 && d.cursor() == 3 && d.selection() == "\n");
	d.setText("é\nñö\nx");
	d.selectLine(1); // non-ASCII: byte offsets
	CHECK(d.anchor() == 3 && d.cursor() == 8 && d.selection() == "ñö\n");
	d.setText("a\nb\nc");
	d.selectLine(1);
	d.toggleComment(); // only that line (the selection ends at column 0 of the next)
	CHECK(d.text() == "a\n// b\nc" && d.selection() == "b\n" && d.cursor() == 7);
}

TEST(doc_lines_columns_and_vertical_moves) {
	Doc d("first line\n\nthird\n");
	CHECK(d.lineCount() == 4);
	CHECK(d.lineStart(1) == 11 && d.lineEnd(1) == 11 && d.lineStart(2) == 12 && d.lineEnd(2) == 17);
	CHECK(d.lineEnd(3) == 18 && d.lineStart(3) == 18);
	CHECK(d.lineOf(0) == 0 && d.lineOf(10) == 0 && d.lineOf(11) == 1 && d.lineOf(17) == 2 && d.lineOf(18) == 3);
	CHECK(d.column(15) == 3 && d.at(2, 3) == 15 && d.at(1, 5) == 11 && d.at(9, 0) == 18);
	d.setCursor(7); // column 7
	d.vertical(1, false);
	CHECK(d.cursor() == 11); // the empty line
	d.vertical(1, false);
	CHECK(d.cursor() == 17); // "third" is shorter: its end
	d.vertical(-2, false);
	CHECK(d.cursor() == 7); // back at column 7
	d.vertical(-1, true);
	CHECK(d.cursor() == 0 && d.anchor() == 7); // above the first line: its start
	d.setCursor(13);
	d.vertical(9, false);
	CHECK(d.cursor() == 18);
	d.setCursor(14);
	d.home(false);
	CHECK(d.cursor() == 12);
	d.end(true);
	CHECK(d.cursor() == 17 && d.selection() == "third");
}

TEST(doc_newline_keeps_indentation) {
	Doc d("stack(\n  s(\"bd\"),");
	d.setCursor(d.text().size());
	d.newline();
	CHECK(d.text() == "stack(\n  s(\"bd\"),\n  " && d.cursor() == d.text().size());
	d.type("x");
	d.setCursor(8); // inside the indentation: only what is before the cursor
	d.newline();
	CHECK(d.text().compare(0, 12, "stack(\n \n  s") == 0);
	CHECK(std::string(editor::kIndent) == "  ");
}

TEST(doc_toggle_comment) {
	Doc d("a\n  b");
	d.toggleComment(); // no selection: the cursor's line
	CHECK(d.text() == "// a\n  b" && d.cursor() == 3);
	d.toggleComment();
	CHECK(d.text() == "a\n  b" && d.cursor() == 0);
	// Several lines: the markers line up at the least indentation; blank lines stay.
	d.setText("    x\n  y\n   \n    z");
	d.selectAll();
	d.toggleComment();
	CHECK(d.text() == "  //   x\n  // y\n   \n  //   z");
	d.toggleComment(); // all commented: off again
	CHECK(d.text() == "    x\n  y\n   \n    z");
	// Mixed: all get commented (the commented one twice).
	d.setText("// a\nb");
	d.selectAll();
	d.toggleComment();
	CHECK(d.text() == "// // a\n// b");
	// With and without the space after //.
	d.setText("//a\n//  b\n  // c");
	d.selectAll();
	d.toggleComment();
	CHECK(d.text() == "a\n b\n  c");
	// A selection ending at column 0 leaves that line out.
	d.setText("a\nb\nc");
	d.select(0, 4);
	d.toggleComment();
	CHECK(d.text() == "// a\n// b\nc" && d.anchor() == 3 && d.cursor() == 10);
	d.select(0, 0);
	d.toggleComment(); // the first line is "// a": uncommented
	CHECK(d.text() == "a\n// b\nc");
	// A lone blank line gets the marker at its end; blank ones among others stay.
	d.setText("  \nx");
	d.toggleComment();
	CHECK(d.text() == "  // \nx" && d.cursor() == 0);
	d.setText("  \n\n");
	d.select(0, 4);
	d.toggleComment();
	CHECK(d.text() == "  \n\n");
	// The cursor inside the removed marker clamps to where it was; the selection follows its text.
	d.setText("// ab");
	d.setCursor(1);
	d.toggleComment();
	CHECK(d.text() == "ab" && d.cursor() == 0);
	d.setText("  ab\n  cd");
	d.select(3, 8);
	d.toggleComment();
	CHECK(d.text() == "  // ab\n  // cd" && d.selection() == "b\n  // c");
	// One undo step, never joined with typing; it restores text and selection.
	d.setText("ab");
	d.setCursor(2);
	d.type("c");
	d.select(0, 3);
	d.toggleComment();
	d.type("d");
	CHECK(d.undo() && d.text() == "// abc" && d.undo() && d.text() == "abc" && d.anchor() == 0 && d.cursor() == 3);
	d.setText("a\nb");
	d.select(1, 3);
	d.toggleComment();
	CHECK(d.undo() && d.text() == "a\nb" && d.anchor() == 1 && d.cursor() == 3);
	CHECK(d.redo() && d.text() == "// a\n// b");
	// Non-ASCII.
	d.setText("  žluť\n  é");
	d.selectAll();
	d.toggleComment();
	CHECK(d.text() == "  // žluť\n  // é" && d.cursor() == d.text().size());
	d.toggleComment();
	CHECK(d.text() == "  žluť\n  é" && d.cursor() == d.text().size());
}

TEST(doc_tokenizer_strudel_code) {
	CHECK(tokens("$: s(\"bd*2 [sd hh]\").bank('tr909') // drums") ==
		  "L:$: F:s S:\"bd*2 [sd hh]\" F:bank S:'tr909' C:// drums");
	CHECK(tokens("note(`<c e\n g>`).gain(.5).lpf(1e3)") == "F:note S:`<c e\n g>` F:gain N:.5 F:lpf N:1e3");
	CHECK(tokens("const x = await foo /* a\nb */ ? 0x1f : -2") ==
		  "K:const O:= K:await C:/* a\nb */ O:? N:0x1f O:: O:- N:2");
	CHECK(tokens("_$: n(\"0 2\".add(1))") == "L:_$: F:n S:\"0 2\" F:add N:1");
	CHECK(tokens("  d1: s(\"bd\")\nx ? a : b") == "L:d1: F:s S:\"bd\" O:? O::");
	CHECK(tokens("s(\"bd \\\" sd\") 'open\nnext") == "F:s S:\"bd \\\" sd\" S:'open");
	CHECK(tokens("x => y && z") == "O:=> O:&&");
	CHECK(tokens("\"é\" `unterminated") == "S:\"é\" S:`unterminated");
	Doc d("s(\"bd\")");
	CHECK(d.tokens().size() == 2);
	d.setCursor(7);
	d.type(" // x");
	CHECK(d.tokens().size() == 3 && d.tokens()[2].kind == Token::Comment);
}

TEST(doc_spans_utf16_to_utf8) {
	// JS offsets count UTF-16 units: é is 1 (2 bytes), 🎵 is 2 (4 bytes).
	std::string code = "// é🎵\ns(\"bd sd\")";
	Doc d(code);
	d.evaluated(1);
	// "bd" in UTF-16: "// é🎵\n" is 7 units, then s(" -> 10.
	CHECK(at(d, 1, 10, 12) == "bd");
	CHECK(at(d, 1, 13, 15) == "sd");
	CHECK(at(d, 1, 3, 4) == "é");
	CHECK(at(d, 1, 4, 6) == "🎵");
	CHECK(at(d, 1, 0, 0) == "-");   // empty
	CHECK(at(d, 1, 10, 99) == "-"); // beyond the code
	CHECK(at(d, 2, 10, 12) == "-"); // no such code
}

TEST(doc_spans_through_edits) {
	Doc d("s(\"bd sd\")");
	d.evaluated(1);
	CHECK(at(d, 1, 3, 5) == "bd" && at(d, 1, 6, 8) == "sd");
	// Before a span: it moves along, also with a multi-byte char.
	d.setCursor(0);
	d.type(editor::utf8(0x1f3b5));
	d.insert("$: ");
	CHECK(d.text() == "🎵$: s(\"bd sd\")" && at(d, 1, 3, 5) == "bd" && at(d, 1, 6, 8) == "sd");
	// Right before / after a span: it keeps its chars.
	size_t bd = d.text().find("bd");
	d.setCursor(bd);
	d.type("x");
	d.setCursor(bd + 3);
	d.type("y");
	CHECK(d.text() == "🎵$: s(\"xbdy sd\")" && at(d, 1, 3, 5) == "bd");
	// Inside a span: an insertion grows it, deleting its first or last char drops it.
	size_t sd = d.text().find("sd");
	d.setCursor(sd + 1);
	d.type("n");
	CHECK(at(d, 1, 6, 8) == "snd");
	d.select(sd, sd + 1);
	d.del();
	CHECK(at(d, 1, 6, 8) == "-" && at(d, 1, 3, 5) == "bd");
	// After it: nothing changes. Undo brings the chars back, as new chars: still dropped.
	d.setCursor(d.text().size());
	d.insert("\n// more");
	CHECK(at(d, 1, 3, 5) == "bd");
	d.undo();
	d.undo();
	CHECK(at(d, 1, 6, 8) == "-");
	// Selecting across a span and typing over it drops it.
	d.select(d.text().find("x"), d.text().find("y") + 1);
	d.type("z");
	CHECK(at(d, 1, 3, 5) == "-");
	// A new evaluate maps from its own text.
	d.evaluated(2);
	d.report(2, true, 0);
	CHECK(at(d, 2, 0, 2) == "🎵" && at(d, 2, 2, 4) == "$:");
	CHECK(at(d, 1, 3, 5) == "-"); // the old code is gone
	// setText (a patch loads): no evaluated code.
	d.setText("s(\"bd\")");
	CHECK(at(d, 2, 0, 1) == "-");
}

TEST(doc_reports_keep_the_playing_code_and_map_the_error_line) {
	Doc d("s(\"bd\")");
	d.evaluated(1);
	d.report(1, true, 0);
	d.setCursor(0);
	d.insert("x\n");
	d.evaluated(2); // a syntax error on line 2
	d.report(2, false, 2);
	CHECK(at(d, 1, 3, 5) == "bd"); // 1 still plays
	CHECK(d.errorLine() == 1);
	d.setCursor(0);
	d.insert("// top\n");
	CHECK(d.errorLine() == 2); // the error stays on its line
	d.report(2, false, 2);     // the same report again: nothing changes
	CHECK(d.errorLine() == 2 && at(d, 1, 3, 5) == "bd");
	// Superseded before the JS thread took it: 3 never reports, 4 does.
	d.evaluated(3);
	d.evaluated(4);
	d.report(4, true, 0);
	CHECK(d.errorLine() == -1 && at(d, 1, 3, 5) == "-" && at(d, 3, 0, 1) == "-");
	CHECK(at(d, 4, 0, 6) == "// top");
	// Deleting the error's line (all its chars) hides the marker.
	d.evaluated(5);
	d.report(5, false, 1);
	CHECK(d.errorLine() == 0);
	d.select(0, 7);
	d.del();
	CHECK(d.errorLine() == -1);
	// A runtime error has no line.
	d.evaluated(6);
	d.report(6, false, 0);
	CHECK(d.errorLine() == -1);
	// A stuck JS thread: at most kMaxEvaluated versions, the playing one kept.
	for (uint32_t id = 7; id < 30; id++)
		d.evaluated(id);
	CHECK(at(d, 4, 7, 8) == "x"); // "// top\nx..." when 4 was evaluated
	CHECK(at(d, 23, 0, 1) == "x" && at(d, 29, 0, 1) == "x");
	CHECK(at(d, 22, 0, 1) == "-");
}

TEST(highlights_bounded) {
	filo::Highlights h;
	for (int i = 0; i < 2000; i++)
		h.push({i, i + 100, 1, i, i + 1}, 0);
	CHECK(h.size() == filo::Highlights::kMax); // full: the new ones are dropped
	std::vector<filo::Highlight> out;
	h.playing(50, out);
	CHECK(out.size() == 51 && out.front().from == 0 && out.back().from == 50);
	h.push({5000, 5100, 2, 0, 1}, 1000);                 // full: the ones over by 1000 go first
	CHECK(h.size() == filo::Highlights::kMax - 901 + 1); // to = i + 100 <= 1000: i = 0..900

	h.playing(5050, out);
	CHECK(out.size() == 1 && out[0].id == 2);
	h.playing(4000, out);
	CHECK(out.empty());
	h.clear();
	CHECK(h.size() == 0);
}
