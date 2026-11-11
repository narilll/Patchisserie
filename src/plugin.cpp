#include <rack.hpp>

#include "hydra.hpp"

using namespace rack;

Plugin* pluginInstance;

// ponytail: Icing is the M1b GL-window spike (hard-coded shader); M5 adds hydra-synth, the AUDIO input and Filo.
struct Icing : Module {
	hydra::WindowGeometry window;

	Icing() { config(0, 0, 0, 0); }

	json_t* dataToJson() override {
		const hydra::WindowGeometry& g = window;
		return json_pack("{s:{s:i, s:i, s:i, s:i, s:b}}", "window", "x", g.x, "y", g.y, "w", g.w, "h", g.h,
						 "fullscreen", g.fullscreen);
	}

	void dataFromJson(json_t* root) override {
		hydra::WindowGeometry& g = window;
		int fullscreen = g.fullscreen;
		json_unpack(root, "{s?{s?i, s?i, s?i, s?i, s?b}}", "window", "x", &g.x, "y", &g.y, "w", &g.w, "h", &g.h,
					"fullscreen", &fullscreen);
		g.fullscreen = fullscreen;
		g.w = clamp(g.w, 64, 16384); // x/y are checked against the connected monitors when the window opens
		g.h = clamp(g.h, 64, 16384);
	}
};

// Matches the preview frame in res/Icing.svg (mm).
static const math::Rect kPreviewMm = math::Rect(2.54, 12.7, 76.2, 42.8625);

struct IcingWidget : ModuleWidget {
	hydra::Renderer renderer;
	hydra::Window output;
	bool outputOpen = false;
	double startTime = system::getTime();

	IcingWidget(Icing* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Icing.svg")));
	}

	~IcingWidget() override {
		output.destroy();
		if (APP->window) // null at app exit, where onContextDestroy has already freed everything
			renderer.destroy();
	}

	void onContextDestroy(const ContextDestroyEvent& e) override {
		output.destroy(); // its context shares the dying one; reopening creates a fresh window
		renderer.destroy();
		outputOpen = false;
		ModuleWidget::onContextDestroy(e);
	}

	void step() override {
		ModuleWidget::step();
		if (!module) // module browser preview
			return;
		// Rendered here, not in draw(): draw() is skipped for off-screen modules, the output window must keep going.
		renderer.render(APP->window->vg, system::getTime() - startTime);
		output.update(APP->window->win, renderer, static_cast<Icing*>(module)->window, outputOpen);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) { // like lights: drawn after the room-brightness dimming
			math::Rect r(mm2px(kPreviewMm.pos), mm2px(kPreviewMm.size));
			renderer.drawPreview(args.vg, r.pos.x, r.pos.y, r.size.x, r.size.y);
		}
		ModuleWidget::drawLayer(args, layer);
	}

	void appendContextMenu(Menu* menu) override {
		Icing* m = static_cast<Icing*>(module);
		menu->addChild(new MenuSeparator);
		// Only flags here: menu actions run inside GLFW callbacks, step() applies them.
		menu->addChild(createBoolPtrMenuItem("Output window", "", &outputOpen));
		menu->addChild(createBoolPtrMenuItem("Fullscreen", "Esc", &m->window.fullscreen));
	}
};

void init(Plugin* p) {
	pluginInstance = p;
	p->addModel(createModel<Icing, IcingWidget>("Icing"));
}
