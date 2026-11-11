#pragma once
// Filo's code editor model (M3): UTF-8 text with a cursor and a selection, edits with undo/redo, lines/columns and
// words, a JS-ish tokenizer for the syntax colours, and where the code spans of what plays (and a syntax error's line)
// are in the text being edited. Pure logic, no Rack or NanoVG: src/editor/view.cpp draws it, test/test_doc.cpp tests
// it. Offsets are bytes, always on codepoint boundaries; columns count codepoints.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace editor {

// --- UTF-8 ---

inline bool isTail(char c) {
	return ((unsigned char)c & 0xc0) == 0x80;
}
/** Start of the codepoint after the one at i. */
inline size_t nextChar(const std::string& s, size_t i) {
	if (i >= s.size())
		return s.size();
	for (i++; i < s.size() && isTail(s[i]); i++) {
	}
	return i;
}
/** Start of the codepoint before i. */
inline size_t prevChar(const std::string& s, size_t i) {
	if (i == 0)
		return 0;
	for (i--; i > 0 && isTail(s[i]); i--) {
	}
	return i;
}
inline std::string utf8(uint32_t c) {
	std::string s;
	if (c < 0x80)
		s += (char)c;
	else if (c < 0x800)
		s += (char)(0xc0 | c >> 6), s += (char)(0x80 | (c & 0x3f));
	else if (c < 0x10000)
		s += (char)(0xe0 | c >> 12), s += (char)(0x80 | (c >> 6 & 0x3f)), s += (char)(0x80 | (c & 0x3f));
	else
		s += (char)(0xf0 | c >> 18), s += (char)(0x80 | (c >> 12 & 0x3f)), s += (char)(0x80 | (c >> 6 & 0x3f)),
			s += (char)(0x80 | (c & 0x3f));
	return s;
}
/** Identifier characters, and what a double-click selects (non-ASCII counts as a letter). */
inline bool isWord(char c) {
	unsigned char u = (unsigned char)c;
	return std::isalnum(u) || u == '_' || u == '$' || u >= 0x80;
}

/** The editor's indentation unit: Tab inserts it. */
constexpr const char* kIndent = "  ";

/** Pasted text: CR LF and CR to LF, tabs to kIndent (columns are codepoints, a tab is one). */
inline std::string clean(const std::string& s) {
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '\r') {
			out += '\n';
			if (i + 1 < s.size() && s[i + 1] == '\n')
				i++;
		} else if (s[i] == '\t')
			out += kIndent;
		else
			out += s[i];
	}
	return out;
}

// --- Syntax colours ---

struct Token {
	enum Kind : uint8_t { Comment, String, Number, Keyword, Call, Label, Operator };
	size_t begin, end;
	Kind kind;
};

/** JS-ish tokens, sorted, without plain text (identifiers, punctuation, space): // and block comments, '' "" and ``
strings (`` and block comments span lines, the others end at the line), numbers, keywords, identifiers before '('
(calls), `name:` at a line start (Strudel's `$:` blocks), operators. ponytail: no regex literals, ${} is string. */
inline std::vector<Token> tokenize(const std::string& s) {
	static const char* const keywords[] = {
		"async",  "await", "break",      "case",    "catch",     "class",   "const", "continue", "default",  "delete",
		"do",     "else",  "export",     "extends", "false",     "finally", "for",   "from",     "function", "if",
		"import", "in",    "instanceof", "let",     "new",       "null",    "of",    "return",   "switch",   "this",
		"throw",  "true",  "try",        "typeof",  "undefined", "var",     "void",  "while",    "yield"};
	auto isOperator = [](char c) { return c && std::strchr("+-*/%=<>!&|^~?:", c); };
	std::vector<Token> out;
	size_t n = s.size(), i = 0;
	bool lineStart = true; // only space since the last newline
	while (i < n) {
		char c = s[i];
		size_t b = i;
		if (c == '\n' || c == ' ' || c == '\t' || c == '\r') {
			lineStart = lineStart || c == '\n';
			i++;
			continue;
		}
		bool first = lineStart;
		lineStart = false;
		Token::Kind kind = Token::Comment;
		if (c == '/' && i + 1 < n && (s[i + 1] == '/' || s[i + 1] == '*')) {
			size_t e = s[i + 1] == '/' ? s.find('\n', i) : s.find("*/", i + 2);
			i = e == std::string::npos ? n : s[i + 1] == '/' ? e : e + 2;
			kind = Token::Comment;
		} else if (c == '"' || c == '\'' || c == '`') {
			for (i++; i < n && s[i] != c && (c == '`' || s[i] != '\n'); i++)
				if (s[i] == '\\' && i + 1 < n)
					i++; // an escape (a multi-byte char's tail bytes are never quotes)
			if (i < n && s[i] == c)
				i++;
			kind = Token::String;
		} else if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < n && std::isdigit((unsigned char)s[i + 1]))) {
			for (i++; i < n && (std::isalnum((unsigned char)s[i]) || s[i] == '.' || s[i] == '_'); i++)
				if ((s[i] == 'e' || s[i] == 'E') && i + 1 < n && (s[i + 1] == '-' || s[i + 1] == '+'))
					i++; // 1e-3
			kind = Token::Number;
		} else if (isWord(c)) {
			while (i < n && isWord(s[i]))
				i++;
			size_t j = i;
			while (j < n && (s[j] == ' ' || s[j] == '\t'))
				j++;
			std::string word = s.substr(b, i - b);
			if (first && i < n && s[i] == ':') {
				i++;
				kind = Token::Label;
			} else if (std::find_if(std::begin(keywords), std::end(keywords),
									[&](const char* k) { return word == k; }) != std::end(keywords))
				kind = Token::Keyword;
			else if (j < n && s[j] == '(')
				kind = Token::Call;
			else
				continue;
		} else if (isOperator(c)) {
			while (i < n && isOperator(s[i]))
				i++;
			kind = Token::Operator;
		} else {
			i++; // punctuation
			continue;
		}
		out.push_back({b, i, kind});
	}
	return out;
}

