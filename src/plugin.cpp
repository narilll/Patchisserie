#include <osdialog.h>
#include <rack.hpp>

#include "editor/view.hpp"
#include "engine.hpp"
#include "hydra.hpp"

using namespace rack;

Plugin* pluginInstance;

// --- Plugin-wide: online samples cache, the local samples folder (Patchisserie.json) ---

static fetch::Worker& web() {
	// Destroyed at plugin exit: idle threads are joined, a download in flight gets a short grace period.
	static fetch::Worker w(asset::user("Patchisserie/cache"), [](const std::string& url, const std::string& file) {
		return network::requestDownload(url, file);
	});
	return w;
}

static std::string samplesDir; // UI thread

static void loadSettings() {
	json_t* root = json_load_file(asset::user("Patchisserie.json").c_str(), 0, nullptr);
	const char* dir = nullptr;
	if (root && json_unpack(root, "{s?s}", "samplesDir", &dir) == 0 && dir)
		samplesDir = dir;
	json_decref(root);
}

static void saveSettings() {
	json_t* root = json_pack("{s:s}", "samplesDir", samplesDir.c_str());
	json_dump_file(root, asset::user("Patchisserie.json").c_str(), JSON_INDENT(2));
	json_decref(root);
}

// --- Filo ---

static const char kDefaultCode[] = "$: s(\"bd sd\").bank(\"tr909\")\n$: note(\"c3 e3 g3\").midi()\n";
static const std::vector<int> kDivisions = {1, 2, 4, 8, 16, 24, 32, 48, 96};

struct Filo : Module {
	enum ParamId { PLAY_PARAM, STOP_PARAM, FULLSCREEN_PARAM, PARAMS_LEN };
	enum InputId { PLAY_INPUT, STOP_INPUT, CLOCK_INPUT, RESET_INPUT, INPUTS_LEN };
	enum OutputId {
		L_OUTPUT,
		R_OUTPUT,
		VOCT_OUTPUT,
		GATE_OUTPUT,
		VEL_OUTPUT,
		TRIG_OUTPUT,
		CC_OUTPUT,
		CLK_OUTPUT,
		RESET_OUTPUT,
		OUTPUTS_LEN
	};

	std::unique_ptr<filo::Engine> engine;
	// Settings: set by the UI thread (menu, JSON), read by process(), like Rack's own modules.
	int voices = 16, polyMode = filo::VoiceAllocator::Rotate, clkDivision = 16, ppc = 16;
	bool autoPlay = true;
	// The code, its undo history and what of it plays (UI thread): the panel's and the fullscreen editor's.
	editor::Doc doc{kDefaultCode};
	bool added = false;

	dsp::SchmittTrigger playIn, stopIn, clockIn, resetIn;
	dsp::BooleanTrigger playButton, stopButton;
	dsp::PulseGenerator trigPulse[16], clkPulse, resetPulse;
	filo::Sequencer::Outputs out;
	std::atomic<float> mono{0}; // audio thread -> the Icing on the right: (L + R) / 2, its unpatched AUDIO input

