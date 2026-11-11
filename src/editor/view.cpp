#include "editor/view.hpp"

#include <algorithm>
#include <cmath>

using namespace rack;

extern Plugin* pluginInstance; // src/plugin.cpp

namespace editor {

namespace {

constexpr float kPad = 4;                                        // px around the text
constexpr float kHeader = 24;                                    // px, the overlay's title bar
const char* const kFont = "res/fonts/ShareTechMono-Regular.ttf"; // Rack's monospace font

// On the panel's dark brown (res/Filo.svg).
const NVGcolor kBackground = nvgRGB(0x2a, 0x1f, 0x17);
const NVGcolor kAccent = nvgRGB(0xc9, 0x8b, 0x4f);
const NVGcolor kText = nvgRGB(0xf3, 0xe6, 0xd3);
const NVGcolor kMessage = nvgRGB(0xf3, 0xc8, 0x8e);
const NVGcolor kDim = nvgRGB(0x8a, 0x75, 0x60);
const NVGcolor kError = nvgRGB(0xff, 0x6b, 0x5b);
const NVGcolor kSelection = nvgRGBA(0xc9, 0x8b, 0x4f, 0x60);
const NVGcolor kPlaying = nvgRGBA(0xf3, 0xe6, 0xd3, 0x24);

NVGcolor color(Token::Kind k) {
	switch (k) {
	case Token::Comment:
		return kDim;
	case Token::String:
		return nvgRGB(0xe8, 0xb8, 0x6a);
	case Token::Number:
		return nvgRGB(0xf0, 0x8a, 0x5d);
	case Token::Keyword:
		return nvgRGB(0xd5, 0x8f, 0xc9);
	case Token::Call:
		return nvgRGB(0x7f, 0xc6, 0xe8);
	case Token::Label:
		return kMessage;
	case Token::Operator:
		return kAccent;
	}
	return kText;
}

#if defined ARCH_MAC
// Option+arrows: words; Cmd+arrows: line and document ends.
constexpr int kWordMod = GLFW_MOD_ALT;
#else
constexpr int kWordMod = GLFW_MOD_CONTROL;
#endif

constexpr float kTipFont = 13; // px on the scene: the same at any rack zoom
constexpr int kDetailCols = 60, kDescLines = 4, kExampleLines = 6;

/** res/strudel-docs.json (scripts/build-docs.mjs), shared by all Filos: loaded on first use; empty (no completions,
no hover docs) if it can't be read. */
const std::vector<Entry>& docs() {
	static std::vector<Entry> entries;
	static bool loaded = false;
	if (loaded)
		return entries;
	loaded = true;
	std::string path = asset::plugin(pluginInstance, "res/strudel-docs.json");
	json_error_t err;
	json_t* root = json_load_file(path.c_str(), 0, &err);
	auto str = [](json_t* j) {
		const char* s = json_string_value(j);
		return std::string(s ? s : "");
	};
	size_t i, k;
	json_t *e, *v;
	json_array_foreach(root, i, e) {
		Entry x;
		x.name = str(json_object_get(e, "name"));
		x.signature = str(json_object_get(e, "signature"));
		x.description = str(json_object_get(e, "description"));
		x.example = str(json_array_get(json_object_get(e, "examples"), 0));
		x.source = str(json_object_get(e, "source"));
		json_array_foreach(json_object_get(e, "synonyms"), k, v) x.synonyms.push_back(str(v));
		if (!x.name.empty())
			entries.push_back(std::move(x));
	}
	if (entries.empty())
		WARN("Filo: no completions or hover docs, %s: %s", path.c_str(), root ? "no entries" : err.text);
	json_decref(root);
	return entries;
}

} // namespace

// --- Tip ---

/** The completion popup or the hover docs, on APP->scene: over the module and the overlay, neither clipped by them nor
zoomed with the rack. Passive: its View sets what it shows each step. */
struct Tip : widget::Widget {
	std::vector<Match> items, details; // the popup's rows (none: hover docs), the entries described
	int sel = 0, top = 0;
	math::Vec at;    // the identifier's top left, scene px
	float lineH = 0; // its line's height, scene px

