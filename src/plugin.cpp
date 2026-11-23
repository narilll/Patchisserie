#include <osdialog.h>
#include <rack.hpp>

#include "editor/grid.hpp"
#include "editor/view.hpp"
#include "engine.hpp"
#include "hydra.hpp"
#include "lattice.hpp"
#include "push2.hpp"
#include "tart.hpp"

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

// --- Output and editor windows: their geometry in the patch ---

static json_t* geometryJson(const hydra::WindowGeometry& g) {
	return json_pack("{s:i, s:i, s:i, s:i, s:b}", "x", g.x, "y", g.y, "w", g.w, "h", g.h, "fullscreen", g.fullscreen);
}

static void geometryFromJson(json_t* o, hydra::WindowGeometry& g) {
	int fullscreen = g.fullscreen;
	json_unpack(o, "{s?i, s?i, s?i, s?i, s?b}", "x", &g.x, "y", &g.y, "w", &g.w, "h", &g.h, "fullscreen", &fullscreen);
	g.fullscreen = fullscreen;
	g.w = clamp(g.w, 64, 16384); // x/y are checked against the connected monitors when the window opens
	g.h = clamp(g.h, 64, 16384);
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
	enum LightId { PLAY_LIGHT, LIGHTS_LEN };

	std::unique_ptr<filo::Engine> engine;
	// Settings: set by the UI thread (menu, JSON), read by process(), like Rack's own modules.
	int voices = 16, polyMode = filo::VoiceAllocator::Rotate, clkDivision = 16, ppc = 16;
	bool autoPlay = true;
	// The code, its undo history and what of it plays (UI thread): the panel's and the editor window's.
	editor::Doc doc{kDefaultCode};
	bool added = false;
	hydra::WindowGeometry window{100, 100, 960, 640}; // the editor window's (UI thread), saved in the patch
	bool windowOpen = false;
	std::string theme = "Patchisserie"; // the editor's colours (UI thread), saved in the patch

	dsp::SchmittTrigger playIn, stopIn, clockIn, resetIn;
	dsp::BooleanTrigger stopButton; // the PLAY button evaluates the code: FiloWidget::step(), on the UI thread
	dsp::PulseGenerator trigPulse[16], clkPulse, resetPulse;
	filo::Sequencer::Outputs out;
	// midin() and midikeys() in the code: driver, device and channel from the menu (Rack's), saved in the patch.
	midi::InputQueue midiInput;
	// process()'s, reused: tryPop() copies into its bytes, which keep their capacity (Rack's own MIDI modules make one
	// per sample). ponytail: while messages wait, tryPop() locks Rack's queue mutex and frees the popped one, as in
	// every Rack MIDI module; idle it returns before that.
	midi::Message midiMessage;
	std::atomic<float> mono{0}; // audio thread -> the Icing on the right: (L + R) / 2, its unpatched AUDIO input

	Filo() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configButton(PLAY_PARAM, "Play (evaluates the code, like Cmd/Ctrl+Enter)");
		configButton(STOP_PARAM, "Stop (hush, gates down)");
		configButton(FULLSCREEN_PARAM, "Editor window (F11 there: fullscreen)");
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
		bool play = playIn.process(inputs[PLAY_INPUT].getVoltage(), 0.1f, 1.f); // the transport only
		bool stop = stopButton.process(params[STOP_PARAM].getValue() > 0.f);    // both triggers see every sample
		stop |= stopIn.process(inputs[STOP_INPUT].getVoltage(), 0.1f, 1.f);
		if (play)
			engine->play();
		if (stop)
			engine->stop();
		if (args.frame % 4096 == 0)
			engine->setBlockFrames(APP->engine->getBlockFrames());
		while (midiInput.tryPop(&midiMessage, args.frame)) // Rack filters the menu's channel
			engine->midiMessage(midiMessage.getStatus(), midiMessage.getChannel(), midiMessage.getNote(),
								midiMessage.getValue());
		engine->midiDevice.store(midiInput.getDeviceId() >= 0, std::memory_order_relaxed);
		filo::Sequencer& seq = engine->seq;
		lights[PLAY_LIGHT].setBrightness(seq.started() ? 1.f : 0.f);
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
		return json_pack("{s:s, s:i, s:i, s:i, s:i, s:b, s:o, s:o, s:o, s:o, s:b, s:s}", "code", doc.text().c_str(),
						 "voices", voices, "polyMode", polyMode, "clkDivision", clkDivision, "ppc", ppc, "autoPlay",
						 autoPlay, "trigMap", mapJson(trig), "ccMap", mapJson(cc), "midi", midiInput.toJson(), "window",
						 geometryJson(window), "windowOpen", windowOpen, "theme", theme.c_str());
	}

	void dataFromJson(json_t* root) override {
		const char* code = nullptr;
		const char* themeName = nullptr;
		int play = autoPlay, open = windowOpen;
		json_t *trig = nullptr, *cc = nullptr, *midiJ = nullptr, *windowJ = nullptr;
		json_unpack(root, "{s?s, s?i, s?i, s?i, s?i, s?b, s?o, s?o, s?o, s?o, s?b, s?s}", "code", &code, "voices",
					&voices, "polyMode", &polyMode, "clkDivision", &clkDivision, "ppc", &ppc, "autoPlay", &play,
					"trigMap", &trig, "ccMap", &cc, "midi", &midiJ, "window", &windowJ, "windowOpen", &open, "theme",
					&themeName);
		// Patch data is a real boundary.
		voices = clamp(voices, 1, 16);
		polyMode = clamp(polyMode, 0, (int)filo::VoiceAllocator::MPE);
		if (std::find(kDivisions.begin(), kDivisions.end(), clkDivision) == kDivisions.end())
			clkDivision = 16;
		if (std::find(kDivisions.begin(), kDivisions.end(), ppc) == kDivisions.end())
			ppc = 16;
		autoPlay = play;
		geometryFromJson(windowJ, window); // older patches have none
		windowOpen = open;
		// An unknown name (an older patch, a theme Strudel dropped): the default.
		const std::vector<std::string>& names = editor::themeNames();
		theme = themeName && std::find(names.begin(), names.end(), themeName) != names.end() ? themeName : names[0];
		if (midiJ) // older patches have none
			midiInput.fromJson(midiJ);
		engine->setMaps(trig ? mapFrom(trig) : filo::Engine::Map(), cc ? mapFrom(cc) : filo::Engine::Map());
		if (code)
			doc.setText(code);
		if (added) // a preset or undo on a module that is running already; a new one evaluates in onAdd()
			evaluate(autoPlay);
	}
};

// Panel layout (mm), matching res/Filo.svg: the editor with its status line at the bottom.
static const math::Rect kEditorMm = math::Rect(3, 12, 95.6, 76);
// The panels' text: on the aluminium, and on the dark output band in the aluminium's colour.
static const NVGcolor kInk = nvgRGB(0, 0, 0), kPaper = nvgRGB(0xe6, 0xe6, 0xe0);

// Row 1: three buttons then four inputs, evenly spaced.
static float kRow1X(int i) {
	return 10.3f + 13.5f * i;
}

/** The panel's text (res/fonts, IBM Plex): the name, the foot and the port labels. Sizes and spacing in mm. */
struct PanelText : widget::Widget {
	enum Face { MonoBold, Condensed };
	struct Item {
		Vec pos; // px
		std::string text;
		Face face;
		float size, spacing; // mm
		NVGcolor color;
		int align;
	};
	std::vector<Item> items;

	void add(Vec mm, const std::string& text, Face face, float size, float spacing, NVGcolor color,
			 int align = NVG_ALIGN_LEFT) {
		items.push_back({mm2px(mm), text, face, size, spacing, color, align | NVG_ALIGN_BASELINE});
	}

	/** A port's label, centred above it; an output's sits on the dark band. */
	void label(Vec mm, const char* text, bool output = false) {
		add(Vec(mm.x, mm.y - 5.3f), text, MonoBold, 2.8f, 0, output ? kPaper : kInk, NVG_ALIGN_CENTER);
	}

	/** The module's name top left above the rule, PATCHISSERIE bottom left below the foot rule. */
	void band(const char* name) {
		add(Vec(3.2f, 8.8f), name, MonoBold, 6.f, 0.4f, kInk);
		add(Vec(3.2f, 127.5f), "PATCHISSERIE", Condensed, 2.4f, 0.6f, kInk);
	}