	Filo() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, 0);
		configButton(PLAY_PARAM, "Play");
		configButton(STOP_PARAM, "Stop (hush, gates down)");
		configButton(FULLSCREEN_PARAM, "Fullscreen editor (Esc closes)");
		configInput(PLAY_INPUT, "Play trigger");
		configInput(STOP_INPUT, "Stop trigger");
		configInput(CLOCK_INPUT, "Clock (pulses per cycle: menu)");
		configInput(RESET_INPUT, "Reset to cycle 0");
		configOutput(L_OUTPUT, "Left");
		configOutput(R_OUTPUT, "Right");
		configOutput(VOCT_OUTPUT, ".midi() pitch (1 V/oct)");
		configOutput(GATE_OUTPUT, ".midi() gate");
		configOutput(VEL_OUTPUT, ".midi() velocity (0-10 V)");
		configOutput(TRIG_OUTPUT, ".midi() triggers by sound name");
		configOutput(CC_OUTPUT, ".midi() CC (0-10 V)");
		configOutput(CLK_OUTPUT, "Clock (pulses per cycle: menu)");
		configOutput(RESET_OUTPUT, "Reset (on play and reset)");
		filo::EngineConfig c;
		c.sampleRate = APP->engine->getSampleRate();
		c.resDir = asset::plugin(pluginInstance, "res");
		c.samplesDir = samplesDir;
		c.web = &web();
		c.log = [](const std::string& line, bool warning) {
			if (warning)
				WARN("Filo: %s", line.c_str());
			else
				INFO("Filo: %s", line.c_str());
		};
		engine.reset(new filo::Engine(c));
	}

	void evaluate(bool play) { doc.evaluated(engine->evaluate(doc.text(), play)); }

	void onAdd(const AddEvent&) override {
		added = true;
		evaluate(autoPlay);
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override { engine->setSampleRate(e.sampleRate); }

	void process(const ProcessArgs& args) override {
		bool play = playButton.process(params[PLAY_PARAM].getValue() > 0.f);
		play |= playIn.process(inputs[PLAY_INPUT].getVoltage(), 0.1f, 1.f); // both triggers see every sample
		bool stop = stopButton.process(params[STOP_PARAM].getValue() > 0.f);
		stop |= stopIn.process(inputs[STOP_INPUT].getVoltage(), 0.1f, 1.f);
		if (play)
			engine->play();
		if (stop)
			engine->stop();
		if (args.frame % 4096 == 0)
			engine->setBlockFrames(APP->engine->getBlockFrames());
		filo::Sequencer& seq = engine->seq;
		seq.voices.setChannels(voices);
		seq.voices.setMode((filo::VoiceAllocator::Mode)polyMode);
		seq.clockOut.setDivision(clkDivision);
		seq.clock.setPulsesPerCycle(ppc);
		filo::Sequencer::Inputs in;
		in.clockConnected = inputs[CLOCK_INPUT].isConnected();
		in.clockEdge = clockIn.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
		in.resetEdge = resetIn.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f);
		float l, r;
		if (engine->process(in, out, l, r))
			resetPulse.trigger(1e-3f);
		outputs[L_OUTPUT].setVoltage(5.f * l); // superdough's full scale 1.0 = 5 V
		outputs[R_OUTPUT].setVoltage(5.f * r);
		mono.store(0.5f * (l + r), std::memory_order_relaxed);
		for (int id : {VOCT_OUTPUT, GATE_OUTPUT, VEL_OUTPUT})
			outputs[id].setChannels(out.channels);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
		// GCC 12+ false positive in Rack's Port::setChannels() for a constant 16: its loop that clears the channels
		// above never runs then (a port has at most 16).
		outputs[TRIG_OUTPUT].setChannels(16);
		outputs[CC_OUTPUT].setChannels(16);
#pragma GCC diagnostic pop
		for (int c = 0; c < 16; c++) {
			outputs[VOCT_OUTPUT].setVoltage(out.voct[c], c);
			outputs[GATE_OUTPUT].setVoltage(out.gate[c], c);
			outputs[VEL_OUTPUT].setVoltage(out.vel[c], c);
			if (out.trig[c])
				trigPulse[c].trigger(1e-3f);
			outputs[TRIG_OUTPUT].setVoltage(trigPulse[c].process(args.sampleTime) ? 10.f : 0.f, c);
			outputs[CC_OUTPUT].setVoltage(out.cc[c], c);
		}
		if (out.clk)
			clkPulse.trigger(1e-3f);
		outputs[CLK_OUTPUT].setVoltage(clkPulse.process(args.sampleTime) ? 10.f : 0.f);
		outputs[RESET_OUTPUT].setVoltage(resetPulse.process(args.sampleTime) ? 10.f : 0.f);
	}

	static json_t* mapJson(const filo::Engine::Map& m) {
		json_t* o = json_object();
		for (auto& kv : m)
			json_object_set_new(o, kv.first.c_str(), json_integer(kv.second));
		return o;
	}
	static filo::Engine::Map mapFrom(json_t* o) {
		filo::Engine::Map m;
		const char* key;
		json_t* v;
		json_object_foreach(o, key, v) {
			if (json_is_integer(v))
				m.push_back({key, (int)json_integer_value(v)});
		}
		return m;
	}

	json_t* dataToJson() override {
		filo::Engine::Map trig, cc;
		engine->getMaps(trig, cc);
		return json_pack("{s:s, s:i, s:i, s:i, s:i, s:b, s:o, s:o}", "code", doc.text().c_str(), "voices", voices,
						 "polyMode", polyMode, "clkDivision", clkDivision, "ppc", ppc, "autoPlay", autoPlay, "trigMap",
						 mapJson(trig), "ccMap", mapJson(cc));
	}

	void dataFromJson(json_t* root) override {
		const char* code = nullptr;
		int play = autoPlay;
		json_t *trig = nullptr, *cc = nullptr;
		json_unpack(root, "{s?s, s?i, s?i, s?i, s?i, s?b, s?o, s?o}", "code", &code, "voices", &voices, "polyMode",
					&polyMode, "clkDivision", &clkDivision, "ppc", &ppc, "autoPlay", &play, "trigMap", &trig, "ccMap",
					&cc);
		// Patch data is a real boundary.
		voices = clamp(voices, 1, 16);
		polyMode = clamp(polyMode, 0, (int)filo::VoiceAllocator::MPE);
		if (std::find(kDivisions.begin(), kDivisions.end(), clkDivision) == kDivisions.end())
			clkDivision = 16;
		if (std::find(kDivisions.begin(), kDivisions.end(), ppc) == kDivisions.end())
			ppc = 16;
		autoPlay = play;
		engine->setMaps(trig ? mapFrom(trig) : filo::Engine::Map(), cc ? mapFrom(cc) : filo::Engine::Map());
		if (code)
			doc.setText(code);
		if (added) // a preset or undo on a module that is running already; a new one evaluates in onAdd()
			evaluate(autoPlay);
	}
};