	void step() override {
		if (parent) // the scene: the whole window
			box = math::Rect(math::Vec(), parent->box.size);
		Widget::step();
	}
	void draw(const DrawArgs& args) override;
};

void Tip::draw(const DrawArgs& args) {
	std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system(kFont));
	if (!font || font->handle < 0 || details.empty())
		return;
	NVGcontext* vg = args.vg;
	nvgFontFaceId(vg, font->handle);
	nvgFontSize(vg, kTipFont);
	nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
	float cw = nvgTextBounds(vg, 0, 0, "0000000000", nullptr, nullptr) / 10, rh = std::round(kTipFont * 1.4f);
	math::Vec size = box.size;
	auto frame = [&](math::Rect r) {
		nvgBeginPath(vg);
		nvgRoundedRect(vg, r.pos.x, r.pos.y, r.size.x, r.size.y, 3);
		nvgFillColor(vg, kBackground);
		nvgFill(vg);
		nvgStrokeColor(vg, kAccent);
		nvgStrokeWidth(vg, 1);
		nvgStroke(vg);
	};
	// Under the identifier's line, above it if there's no room there.
	auto place = [&](math::Vec s, float x) {
		float below = at.y + lineH, y = (below + s.y <= size.y || at.y < s.y) ? below : at.y - s.y;
		return math::Rect(math::Vec(math::clamp(x, 0.f, size.x - s.x), y), s);
	};

	// The popup's list: a row per name, "hydra" for Hydra's, a scroll bar when there are more.
	math::Rect list;
	if (!items.empty()) {
		size_t cols = 10;
		for (const Match& m : items)
			cols = std::max(cols, m.label.size() + (m.entry->hydra() ? 7 : 0));
		int n = (int)items.size(), rows = std::min(n, Completion::kRows);
		list = place(math::Vec(std::min(cols, (size_t)40) * cw + 2 * kPad, rows * rh + 2 * kPad), at.x - kPad);
		frame(list);
		nvgSave(vg);
		nvgIntersectScissor(vg, list.pos.x, list.pos.y, list.size.x, list.size.y);
		for (int i = 0; i < rows && top + i < n; i++) {
			const Match& m = items[top + i];
			float y = list.pos.y + kPad + i * rh;
			if (top + i == sel) {
				nvgBeginPath(vg);
				nvgRect(vg, list.pos.x + 1, y, list.size.x - 2, rh);
				nvgFillColor(vg, kSelection);
				nvgFill(vg);
			}
			nvgFillColor(vg, kText);
			nvgText(vg, list.pos.x + kPad, y + rh / 2, m.label.c_str(), nullptr);
			if (m.entry->hydra()) {
				nvgFillColor(vg, kDim);
				nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
				nvgText(vg, list.getRight() - kPad - 2, y + rh / 2, "hydra", nullptr);
				nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			}
		}
		if (n > rows) {
			float h = list.size.y - 2 * kPad;
			nvgBeginPath(vg);
			nvgRect(vg, list.getRight() - 3, list.pos.y + kPad + h * top / n, 2, h * rows / n);
			nvgFillColor(vg, kDim);
			nvgFill(vg);
		}
		nvgRestore(vg);
	}

