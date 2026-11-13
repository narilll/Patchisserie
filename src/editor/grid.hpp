#pragma once
// Lattice's grid editor model: a cursor (origin plus signed extent, Orca's cursor.js) over the UI's snapshot of the
// machine's grid, edits with undo/redo written through Machine::write. Pure logic, no Rack or NanoVG: the panel view
// and the window view map keys and mouse to these calls and draw `grid`, `marks` and selection(); test/test_grid.cpp
// tests it.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "lattice.hpp"

namespace editor {

struct GridEdit {
	static constexpr size_t kMaxUndo = 1000;
	static constexpr size_t kMaxUndoBytes = 8 << 20; // a 256x256 erase holds 128 KB: the step cap alone allows 100+ MB

	lattice::Machine* machine = nullptr;
	lattice::Grid grid;               // the UI's snapshot: every edit refreshes it
	std::vector<uint8_t> marks;       // and its marks
	int x = 0, y = 0, dx = 0, dy = 0; // origin and extent (any sign): the selection spans (x,y)..(x+dx,y+dy) inclusive
	bool insert = false;              // typing moves the origin right

	/** Takes the machine's latest grid and keeps the cursor inside it; true if anything changed. */
	bool refresh() {
		bool changed = machine->snapshot(grid, marks);
		clamp();
		return changed;
	}

	/** The selection normalized: w, h >= 1. */
	lattice::Rect selection() const {
		int x1 = x + dx, y1 = y + dy;
		return {std::min(x, x1), std::min(y, y1), std::abs(dx) + 1, std::abs(dy) + 1};
	}

	// --- Cursor
	/** Moves the origin, keeping the extent; both clamp to the grid. */
	void move(int mx, int my) {
		x += mx, y += my;
		clamp();
	}
	void scale(int sx, int sy) {
		dx += sx, dy += sy;
		clamp();
	}
	void place(int cx, int cy) {
		x = cx, y = cy, dx = dy = 0;
		clamp();
	}
	/** Mouse drag: the selection from the origin to a cell. */
	void extendTo(int cx, int cy) {
		dx = cx - x, dy = cy - y;
		clamp();
	}
	void selectAll() {
		x = y = 0;
		dx = grid.w - 1, dy = grid.h - 1;
		clamp();
	}
	/** Esc: true if it shrank a multi-cell selection (otherwise the key isn't consumed). */
	bool collapse() {
		bool multi = dx || dy;
		dx = dy = 0;
		return multi;
	}

	// --- Edits: each one undo step
	/** Writes at the origin, not the whole selection. */
	void type(char c) {
		if (!lattice::isGlyph(c))
			return;
		commit({put({x, y, 1, 1}, std::string(1, c))});
		if (insert)
			move(1, 0);
	}
	void erase() {
		lattice::Rect r = selection();
		commit({put(r, std::string((size_t)r.w * r.h, '.'))});
	}
	/** Cmd/Ctrl+/ (Orca JS's comment()): `#` on the left and right edge of each selected row, or `.` if the top-left
	cell is `#`. A 1-wide selection writes one cell (Orca JS writes a second one outside it). */
	void comment() {
		lattice::Rect r = selection();
		char c = grid.at(r.x, r.y) == '#' ? '.' : '#';
		std::string g;
		for (int j = r.y; j < r.y + r.h; j++)
			for (int i = r.x; i < r.x + r.w; i++)
				g += i == r.x || i == r.x + r.w - 1 ? c : grid.at(i, j);
		commit({put(r, g)});
	}
	std::string copy() const {
		lattice::Rect r = selection();
		std::string s;
		for (int j = r.y; j < r.y + r.h; j++) {
			if (j > r.y)
				s += '\n';
			for (int i = r.x; i < r.x + r.w; i++)
				s += grid.at(i, j);
		}
		return s;
	}
	std::string cut() {
		std::string s = copy();
		erase();
		return s;
	}
	/** At the selection's top-left, clipped at the grid's edge; cells past a short line's end keep their content. */
	void paste(const std::string& text) {
		std::vector<std::string> lines;
		size_t b = 0;
		for (size_t e; (e = text.find('\n', b)) != std::string::npos; b = e + 1) {
			lines.push_back(text.substr(b, e - b));
			if (!lines.back().empty() && lines.back().back() == '\r')
				lines.back().pop_back();
		}
		if (b < text.size()) // no trailing newline: the last line; a trailing one adds nothing
			lines.push_back(text.substr(b));
		lattice::Rect s = selection();
		int w = 0;
		for (const std::string& l : lines)
			w = std::max(w, (int)l.size());
		w = std::min(w, grid.w - s.x);
		int h = std::min((int)lines.size(), grid.h - s.y);
		if (w <= 0 || h <= 0)
			return;
		std::string g;
		for (int j = 0; j < h; j++)
			for (int i = 0; i < w; i++)
				g += i < (int)lines[j].size() ? lines[j][i] : grid.at(s.x + i, s.y + j);
		commit({put({s.x, s.y, w, h}, g)});
		x = s.x, y = s.y, dx = w - 1, dy = h - 1;
	}
	/** Alt+arrows: moves the selected block's content; a block at the edge doesn't move. */
	void drag(int mx, int my) {
		lattice::Rect s = selection();
		mx = std::clamp(mx, -s.x, grid.w - s.x - s.w);
		my = std::clamp(my, -s.y, grid.h - s.y - s.h);
		if (!mx && !my)
			return;
		std::string flat = copy();
		flat.erase(std::remove(flat.begin(), flat.end(), '\n'), flat.end());
		Step st;
		st.push_back(put(s, std::string(flat.size(), '.')));
		st.push_back(put({s.x + mx, s.y + my, s.w, s.h}, flat));
		commit(std::move(st));
		x += mx, y += my;
		clamp();
	}