// Panel layout (mm), matching res/Filo.svg: the editor with its status line at the bottom.
static const math::Rect kEditorMm = math::Rect(3, 9, 197.2, 87);

/** The port labels. */
struct PanelText : widget::Widget {
	std::vector<std::pair<math::Vec, std::string>> labels; // px, centered

	void draw(const DrawArgs& args) override {
		nvgFontFaceId(args.vg, APP->window->uiFont->handle);
		nvgFontSize(args.vg, 9);
		nvgFillColor(args.vg, nvgRGB(0x4a, 0x33, 0x22));
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
		for (auto& l : labels)
			nvgText(args.vg, l.first.x, l.first.y, l.second.c_str(), nullptr);
	}
};

struct FiloWidget : ModuleWidget {
	editor::View* view;                // the panel's editor
	WeakPtr<editor::Overlay> overlay;  // the fullscreen editor, while open
	editor::Doc preview{kDefaultCode}; // the module browser's
	bool fullscreenDown = false;

	FiloWidget(Filo* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Filo.svg")));
		view = createWidget<editor::View>(mm2px(kEditorMm.pos));
		view->box.size = mm2px(kEditorMm.size);
		view->doc = module ? &module->doc : &preview;
		view->engine = module ? module->engine.get() : nullptr;
		addChild(view);
		auto* text = createWidget<PanelText>(Vec());
		text->box.size = box.size;
		addChild(text);
		auto label = [&](float x, float y, const char* name) { text->labels.push_back({mm2px(Vec(x, y - 5)), name}); };
		const char* names[] = {"PLAY", "STOP", "FULL"};
		for (int i = 0; i < Filo::PARAMS_LEN; i++) {
			addParam(createParamCentered<VCVButton>(mm2px(Vec(9 + 13 * i, 105)), module, i));
			label(9 + 13 * i, 105, names[i]);
		}
		const char* inputs[] = {"PLAY", "STOP", "CLOCK", "RESET"};
		for (int i = 0; i < Filo::INPUTS_LEN; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.5 + 10 * i, 117)), module, i));
			label(6.5 + 10 * i, 117, inputs[i]);
		}
		const char* outputs[] = {"L", "R", "V/OCT", "GATE", "VEL", "TRIG", "CC", "CLK", "RESET"};
		for (int i = 0; i < Filo::OUTPUTS_LEN; i++) {
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(55 + 17.5f * i, 112)), module, i));
			label(55 + 17.5f * i, 112, outputs[i]);
		}
	}

	~FiloWidget() override {
		if (editor::Overlay* o = overlay.get()) // still open: the module (its doc) is deleted right after this
			o->detach();
	}

	void step() override {
		ModuleWidget::step();
		// The FULL button (Rack holds a momentary press for a frame at least).
		bool down = module && module->params[Filo::FULLSCREEN_PARAM].getValue() > 0.f;
		if (down && !fullscreenDown && !overlay.get()) {
			auto* o = new editor::Overlay(view);
			APP->scene->addChild(o);
			APP->event->setSelectedWidget(o->view);
			overlay = o;
		}
		fullscreenDown = down;
	}

	void appendContextMenu(Menu* menu) override {
		Filo* m = static_cast<Filo*>(module);
		menu->addChild(new MenuSeparator);
		std::vector<std::string> counts, divisions;
		for (int i = 1; i <= 16; i++)
			counts.push_back(std::to_string(i));
		for (int d : kDivisions)
			divisions.push_back(std::to_string(d));
		auto index = [](int value) {
			return (size_t)(std::find(kDivisions.begin(), kDivisions.end(), value) - kDivisions.begin());
		};
		menu->addChild(createIndexSubmenuItem(
			"Voices", counts, [=] { return (size_t)m->voices - 1; }, [=](size_t i) { m->voices = (int)i + 1; }));
		menu->addChild(createIndexSubmenuItem(
			"Polyphony mode", {"Rotate", "Reuse", "Reset", "MPE"}, [=] { return (size_t)m->polyMode; },
			[=](size_t i) { m->polyMode = (int)i; }));
		menu->addChild(createIndexSubmenuItem(
			"CLK output: pulses per cycle", divisions, [=] { return index(m->clkDivision); },
			[=](size_t i) { m->clkDivision = kDivisions[i]; }));
		menu->addChild(createIndexSubmenuItem(
			"CLOCK input: pulses per cycle", divisions, [=] { return index(m->ppc); },
			[=](size_t i) { m->ppc = kDivisions[i]; }));
		menu->addChild(createBoolPtrMenuItem("Auto-play", "", &m->autoPlay));
		menu->addChild(createMenuItem("Reset TRIG/CC channel maps", "", [=] { m->engine->resetMaps(); }));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Local samples: " + (samplesDir.empty() ? "none" : samplesDir)));
		menu->addChild(createMenuItem("Choose local samples folder...", "", [=] {
			char* dir =
				osdialog_file(OSDIALOG_OPEN_DIR, samplesDir.empty() ? nullptr : samplesDir.c_str(), nullptr, nullptr);
			if (!dir)
				return;
			samplesDir = dir;
			std::free(dir);
			saveSettings();
			m->engine->setSamplesDir(samplesDir); // other Filos pick it up when they start
		}));
		if (!samplesDir.empty())
			menu->addChild(createMenuItem("Forget local samples folder", "", [=] {
				samplesDir.clear();
				saveSettings();
				m->engine->setSamplesDir(samplesDir);
			}));
		double mb = fetch::size(web().root()) / 1e6;
		menu->addChild(
			createMenuItem("Clear online samples cache", string::f("%.1f MB", mb), [] { fetch::clear(web().root()); }));
	}
};