// --- The document ---

class Doc {
  public:
	static constexpr size_t kMaxUndo = 1000, kMaxEvaluated = 8;

	explicit Doc(const std::string& text = "") { setText(text); }

	const std::string& text() const { return text_; }
	/** Counts the text's changes (what caches key on). */
	uint64_t version() const { return version_; }
	/** Replaces everything (a patch loads): no undo, no evaluated versions, the cursor at the start. */
	void setText(const std::string& text) {
		text_ = text;
		undo_.clear();
		redo_.clear();
		evals_.clear();
		error_ = 0;
		cursor_ = anchor_ = 0;
		open_ = false;
		goal_ = -1;
		changed();
	}

	// --- Cursor and selection [selBegin(), selEnd()): the cursor is the end that moves ---

	size_t cursor() const { return cursor_; }
	size_t anchor() const { return anchor_; }
	size_t selBegin() const { return std::min(cursor_, anchor_); }
	size_t selEnd() const { return std::max(cursor_, anchor_); }
	bool hasSelection() const { return cursor_ != anchor_; }
	std::string selection() const { return text_.substr(selBegin(), selEnd() - selBegin()); }
	/** Moves the cursor (back to a codepoint start); `select` (Shift) keeps the anchor where it is. */
	void setCursor(size_t pos, bool select = false) {
		cursor_ = snap(pos);
		if (!select)
			anchor_ = cursor_;
		open_ = false;
		goal_ = -1;
	}
	void select(size_t anchor, size_t cursor) {
		setCursor(anchor);
		setCursor(cursor, true);
	}
	void selectAll() { select(0, text_.size()); }
	/** A double-click at the char that starts at pos: its word, its run of spaces, or itself. */
	void selectWord(size_t pos) {
		size_t b = 0, e = 0;
		wordAt(pos, b, e);
		select(b, e);
	}

	/** A triple-click on a line: all of it with its newline (cursor at the next line's start; on the last: the end). */
	void selectLine(int line) { select(lineStart(line), line + 1 < lineCount() ? lineStart(line + 1) : text_.size()); }

	// --- Movement (`select`: Shift extends the selection) ---

	void left(bool select) { setCursor(!select && hasSelection() ? selBegin() : prevChar(text_, cursor_), select); }
	void right(bool select) { setCursor(!select && hasSelection() ? selEnd() : nextChar(text_, cursor_), select); }
	void wordLeft(bool select) { setCursor(wordStart(cursor_), select); }
	void wordRight(bool select) { setCursor(wordEnd(cursor_), select); }
	void home(bool select) { setCursor(lineStart(lineOf(cursor_)), select); }
	void end(bool select) { setCursor(lineEnd(lineOf(cursor_)), select); }
	/** `lines` up (< 0) or down, at the column the vertical moves started from; beyond the first / last line: to the
	start / end. */
	void vertical(int lines, bool select) {
		int col = goal_ >= 0 ? goal_ : column(cursor_), line = lineOf(cursor_) + lines;
		setCursor(line < 0 ? 0 : line >= lineCount() ? text_.size() : at(line, col), select);
		goal_ = col;
	}