	void draw(const DrawArgs& args) override {
		static const char* const files[] = {"IBMPlexMono-SemiBold", "IBMPlexSansCondensed-SemiBold"};
		std::shared_ptr<window::Font> fonts[2];
		for (int i = 0; i < 2; i++) // as Rack recommends: loaded on every draw (cached by the window)
			fonts[i] =
				APP->window->loadFont(asset::plugin(pluginInstance, std::string("res/fonts/") + files[i] + ".ttf"));
		// Drawn in screen pixels from a whole-pixel origin, so the glyphs land on the pixel grid instead of being
		// resampled, which blurs small text when zoomed out. ponytail: assumes Rack's unrotated, uniform zoom.
		float t[6];
		nvgCurrentTransform(args.vg, t);
		nvgResetTransform(args.vg);
		for (const Item& it : items) {
			if (!fonts[it.face])
				continue;
			nvgFontFaceId(args.vg, fonts[it.face]->handle);
			nvgFontSize(args.vg, mm2px(it.size) * t[0]);
			nvgTextLetterSpacing(args.vg, mm2px(it.spacing) * t[0]);
			nvgFillColor(args.vg, it.color);
			nvgTextAlign(args.vg, it.align);
			nvgText(args.vg, std::round(t[0] * it.pos.x + t[4]), std::round(t[3] * it.pos.y + t[5]), it.text.c_str(),
					nullptr);
		}
		nvgTextLetterSpacing(args.vg, 0);
	}
};

struct FiloWidget : ModuleWidget {
	editor::View* view;                           // the panel's editor
	std::unique_ptr<editor::Window> editorWindow; // null: the module browser's preview
	editor::Doc preview{kDefaultCode};            // the module browser's
	bool fullscreenDown = false, playDown = false;

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
		text->band("FILO");
		auto label = [&](float x, float y, const char* name, bool output = false) {
			text->label(Vec(x, y), name, output);
		};
		const char* names[] = {"PLAY", "STOP", "FULL"};
		for (int i = 0; i < Filo::PARAMS_LEN; i++) {
			Vec pos = mm2px(Vec(kRow1X(i), 100));
			if (i == Filo::PLAY_PARAM) // lit red while playing
				addParam(createLightParamCentered<VCVLightBezel<RedLight>>(pos, module, i, Filo::PLAY_LIGHT));
			else
				addParam(createParamCentered<VCVButton>(pos, module, i));
			label(kRow1X(i), 100, names[i]);
		}
		const char* inputs[] = {"PLAY", "STOP", "CLOCK", "RESET"};
		for (int i = 0; i < Filo::INPUTS_LEN; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(kRow1X(3 + i), 100)), module, i));
			label(kRow1X(3 + i), 100, inputs[i]);
		}
		const char* outputs[] = {"L", "R", "V/OCT", "GATE", "VEL", "TRIG", "CC", "CLK", "RESET"};
		for (int i = 0; i < Filo::OUTPUTS_LEN; i++) {
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(7 + 10.95f * i, 117)), module, i));
			label(7 + 10.95f * i, 117, outputs[i], true);
		}
		if (module) // (destroyed before the module, which holds its doc and geometry)
			editorWindow.reset(
				new editor::Window(module->doc, module->engine.get(), module->window, module->windowOpen));
	}

	void onContextDestroy(const ContextDestroyEvent& e) override {
		if (editorWindow) // its context shares the dying one; the next step() opens a fresh one
			editorWindow->destroy();
		ModuleWidget::onContextDestroy(e);
	}

	void step() override {
		ModuleWidget::step();
		hydra::collectWindows();
		if (module) { // the menu's choice, in both editors at once
			const editor::EditorTheme* t = &editor::themeNamed(static_cast<Filo*>(module)->theme);
			view->theme = t;
			static_cast<editor::View*>(editorWindow->view)->theme = t;
		}
		// The FULL button (Rack holds a momentary press for a frame at least).
		bool down = module && module->params[Filo::FULLSCREEN_PARAM].getValue() > 0.f;
		if (down && !fullscreenDown)
			editorWindow->show();
		fullscreenDown = down;
		// PLAY: evaluate what the editor shows and play (like strudel.cc's play button; already playing: just update).
		down = module && module->params[Filo::PLAY_PARAM].getValue() > 0.f;
		if (down && !playDown)
			static_cast<Filo*>(module)->evaluate(true);
		playDown = down;
		// Drawn here, not in draw(): draw() is skipped for off-screen modules.
		// ponytail: titled by module id; the patch's name is in patch.hpp, which isn't Rack's public API.
		if (editorWindow)
			editorWindow->update(APP->window->win, "Filo — " + std::to_string(module->id));
	}

	void appendContextMenu(Menu* menu) override {
		Filo* m = static_cast<Filo*>(module);
		menu->addChild(new MenuSeparator);
		// Only flags here: menu actions run inside GLFW callbacks, step() applies them.
		menu->addChild(createBoolPtrMenuItem("Editor window", "", &m->windowOpen));
		menu->addChild(createBoolPtrMenuItem("Editor window fullscreen", "F11", &m->window.fullscreen));
		menu->addChild(createSubmenuItem("Theme", m->theme, [=](Menu* sub) {
			for (const std::string& name : editor::themeNames())
				sub->addChild(
					createCheckMenuItem(name, "", [=] { return m->theme == name; }, [=] { m->theme = name; }));
		}));
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
		menu->addChild(createMenuLabel("MIDI input (midin(), midikeys() in the code)"));
		appendMidiMenu(menu, &m->midiInput);

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

	json_t* dataToJson() override { return json_pack("{s:o}", "window", geometryJson(window)); }

	void dataFromJson(json_t* root) override { geometryFromJson(json_object_get(root, "window"), window); }
};

// Matches res/Icing.svg (mm): the jack's band on the left, the preview from there to the right edge.
static const math::Rect kPreviewMm = math::Rect(12, 12, 138.4, 111.6); // ends above the footer rule (124.6)
static const Vec kAudioMm = Vec(5.5, 118);

struct IcingWidget;

/** The Hydra preview: the image fitted to its box (rest black), or the hint. Double-click opens the output window. */
struct IcingPreview : widget::Widget {
	IcingWidget* icing;
	explicit IcingPreview(IcingWidget* w) : icing(w) {}
	void draw(const DrawArgs& args) override;
	void drawLayer(const DrawArgs& args, int layer) override;
	// No mouse handlers: the press goes on to the module, which drags it and opens its menu as elsewhere on the panel.
};

struct IcingWidget : ModuleWidget {
	hydra::Renderer renderer;
	hydra::Window output;
	hydra::Analyser analyser;
	hydra::CanvasSize canvas{0, 0}, wanted{0, 0}; // sent to Hydra; the latest wish, sent once it holds for a while
	int wantedFrames = 0;
	bool outputOpen = false;
	int64_t filoId = -1; // the Filo whose Hydra this renders (-1: none)
	IcingPreview* preview;

	IcingWidget(Icing* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Icing.svg")));
		addInput(createInputCentered<PJ301MPort>(mm2px(kAudioMm), module, Icing::AUDIO_INPUT));
		auto* text = createWidget<PanelText>(Vec());
		text->box.size = box.size;
		text->band("ICING");
		text->label(kAudioMm, "AUDIO");
		addChild(text);
		preview = new IcingPreview(this);
		preview->box = math::Rect(mm2px(kPreviewMm.pos), mm2px(kPreviewMm.size));
		addChild(preview);
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
			// The canvas follows the output window (the preview aspect without one). A resize storm applies once the
			// size has held for ~0.1 s; the first size at once.
			hydra::CanvasSize want =
				hydra::canvasSize(output.fbWidth, output.fbHeight, kPreviewMm.size.x / kPreviewMm.size.y);
			wantedFrames = want == wanted ? wantedFrames + 1 : 0;
			wanted = want;
			if (canvas.w == 0 || wantedFrames >= 6)
				canvas = wanted;
			q.width = canvas.w, q.height = canvas.h;
			engine->hydra.request(q);
			renderer.update(APP->window->vg, engine->hydra, [=](const std::string& line) { engine->log(line, true); });
		}
		if (outputOpen && output.win && glfwGetKey(output.win, GLFW_KEY_ESCAPE) == GLFW_PRESS) // sticky: a tap counts
			m->window.fullscreen = false;
		output.update(APP->window->win, "Icing", m->window, outputOpen, [&](int fw, int fh) {
			glClear(GL_COLOR_BUFFER_BIT);
			if (f)
				renderer.present(fw, fh);
		});
	}

	void appendContextMenu(Menu* menu) override {
		Icing* m = static_cast<Icing*>(module);
		menu->addChild(new MenuSeparator);
		// Only flags here: menu actions run inside GLFW callbacks, step() applies them.
		menu->addChild(createBoolPtrMenuItem("Output window", "", &outputOpen));
		menu->addChild(createBoolPtrMenuItem("Fullscreen", "Esc", &m->window.fullscreen));
	}

	// Rack sends the double-click to the widget that took the press: this module (the preview has no mouse handlers).
	// On the preview it opens the output window, or brings it to the front.
	void onDoubleClick(const DoubleClickEvent& e) override {
		if (!module || !preview->box.contains(APP->scene->rack->getMousePos().minus(box.pos))) {
			ModuleWidget::onDoubleClick(e);
			return;
		}
		outputOpen = true;
		if (GLFWwindow* win = output.win) { // step() creates it otherwise
			glfwShowWindow(win);
			glfwFocusWindow(win);
		}
	}
};