// --- Icing: Hydra for the Filo on its left ---

static Model* modelFilo;

struct Icing : Module {
	enum InputId { AUDIO_INPUT, INPUTS_LEN };

	hydra::WindowGeometry window;
	/** Audio -> UI thread: what a.fft analyses, in Web Audio's full scale (5 V = 1). */
	dsp::RingBuffer<float, 1 << 13> audio;

	Icing() {
		config(0, INPUTS_LEN, 0, 0);
		configInput(AUDIO_INPUT, "Audio for a.fft (unpatched: the Filo's L+R)");
	}

	void process(const ProcessArgs&) override {
		Module* left = leftExpander.module;
		float x = 0;
		if (inputs[AUDIO_INPUT].isConnected())
			x = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		else if (left && left->model == modelFilo)
			x = static_cast<Filo*>(left)->mono.load(std::memory_order_relaxed);
		if (!audio.full()) // drained every UI frame; full while nothing renders
			audio.push(x);
	}

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
static const Vec kAudioMm = Vec(40.64, 108);

struct IcingWidget : ModuleWidget {
	hydra::Renderer renderer;
	hydra::Window output;
	hydra::Analyser analyser;
	bool outputOpen = false;
	int64_t filoId = -1; // the Filo whose Hydra this renders (-1: none)