	// --- Editing: each one undo step; chars typed, backspaced or deleted in a row coalesce into one ---

	/** Typed text, over the selection. */
	void type(const std::string& s) { replace(selBegin(), selEnd() - selBegin(), s, Typing); }
	/** Pasted text, over the selection. */
	void insert(const std::string& s) { replace(selBegin(), selEnd() - selBegin(), s, Other); }
	/** An accepted completion: name replaces [from, cursor), the cursor after it; one undo step (none if it's there).
	 */
	void complete(size_t from, const std::string& name) {
		if (from <= cursor_ && text_.compare(from, cursor_ - from, name) != 0)
			replace(from, cursor_ - from, name, Other);
	}
	/** Enter: a newline with the current line's indentation (up to the cursor). */
	void newline() {
		size_t from = selBegin(), ls = lineStart(lineOf(from)), i = ls;
		while (i < from && (text_[i] == ' ' || text_[i] == '\t'))
			i++;
		replace(from, selEnd() - from, "\n" + text_.substr(ls, i - ls), Other);
	}
	/** The selection, else the codepoint (or word) before the cursor. */
	void backspace(bool word = false) {
		if (hasSelection())
			return replace(selBegin(), selEnd() - selBegin(), "", Other);
		size_t b = word ? wordStart(cursor_) : prevChar(text_, cursor_);
		if (b < cursor_)
			replace(b, cursor_ - b, "", word ? Other : Backspace);
	}
	/** The selection, else the codepoint (or word) after the cursor. */
	void del(bool word = false) {
		if (hasSelection())
			return replace(selBegin(), selEnd() - selBegin(), "", Other);
		size_t e = word ? wordEnd(cursor_) : nextChar(text_, cursor_);
		if (e > cursor_)
			replace(cursor_, e - cursor_, "", word ? Other : Delete);
	}
	/** Cmd/Ctrl+/ like CodeMirror's: the lines the selection touches (not a last one it only enters at column 0) lose
	their "//" if all have it, else get "// " at the least indentation. Blank lines stay, but a lone one gets it at its
	end. One undo step, the selection kept on its text. ponytail: the step replaces the whole block (playing spans in
	it are dropped until the next evaluate). */
	void toggleComment() {
		int l0 = lineOf(selBegin()), l1 = lineOf(selEnd());
		if (l1 > l0 && selEnd() == lineStart(l1))
			l1--;
		struct Edit {
			size_t pos, del, ins;
		};
		std::vector<Edit> edits;
		std::vector<size_t> indents;
		size_t least = SIZE_MAX;
		bool all = true;
		for (int l = l0; l <= l1; l++) {
			size_t i = lineStart(l);
			while (i < lineEnd(l) && (text_[i] == ' ' || text_[i] == '\t'))
				i++;
			if (i == lineEnd(l))
				continue; // blank
			indents.push_back(i);
			least = std::min(least, i - lineStart(l));
			all = all && text_.compare(i, 2, "//") == 0;
		}
		if (indents.empty()) {
			if (l0 == l1)
				edits.push_back({lineEnd(l0), 0, 3});
		} else if (all)
			for (size_t i : indents)
				edits.push_back({i, text_[i + 2] == ' ' ? 3u : 2u, 0});
		else {
			int k = 0;
			for (int l = l0; l <= l1; l++)
				if (k < (int)indents.size() && indents[k] >= lineStart(l) && indents[k] <= lineEnd(l))
					edits.push_back({lineStart(l) + least, 0, 3}), k++;
		}
		if (edits.empty())
			return;
		size_t from = lineStart(l0), to = lineEnd(l1), a = anchor_, c = cursor_;
		std::string out;
		size_t at = from;
		auto map = [&](size_t p) {
			size_t q = p;
			for (const Edit& e : edits)
				if (p >= e.pos + e.del)
					q = q + e.ins - e.del;
				else if (p > e.pos)
					q = q - (p - e.pos);
			return q;
		};
		a = map(a), c = map(c);
		for (const Edit& e : edits) {
			out += text_.substr(at, e.pos - at) + (e.ins ? "// " : "");
			at = e.pos + e.del;
		}
		out += text_.substr(at, to - at);
		replace(from, to - from, out, Other);
		anchor_ = a, cursor_ = c;
	}
	/** Back to before the last step, its selection too. */
	bool undo() {
		if (undo_.empty())
			return false;
		Step st = undo_.back();
		undo_.pop_back();
		apply(st.pos, st.inserted.size(), st.removed);
		cursor_ = st.cursor, anchor_ = st.anchor;
		redo_.push_back(st);
		open_ = false;
		goal_ = -1;
		return true;
	}
	bool redo() {
		if (redo_.empty())
			return false;
		Step st = redo_.back();
		redo_.pop_back();
		apply(st.pos, st.removed.size(), st.inserted);
		cursor_ = anchor_ = st.pos + st.inserted.size();
		undo_.push_back(st);
		open_ = false;
		goal_ = -1;
		return true;
	}

