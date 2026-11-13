#pragma once
// Filo's code editor in Rack: View draws an editor::Doc with NanoVG and edits it from the mouse and keyboard (one class
// for the panel and the editor window: they share the module's Doc, each scrolls on its own); Window is the editor in
// an OS window of its own. Thin: the editing logic is in doc.hpp, the completions' in complete.hpp.
#include <cmath>
#include <string>
#include <vector>

#include <rack.hpp>

#include "editor/complete.hpp"
#include "editor/doc.hpp"
#include "editor/grid.hpp"
#include "editor/keys.hpp"
#include "editor/theme.hpp"
#include "engine.hpp"
#include "hydra.hpp"

namespace editor {

struct Tip;
struct Window;

/** The editor's colours: Patchisserie's own, or one of strudel.cc's themes (res/editor-themes.json, from
scripts/build-themes.mjs). */
struct EditorTheme {
	NVGcolor background, text, plain, caret, selection, gutter, border, dim, row, playing, error;
	NVGcolor comment, string, number, keyword, label, op, call;
};

/** The built-in theme, the default. */
const EditorTheme& patchisserie();
/** The theme names a menu offers: Patchisserie, then strudel.cc's in its order. Loads the themes on first use. */
const std::vector<std::string>& themeNames();
/** The theme of that name, Patchisserie for an unknown one (a name from a patch, say). Valid until the plugin unloads.
 */
const EditorTheme& themeNamed(const std::string& name);

struct View : rack::widget::Widget {
	Doc* doc = nullptr;
	filo::Engine* engine = nullptr; // null: the module browser's preview
	float fontSize = 12;
	/** The editor window this is in: its events, mouse, keys and fonts, drawn in draw(). Null: the panel's, in Rack's
	window, drawn in layer 1, lit like Rack's displays. */
	Window* host = nullptr;
	const EditorTheme* theme = nullptr; // the module's; null: Patchisserie (the module browser's preview)

	~View() override;
	void step() override;
	void draw(const DrawArgs& args) override;
	void drawLayer(const DrawArgs& args, int layer) override;
	void onHover(const HoverEvent& e) override;
	void onButton(const ButtonEvent& e) override;
	void onDoubleClick(const DoubleClickEvent& e) override;
	void onDragMove(const DragMoveEvent& e) override;
	void onDragEnd(const DragEndEvent& e) override;
	void onHoverScroll(const HoverScrollEvent& e) override;
	void onSelectKey(const SelectKeyEvent& e) override;
	void onSelectText(const SelectTextEvent& e) override;

  private:
	rack::math::Vec scroll_; // px of the text scrolled out at the top / left
	float charW_ = 7;        // measured when drawn
	uint64_t seenVersion_ = UINT64_MAX;
	size_t seenCursor_ = SIZE_MAX;
	int columns_ = 0; // the longest line's
	bool dragging_ = false, wordDrag_ = false;
	rack::math::Vec pressAt_;
	double pressTime_ = -1e9;
	int clicks_ = 0;
	std::string message_, error_; // the engine's, this frame
	std::vector<filo::Highlight> playing_;
	Completion completion_;
	rack::math::Vec hoverPos_;
	size_t hoverBegin_ = SIZE_MAX; // the identifier under the mouse
	double hoverSince_ = 0;
	std::vector<Match> hover_; // its docs, once the mouse rested on it
	Tip* tip_ = nullptr;       // on APP->scene or the window's root: the popup or the hover docs