	// Each entry's box: its signature (under the name it's listed as), its other names, the description wrapped (cut
	// at a few lines) with its `code` in another colour, and the first example.
	enum Kind { Plain, Text, Example };
	struct Line {
		std::string text;
		NVGcolor color;
		Kind kind;
	};
	float maxW = std::max(10 * cw, std::min(kDetailCols * cw, size.x - 2 * kPad)), w = 0;
	size_t maxCols = (size_t)(maxW / cw);
	std::vector<std::vector<Line>> boxes;
	for (const Match& m : details) {
		const Entry& e = *m.entry;
		std::vector<Line> ls;
		std::string sig = e.signature, others;
		if (m.label != e.name && sig.compare(0, e.name.size(), e.name) == 0)
			sig = m.label + sig.substr(e.name.size());
		ls.push_back({sig, color(Token::Call), Plain});
		std::vector<std::string> names = e.synonyms;
		names.insert(names.begin(), e.name);
		for (const std::string& n : names)
			if (n != m.label)
				others += (others.empty() ? "Synonyms: " : ", ") + n;
		if (!others.empty())
			ls.push_back({others, kDim, Plain});
		std::string desc = e.description;
		for (size_t p; (p = desc.find("\n\n")) != std::string::npos;) // paragraphs: no blank lines here
			desc.erase(p, 1);
		NVGtextRow rows[kDescLines];
		const char* end = desc.data() + desc.size();
		int n = desc.empty() ? 0 : nvgTextBreakLines(vg, desc.data(), end, maxW, rows, kDescLines);
		for (int i = 0; i < n; i++) {
			std::string t(rows[i].start, rows[i].end);
			if (i == n - 1 && rows[i].next < end) { // more: cut
				size_t cut = std::min(t.size(), maxCols - 3);
				while (cut > 0 && isTail(t[cut]))
					cut--;
				t = t.substr(0, cut) + "...";
			}
			ls.push_back({t, kText, Text});
		}
		for (size_t a = 0, k = 0; a < e.example.size(); k++) {
			size_t b = std::min(e.example.find('\n', a), e.example.size());
			ls.push_back({k < (size_t)kExampleLines ? e.example.substr(a, b - a) : "...", kText, Example});
			a = k < (size_t)kExampleLines ? b + 1 : e.example.size();
		}
		for (const Line& l : ls)
			w = std::max(w, nvgTextBounds(vg, 0, 0, l.text.c_str(), nullptr, nullptr));
		boxes.push_back(std::move(ls));
	}
	w = std::min(w, maxW) + 2 * kPad;
	auto height = [&](const std::vector<Line>& ls) { return ls.size() * rh + 2 * kPad; };
	float total = -2;
	for (const std::vector<Line>& ls : boxes)
		total += height(ls) + 2;
	// Next to the list (right, else left), or under / above the hovered identifier, stacked.
	math::Vec pos;
	if (!items.empty()) {
		pos.x = list.getRight() + 2 + w <= size.x ? list.getRight() + 2 : list.pos.x - 2 - w;
		pos.x = math::clamp(pos.x, 0.f, size.x - w);
		pos.y = math::clamp(list.pos.y, 0.f, size.y - total);
	} else
		pos = place(math::Vec(w, total), at.x - kPad).pos;
	for (const std::vector<Line>& ls : boxes) {
		math::Rect r(pos, math::Vec(w, height(ls)));
		frame(r);
		nvgSave(vg);
		nvgIntersectScissor(vg, r.pos.x, r.pos.y, r.size.x, r.size.y);
		float y = r.pos.y + kPad;
		bool code = false;
		for (const Line& l : ls) {
			float x = r.pos.x + kPad;
			if (l.kind == Example) {
				nvgBeginPath(vg);
				nvgRect(vg, r.pos.x + 1, y, r.size.x - 2, rh);
				nvgFillColor(vg, nvgRGBA(0, 0, 0, 0x50));
				nvgFill(vg);
			}
			if (l.kind != Text) {
				nvgFillColor(vg, l.color);
				nvgText(vg, x, y + rh / 2, l.text.c_str(), nullptr);
			} else
				for (size_t a = 0; a <= l.text.size();) { // `code` toggles at each backtick (not drawn)
					size_t b = std::min(l.text.find('`', a), l.text.size());
					nvgFillColor(vg, code ? kMessage : l.color);
					if (b > a)
						x = nvgText(vg, x, y + rh / 2, l.text.data() + a, l.text.data() + b);
					if (b < l.text.size())
						code = !code;
					a = b + 1;
				}
			y += rh;
		}
		nvgRestore(vg);
		pos.y += r.size.y + 2;
	}
}

// --- View ---

View::~View() {
	if (tip_)
		tip_->requestDelete(); // APP->scene deletes it in its next step (or with itself, after this)
}

void View::step() {
	Widget::step();
	if (!doc)
		return;
	if (engine) {
		filo::Engine::Report r = engine->report();
		doc->report(r.id, r.ok, r.line);
		error_ = r.ok ? "" : r.error.substr(0, r.error.find('\n'));
		message_ = engine->message();
		message_ = message_.substr(0, message_.find('\n'));
	}
	bool changed = doc->version() != seenVersion_;
	if (changed) {
		columns_ = 0;
		for (int l = 0; l < doc->lineCount(); l++)
			columns_ = std::max(columns_, doc->column(doc->lineEnd(l)));
	}
	if (changed || doc->cursor() != seenCursor_) // typed, moved, or the other view did: the cursor stays in sight
		follow();
	if (completion_.open && APP->event->getSelectedWidget() != this) // clicked elsewhere
		completion_.close();
	else if (completion_.open && (changed || doc->cursor() != seenCursor_)) // undone, a patch loaded, ...
		completion_.update(*doc, docs(), false);
	seenVersion_ = doc->version();
	seenCursor_ = doc->cursor();
	clampScroll();
	hover(changed);
	showTip();
}

/** The hover docs: the identifier under the mouse, once it stayed on it for 0.5 s. */
void View::hover(bool changed) {
	size_t b = 0, e = 0;
	int line = (int)std::floor((hoverPos_.y - kPad + scroll_.y) / lineH());
	bool over = APP->event->getHoveredWidget() == this && !dragging_ && hoverPos_.x >= gutterW() &&
				hoverPos_.y < textH() && line >= 0 && line < doc->lineCount() &&
				identAt(*doc, hit(hoverPos_, false), b, e);
	if (!over || changed) {
		hoverBegin_ = SIZE_MAX;
		hover_.clear();
	} else if (b != hoverBegin_) {
		hoverBegin_ = b;
		hoverSince_ = system::getTime();
		hover_.clear();
	} else if (hover_.empty() && system::getTime() - hoverSince_ >= 0.5)
		hover_ = lookup(docs(), doc->text().substr(b, e - b));
}