	// --- Lines (0-based) and columns ---

	int lineCount() const { return (int)lines_.size(); }
	size_t lineStart(int line) const { return lines_[line]; }
	/** Before its '\n'. */
	size_t lineEnd(int line) const { return line + 1 < lineCount() ? lines_[line + 1] - 1 : text_.size(); }
	int lineOf(size_t pos) const {
		return (int)(std::upper_bound(lines_.begin(), lines_.end(), pos) - lines_.begin()) - 1;
	}
	int column(size_t pos) const {
		int c = 0;
		for (size_t i = lineStart(lineOf(pos)); i < pos; i = nextChar(text_, i))
			c++;
		return c;
	}
	/** The offset of a line and column, the column clamped to the line's end. */
	size_t at(int line, int column) const {
		line = std::max(0, std::min(line, lineCount() - 1));
		size_t i = lineStart(line), e = lineEnd(line);
		for (; column > 0 && i < e; column--)
			i = nextChar(text_, i);
		return i;
	}

	// --- Words ---

	/** Where Alt/Ctrl+Left goes: back over non-word chars, then over the word. */
	size_t wordStart(size_t pos) const {
		while (pos > 0 && !isWord(text_[prevChar(text_, pos)]))
			pos = prevChar(text_, pos);
		while (pos > 0 && isWord(text_[prevChar(text_, pos)]))
			pos = prevChar(text_, pos);
		return pos;
	}
	size_t wordEnd(size_t pos) const {
		while (pos < text_.size() && !isWord(text_[pos]))
			pos = nextChar(text_, pos);
		while (pos < text_.size() && isWord(text_[pos]))
			pos = nextChar(text_, pos);
		return pos;
	}
	/** The word, run of spaces or other char starting at pos (at a line's end: the char before). */
	void wordAt(size_t pos, size_t& begin, size_t& end) const {
		pos = snap(pos);
		if ((pos == text_.size() || text_[pos] == '\n') && pos > 0 && text_[pos - 1] != '\n')
			pos = prevChar(text_, pos);
		begin = end = pos;
		if (pos == text_.size() || text_[pos] == '\n')
			return;
		auto kind = [](char c) { return isWord(c) ? 0 : c == ' ' || c == '\t' ? 1 : 2; };
		int k = kind(text_[pos]);
		end = nextChar(text_, pos);
		if (k == 2)
			return;
		while (begin > 0 && kind(text_[prevChar(text_, begin)]) == k)
			begin = prevChar(text_, begin);
		while (end < text_.size() && kind(text_[end]) == k)
			end = nextChar(text_, end);
	}

	/** tokenize(text()), cached per version. */
	const std::vector<Token>& tokens() const {
		if (tokensVersion_ != version_)
			tokens_ = tokenize(text_), tokensVersion_ = version_;
		return tokens_;
	}

	// --- What plays: spans of evaluated code, in the text as it is now ---
	// Each evaluated version maps its UTF-16 offsets (JS string positions) to where those chars are now: edits shift
	// the chars after them and drop the ones they delete. Versions the engine has moved past are dropped.