	float lineH() const { return std::round(fontSize * 1.3f); }
	float statusH() const { return std::round(fontSize * 1.6f); }
	/** The line numbers' column: at least 2 digits, kPad around. */
	float gutterW() const;
	float textH() const { return box.size.y - statusH(); }
	void paint(NVGcontext* vg);
	size_t hit(rack::math::Vec pos, bool nearest) const;
	void follow();
	void clampScroll();
	void evaluate();
	bool completionKey(const SelectKeyEvent& e);
	void hover(bool changed);
	void showTip();
};

/** Lattice's grid editor: a GridEdit drawn as Orca's cell grid with NanoVG, edited from the mouse and keyboard. One
class for the panel and the window, like View: they share the module's GridEdit, each scrolls on its own. */
struct GridView : rack::widget::Widget {
	GridEdit* edit = nullptr;
	lattice::Machine* machine = nullptr; // null: the module browser's preview (no transport keys)
	const float* bpm = nullptr;          // the module's setting, for the status line
	float fontSize = 12;
	/** The window's view: the cells take the largest whole-px font in [6, 2 x fontSize] at which the whole grid shows
	above the status line. (ponytail: below 6 px it scrolls; 256x256 needs fullscreen and scrolling.) The panel (false):
	cells use fontSize and scroll. */
	bool fit = false;
	/** The editor window this is in, as View::host. */
	Window* host = nullptr;
	const Theme* theme = nullptr; // the module's; null: kPresets[0] (the module browser's preview)
	bool dots = true;             // empty cells show a dim dot (Lattice's "Show empty cells")

	void step() override;
	void draw(const DrawArgs& args) override;
	void drawLayer(const DrawArgs& args, int layer) override;
	void onButton(const ButtonEvent& e) override;
	void onDragMove(const DragMoveEvent& e) override;
	void onDragEnd(const DragEndEvent& e) override;
	void onHoverScroll(const HoverScrollEvent& e) override;
	void onSelectKey(const SelectKeyEvent& e) override;
	void onSelectText(const SelectTextEvent& e) override;
	/** A cell's size, px (glcheck). */
	rack::math::Vec cellSize() const { return {charW_, lineH()}; }

  private:
	rack::math::Vec scroll_; // px of the grid scrolled out at the top / left
	float charW_ = 7.2f;     // measured when drawn
	float cellFont_ = 12;    // the cells' font px: fontSize, or fitted
	float charRatio_ = 0;    // char advance / font px; 0: not measured yet
	int seenX_ = -1, seenY_ = -1;
	bool dragging_ = false;

	float lineH() const { return std::round(cellFont_ * 1.3f); }
	float statusH() const { return std::round(fontSize * 1.6f); }
	float gridH() const { return box.size.y - statusH(); }
	void paint(NVGcontext* vg);
	void cellAt(rack::math::Vec pos, int& cx, int& cy) const;
	void follow();
	void clampScroll();
};

/** An editor in an OS window of its own (hydra::Window, Icing's output window): Filo's View of the module's Doc, or
Lattice's GridView, with
the window's own events (its GLFW callbacks into an EventState, as Rack's window does), NanoVG context and fonts. FULL
and the module's menu open it, its title bar closes (hides) it. UI thread. */
struct Window {
	hydra::Window window;
	rack::widget::EventState events;
	rack::widget::Widget* root; // the window's: the view, its Tip
	rack::widget::Widget* view; // a View or a GridView
	NVGcontext* vg = nullptr;   // the window's context's (from its first frame on)
	int mono = -1, ui = -1;     // its fonts, in vg
	rack::math::Vec mouse;      // in the root's coordinates
	float mouseScale = 1;       // window coordinates -> the root's (HiDPI), from the last frame

	/** Filo's: a View of `doc`. The geometry and `open` (the module's, saved in the patch) say where and whether it is
	open. */
	Window(Doc& doc, filo::Engine* engine, hydra::WindowGeometry& g, bool& open);
	/** Any view (Lattice's GridView, whose `host` the caller sets to this): owned by the window, which sizes it to its
	framebuffer. */
	Window(rack::widget::Widget* view, hydra::WindowGeometry& g, bool& open);
	~Window();
	/** Each Rack frame, outside GLFW callbacks (FiloWidget::step()): the window opened (titled `title`), hidden, moved
	and drawn as the geometry and `open` say (its title bar, F11 and Esc write back into them). Leaves `rack`'s context
	current. */
	void update(GLFWwindow* rack, const std::string& title);
	/** Opens it, or brings it to the front (FULL). */
	void show();
	/** A frame (`fbW` x `fbH` px) into the bound framebuffer, the window's context current. */
	void draw(int fbW, int fbH);
	/** Frees its NanoVG context and its GLFW window (hydra::Window::destroy(): safe inside GLFW callbacks); update()
	makes new ones. */
	void destroy();

  private:
	hydra::WindowGeometry& g_;
	bool& open_;
	void key(int key, int scancode, int action, int mods);
};

} // namespace editor