void IcingPreview::draw(const DrawArgs& args) {
	if (icing->filoId >= 0)
		return;
	std::shared_ptr<window::Font> font =
		APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/IBMPlexSansCondensed-SemiBold.ttf"));
	if (!font)
		return;
	nvgFontFaceId(args.vg, font->handle);
	nvgFontSize(args.vg, 11);
	nvgTextLetterSpacing(args.vg, 0);
	nvgFillColor(args.vg, nvgRGB(0xc6, 0xc7, 0xc2));
	nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
	nvgText(args.vg, box.size.x / 2, box.size.y / 2, "Place directly right of a Filo", nullptr);
}

void IcingPreview::drawLayer(const DrawArgs& args, int layer) {
	if (layer == 1 && icing->filoId >= 0) // like lights: drawn after the room-brightness dimming
		icing->renderer.drawPreview(args.vg, 0, 0, box.size.x, box.size.y);
}

// --- Lattice: Orca on a grid ---

struct Lattice : Module {
	enum ParamId { PLAY_PARAM, STOP_PARAM, FULLSCREEN_PARAM, PARAMS_LEN };
	enum InputId { PLAY_INPUT, STOP_INPUT, CLOCK_INPUT, RESET_INPUT, INPUTS_LEN };
	enum OutputId {
		VOCT_OUTPUT,
		GATE_OUTPUT,
		VEL_OUTPUT,
		TRIG_OUTPUT,
		CC_OUTPUT,
		CLK_OUTPUT,
		RESET_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId { PLAY_LIGHT, LIGHTS_LEN };

	lattice::Machine machine;
	editor::GridEdit edit; // the grid's cursor and undo history (UI thread): the panel's and the window's
	// Settings: set by the UI thread (menu, JSON), read by process(), like Filo's.
	float bpm = 120;
	int voices = 16, polyMode = filo::VoiceAllocator::MPE, clkDivision = 4, ppc = 4;
	bool autoPlay = true;
	hydra::WindowGeometry window{100, 100, 960, 640}; // the grid window's (UI thread), saved in the patch
	bool windowOpen = false;
	editor::Theme theme = editor::kPresets[0].theme; // the grid's colours (UI thread), saved in the patch
	std::string themeName = editor::kPresets[0].name;
	bool showDots = true; // empty cells show a dim dot (UI thread), saved in the patch

	dsp::SchmittTrigger playIn, stopIn, clockIn, resetIn;
	dsp::BooleanTrigger playButton, stopButton; // Lattice has nothing to evaluate: both buttons are transport
	dsp::PulseGenerator trigPulse[16], clkPulse, resetPulse;
	lattice::Machine::Outputs out{};

	Lattice() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configButton(PLAY_PARAM, "Play from frame 0 when stopped (Cmd/Ctrl+Enter; Space in the grid: pause/resume)");
		configButton(STOP_PARAM, "Stop (gates down)");
		configButton(FULLSCREEN_PARAM, "Grid window (F11 there: fullscreen)");
		configInput(PLAY_INPUT, "Play trigger (from frame 0)");
		configInput(STOP_INPUT, "Stop trigger");
		configInput(CLOCK_INPUT, "Clock (pulses per beat: menu)");
		configInput(RESET_INPUT, "Reset to frame 0");
		configOutput(VOCT_OUTPUT, "Pitch (1 V/oct; MPE: voice = Orca channel)");
		configOutput(GATE_OUTPUT, "Gate");
		configOutput(VEL_OUTPUT, "Velocity (0-10 V)");
		configOutput(TRIG_OUTPUT, "Triggers by Orca channel");
		configOutput(CC_OUTPUT, "CC by knob 0-F (0-10 V)");
		configOutput(CLK_OUTPUT, "Clock (pulses per beat: menu)");
		configOutput(RESET_OUTPUT, "Reset (on play and reset)");
		edit.machine = &machine;
		edit.refresh();
	}

	void onAdd(const AddEvent&) override {
		if (autoPlay)
			machine.request(lattice::Machine::Transport::Play);
	}

	void process(const ProcessArgs& args) override {
		bool play = playIn.process(inputs[PLAY_INPUT].getVoltage(), 0.1f, 1.f); // all triggers see every sample
		play |= playButton.process(params[PLAY_PARAM].getValue() > 0.f);
		bool stop = stopButton.process(params[STOP_PARAM].getValue() > 0.f);
		stop |= stopIn.process(inputs[STOP_INPUT].getVoltage(), 0.1f, 1.f);
		if (play)
			machine.request(lattice::Machine::Transport::Play);
		if (stop)
			machine.request(lattice::Machine::Transport::Stop);
		machine.setSampleRate(args.sampleRate);
		machine.setBpm(bpm);
		machine.setPulsesPerBeat(ppc);
		machine.setClockDivision(clkDivision);
		machine.setVoices(voices);
		machine.setPolyMode((filo::VoiceAllocator::Mode)polyMode);
		lattice::Machine::Inputs in;
		in.clockConnected = inputs[CLOCK_INPUT].isConnected();
		in.clockEdge = clockIn.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
		in.resetEdge = resetIn.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f);
		machine.process(in, out);
		lights[PLAY_LIGHT].setBrightness(machine.playing() ? 1.f : 0.f);
		if (out.reset)
			resetPulse.trigger(1e-3f);
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

	json_t* dataToJson() override {
		json_t* themeJ = json_object();
		for (int i = 0; i < editor::kSlots; i++)
			json_object_set_new(themeJ, editor::kSlotNames[i], json_string(editor::hex(theme.c[i]).c_str()));
		return json_pack("{s:s, s:f, s:i, s:i, s:i, s:i, s:b, s:o, s:b, s:o, s:s, s:b}", "grid", machine.text().c_str(),
						 "bpm", (double)bpm, "voices", voices, "polyMode", polyMode, "clkDivision", clkDivision, "ppc",
						 ppc, "autoPlay", autoPlay, "window", geometryJson(window), "windowOpen", windowOpen, "theme",
						 themeJ, "themeName", themeName.c_str(), "showDots", showDots);
	}

	void dataFromJson(json_t* root) override {
		const char* grid = nullptr;
		double tempo = bpm;
		int play = autoPlay, open = windowOpen, dots = showDots;
		json_t* windowJ = nullptr;
		json_unpack(root, "{s?s, s?F, s?i, s?i, s?i, s?i, s?b, s?o, s?b, s?b}", "grid", &grid, "bpm", &tempo, "voices",
					&voices, "polyMode", &polyMode, "clkDivision", &clkDivision, "ppc", &ppc, "autoPlay", &play,
					"window", &windowJ, "windowOpen", &open, "showDots", &dots);
		// Patch data is a real boundary.
		bpm = clamp((float)tempo, 20.f, 300.f);
		voices = clamp(voices, 1, 16);
		polyMode = clamp(polyMode, 0, (int)filo::VoiceAllocator::MPE);
		if (std::find(kDivisions.begin(), kDivisions.end(), clkDivision) == kDivisions.end())
			clkDivision = 4;
		if (std::find(kDivisions.begin(), kDivisions.end(), ppc) == kDivisions.end())
			ppc = 4;
		autoPlay = play;
		geometryFromJson(windowJ, window);
		windowOpen = open;
		showDots = dots;
		json_t* themeJ = json_object_get(root, "theme");
		editor::Theme t;
		bool ok = json_is_object(themeJ);
		for (int i = 0; ok && i < editor::kSlots; i++) {
			json_t* c = json_object_get(themeJ, editor::kSlotNames[i]);
			ok = json_is_string(c) && editor::fromHex(json_string_value(c), t.c[i]);
		}
		if (ok) {
			theme = t;
			json_t* name = json_object_get(root, "themeName");
			themeName = json_is_string(name) ? json_string_value(name) : "";
		}
		if (grid)
			edit.load(grid);
	}
};