	/** The current text went to the engine as code `id`. */
	void evaluated(uint32_t id) {
		Evaluated ev;
		ev.id = id;
		ev.lines.push_back(0);
		for (size_t i = 0; i < text_.size(); i = nextChar(text_, i)) {
			ev.pos.push_back((int)i);
			if ((unsigned char)text_[i] >= 0xf0) // beyond the BMP: a surrogate pair in JS
				ev.pos.push_back((int)i);
			if (text_[i] == '\n')
				ev.lines.push_back((int)ev.pos.size());
		}
		ev.pos.push_back((int)text_.size());
		evals_.push_back(std::move(ev));
		while (evals_.size() > kMaxEvaluated) // the JS thread is stuck: keep the playing one
			evals_.erase(
				std::find_if(evals_.begin(), evals_.end(), [&](const Evaluated& e) { return e.id != active_; }));
	}
	/** The engine's report of evaluate `id` (Engine::report(); the same one again is ignored). */
	void report(uint32_t id, bool ok, int line) {
		if (id <= reported_)
			return;
		reported_ = id;
		if (ok)
			active_ = id;
		error_ = ok ? 0 : id;
		errorLine_ = line;
		evals_.erase(std::remove_if(evals_.begin(), evals_.end(),
									[&](const Evaluated& e) { return e.id < id && e.id != active_; }),
					 evals_.end());
	}
	/** Where UTF-16 span [start, end) of code `id` is now; false if that code is gone or the span's first or last
	char was deleted (an edit inside it shrinks or grows it). */
	bool locate(uint32_t id, int start, int end, size_t& begin, size_t& endOut) const {
		const Evaluated* ev = find(id);
		if (!ev || start < 0 || end <= start || (size_t)end >= ev->pos.size())
			return false;
		int a = ev->pos[start], z = ev->pos[end - 1];
		if (a < 0 || z < a)
			return false;
		begin = (size_t)a;
		endOut = nextChar(text_, (size_t)z);
		return true;
	}
	/** The current line (0-based) of the last report's syntax error, -1: none (or its line is gone). */
	int errorLine() const {
		const Evaluated* ev = error_ ? find(error_) : nullptr;
		if (!ev || errorLine_ < 1 || (size_t)errorLine_ > ev->lines.size())
			return -1;
		int from = ev->lines[errorLine_ - 1];
		int to = (size_t)errorLine_ < ev->lines.size() ? ev->lines[errorLine_] : (int)ev->pos.size();
		for (int i = from; i < to; i++) // its first char left
			if (ev->pos[i] >= 0)
				return lineOf((size_t)ev->pos[i]);
		return -1;
	}

  private:
	enum Kind { Other, Typing, Backspace, Delete };
	struct Step {
		size_t pos;
		std::string removed, inserted;
		size_t cursor, anchor; // before
		Kind kind;
	};
	struct Evaluated {
		uint32_t id;
		std::vector<int> pos;   // per UTF-16 unit of that code (and its end): the byte offset of its char now, -1: gone
		std::vector<int> lines; // UTF-16 offset of each line's start
	};

	size_t snap(size_t pos) const {
		pos = std::min(pos, text_.size());
		while (pos > 0 && pos < text_.size() && isTail(text_[pos]))
			pos--;
		return pos;
	}

	const Evaluated* find(uint32_t id) const {
		for (const Evaluated& e : evals_)
			if (e.id == id)
				return &e;
		return nullptr;
	}

	/** An edit: [pos, pos + len) becomes s, the cursor after it. */
	void replace(size_t pos, size_t len, const std::string& s, Kind kind) {
		Step st{pos, text_.substr(pos, len), s, cursor_, anchor_, kind};
		Step* last = open_ && !undo_.empty() && undo_.back().kind == kind ? &undo_.back() : nullptr;
		if (last && kind == Typing && len == 0 && pos == last->pos + last->inserted.size())
			last->inserted += s;
		else if (last && kind == Backspace && pos + len == last->pos)
			last->pos = pos, last->removed = st.removed + last->removed;
		else if (last && kind == Delete && pos == last->pos)
			last->removed += st.removed;
		else {
			undo_.push_back(std::move(st));
			if (undo_.size() > kMaxUndo)
				undo_.erase(undo_.begin());
		}
		redo_.clear();
		apply(pos, len, s);
		cursor_ = anchor_ = pos + s.size();
		open_ = kind != Other;
		goal_ = -1;
	}

	void apply(size_t pos, size_t len, const std::string& s) {
		text_.replace(pos, len, s);
		int delta = (int)s.size() - (int)len;
		for (Evaluated& ev : evals_)
			for (int& v : ev.pos)
				if (v >= 0 && (size_t)v >= pos)
					v = (size_t)v < pos + len ? -1 : v + delta;
		changed();
	}

	void changed() {
		lines_.assign(1, 0);
		for (size_t i = 0; i < text_.size(); i++)
			if (text_[i] == '\n')
				lines_.push_back(i + 1);
		version_++;
	}

	std::string text_;
	std::vector<size_t> lines_; // line starts
	uint64_t version_ = 0;
	size_t cursor_ = 0, anchor_ = 0;
	int goal_ = -1;     // the column vertical moves keep
	bool open_ = false; // the last undo step may take more of the same kind (no cursor move since)
	std::vector<Step> undo_, redo_;
	mutable std::vector<Token> tokens_;
	mutable uint64_t tokensVersion_ = UINT64_MAX;
	std::vector<Evaluated> evals_; // oldest first
	uint32_t reported_ = 0, active_ = 0, error_ = 0;
	int errorLine_ = 0;
};

} // namespace editor
