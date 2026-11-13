// editor::GridEdit (src/editor/grid.hpp): cursor, edits, clipboard, drag and undo over a stopped lattice::Machine.
#include <string>

#include "check.hpp"
#include "editor/grid.hpp"

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