/** The tip: the popup (and the selected entry's docs), else the hover docs, at their identifier while in sight. */
void View::showTip() {
	bool popup = completion_.open, show = popup || !hover_.empty();
	size_t at = popup ? completion_.from : hoverBegin_;
	math::Vec p;
	if (show) {
		p = math::Vec(gutterW() + doc->column(at) * charW_ - scroll_.x, kPad + doc->lineOf(at) * lineH() - scroll_.y);
		show = p.y >= 0 && p.y + lineH() <= textH() + 1 && p.x >= gutterW() - 1 && p.x <= box.size.x;
	}
	if (!show) {
		if (tip_)
			tip_->hide();
		return;
	}
	if (!tip_) {
		tip_ = new Tip;
		APP->scene->addChild(tip_);
	}
	tip_->show();
	tip_->items = popup ? completion_.items : std::vector<Match>();
	tip_->details = popup ? std::vector<Match>{completion_.items[completion_.sel]} : hover_;
	tip_->sel = completion_.sel;
	tip_->top = completion_.top;
	tip_->at = getAbsoluteOffset(p);
	tip_->lineH = lineH() * getAbsoluteZoom();
}

float View::gutterW() const {
	int digits = std::max(2, (int)std::to_string(doc->lineCount()).size());
	return 2 * kPad + digits * charW_;
}

void View::follow() {
	int line = doc->lineOf(doc->cursor());
	float top = line * lineH(), x = doc->column(doc->cursor()) * charW_;
	float h = textH() - 2 * kPad, w = box.size.x - gutterW() - kPad;
	if (top < scroll_.y)
		scroll_.y = top;
	else if (top + lineH() > scroll_.y + h)
		scroll_.y = top + lineH() - h;
	if (x < scroll_.x)
		scroll_.x = std::max(0.f, x - 8 * charW_);
	else if (x + charW_ > scroll_.x + w)
		scroll_.x = x + charW_ - w + 8 * charW_;
}

void View::clampScroll() {
	float maxY = std::max(0.f, doc->lineCount() * lineH() - (textH() - 2 * kPad));
	float maxX = std::max(0.f, (columns_ + 1) * charW_ - (box.size.x - gutterW() - kPad));
	scroll_.x = math::clamp(scroll_.x, 0.f, maxX);
	scroll_.y = math::clamp(scroll_.y, 0.f, maxY);
}

size_t View::hit(math::Vec pos, bool nearest) const {
	int line = (int)std::floor((pos.y - kPad + scroll_.y) / lineH());
	float c = (pos.x - gutterW() + scroll_.x) / charW_; // in the gutter: negative, the line's start
	int col = std::max(0, (int)(nearest ? std::round(c) : std::floor(c)));
	return doc->at(math::clamp(line, 0, doc->lineCount() - 1), col);
}

void View::draw(const DrawArgs& args) {
	if (!panel)
		paint(args.vg);
	Widget::draw(args);
}

void View::drawLayer(const DrawArgs& args, int layer) {
	if (panel && layer == 1)
		paint(args.vg);
	Widget::drawLayer(args, layer);
}