/** The BPM slider of the module's menu: Rack's ui::Slider over a tempo held elsewhere; the slider owns the quantity. */
struct BpmQuantity : Quantity {
	std::function<float()> get;
	std::function<void(float)> set;
	BpmQuantity(std::function<float()> get, std::function<void(float)> set) : get(get), set(set) {}
	void setValue(float v) override { set(clamp(v, 20.f, 300.f)); }
	float getValue() override { return get(); }
	float getMinValue() override { return 20.f; }
	float getMaxValue() override { return 300.f; }
	float getDefaultValue() override { return 120.f; }
	std::string getLabel() override { return "BPM"; }
	int getDisplayPrecision() override { return 0; }
};

struct BpmSlider : ui::Slider {
	explicit BpmSlider(Quantity* q) {
		box.size.x = 200;
		quantity = q;
	}
	~BpmSlider() override { delete quantity; }
};

/** The Grid size menu's "WxH" field: Enter resizes the grid and closes the menu; anything else does nothing. */
struct GridSizeField : ui::TextField {
	std::function<void(int, int)> resize;
	GridSizeField() { box.size.x = 120; }
	void onAction(const ActionEvent& e) override {
		int w, h;
		if (std::sscanf(text.c_str(), "%dx%d", &w, &h) != 2)
			return;
		resize(w, h);
		if (auto* overlay = getAncestorOfType<ui::MenuOverlay>())
			overlay->requestDelete();
		e.consume(this);
	}
};

struct LatticeWidget : ModuleWidget {
	std::unique_ptr<lattice::Machine> previewMachine; // the module browser's grid, in place of the module's
	editor::GridEdit preview;
	std::unique_ptr<editor::Window> gridWindow;                // null: the module browser's preview
	editor::GridView *panelView = nullptr, *bigView = nullptr; // (owned by the widget tree and the window)
	bool fullscreenDown = false;

	LatticeWidget(Lattice* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Lattice.svg")));
		if (!module) {
			previewMachine.reset(new lattice::Machine);
			preview.machine = previewMachine.get();
			preview.refresh();
		}
		auto* view = createWidget<editor::GridView>(mm2px(kEditorMm.pos));
		view->box.size = mm2px(kEditorMm.size);
		view->edit = module ? &module->edit : &preview;
		view->machine = module ? &module->machine : nullptr;
		view->bpm = module ? &module->bpm : nullptr;
		view->theme = module ? &module->theme : nullptr;
		panelView = view;
		addChild(view);
		auto* text = createWidget<PanelText>(Vec());
		text->box.size = box.size;
		addChild(text);
		text->band("LATTICE");
		auto label = [&](float x, float y, const char* name, bool output = false) {
			text->label(Vec(x, y), name, output);
		};
		const char* names[] = {"PLAY", "STOP", "FULL"};
		for (int i = 0; i < Lattice::PARAMS_LEN; i++) {
			Vec pos = mm2px(Vec(kRow1X(i), 100));
			if (i == Lattice::PLAY_PARAM) // lit red while playing
				addParam(createLightParamCentered<VCVLightBezel<RedLight>>(pos, module, i, Lattice::PLAY_LIGHT));
			else
				addParam(createParamCentered<VCVButton>(pos, module, i));
			label(kRow1X(i), 100, names[i]);
		}
		const char* inputs[] = {"PLAY", "STOP", "CLOCK", "RESET"};
		for (int i = 0; i < Lattice::INPUTS_LEN; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(kRow1X(3 + i), 100)), module, i));
			label(kRow1X(3 + i), 100, inputs[i]);
		}
		const char* outputs[] = {"V/OCT", "GATE", "VEL", "TRIG", "CC", "CLK", "RESET"};
		for (int i = 0; i < Lattice::OUTPUTS_LEN; i++) {
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(kRow1X(i), 117)), module, i));
			label(kRow1X(i), 117, outputs[i], true);
		}
		if (module) { // (destroyed before the module, which holds its grid and geometry)
			auto* big = new editor::GridView;
			big->edit = &module->edit;
			big->machine = &module->machine;
			big->bpm = &module->bpm;
			big->theme = &module->theme;
			bigView = big;
			big->fontSize = 16;
			big->fit = true;
			gridWindow.reset(new editor::Window(big, module->window, module->windowOpen));
			big->host = gridWindow.get();
		}
	}

	void onContextDestroy(const ContextDestroyEvent& e) override {
		if (gridWindow) // its context shares the dying one; the next step() opens a fresh one
			gridWindow->destroy();
		ModuleWidget::onContextDestroy(e);
	}

	void step() override {
		ModuleWidget::step();
		hydra::collectWindows();
		if (module) {
			bool dots = static_cast<Lattice*>(module)->showDots;
			panelView->dots = bigView->dots = dots;
		}
		// The FULL button (Rack holds a momentary press for a frame at least).
		bool down = module && module->params[Lattice::FULLSCREEN_PARAM].getValue() > 0.f;
		if (down && !fullscreenDown)
			gridWindow->show();
		fullscreenDown = down;
		// Drawn here, not in draw(): draw() is skipped for off-screen modules.
		if (gridWindow)
			gridWindow->update(APP->window->win, "Lattice — " + std::to_string(module->id));
	}

	// Grid size and Load wipe cells the grid's own undo can't restore: Rack's undo history takes the whole module
	// (Lattice::dataFromJson reloads the grid).
	static void undoable(Lattice* m, const char* name, const std::function<void()>& change) {
		json_t* oldJ = m->toJson();
		change();
		auto* h = new history::ModuleChange;
		h->name = name;
		h->moduleId = m->id;
		h->oldModuleJ = oldJ;
		h->newModuleJ = m->toJson();
		APP->history->push(h);
	}

	void appendContextMenu(Menu* menu) override {
		Lattice* m = static_cast<Lattice*>(module);
		menu->addChild(new MenuSeparator);
		// Only flags here: menu actions run inside GLFW callbacks, step() applies them.
		menu->addChild(createBoolPtrMenuItem("Grid window", "", &m->windowOpen));
		menu->addChild(createBoolPtrMenuItem("Grid window fullscreen", "F11", &m->window.fullscreen));
		menu->addChild(new MenuSeparator);
		menu->addChild(new BpmSlider(new BpmQuantity([=] { return m->bpm; }, [=](float v) { m->bpm = v; })));
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
			"CLK output: pulses per beat", divisions, [=] { return index(m->clkDivision); },
			[=](size_t i) { m->clkDivision = kDivisions[i]; }));
		menu->addChild(createIndexSubmenuItem(
			"CLOCK input: pulses per beat", divisions, [=] { return index(m->ppc); },
			[=](size_t i) { m->ppc = kDivisions[i]; }));
		menu->addChild(createBoolPtrMenuItem("Auto-play", "", &m->autoPlay));

		menu->addChild(new MenuSeparator);
		menu->addChild(createSubmenuItem("Grid size", "", [=](Menu* sub) {
			struct Preset {
				int w, h;
				const char* note;
			};
			for (Preset p :
				 {Preset{38, 12, " (panel)"}, Preset{lattice::kDefaultWidth, lattice::kDefaultHeight, " (default)"},
				  Preset{64, 32, ""}, Preset{96, 48, ""}, Preset{128, 64, ""}, Preset{256, 256, ""}})
				sub->addChild(createCheckMenuItem(
					std::to_string(p.w) + "×" + std::to_string(p.h) + p.note, "",
					[=] { return m->machine.width() == p.w && m->machine.height() == p.h; },
					[=] { undoable(m, "resize Lattice grid", [=] { m->edit.resize(p.w, p.h); }); }));
			sub->addChild(createMenuLabel("Custom size (W x H, Enter)"));
			auto* field = new GridSizeField;
			field->setText(std::to_string(m->machine.width()) + "x" + std::to_string(m->machine.height()));
			field->resize = [=](int w, int h) { undoable(m, "resize Lattice grid", [=] { m->edit.resize(w, h); }); };
			sub->addChild(field);
		}));
		menu->addChild(createBoolPtrMenuItem("Show empty cells", "", &m->showDots));
		menu->addChild(createSubmenuItem("Theme", m->themeName, [=](Menu* sub) {
			bool known = false;
			for (const editor::Preset& p : editor::kPresets) {
				known |= m->theme == p.theme;
				sub->addChild(createCheckMenuItem(
					p.name, "", [=] { return m->theme == p.theme; },
					[=] {
						m->theme = p.theme;
						m->themeName = p.name;
					}));
			}
			if (!known)
				sub->addChild(createMenuLabel(m->themeName + " (loaded)"));
			sub->addChild(new MenuSeparator);
			sub->addChild(createMenuItem("Load theme (.svg)...", "", [=] {
				osdialog_filters* filters = osdialog_filters_parse("Hundred Rabbits theme:svg");
				char* path = osdialog_file(OSDIALOG_OPEN, nullptr, nullptr, filters);
				osdialog_filters_free(filters);
				if (!path)
					return;
				std::string file = path;
				std::free(path);
				try {
					std::vector<uint8_t> data = system::readFile(file);
					editor::Theme t;
					if (editor::parseTheme(std::string(data.begin(), data.end()), t)) {
						m->theme = t;
						m->themeName = system::getStem(file);
					} else {
						osdialog_message(
							OSDIALOG_WARNING, OSDIALOG_OK,
							"Not a theme: it needs the 9 Hundred Rabbits colours (background, f_high..b_inv).");
					}
				} catch (const std::exception& e) {
					WARN("Lattice: can't read %s: %s", file.c_str(), e.what());
				}
			}));
		}));
		menu->addChild(createMenuItem("Load .orca file...", "", [=] {
			osdialog_filters* filters = osdialog_filters_parse("Orca:orca");
			char* path = osdialog_file(OSDIALOG_OPEN, nullptr, nullptr, filters);
			osdialog_filters_free(filters);
			if (!path)
				return;
			std::string file = path;
			std::free(path);
			try {
				std::vector<uint8_t> data = system::readFile(file);
				undoable(m, "load .orca", [&] { m->edit.load(std::string(data.begin(), data.end())); });
			} catch (const std::exception& e) {
				WARN("Lattice: can't read %s: %s", file.c_str(), e.what());
			}
		}));
		menu->addChild(createMenuItem("Save .orca file...", "", [=] {
			osdialog_filters* filters = osdialog_filters_parse("Orca:orca");
			char* path = osdialog_file(OSDIALOG_SAVE, nullptr, "lattice.orca", filters);
			osdialog_filters_free(filters);
			if (!path)
				return;
			std::string file = path;
			std::free(path);
			if (system::getExtension(file) != ".orca")
				file += ".orca";
			std::string text = m->machine.text();
			try {
				system::writeFile(file, std::vector<uint8_t>(text.begin(), text.end()));
			} catch (const std::exception& e) {
				WARN("Lattice: can't write %s: %s", file.c_str(), e.what());
			}
		}));
	}
};

