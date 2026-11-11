// Completions and hover docs (src/editor/complete.hpp): the identifier before the cursor, ranking, accepting, and the
// popup's open / filter / close logic.
#include <string>

#include "check.hpp"
#include "editor/complete.hpp"

using editor::Completion;
using editor::Doc;
using editor::Entry;
using editor::Match;

namespace {

const std::vector<Entry>& docs() {
	static const std::vector<Entry> d = {
		{"lpf", "lpf(frequency)", "Low-pass filter.", "s(\"bd\").lpf(500)", "strudel", {"cutoff", "ctf", "lp"}},
		{"lpq", "lpq(q)", "", "", "strudel", {"resonance"}},
		{"note", "note()", "", "", "strudel", {}},
		{"noise", "noise()", "", "", "strudel", {}},
		{"noise", "noise(scale = 10, offset = 0.1)", "Hydra source", "", "hydra", {}},
		{"Note", "Note()", "", "", "strudel", {}},
		{"$x", "$x()", "", "", "strudel", {}},
		{"s", "s(sound)", "", "", "strudel", {"sound"}},
	};
	return d;
}

/** The identifier before the end of `code` (or "-": none). */
std::string prefix(const std::string& code) {
	Doc d(code);
	d.setCursor(code.size());
	size_t b = 0;
	return editor::identBefore(d, d.cursor(), b) ? code.substr(b) : "-";
}

/** Labels joined by spaces, a Hydra one as "label/h". */
std::string labels(const std::vector<Match>& ms) {
	std::string out;
	for (const Match& m : ms)
		out += (out.empty() ? "" : " ") + m.label + (m.entry->hydra() ? "/h" : "");
	return out;
}

/** A doc with the cursor at its end, `typed` typed char by char as the view does (each one updates the popup). */
Doc typing(Completion& c, const std::string& before, const std::string& typed) {
	Doc d(before);
	d.setCursor(before.size());
	for (char ch : typed) {
		d.type(std::string(1, ch));
		c.update(d, docs(), editor::isIdent(ch));
	}
	return d;
}

} // namespace

TEST(complete_prefix_before_the_cursor) {
	CHECK(prefix("note(\"c\").lp") == "lp"); // after '.'
	CHECK(prefix("x = no") == "no");
	CHECK(prefix("x.") == "");      // empty: Ctrl+Space offers everything
	CHECK(prefix("$x") == "$x");    // $ is an identifier char
	CHECK(prefix("a = 12") == "-"); // a number
	CHECK(prefix("0.25") == "-");
	CHECK(prefix("éno") == "-"); // after a non-ASCII letter: not an ASCII identifier
	CHECK(prefix("// lp") == "-");
	CHECK(prefix("/* lp") == "-");
	CHECK(prefix("/* x */ lp") == "lp");
	CHECK(prefix("s(\"bd lp") == "-"); // unclosed string
	CHECK(prefix("s('bd lp") == "-");
	CHECK(prefix("s(`a\nlp") == "-");  // template strings span lines
	CHECK(prefix("s(\"bd\")") == "");  // after a closed string
	CHECK(prefix("// c\nlp") == "lp"); // the comment ended with its line
	Doc d("note(\"c e\").lpf(1)");
	size_t b = 0, e = 0;
	CHECK(editor::identAt(d, 12, b, e) && d.text().substr(b, e - b) == "lpf"); // the 'l'
	CHECK(editor::identAt(d, 14, b, e) && b == 12 && e == 15);                 // the 'f'
	CHECK(!editor::identAt(d, 6, b, e));                                       // 'c' in the string
	CHECK(!editor::identAt(d, 15, b, e));                                      // '(' then the number 1
	CHECK(!editor::identAt(d, 16, b, e));
}

TEST(complete_ranking_names_synonyms_case) {
	CHECK(labels(editor::complete(docs(), "lp")) == "lp lpf lpq");
	CHECK(labels(editor::complete(docs(), "cut")) == "cutoff");                 // a synonym is a completion of its own
	CHECK(labels(editor::complete(docs(), "no")) == "note noise noise/h Note"); // same case, then any; shorter first
	CHECK(labels(editor::complete(docs(), "No")) == "Note note noise noise/h");
	CHECK(labels(editor::complete(docs(), "so")) == "sound");
	CHECK(labels(editor::complete(docs(), "xyz")) == "");
	CHECK(editor::complete(docs(), "", 3).size() == 3); // capped
	CHECK(editor::complete(docs(), "").size() == 13);
	CHECK(labels(editor::lookup(docs(), "noise")) == "noise noise/h");
	CHECK(labels(editor::lookup(docs(), "ctf")) == "ctf");
	CHECK(editor::lookup(docs(), "ctf")[0].entry->name == "lpf");
	CHECK(editor::lookup(docs(), "lp").size() == 1); // not lpf, lpq
	CHECK(editor::lookup(docs(), "nois").empty());
}