void View::paint(NVGcontext* vg) {
	std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system(kFont));
	if (!doc || !font || font->handle < 0)
		return;
	const std::string& s = doc->text();
	float lh = lineH(), th = textH(), w = box.size.x;
	nvgSave(vg);
	nvgIntersectScissor(vg, 0, 0, w, th);
	nvgFontFaceId(vg, font->handle);
	nvgFontSize(vg, fontSize);
	nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
	charW_ = nvgTextBounds(vg, 0, 0, "0000000000", nullptr, nullptr) / 10;
	// Only the visible lines: the cost per frame doesn't grow with the document.
	int first = std::max(0, (int)(scroll_.y / lh)), last = std::min(doc->lineCount() - 1, (int)((scroll_.y + th) / lh));
	auto X = [&](int col) { return gutterW() + col * charW_ - scroll_.x; };
	auto Y = [&](int line) { return kPad + line * lh - scroll_.y; };
	// [b, e) as a box per visible line, half a char for a newline in it.
	auto mark = [&](size_t b, size_t e, NVGcolor c, bool outline) {
		for (int l = std::max(first, doc->lineOf(b)), l1 = std::min(last, doc->lineOf(e)); l <= l1; l++) {
			size_t le = doc->lineEnd(l);
			float x0 = X(doc->column(std::max(b, doc->lineStart(l)))), x1 = X(doc->column(std::min(e, le)));
			x1 += e > le ? charW_ / 2 : 0;
			nvgBeginPath(vg);
			nvgRoundedRect(vg, x0, Y(l), x1 - x0, lh, outline ? 2 : 0);
			nvgFillColor(vg, c);
			nvgFill(vg);
			if (outline) {
				nvgStrokeColor(vg, kText);
				nvgStrokeWidth(vg, 1);
				nvgStroke(vg);
			}
		}
	};

	int err = doc->errorLine();
	bool errVisible = err >= first && err <= last;
	if (errVisible) {
		nvgBeginPath(vg);
		nvgRect(vg, 0, Y(err), w, lh);
		nvgFillColor(vg, nvgRGBA(0xff, 0x6b, 0x5b, 0x30));
		nvgFill(vg);
	}
	if (doc->hasSelection())
		mark(doc->selBegin(), doc->selEnd(), kSelection, false);
	if (engine) {
		engine->highlights(playing_);
		for (const filo::Highlight& h : playing_) {
			size_t b = 0, e = 0;
			if (doc->locate(h.id, h.start, h.end, b, e))
				mark(b, e, kPlaying, true);
		}
	}

	// Text: a run per token (and between them); a non-ASCII char on its own, so columns stay put whatever its glyph
	// (the fallback fonts' are wider).
	auto run = [&](size_t a, size_t b, int col, float y) {
		bool ascii = std::all_of(s.begin() + a, s.begin() + b, [](char c) { return (unsigned char)c < 0x80; });
		if (ascii) {
			if (X(col) < w && X(col + (int)(b - a)) > 0)
				nvgText(vg, X(col), y, s.data() + a, s.data() + b);
			return col + (int)(b - a);
		}
		for (size_t i = a, j; i < b; i = j, col++) {
			j = nextChar(s, i);
			nvgText(vg, X(col), y, s.data() + i, s.data() + j);
		}
		return col;
	};
	const std::vector<Token>& toks = doc->tokens();
	auto tok = std::lower_bound(toks.begin(), toks.end(), doc->lineStart(first),
								[](const Token& t, size_t p) { return t.end <= p; });
	for (int l = first; l <= last; l++) {
		size_t p = doc->lineStart(l), le = doc->lineEnd(l);
		float y = Y(l) + lh / 2;
		for (int col = 0; p < le && X(col) < w;) {
			while (tok != toks.end() && tok->end <= p)
				tok++;
			bool in = tok != toks.end() && tok->begin <= p;
			size_t q = std::min(le, in ? tok->end : tok != toks.end() ? tok->begin : le);
			nvgFillColor(vg, in ? color(tok->kind) : kText);
			col = run(p, q, col, y);
			p = q;
		}
	}
	if (errVisible && !error_.empty()) {
		nvgFillColor(vg, kError);
		nvgText(vg, X(doc->column(doc->lineEnd(err)) + 2), Y(err) + lh / 2, error_.c_str(), nullptr);
	}
	if (APP->event->getSelectedWidget() == this) {
		int l = doc->lineOf(doc->cursor());
		nvgBeginPath(vg);
		nvgRect(vg, X(doc->column(doc->cursor())) - 0.5f, Y(l) + 1, 1.5f, lh - 2);
		nvgFillColor(vg, kMessage);
		nvgFill(vg);
	}

	// Line numbers over the text scrolled under them, the error line's marker at the left edge.
	float gw = gutterW();
	nvgBeginPath(vg);
	nvgRect(vg, 0, 0, gw, th);
	nvgFillColor(vg, kBackground);
	nvgFill(vg);
	nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
	int cur = doc->lineOf(doc->cursor());
	for (int l = first; l <= last; l++) {
		nvgFillColor(vg, l == err ? kError : l == cur ? kText : kDim);
		std::string n = std::to_string(l + 1);
		nvgText(vg, gw - kPad, Y(l) + lh / 2, n.c_str(), nullptr);
	}
	if (errVisible) {
		nvgBeginPath(vg);
		nvgRect(vg, 0, Y(err), 2, lh);
		nvgFillColor(vg, kError);
		nvgFill(vg);
	}
	nvgRestore(vg);

	// Status line: the engine's last message, frames played as silence.
	float sh = statusH();
	nvgSave(vg);
	nvgIntersectScissor(vg, 0, th, w, sh);
	nvgBeginPath(vg);
	nvgRect(vg, kPad, th, w - 2 * kPad, 1);
	nvgFillColor(vg, kDim);
	nvgFill(vg);
	nvgFontFaceId(vg, APP->window->uiFont->handle);
	nvgFontSize(vg, std::round(fontSize * 0.9f));
	float uw = 0;
	if (engine) {
		std::string under = "underflow " + std::to_string(engine->missingFrames.load());
		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, kDim);
		nvgText(vg, w - kPad, th + sh / 2, under.c_str(), nullptr);
		uw = nvgTextBounds(vg, 0, 0, under.c_str(), nullptr, nullptr) + 3 * kPad;
	}
	nvgIntersectScissor(vg, 0, th, w - uw, sh);
	nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
	nvgFillColor(vg, error_.empty() ? kMessage : kError);
	nvgText(vg, kPad, th + sh / 2, message_.c_str(), nullptr);
	nvgRestore(vg);
}