// --- Tart: Ableton Push 2 as a Rack controller (E2.3: transport and clock, tracks, Note mode, colours, scales) ---

static Model *modelTart, *modelSlice;

struct Tart : Module {
	enum InputId { PLAY_INPUT, STOP_INPUT, CLOCK_INPUT, RESET_INPUT, TEMPO_INPUT, INPUTS_LEN };
	enum OutputId { CLK_OUTPUT, RESET_OUTPUT, RUN_OUTPUT, METRO_OUTPUT, OUTPUTS_LEN };
	enum LightId { PUSH_LIGHT, PUSH_RED_LIGHT, LIGHTS_LEN }; // one green-red light: green first

	// The Push's User port, both directions: ports found by name or picked in the menu, saved in the patch.
	midi::InputQueue pushIn;
	midi::Output pushOut;
	tart::Machine machine;
	push2::Event pending[64]; // decoded, not yet handled: the machine's lock was busy
	int nPending = 0;
	push2::Leds want, sent;       // the LEDs the state asks for, the ones the Push has been sent
	int clkDivision = 4, ppc = 4; // CLK output and CLOCK input pulses per beat: set by the UI thread, read by process()
	dsp::SchmittTrigger playIn, stopIn, clockIn, resetIn;
	dsp::PulseGenerator clkPulse, resetPulse, metroPulse;
	float metroV = 0;
	tart::Outputs out{};
	// process()'s messages, reused so the audio thread doesn't allocate: LEDs, MIDI clock and start, the User-mode
	// SysEx, a palette entry and the palette reapply. ponytail: sendMessage() and tryPop() lock inside Rack's MIDI
	// drivers, as in every Rack MIDI module.
	midi::Message in, led, one, userMode, palette, reapply;
	std::atomic<bool> resync{false}; // UI -> audio: the Push (re)opened: set User mode and send every LED again
	std::atomic<bool> owner{false};  // UI -> audio: this Tart holds the Push
	std::atomic<int> pushState{0};   // UI -> audio, for the light: 0 none, 1 MIDI only, 2 MIDI and display
	bool pushAuto = true;            // UI thread only

	Tart() {
		config(0, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configInput(PLAY_INPUT, "Play trigger (from the start)");
		configInput(STOP_INPUT, "Stop trigger");
		configInput(CLOCK_INPUT, "Clock (pulses per beat: menu)");
		configInput(RESET_INPUT, "Reset to the start");
		configInput(TEMPO_INPUT, "Tempo (0 V = 120 BPM, 1 V/oct)");
		configOutput(CLK_OUTPUT, "Clock (pulses per beat: menu)");
		configOutput(RESET_OUTPUT, "Reset (on play and reset)");
		configOutput(RUN_OUTPUT, "Run (high while playing)");
		configOutput(METRO_OUTPUT, "Metronome (10 V downbeat, 5 V other beats)");
		configLight(PUSH_LIGHT, "Push 2 (green: MIDI and display, red: MIDI only)");
		pushOut.channel = -1; // else Rack overwrites the channel byte, which carries the LED animation
		one.setSize(1);
		userMode.bytes.assign(push2::kUserMode, push2::kUserMode + sizeof push2::kUserMode);
		reapply.bytes.assign(push2::kReapplyPalette, push2::kReapplyPalette + sizeof push2::kReapplyPalette);
		palette.bytes.resize(17); // the capacity process() writes into
	}

	/** The 16 palette entries of the track colours, then the reapply: 17 SysEx messages (audio thread). */
	void sendPalette(int64_t frame) {
		for (int k = 0; k < 16; k++) {
			uint32_t c = out.rgb[k];
			push2::paletteEntry((uint8_t)(tart::kPaletteFull + k), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, 0,
								palette.bytes.data());
			palette.setFrame(frame);
			pushOut.sendMessage(palette);
		}
		reapply.setFrame(frame);
		pushOut.sendMessage(reapply);
	}

	void process(const ProcessArgs& args) override {
		bool own = owner.load(std::memory_order_relaxed); // another Tart's Push: the queue stays empty, the clock runs
		while (pushIn.tryPop(&in, args.frame)) {          // Rack filters the menu's channel
			push2::Event e;
			if (own && nPending < 64 && push2::decode(in.bytes.data(), in.bytes.size(), e))
				pending[nPending++] = e;
		}
		bool sync = own && resync.load(std::memory_order_relaxed) && resync.exchange(false);
		tart::Inputs ti;
		ti.sampleRate = args.sampleRate;
		ti.frame = args.frame;
		ti.play = playIn.process(inputs[PLAY_INPUT].getVoltage(), 0.1f, 1.f); // all triggers see every sample
		ti.stop = stopIn.process(inputs[STOP_INPUT].getVoltage(), 0.1f, 1.f);
		ti.reset = resetIn.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f);
		ti.clockConnected = inputs[CLOCK_INPUT].isConnected();
		ti.clockEdge = clockIn.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
		ti.tempoCv = inputs[TEMPO_INPUT].getVoltage();
		ti.ppqnIn = ppc;
		ti.ppqnOut = clkDivision;
		ti.events = pending;
		ti.nEvents = nPending;
		ti.leds = sync || args.frame % 256 == 0;
		Module* right = rightExpander.module; // a Slice on the right gets the tracks' voices (one sample late)
		tart::SliceFrame* slice =
			right && right->model == modelSlice ? (tart::SliceFrame*)right->leftExpander.producerMessage : nullptr;
		machine.process(ti, out, want, slice);
		if (slice)
			right->leftExpander.requestMessageFlip();
		if (out.consumed)
			nPending = 0;
		if (sync) {
			if (!out.ledsDone) {
				resync.store(true); // the UI thread holds the lock: again next sample
			} else {
				userMode.setFrame(args.frame);
				pushOut.sendMessage(userMode);
				sendPalette(args.frame);
				one.bytes[0] = 0xFA; // Start: the Push animates LEDs only while it gets clock and start
				one.setFrame(args.frame);
				pushOut.sendMessage(one);
				for (push2::Led& l : sent.note) // none matches `want`: all of it goes out, 32 messages at a time
					l = {255, 255};
				for (push2::Led& l : sent.cc)
					l = {255, 255};
			}
		}
		if (own && !sync && out.palette) // a track colour changed
			sendPalette(args.frame);
		if (own && (out.midiStart || out.midiClock)) {
			one.bytes[0] = out.midiStart ? 0xFA : 0xF8; // a Start and a Clock on one sample: Start, the Clock next
			one.setFrame(args.frame);
			pushOut.sendMessage(one);
		}
		if (out.reset)
			resetPulse.trigger(1e-3f);
		if (out.clk)
			clkPulse.trigger(1e-3f);
		if (out.metro)
			metroPulse.trigger(1e-3f), metroV = out.metroV;
		outputs[CLK_OUTPUT].setVoltage(clkPulse.process(args.sampleTime) ? 10.f : 0.f);
		outputs[RESET_OUTPUT].setVoltage(resetPulse.process(args.sampleTime) ? 10.f : 0.f);
		outputs[RUN_OUTPUT].setVoltage(out.run ? 10.f : 0.f);
		outputs[METRO_OUTPUT].setVoltage(metroPulse.process(args.sampleTime) ? metroV : 0.f);
		if (args.frame % 256 == 0) {
			uint8_t msg[32][3];
			int n = own ? push2::diff(want, sent, false, msg, 32) : 0;
			for (int i = 0; i < n; i++) {
				std::copy(msg[i], msg[i] + 3, led.bytes.begin());
				led.setFrame(args.frame);
				pushOut.sendMessage(led);
			}
			int state = own ? pushState.load(std::memory_order_relaxed) : 0;
			lights[PUSH_LIGHT].setBrightness(state == 2 ? 1.f : 0.f);
			lights[PUSH_RED_LIGHT].setBrightness(state == 1 ? 1.f : 0.f);
		}
	}