	IcingWidget(Icing* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Icing.svg")));
		addInput(createInputCentered<PJ301MPort>(mm2px(kAudioMm), module, Icing::AUDIO_INPUT));
		auto* text = createWidget<PanelText>(Vec());
		text->box.size = box.size;
		text->labels.push_back({mm2px(kAudioMm.minus(Vec(0, 5))), "AUDIO"});
		addChild(text);
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

	/** The Filo directly on the left, if any. Rack sets leftExpander.moduleId on this thread (the engine resolves
	.module on its own). */
	Filo* filo() const {
		int64_t id = module->leftExpander.moduleId;
		Module* m = id >= 0 ? APP->engine->getModule(id) : nullptr;
		return m && m->model == modelFilo ? static_cast<Filo*>(m) : nullptr;
	}

	void step() override {
		ModuleWidget::step();
		hydra::collectWindows();
		if (!module) // module browser preview
			return;
		Icing* m = static_cast<Icing*>(module);
		Filo* f = filo();
		if ((f ? f->id : -1) != filoId) { // moved, or its Filo was deleted or added
			filoId = f ? f->id : -1;
			renderer.reset();
		}
		while (!m->audio.empty())
			analyser.push(m->audio.shift());
		// Rendered here, not in draw(): draw() is skipped for off-screen modules, the output window must keep going.
		if (f) {
			filo::Engine* engine = f->engine.get();
			hydra::Exchange::Request q;
			q.time = system::getTime();
			q.total = analyser.analyse(APP->engine->getSampleRate(), q.specific);
			engine->hydra.request(q);
			renderer.update(APP->window->vg, engine->hydra, [=](const std::string& line) { engine->log(line, true); });
		}
		output.update(APP->window->win, f ? &renderer : nullptr, m->window, outputOpen);
	}

	void draw(const DrawArgs& args) override {
		ModuleWidget::draw(args);
		if (filoId >= 0)
			return;
		math::Rect r(mm2px(kPreviewMm.pos), mm2px(kPreviewMm.size));
		nvgFontFaceId(args.vg, APP->window->uiFont->handle);
		nvgFontSize(args.vg, 11);
		nvgFillColor(args.vg, nvgRGB(0xe8, 0xa7, 0xc3));
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgText(args.vg, r.getCenter().x, r.getCenter().y, "Place directly right of a Filo", nullptr);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1 && filoId >= 0) { // like lights: drawn after the room-brightness dimming
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
	loadSettings();
	p->addModel(modelFilo = createModel<Filo, FiloWidget>("Filo"));
	p->addModel(createModel<Icing, IcingWidget>("Icing"));
}