// --- Mouse ---

void View::onHover(const HoverEvent& e) {
	hoverPos_ = e.pos;
	e.consume(this); // hovered (getHoveredWidget()): the hover docs
}

void View::onButton(const ButtonEvent& e) {
	if (!doc || e.button != GLFW_MOUSE_BUTTON_LEFT) // right: the module's menu
		return;
	if (e.action == GLFW_PRESS)
		completion_.close();
	if (e.action == GLFW_PRESS && e.pos.y < textH()) { // (on the status line: only selects this)
		// Rack reports double-clicks only (0.5 s); a third press within that and close by is a triple-click.
		double now = system::getTime();
		clicks_ = now - pressTime_ <= 0.5 && e.pos.minus(pressAt_).norm() <= 4.f ? clicks_ + 1 : 1;
		pressTime_ = now;
		pressAt_ = e.pos;
		dragging_ = true;
		wordDrag_ = false;
		doc->setCursor(hit(e.pos, true), e.mods & GLFW_MOD_SHIFT);
		if (clicks_ == 3) { // the whole line, text or gutter, past its end too
			doc->selectLine(doc->lineOf(hit(e.pos, false)));
			wordDrag_ = true; // (a double-click's drag doesn't extend, so neither does this)
			clicks_ = 0;
		}
	}
	e.consume(this); // selects this: the keys come here
}

void View::onDoubleClick(const DoubleClickEvent& e) {
	if (!doc || !dragging_) // (a click on the overlay's title bar gives this the drag too)
		return;
	doc->selectWord(hit(pressAt_, false));
	wordDrag_ = true; // the word stays selected, however the mouse moves
}

void View::onDragMove(const DragMoveEvent& e) {
	if (!doc || !dragging_ || wordDrag_ || e.mouseDelta.isZero())
		return;
	// Beyond the box too (follow() scrolls): the mouse in local coordinates.
	math::Vec pos = APP->scene->getMousePos().minus(getAbsoluteOffset(math::Vec())).div(getAbsoluteZoom());
	doc->setCursor(hit(pos, true), true);
}

void View::onDragEnd(const DragEndEvent& e) {
	dragging_ = false;
}

void View::onHoverScroll(const HoverScrollEvent& e) {
	if (!doc)
		return;
	math::Vec d = e.scrollDelta;
#if !defined ARCH_MAC // like Rack's ScrollWidget: Shift scrolls sideways (macOS does it already)
	if (APP->window->getMods() & GLFW_MOD_SHIFT)
		d = d.flip();
#endif
	scroll_ = scroll_.minus(d);
	clampScroll();
	e.consume(this); // Rack neither pans nor zooms
}

// --- Keyboard ---

void View::evaluate() {
	if (engine)
		doc->evaluated(engine->evaluate(doc->text(), true));
}

/** The completion popup's keys (unmodified): Esc closes it (only), Up/Down and PageUp/PageDown select, Tab or Enter
accept. False: not one of those. */
bool View::completionKey(const SelectKeyEvent& e) {
	int k = e.key;
	if (k != GLFW_KEY_ESCAPE && k != GLFW_KEY_UP && k != GLFW_KEY_DOWN && k != GLFW_KEY_PAGE_UP &&
		k != GLFW_KEY_PAGE_DOWN && k != GLFW_KEY_TAB && k != GLFW_KEY_ENTER && k != GLFW_KEY_KP_ENTER)
		return false;
	e.consume(this); // Esc: not the overlay's, not Rack's
	if (e.action != GLFW_PRESS && e.action != GLFW_REPEAT)
		return true;
	if (k == GLFW_KEY_ESCAPE)
		completion_.close();
	else if (k == GLFW_KEY_UP || k == GLFW_KEY_DOWN)
		completion_.move(k == GLFW_KEY_UP ? -1 : 1, true);
	else if (k == GLFW_KEY_PAGE_UP || k == GLFW_KEY_PAGE_DOWN)
		completion_.move(k == GLFW_KEY_PAGE_UP ? -Completion::kRows : Completion::kRows, false);
	else
		completion_.accept(*doc);
	return true;
}