	json_t* dataToJson() override {
		auto st = std::make_unique<tart::State>(); // large: never on the stack
		machine.save(*st);
		const tart::Transport& t = st->s.transport;
		const tart::Scale& sc = st->s.scale;
		const tart::Ui& u = st->s.ui;
		json_t* tracks = json_array();
		for (const tart::Track& tr : st->s.track)
			json_array_append_new(tracks, json_pack("{s:s, s:i, s:i, s:i}", "name", tr.name, "color", tr.color,
													"voices", tr.voices, "octave", tr.octave));
		return json_pack("{s:b, s:o, s:o, s:{s:f, s:i, s:i, s:b}, s:{s:i, s:i, s:i, s:b, s:b}, "
						 "s:{s:i, s:i, s:i, s:i, s:i, s:b, s:i}, s:o, s:i, s:i}",
						 "pushAuto", pushAuto, "pushIn", pushIn.toJson(), "pushOut", pushOut.toJson(), "transport",
						 "bpm", (double)t.bpm, "swing", (int)t.swing, "quant", (int)t.quant, "metronome", t.metronome,
						 "scale", "root", (int)sc.root, "scale", (int)sc.scale, "layout", (int)sc.layout, "inKey",
						 sc.inKey, "fixed", sc.fixed, "ui", "pads", (int)u.pads, "noteLayout", (int)u.noteLayout,
						 "screen", (int)u.screen, "track", (int)u.track, "scene", (int)u.scene, "fixedOn", u.fixedOn,
						 "fixedBars", (int)u.fixedBars, "tracks", tracks, "clkDivision", clkDivision, "ppc", ppc);
	}

	void dataFromJson(json_t* root) override {
		int autoFind = pushAuto;
		json_t *inJ = nullptr, *outJ = nullptr, *transportJ = nullptr, *scaleJ = nullptr, *uiJ = nullptr,
			   *tracksJ = nullptr;
		json_unpack(root, "{s?b, s?o, s?o, s?o, s?o, s?o, s?o, s?i, s?i}", "pushAuto", &autoFind, "pushIn", &inJ,
					"pushOut", &outJ, "transport", &transportJ, "scale", &scaleJ, "ui", &uiJ, "tracks", &tracksJ,
					"clkDivision", &clkDivision, "ppc", &ppc);
		pushAuto = autoFind;
		if (inJ)
			pushIn.fromJson(inJ);
		if (outJ)
			pushOut.fromJson(outJ);
		pushOut.channel = -1;
		if (std::find(kDivisions.begin(), kDivisions.end(), clkDivision) == kDivisions.end())
			clkDivision = 4;
		if (std::find(kDivisions.begin(), kDivisions.end(), ppc) == kDivisions.end())
			ppc = 4;
		auto st = std::make_unique<tart::State>();
		machine.save(*st);
		// Patch data is a real boundary: values are clamped into their field's type here, sanitize() does the rest.
		auto u8 = [](int v) { return (uint8_t)clamp(v, 0, 255); };
		tart::Transport& t = st->s.transport;
		double bpm = t.bpm;
		int swing = t.swing, quant = t.quant, metronome = t.metronome;
		json_unpack(transportJ, "{s?F, s?i, s?i, s?b}", "bpm", &bpm, "swing", &swing, "quant", &quant, "metronome",
					&metronome);
		t.bpm = (float)bpm;
		t.swing = u8(swing);
		t.quant = u8(quant);
		t.metronome = metronome;
		tart::Scale& sc = st->s.scale;
		int rootNote = sc.root, scale = sc.scale, layout = sc.layout, inKey = sc.inKey, fixed = sc.fixed;
		json_unpack(scaleJ, "{s?i, s?i, s?i, s?b, s?b}", "root", &rootNote, "scale", &scale, "layout", &layout, "inKey",
					&inKey, "fixed", &fixed);
		sc.root = u8(rootNote), sc.scale = u8(scale), sc.layout = u8(layout), sc.inKey = inKey, sc.fixed = fixed;
		tart::Ui& ui = st->s.ui;
		int pads = ui.pads, noteLayout = ui.noteLayout, screen = ui.screen, track = ui.track, scene = ui.scene,
			fixedOn = ui.fixedOn, fixedBars = ui.fixedBars;
		json_unpack(uiJ, "{s?i, s?i, s?i, s?i, s?i, s?b, s?i}", "pads", &pads, "noteLayout", &noteLayout, "screen",
					&screen, "track", &track, "scene", &scene, "fixedOn", &fixedOn, "fixedBars", &fixedBars);
		ui.pads = u8(pads), ui.noteLayout = u8(noteLayout), ui.screen = u8(screen), ui.track = u8(track);
		ui.scene = u8(scene), ui.fixedOn = fixedOn, ui.fixedBars = u8(fixedBars);
		for (size_t i = 0; i < tart::kTracks && i < json_array_size(tracksJ); i++) {
			tart::Track& tr = st->s.track[i];
			const char* name = nullptr;
			int color = tr.color, voices = tr.voices, octave = tr.octave;
			json_unpack(json_array_get(tracksJ, i), "{s?s, s?i, s?i, s?i}", "name", &name, "color", &color, "voices",
						&voices, "octave", &octave);
			if (name)
				tart::setName(tr, name);
			tr.color = u8(color), tr.voices = u8(voices), tr.octave = (int8_t)clamp(octave, -128, 127);
		}
		tart::sanitize(*st);
		machine.load(*st);
	}
};

// Matches res/Tart.svg (mm): the dark area under the top band, where the display's mirror goes, and the PUSH light.
static const math::Rect kMirrorMm = math::Rect(3, 12, 95.6, 15.9); // the display's 6:1
static const Vec kPushLightMm = Vec(10.3, 38);

/** The display's mirror: the last frame drawn for the Push, scaled into the box. */
struct TartMirror : widget::Widget {
	push2::Screen* screen;
	explicit TartMirror(push2::Screen* s) : screen(s) {}
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) // like lights: drawn after the room-brightness dimming
			screen->drawMirror(args.vg, 0, 0, box.size.x, box.size.y);
	}
};

/** The Tart that owns the Push (-1: none): the first to step claims it, one at a time. UI thread. */
static int64_t pushOwner = -1;

/** Points `port` at the Push 2's User port unless it is on one already, choosing a driver first if it has none.
`reopen`: close and open it again even when it is on the right one. True if it was (re)opened. */
static bool findUserPort(midi::Port* port, bool reopen) {
	if (!port->getDriver()) {
		std::vector<int> drivers = midi::getDriverIds();
		if (drivers.empty())
			return false;
		port->setDriverId(drivers[0]); // ponytail: the first driver; the menu picks another
	}
	std::vector<int> ids = port->getDeviceIds();
	int current = port->getDeviceId();
	bool listed = std::find(ids.begin(), ids.end(), current) != ids.end();
	if (!reopen && listed && push2::isUserPort(port->getDeviceName(current)))
		return false;
	for (int id : ids) {
		if (!push2::isUserPort(port->getDeviceName(id)))
			continue;
		if (reopen)
			port->setDeviceId(-1);
		port->setDeviceId(id);
		return true;
	}
	return false;
}

/** The Track menu's name field: every edit renames the track. */
struct TrackNameField : ui::TextField {
	std::function<void(const std::string&)> set;
	TrackNameField() { box.size.x = 160; }
	void onChange(const ChangeEvent&) override {
		if (set)
			set(text);
	}
};

