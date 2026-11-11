#pragma once
// Filo's completions and hover docs: the documented names (res/strudel-docs.json, scripts/build-docs.mjs) that
// complete the identifier before the cursor or document the one under the mouse, and the completion popup's state.
// Pure logic like doc.hpp: src/editor/view.cpp loads the docs and draws, test/test_complete.cpp tests this.
#include "editor/doc.hpp"

namespace editor {

/** A documented function: Strudel's, Hydra's or one of ours. */
struct Entry {
	std::string name, signature, description, example, source;
	std::vector<std::string> synonyms;
	bool hydra() const { return source == "hydra"; }
};

/** An entry under one of its names: as on strudel.cc, each synonym is a completion of its own (accepting one inserts
that synonym), so the name typed toward is the one inserted. */
struct Match {
	const Entry* entry;
	std::string label;
	bool operator==(const Match& o) const { return entry == o.entry && label == o.label; }
};

/** What an identifier is made of here ([A-Za-z0-9_$]). */
inline bool isIdent(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
}

/** Whether pos is inside a comment or a string (or at the end of an unclosed one, the end of a // line too). */
inline bool inCommentOrString(const Doc& d, size_t pos) {
	const std::vector<Token>& toks = d.tokens();
	auto t = std::lower_bound(toks.begin(), toks.end(), pos, [](const Token& k, size_t p) { return k.end < p; });
	if (t == toks.end() || t->begin >= pos || (t->kind != Token::Comment && t->kind != Token::String))
		return false;
	if (pos < t->end)
		return true;
	const std::string& s = d.text();
	size_t n = t->end - t->begin;
	bool closed = t->kind == Token::String ? n >= 2 && s[t->end - 1] == s[t->begin]
										   : s[t->begin + 1] == '*' && n >= 4 && s.compare(t->end - 2, 2, "*/") == 0;
	return !closed; // ponytail: a string ending in an escaped quote counts as closed
}

/** The identifier [begin, pos) before pos (empty at a non-identifier char, after '.' too): what a completion replaces.
False inside a comment or a string, after a non-ASCII letter, and in a number. */
inline bool identBefore(const Doc& d, size_t pos, size_t& begin) {
	const std::string& s = d.text();
	for (begin = pos; begin > 0 && isIdent(s[begin - 1]); begin--) {
	}
	if ((begin > 0 && isWord(s[begin - 1])) || (begin < pos && std::isdigit((unsigned char)s[begin])))
		return false;
	return !inCommentOrString(d, pos);
}

/** The identifier [begin, end) around the char at pos (hover docs); false as identBefore() is. */
inline bool identAt(const Doc& d, size_t pos, size_t& begin, size_t& end) {
	const std::string& s = d.text();
	if (pos >= s.size() || !isIdent(s[pos]))
		return false;
	for (end = pos; end < s.size() && isIdent(s[end]); end++) {
	}
	return identBefore(d, end, begin) && begin < end && !inCommentOrString(d, begin + 1);
}

/** The names starting with prefix: with its case first, then in any case; within each, shorter first, then
alphabetical, then Strudel's before Hydra's. At most `max`. */
inline std::vector<Match> complete(const std::vector<Entry>& docs, const std::string& prefix, size_t max = 50) {
	auto rank = [&](const std::string& l) { // 0: same case, 1: any case, 2: no match
		if (l.compare(0, prefix.size(), prefix) == 0)
			return 0;
		if (l.size() < prefix.size())
			return 2;
		for (size_t i = 0; i < prefix.size(); i++)
			if (std::tolower((unsigned char)l[i]) != std::tolower((unsigned char)prefix[i]))
				return 2;
		return 1;
	};
	std::vector<std::pair<int, Match>> hits;
	for (const Entry& e : docs) {
		if (int r = rank(e.name); r < 2)
			hits.push_back({r, {&e, e.name}});
		for (const std::string& s : e.synonyms)
			if (int r = rank(s); r < 2)
				hits.push_back({r, {&e, s}});
	}
	std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
		const std::string &x = a.second.label, &y = b.second.label;
		if (a.first != b.first)
			return a.first < b.first;
		if (x.size() != y.size())
			return x.size() < y.size();
		if (x != y)
			return x < y;
		return !a.second.entry->hydra() && b.second.entry->hydra();
	});
	std::vector<Match> out;
	for (size_t i = 0; i < hits.size() && i < max; i++)
		out.push_back(hits[i].second);
	return out;
}

/** Every entry documenting `word` (a name or a synonym): Strudel's first. ponytail: no telling from the code which one
it is (Hydra's osc or Strudel's): the hover docs show all. */
inline std::vector<Match> lookup(const std::vector<Entry>& docs, const std::string& word) {
	std::vector<Match> out;
	for (const Match& m : complete(docs, word, SIZE_MAX))
		if (m.label == word)
			out.push_back(m);
	return out;
}

/** The completion popup: opens as an identifier of 2+ chars is typed, or on request (Ctrl+Space: any length); filters
as the identifier changes; closes when the cursor leaves it, on a selection, or with nothing to offer. */
struct Completion {
	static constexpr int kRows = 8; // visible at once
	bool open = false;
	std::vector<Match> items;
	int sel = 0, top = 0; // the selected item, the first visible
	size_t from = 0;      // the identifier's start

	/** Ctrl+Space. */
	void request(const Doc& d, const std::vector<Entry>& docs) {
		size_t b = 0;
		if (!d.hasSelection() && identBefore(d, d.cursor(), b))
			show(d, docs, b, true);
	}
	/** After any edit or cursor move; `typed`: an identifier char was just typed (may open). */
	void update(const Doc& d, const std::vector<Entry>& docs, bool typed) {
		size_t b = 0;
		bool ok = !d.hasSelection() && identBefore(d, d.cursor(), b);
		if (!open) {
			if (typed && ok && d.cursor() - b >= 2)
				show(d, docs, b, false);
		} else if (!ok || b != from || (b == d.cursor() && !explicit_))
			close();
		else
			show(d, docs, b, explicit_);
	}
	/** Up/Down (wrap), PageUp/PageDown (stop at the ends). */
	void move(int delta, bool wrap) {
		int n = (int)items.size();
		if (n == 0)
			return;
		sel = wrap ? ((sel + delta) % n + n) % n : std::max(0, std::min(n - 1, sel + delta));
		top = std::max(std::min(top, sel), sel - kRows + 1);
		top = std::max(0, std::min(top, n - kRows)); // no empty rows below
	}
	/** Tab/Enter: the selected name replaces the identifier typed (one undo step). */
	void accept(Doc& d) {
		if (open && !items.empty())
			d.complete(from, items[sel].label);
		close();
	}
	void close() {
		open = false;
		items.clear();
	}

  private:
	bool explicit_ = false;

	/** The matches of [b, cursor), the selected one kept if it still matches (as CodeMirror does). */
	void show(const Doc& d, const std::vector<Entry>& docs, size_t b, bool expl) {
		std::vector<Match> next = complete(docs, d.text().substr(b, d.cursor() - b));
		if (next.empty())
			return close();
		Match keep = open && !items.empty() ? items[sel] : Match{nullptr, ""};
		auto it = std::find(next.begin(), next.end(), keep);
		sel = it == next.end() ? 0 : (int)(it - next.begin());
		top = open && it != next.end() ? top : 0;
		items = std::move(next);
		open = true;
		explicit_ = expl;
		from = b;
		move(0, false); // the selected one in sight
	}
};

} // namespace editor