	// --- History
	void undo() {
		if (undo_.empty())
			return;
		Step st = std::move(undo_.back());
		undo_.pop_back();
		undoBytes_ -= bytes(st);
		for (size_t i = st.size(); i-- > 0;)
			machine->write(st[i].r, st[i].before);
		jumpTo(st[0].r);
		redo_.push_back(std::move(st)); // not counted: it holds what undo_ held
		refresh();
	}
	void redo() {
		if (redo_.empty())
			return;
		Step st = std::move(redo_.back());
		redo_.pop_back();
		for (const Diff& d : st)
			machine->write(d.r, d.after);
		jumpTo(st[0].r);
		undoBytes_ += bytes(st);
		undo_.push_back(std::move(st));
		refresh();
	}

	// --- Not undoable here (ponytail: clearing the history is cheaper than diffing whole grids; the module's menu
	// wraps these in Rack's undo history)
	void resize(int w, int h) {
		machine->resize(w, h);
		reset();
	}
	void load(const std::string& text) {
		machine->load(text);
		reset();
	}

  private:
	struct Diff {
		lattice::Rect r;
		std::string before, after;
	};
	using Step = std::vector<Diff>;

	void clamp() {
		x = std::clamp(x, 0, std::max(grid.w, 1) - 1), y = std::clamp(y, 0, std::max(grid.h, 1) - 1);
		dx = std::clamp(x + dx, 0, std::max(grid.w, 1) - 1) - x,
		dy = std::clamp(y + dy, 0, std::max(grid.h, 1) - 1) - y;
	}
	void reset() {
		undo_.clear(), redo_.clear(), undoBytes_ = 0;
		refresh();
	}
	void jumpTo(lattice::Rect r) {
		x = r.x, y = r.y, dx = dy = 0;
		clamp();
	}
	/** Writes glyphs into r (non-glyphs become '.'), returns the diff. */
	Diff put(lattice::Rect r, std::string glyphs) {
		for (char& c : glyphs)
			if (!lattice::isGlyph(c))
				c = '.';
		std::string before = machine->write(r, glyphs);
		return {r, std::move(before), std::move(glyphs)};
	}
	static size_t bytes(const Step& st) {
		size_t n = 0;
		for (const Diff& d : st)
			n += d.before.size() + d.after.size();
		return n;
	}
	void commit(Step st) {
		undoBytes_ += bytes(st);
		undo_.push_back(std::move(st));
		// ponytail: erase from the front is O(n) on <= 1000 steps; a deque if it is ever measured.
		size_t drop = 0;
		while (undo_.size() - drop > 1 && (undo_.size() - drop > kMaxUndo || undoBytes_ > kMaxUndoBytes))
			undoBytes_ -= bytes(undo_[drop++]);
		undo_.erase(undo_.begin(), undo_.begin() + drop);
		redo_.clear();
		refresh();
	}

	std::vector<Step> undo_, redo_;
	size_t undoBytes_ = 0; // of undo_
};

} // namespace editor