/** The Track menu's Color entry: Live's 70 colours as swatches, 14 to a row; a click picks one. */
struct LiveColorGrid : ui::MenuEntry {
	static constexpr float kCell = 16;
	int current = 0;
	std::function<void(int)> set;
	LiveColorGrid() { box.size = Vec(14 * kCell + 8, 5 * kCell + 8); }
	void draw(const DrawArgs& args) override {
		for (int i = 0; i < tart::kColors; i++) {
			const uint8_t* c = tart::kLiveColors[i];
			nvgBeginPath(args.vg);
			nvgRect(args.vg, 4 + i % 14 * kCell, 4 + i / 14 * kCell, kCell - 2, kCell - 2);
			nvgFillColor(args.vg, nvgRGB(c[0], c[1], c[2]));
			nvgFill(args.vg);
			if (i == current) {
				nvgStrokeColor(args.vg, nvgRGB(255, 255, 255));
				nvgStrokeWidth(args.vg, 2);
				nvgStroke(args.vg);
			}
		}
	}
	void onButton(const ButtonEvent& e) override {
		if (e.action != GLFW_PRESS || e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		int col = (int)std::floor((e.pos.x - 4) / kCell), row = (int)std::floor((e.pos.y - 4) / kCell);
		int i = row * 14 + col;
		if (col >= 0 && col < 14 && row >= 0 && row < 5 && i < tart::kColors && set)
			current = i, set(i);
		e.consume(this); // the menu stays open
	}
};

struct TartWidget : ModuleWidget {
	push2::Screen screen;
	push2::Display display;
	std::vector<uint8_t> rgba;
	tart::View view; // the Session as drawn
	bool wasOwner = false;
	uint32_t connectionsSeen = 0, frames = 0;
	int outSeen = -1;
	double lastScan = -1e9, lastPaint = 0;

	TartWidget(Tart* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Tart.svg")));
		auto* mirror = new TartMirror(&screen);
		mirror->box = math::Rect(mm2px(kMirrorMm.pos), mm2px(kMirrorMm.size));
		addChild(mirror);
		addChild(createLightCentered<SmallLight<GreenRedLight>>(mm2px(kPushLightMm), module, Tart::PUSH_LIGHT));
		auto* text = createWidget<PanelText>(Vec());
		text->box.size = box.size;
		text->band("TART");
		text->label(kPushLightMm, "PUSH");
		addChild(text);
		const char* inputs[] = {"PLAY", "STOP", "CLOCK", "RESET", "TEMPO"};
		for (int i = 0; i < Tart::INPUTS_LEN; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(kRow1X(i), 100)), module, i));
			text->label(Vec(kRow1X(i), 100), inputs[i]);
		}
		const char* outputs[] = {"CLK", "RESET", "RUN", "METRO"};
		for (int i = 0; i < Tart::OUTPUTS_LEN; i++) {
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(kRow1X(i), 117)), module, i));
			text->label(Vec(kRow1X(i), 117), outputs[i], true);
		}
	}

	~TartWidget() override {
		if (module && pushOwner == module->id)
			pushOwner = -1; // another Tart claims it on its next step
		display.stop();
		if (APP->window) // null at app exit, where onContextDestroy has already freed everything
			screen.destroy();
	}

	void onContextDestroy(const ContextDestroyEvent& e) override {
		screen.destroy();
		ModuleWidget::onContextDestroy(e);
	}

	void step() override {
		ModuleWidget::step();
		if (!module)
			return;
		Tart* m = static_cast<Tart*>(module);
		if (pushOwner < 0 || !APP->engine->getModule(pushOwner))
			pushOwner = m->id;
		bool own = pushOwner == m->id;
		m->owner.store(own, std::memory_order_relaxed);
		double now = system::getTime();
		if (!own) {
			wasOwner = false; // a duplicated Tart carries the owner's ports
			for (midi::Port* port : {(midi::Port*)&m->pushIn, (midi::Port*)&m->pushOut})
				if (port->getDeviceId() >= 0)
					port->setDeviceId(-1);
			display.stop();
			m->pushState.store(0, std::memory_order_relaxed);
		} else {
			if (!wasOwner)
				m->resync.store(true, std::memory_order_relaxed);
			wasOwner = true;
			display.start();
			uint32_t connections = display.connections();
			bool replug = connections != connectionsSeen;
			if (m->pushAuto && (replug || now - lastScan >= 2)) {
				lastScan = now;
				findUserPort(&m->pushIn, replug);
				if (findUserPort(&m->pushOut, replug))
					m->resync.store(true, std::memory_order_relaxed);
			}
			connectionsSeen = connections;
			m->pushOut.channel = -1; // the menu offers a channel, which would overwrite the animation channels
			if (m->pushOut.getDeviceId() != outSeen) { // found, picked in the menu or unplugged
				outSeen = m->pushOut.getDeviceId();
				m->resync.store(true, std::memory_order_relaxed);
			}
			bool midi = m->pushIn.getDeviceId() >= 0 && m->pushOut.getDeviceId() >= 0;
			m->pushState.store(midi ? (display.status() == push2::Display::Connected ? 2 : 1) : 0,
							   std::memory_order_relaxed);
		}
		// Rendered here, not in draw(): draw() is skipped for off-screen modules, the Push's display must keep going.
		// At most ~60 fps: 1/75 s, not 1/60, so a 60 Hz Rack's frame jitter doesn't skip every other frame.
		if (now - lastPaint < 1.0 / 75)
			return;
		lastPaint = now;
		NVGcontext* vg = APP->window->vg;
		if (!screen.begin(vg))
			return;
		paint(vg, m, own);
		bool send = own && display.status() == push2::Display::Connected;
		if (send)
			rgba.resize((size_t)push2::kWidth * push2::kHeight * 4);
		screen.end(vg, send ? rgba.data() : nullptr);
		if (send)
			display.submit(rgba.data());
	}

	/** The Home screen, with the status line when the display or a port isn't connected, or a message instead. */
	void paint(NVGcontext* vg, Tart* m, bool own) {
		std::shared_ptr<window::Font> mono =
			APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/IBMPlexMono-Regular.ttf"));
		std::shared_ptr<window::Font> sans =
			APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/IBMPlexSansCondensed-SemiBold.ttf"));
		if (!mono || !sans)
			return;
		auto portName = [](midi::Port* p) {
			return p->getDeviceId() >= 0 ? p->getDeviceName(p->getDeviceId()) : std::string("-");
		};
		bool noPush =
			m->pushIn.getDeviceId() < 0 && m->pushOut.getDeviceId() < 0 && display.status() <= push2::Display::NoDevice;
		const char* message = !own ? "Push in use by another Tart" : noPush ? "No Push 2" : nullptr;
		static const char* const names[] = {"display off",  "no display",        "no access",        "no driver",
											"display busy", "display USB error", "display connected"};
		bool connected = display.status() == push2::Display::Connected && m->pushIn.getDeviceId() >= 0 &&
						 m->pushOut.getDeviceId() >= 0;
		std::string status = string::f("%s | in %s | out %s", names[display.status()], portName(&m->pushIn).c_str(),
									   portName(&m->pushOut).c_str());
		m->machine.view(view);
		tart::paint(vg, view, sans->handle, mono->handle, connected ? nullptr : status.c_str(), message);
	}

	void appendContextMenu(Menu* menu) override {
		Tart* m = static_cast<Tart*>(module);
		menu->addChild(new MenuSeparator);
		menu->addChild(new BpmSlider(new BpmQuantity(
			[=] {
				tart::View v;
				m->machine.view(v);
				return v.s.transport.bpm;
			},
			[=](float bpm) { m->machine.edit([=](tart::State& st) { st.s.transport.bpm = bpm; }); })));
		std::vector<std::string> divisions;
		for (int d : kDivisions)
			divisions.push_back(std::to_string(d));
		auto index = [](int value) {
			return (size_t)(std::find(kDivisions.begin(), kDivisions.end(), value) - kDivisions.begin());
		};
		menu->addChild(createIndexSubmenuItem(
			"CLK output: pulses per beat", divisions, [=] { return index(m->clkDivision); },
			[=](size_t i) { m->clkDivision = kDivisions[i]; }));
		menu->addChild(createIndexSubmenuItem(
			"CLOCK input: pulses per beat", divisions, [=] { return index(m->ppc); },
			[=](size_t i) { m->ppc = kDivisions[i]; }));
		menu->addChild(createSubmenuItem("Track", "", [=](Menu* sub) {
			tart::View v;
			m->machine.view(v);
			for (int t = 0; t < tart::kTracks; t++)
				sub->addChild(createSubmenuItem("Track " + std::to_string(t + 1), v.s.track[t].name, [=](Menu* sub2) {
					tart::View now;
					m->machine.view(now);
					sub2->addChild(createMenuLabel("Name"));
					auto* field = new TrackNameField;
					field->setText(now.s.track[t].name);
					field->set = [=](const std::string& name) {
						m->machine.edit([&](tart::State& st) { tart::setName(st.s.track[t], name.c_str()); });
					};
					sub2->addChild(field);
					sub2->addChild(createMenuLabel("Color"));
					auto* grid = new LiveColorGrid;
					grid->current = now.s.track[t].color;
					grid->set = [=](int color) {
						m->machine.edit([=](tart::State& st) { st.s.track[t].color = (uint8_t)color; });
					};
					sub2->addChild(grid);
					std::vector<std::string> counts;
					for (int n = 1; n <= 16; n++)
						counts.push_back(std::to_string(n));
					sub2->addChild(createIndexSubmenuItem(
						"Voices", counts,
						[=] {
							tart::View now;
							m->machine.view(now);
							return (size_t)now.s.track[t].voices - 1;
						},
						[=](size_t i) {
							m->machine.edit([=](tart::State& st) { st.s.track[t].voices = (uint8_t)i + 1; });
						}));
				}));
		}));
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Find Push automatically", "", &m->pushAuto));
		menu->addChild(createMenuLabel("Push MIDI input (User Port)"));
		app::appendMidiMenu(menu, &m->pushIn);
		menu->addChild(createMenuLabel("Push MIDI output (User Port)"));
		app::appendMidiMenu(menu, &m->pushOut);
	}
};

