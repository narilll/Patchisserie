#pragma once
// Filo's code editor in Rack: View draws an editor::Doc with NanoVG and edits it from the mouse and keyboard (one class
// for the panel and the fullscreen overlay: they share the module's Doc, each scrolls on its own); Overlay is the
// fullscreen editor on APP->scene. Thin: the editing logic is in doc.hpp.
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include <rack.hpp>

#include "editor/doc.hpp"
#include "engine.hpp"

namespace editor {

struct View : rack::widget::Widget {
	Doc* doc = nullptr;             // null: inert (its module is gone, the overlay waits for its deletion)
	filo::Engine* engine = nullptr; // null: the module browser's preview
	/** Panel: drawn in layer 1, lit like Rack's displays. The overlay draws in draw(). */
	bool panel = true;
	float fontSize = 12;
	/** Esc, if set (the overlay closes). Unset, Esc goes to Rack (leaves fullscreen). */
	std::function<void()> onEscape;

	void step() override;
	void draw(const DrawArgs& args) override;
	void drawLayer(const DrawArgs& args, int layer) override;
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
	std::string message_, error_; // the engine's, this frame
	std::vector<filo::Highlight> playing_;

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
};

/** The fullscreen editor: covers APP->scene (the whole Rack window); Esc or its Close button closes it. */
struct Overlay : rack::widget::OpaqueWidget {
	View* view;
	explicit Overlay(View* panel);
	/** Removes itself (next step); the panel's editor is selected again. */
	void close();
	/** Its module is going: inert until Rack deletes it (next step). */
	void detach();
	void step() override;
	void draw(const DrawArgs& args) override;
	void onButton(const ButtonEvent& e) override;
	void onHoverKey(const HoverKeyEvent& e) override;

  private:
	rack::WeakPtr<View> panel_;
	rack::math::Rect closeBox() const;
};

} // namespace editor