void View::onSelectKey(const SelectKeyEvent& e) {
	if (!doc)
		return;
	if (completion_.open && (e.mods & RACK_MOD_MASK) == 0 && completionKey(e))
		return;
	// Rack's own: save, save as, quit, open, new. Esc in the panel: Rack leaves fullscreen.
	for (int key : {GLFW_KEY_S, GLFW_KEY_Q, GLFW_KEY_O, GLFW_KEY_N})
		if (e.isKeyCommand(key, RACK_MOD_CTRL))
			return;
	if (e.isKeyCommand(GLFW_KEY_S, RACK_MOD_CTRL | GLFW_MOD_SHIFT) || (e.key == GLFW_KEY_ESCAPE && !onEscape))
		return;
	// Everything else is the editor's, used or not: Rack's would act on the module under the mouse (Backspace
	// deletes it, Cmd+Z undoes in the rack).
	e.consume(this);
	if (e.action != GLFW_PRESS && e.action != GLFW_REPEAT)
		return;
	int mods = e.mods & RACK_MOD_MASK, m = mods & ~GLFW_MOD_SHIFT;
	bool shift = mods & GLFW_MOD_SHIFT;
	int page = std::max(1, (int)(textH() / lineH()) - 1);
	GLFWwindow* win = APP->window->win;

	if (e.key == GLFW_KEY_ESCAPE) {
		if (e.action == GLFW_PRESS)
			onEscape();
	} else if ((e.key == GLFW_KEY_SPACE && mods == GLFW_MOD_CONTROL) // Ctrl+Space everywhere, as in CodeMirror
#if defined ARCH_MAC
			   || e.isKeyCommand(GLFW_KEY_I, GLFW_MOD_SUPER) // macOS: Ctrl+Space often switches the input source
#endif
	) {
		completion_.request(*doc, docs());
	} else if (e.isKeyCommand(GLFW_KEY_ENTER, RACK_MOD_CTRL) || e.isKeyCommand(GLFW_KEY_KP_ENTER, RACK_MOD_CTRL)) {
		completion_.close();
		if (e.action == GLFW_PRESS)
			evaluate();
	} else if (m == RACK_MOD_CTRL && (e.key == GLFW_KEY_SLASH || e.key == GLFW_KEY_KP_DIVIDE || e.keyName == "/"))
		doc->toggleComment(); // by key name too: '/' is elsewhere (or Shift+7) on other layouts
	else if (e.isKeyCommand(GLFW_KEY_PERIOD, RACK_MOD_CTRL)) {
		if (engine)
			engine->stop();
	} else if (e.isKeyCommand(GLFW_KEY_Z, RACK_MOD_CTRL))
		doc->undo();
	else if (e.isKeyCommand(GLFW_KEY_Z, RACK_MOD_CTRL | GLFW_MOD_SHIFT))
		doc->redo();
#if !defined ARCH_MAC
	else if (e.isKeyCommand(GLFW_KEY_Y, RACK_MOD_CTRL))
		doc->redo();
#endif
	else if (e.isKeyCommand(GLFW_KEY_A, RACK_MOD_CTRL))
		doc->selectAll();
	else if (e.isKeyCommand(GLFW_KEY_C, RACK_MOD_CTRL) || e.isKeyCommand(GLFW_KEY_X, RACK_MOD_CTRL)) {
		if (doc->hasSelection()) {
			glfwSetClipboardString(win, doc->selection().c_str());
			if (e.isKeyCommand(GLFW_KEY_X, RACK_MOD_CTRL))
				doc->backspace();
		}
	} else if (e.isKeyCommand(GLFW_KEY_V, RACK_MOD_CTRL)) {
		if (const char* clip = glfwGetClipboardString(win))
			doc->insert(clean(clip));
	} else if ((e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER) && m == 0)
		doc->newline();
	else if (e.key == GLFW_KEY_TAB && mods == 0)
		doc->insert(kIndent);
	else if (e.key == GLFW_KEY_BACKSPACE && (m == 0 || m == kWordMod))
		doc->backspace(m == kWordMod);
	else if (e.key == GLFW_KEY_DELETE && (m == 0 || m == kWordMod))
		doc->del(m == kWordMod);
	else if (e.key == GLFW_KEY_LEFT || e.key == GLFW_KEY_RIGHT) {
		bool left = e.key == GLFW_KEY_LEFT;
		if (m == 0)
			left ? doc->left(shift) : doc->right(shift);
		else if (m == kWordMod)
			left ? doc->wordLeft(shift) : doc->wordRight(shift);
#if defined ARCH_MAC
		else if (m == GLFW_MOD_SUPER)
			left ? doc->home(shift) : doc->end(shift);
#endif
	} else if (e.key == GLFW_KEY_UP || e.key == GLFW_KEY_DOWN) {
		bool up = e.key == GLFW_KEY_UP;
		if (m == 0)
			doc->vertical(up ? -1 : 1, shift);
#if defined ARCH_MAC
		else if (m == GLFW_MOD_SUPER)
			doc->setCursor(up ? 0 : doc->text().size(), shift);
#endif
	} else if (e.key == GLFW_KEY_HOME || e.key == GLFW_KEY_END) {
		bool home = e.key == GLFW_KEY_HOME;
		if (m == 0)
			home ? doc->home(shift) : doc->end(shift);
		else if (m == RACK_MOD_CTRL)
			doc->setCursor(home ? 0 : doc->text().size(), shift);
	} else if (e.key == GLFW_KEY_PAGE_UP && m == 0)
		doc->vertical(-page, shift);
	else if (e.key == GLFW_KEY_PAGE_DOWN && m == 0)
		doc->vertical(page, shift);
	if (completion_.open) // moved out of the identifier, backspaced, ...
		completion_.update(*doc, docs(), false);
}