// --- Slice: one Tart track's voices as jacks, on the right of a Tart or another Slice ---

struct Slice : Module {
	enum OutputId { VOCT_OUTPUT, GATE_OUTPUT, VEL_OUTPUT, CC_OUTPUT, LEVEL_OUTPUT, PAN_OUTPUT, OUTPUTS_LEN };

	tart::SliceFrame msg[2] = {}; // the left neighbour writes one and this module reads the other (Rack flips them)
	int track = -1;               // -1: the chain position; set by the UI thread, read by process()
	uint32_t lastFrame = 0;
	std::atomic<int> shown{-1}; // for the panel: the track number, -1 not chained, -2 chained without a track
	std::atomic<uint32_t> shownRgb{0};

	Slice() {
		config(0, 0, OUTPUTS_LEN, 0);
		configOutput(VOCT_OUTPUT, "V/OCT (one channel per voice)");
		configOutput(GATE_OUTPUT, "Gate (one channel per voice)");
		configOutput(VEL_OUTPUT, "Velocity (one channel per voice)");
		configOutput(CC_OUTPUT, "CC (8 channels, 0 V for now)");
		configOutput(LEVEL_OUTPUT, "Level (10 V for now)");
		configOutput(PAN_OUTPUT, "Pan (0 V for now)");
		leftExpander.producerMessage = &msg[0];
		leftExpander.consumerMessage = &msg[1];
	}

	void process(const ProcessArgs&) override {
		Module* left = leftExpander.module;
		bool chained = left && (left->model == modelTart || left->model == modelSlice);
		const tart::SliceFrame* f = chained ? (const tart::SliceFrame*)leftExpander.consumerMessage : nullptr;
		if (f && f->frame == lastFrame)
			f = nullptr; // nothing new: the left neighbour is bypassed, removed or the engine is paused
		else if (f)
			lastFrame = f->frame;
		int tr = !f ? -1 : track >= 0 ? track : f->hop < tart::kTracks ? f->hop : -1;
		const tart::SliceTrack* t = tr >= 0 ? &f->track[tr] : nullptr;
		if (!chained)
			shown.store(-1, std::memory_order_relaxed);
		else if (f)
			shown.store(t ? tr : -2, std::memory_order_relaxed);
		if (t)
			shownRgb.store(t->rgb, std::memory_order_relaxed);
		int n = t ? t->channels : 1;
		for (int id : {VOCT_OUTPUT, GATE_OUTPUT, VEL_OUTPUT})
			outputs[id].setChannels(n);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
		outputs[CC_OUTPUT].setChannels(tart::kCcs); // as in Lattice: GCC 12+ false positive for a constant
#pragma GCC diagnostic pop
		for (int c = 0; c < n; c++) {
			outputs[VOCT_OUTPUT].setVoltage(t ? t->voct[c] : 0.f, c);
			outputs[GATE_OUTPUT].setVoltage(t ? t->gate[c] : 0.f, c);
			outputs[VEL_OUTPUT].setVoltage(t ? t->vel[c] : 0.f, c);
		}
		for (int c = 0; c < tart::kCcs; c++)
			outputs[CC_OUTPUT].setVoltage(t ? t->cc[c] : 0.f, c);
		outputs[LEVEL_OUTPUT].setVoltage(t ? t->level : 0.f);
		outputs[PAN_OUTPUT].setVoltage(t ? t->pan : 0.f);
		Module* right = rightExpander.module; // the chain goes on, one position further
		if (f && right && right->model == modelSlice) {
			tart::SliceFrame* next = (tart::SliceFrame*)right->leftExpander.producerMessage;
			*next = *f;
			next->hop = (uint8_t)std::min<int>(f->hop + 1, 255);
			right->leftExpander.requestMessageFlip();
		}
	}

	json_t* dataToJson() override { return json_pack("{s:i}", "track", track); }

	void dataFromJson(json_t* root) override {
		int v = track;
		json_unpack(root, "{s?i}", "track", &v);
		track = clamp(v, -1, tart::kTracks - 1);
	}
};

// Matches res/Slice.svg (mm): the number window and the hint under it; the jacks, two to a row.
static const math::Rect kSliceStatusMm = math::Rect(3, 12, 24.5, 62);
static const float kSliceWindowMm = 14;
static Vec sliceJackMm(int i) {
	return Vec(i % 2 ? 21.9f : 8.6f, 85.f + 16.f * (i / 2));
}

/** The track number in its colour, or what to do when there is none. */
struct SliceStatus : widget::Widget {
	Slice* slice = nullptr;
	void draw(const DrawArgs& args) override {
		std::shared_ptr<window::Font> font =
			APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/IBMPlexSansCondensed-SemiBold.ttf"));
		if (!slice || !font)
			return;
		int shown = slice->shown.load(std::memory_order_relaxed);
		uint32_t rgb = slice->shownRgb.load(std::memory_order_relaxed);
		nvgFontFaceId(args.vg, font->handle);
		nvgTextLetterSpacing(args.vg, 0);
		nvgFontSize(args.vg, 30);
		nvgFillColor(args.vg,
					 shown >= 0 ? nvgRGB(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255) : nvgRGB(0x90, 0x92, 0x8c));
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgText(args.vg, box.size.x / 2, mm2px(kSliceWindowMm) / 2,
				shown >= 0 ? std::to_string(shown + 1).c_str() : "-", nullptr);
		if (shown == -1) {
			nvgFontSize(args.vg, 11);
			nvgFillColor(args.vg, nvgRGB(0, 0, 0));
			nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgTextBox(args.vg, 0, mm2px(kSliceWindowMm + 5), box.size.x, "Place right of a Tart or Slice", nullptr);
		}
	}
};

struct SliceWidget : ModuleWidget {
	SliceWidget(Slice* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Slice.svg")));
		auto* status = new SliceStatus;
		status->slice = module;
		status->box = math::Rect(mm2px(kSliceStatusMm.pos), mm2px(kSliceStatusMm.size));
		addChild(status);
		auto* text = createWidget<PanelText>(Vec());
		text->box.size = box.size;
		text->band("SLICE");
		const char* outputs[] = {"V/OCT", "GATE", "VEL", "CC", "LEVEL", "PAN"};
		for (int i = 0; i < Slice::OUTPUTS_LEN; i++) {
			addOutput(createOutputCentered<PJ301MPort>(mm2px(sliceJackMm(i)), module, i));
			text->label(sliceJackMm(i), outputs[i], true);
		}
		addChild(text);
	}

	void appendContextMenu(Menu* menu) override {
		Slice* m = static_cast<Slice*>(module);
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem(
			"Track", {"Chain position", "1", "2", "3", "4", "5", "6", "7", "8"}, [=] { return (size_t)(m->track + 1); },
			[=](size_t i) { m->track = (int)i - 1; }));
	}
};

void init(Plugin* p) {
	pluginInstance = p;
	loadSettings();
	p->addModel(modelFilo = createModel<Filo, FiloWidget>("Filo"));
	p->addModel(createModel<Icing, IcingWidget>("Icing"));
	p->addModel(createModel<Lattice, LatticeWidget>("Lattice"));
	p->addModel(modelTart = createModel<Tart, TartWidget>("Tart"));
	p->addModel(modelSlice = createModel<Slice, SliceWidget>("Slice"));
}