TEST(complete_accept_is_one_undo_step) {
	Completion c;
	Doc d = typing(c, "s(\"bd\").", "cu");
	CHECK(c.open && labels(c.items) == "cutoff" && c.from == 8);
	c.accept(d);
	CHECK(!c.open && d.text() == "s(\"bd\").cutoff" && d.cursor() == d.text().size());
	CHECK(d.undo() && d.text() == "s(\"bd\").cu" && d.cursor() == 10); // the typed prefix back
	CHECK(d.undo() && d.text() == "s(\"bd\").");                       // then the typing
	d.setText("x.lpf");
	d.setCursor(4); // mid-identifier: only [from, cursor) is replaced
	d.complete(2, "lpq");
	CHECK(d.text() == "x.lpqf" && d.cursor() == 5);
	d.complete(2, "lpq"); // already there: no step
	CHECK(d.undo() && d.text() == "x.lpf" && !d.undo());
}

TEST(complete_popup_opens_filters_closes) {
	Completion c;
	Doc d = typing(c, "", "l");
	CHECK(!c.open); // one char: not yet
	d = typing(c, "", "lp");
	CHECK(c.open && labels(c.items) == "lp lpf lpq" && c.sel == 0);
	c.move(-1, true); // Up from the first wraps
	CHECK(c.sel == 2);
	c.move(1, true);
	CHECK(c.sel == 0);
	c.move(1, true);
	d.type("f"); // typing filters, the selected one (lpf) kept
	c.update(d, docs(), true);
	CHECK(c.open && labels(c.items) == "lpf" && c.sel == 0);
	d.backspace(); // Backspace refilters, still on lpf
	c.update(d, docs(), false);
	CHECK(c.open && labels(c.items) == "lp lpf lpq" && c.sel == 1);
	d.backspace();
	c.update(d, docs(), false);
	CHECK(c.open && labels(c.items) == "lp lpf lpq" && c.sel == 1); // "l"
	d.backspace();                                                  // empty: closes (opened by typing)
	c.update(d, docs(), false);
	CHECK(!c.open);

	d = typing(c, "x.", "lp");
	d.type("("); // not an identifier char: closes
	c.update(d, docs(), false);
	CHECK(!c.open);
	d = typing(c, "x.", "lp");
	d.left(false); // the cursor still in the identifier: refilters
	c.update(d, docs(), false);
	CHECK(c.open && c.from == 2 && labels(c.items) == "lp lpf lpq"); // "l"
	d.home(false);                                                   // out of it
	c.update(d, docs(), false);
	CHECK(!c.open);
	d = typing(c, "x.", "lp");
	d.select(0, 2); // a selection closes
	c.update(d, docs(), false);
	CHECK(!c.open);
	d = typing(c, "x.", "zz"); // nothing matches: stays closed
	CHECK(!c.open);
	d = typing(c, "// ", "lp"); // in a comment
	CHECK(!c.open);

	// Ctrl+Space: any length, empty too; it stays open down to an empty identifier.
	d = Doc("x.l");
	d.setCursor(3);
	c.request(d, docs());
	CHECK(c.open && c.from == 2 && labels(c.items) == "lp lpf lpq");
	d.backspace();
	c.update(d, docs(), false);
	CHECK(c.open && c.items.size() == 13);
	d.backspace(); // past its start
	c.update(d, docs(), false);
	CHECK(!c.open);
	d = Doc("\"lp");
	d.setCursor(3);
	c.request(d, docs()); // not in a string
	CHECK(!c.open);
}

TEST(complete_popup_scrolls_with_the_selection) {
	std::vector<Entry> many;
	for (int i = 0; i < 20; i++)
		many.push_back({"fn" + std::string(1, (char)('a' + i)), "", "", "", "strudel", {}});
	Completion c;
	Doc d("fn");
	d.setCursor(2);
	c.request(d, many);
	CHECK(c.items.size() == 20 && c.top == 0);
	c.move(9, false);
	CHECK(c.sel == 9 && c.top == 2);
	c.move(100, false); // PageDown stops at the end
	CHECK(c.sel == 19 && c.top == 12);
	c.move(1, true); // Down wraps to the first
	CHECK(c.sel == 0 && c.top == 0);
	c.move(-1, true);
	CHECK(c.sel == 19 && c.top == 12);
	c.move(-100, false);
	CHECK(c.sel == 0 && c.top == 0);
}