void View::onSelectText(const SelectTextEvent& e) {
	if (!doc || e.codepoint < 32 || e.codepoint == 127)
		return;
	if (e.codepoint == ' ' && (APP->window->getMods() & RACK_MOD_MASK) == GLFW_MOD_CONTROL)
		return e.consume(this); // Ctrl+Space opens the completions (Windows also sends the space)
	doc->type(utf8(e.codepoint));
	completion_.update(*doc, docs(), e.codepoint < 0x80 && isIdent((char)e.codepoint));
	e.consume(this);
}

// --- Overlay ---

Overlay::Overlay(View* panel) : panel_(panel) {
	view = new View;
	view->doc = panel->doc;
	view->engine = panel->engine;
	view->panel = false;
	view->fontSize = 16;
	view->onEscape = [this] { close(); };
	addChild(view);
}

void Overlay::close() {
	if (View* p = panel_.get())
		APP->event->setSelectedWidget(p);
	requestDelete();
}

void Overlay::detach() {
	view->doc = nullptr;
	view->engine = nullptr;
	requestDelete();
}

math::Rect Overlay::closeBox() const {
	return math::Rect(box.size.x - 120, 0, 120, kHeader);
}

void Overlay::step() {
	if (parent) // the scene: the whole window, also after Rack's fullscreen or a resize
		box = math::Rect(math::Vec(), parent->box.size);
	view->box = math::Rect(math::Vec(0, kHeader), box.size.minus(math::Vec(0, kHeader)));
	OpaqueWidget::step();
}

void Overlay::draw(const DrawArgs& args) {
	NVGcontext* vg = args.vg;
	nvgBeginPath(vg);
	nvgRect(vg, 0, 0, box.size.x, box.size.y);
	nvgFillColor(vg, kBackground);
	nvgFill(vg);
	nvgBeginPath(vg);
	nvgRect(vg, 0, 0, box.size.x, kHeader);
	nvgFillColor(vg, kAccent);
	nvgFill(vg);
	nvgFontFaceId(vg, APP->window->uiFont->handle);
	nvgFontSize(vg, 13);
	nvgFillColor(vg, kBackground);
	nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
	std::string title = string::f("Filo   %s+Enter: evaluate   %s+.: stop", RACK_MOD_CTRL_NAME, RACK_MOD_CTRL_NAME);
	nvgText(vg, 8, kHeader / 2, title.c_str(), nullptr);
	math::Rect c = closeBox();
	nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
	nvgText(vg, c.getRight() - 8, kHeader / 2, "Close (Esc)", nullptr);
	OpaqueWidget::draw(args);
}

void Overlay::onButton(const ButtonEvent& e) {
	OpaqueWidget::onButton(e);
	if (e.getTarget() != this || e.button != GLFW_MOUSE_BUTTON_LEFT) // the editor took it
		return;
	if (e.action == GLFW_PRESS && closeBox().contains(e.pos)) {
		close();
		if (View* p = panel_.get())
			e.consume(p); // Rack selects the target after this: the panel's editor, not the overlay going away
	} else
		e.consume(view); // the title bar: the editor gets the keys
}

void Overlay::onHoverKey(const HoverKeyEvent& e) {
	// The editor not selected: Esc closes; the rest stops here (Enter would open the module browser behind this).
	if (e.action == GLFW_PRESS && e.key == GLFW_KEY_ESCAPE)
		close();
	e.consume(this);
}

} // namespace editor
