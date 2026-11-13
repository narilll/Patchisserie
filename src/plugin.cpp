#include <osdialog.h>
#include <rack.hpp>

#include "editor/grid.hpp"
#include "editor/view.hpp"
#include "engine.hpp"
#include "hydra.hpp"
#include "lattice.hpp"

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

/** The BPM slider of the module's menu: Rack's ui::Slider over `module->bpm`; the slider owns the quantity. */
struct BpmQuantity : Quantity {
	float* bpm;
	explicit BpmQuantity(float* bpm) : bpm(bpm) {}
	void setValue(float v) override { *bpm = clamp(v, 20.f, 300.f); }
	float getValue() override { return *bpm; }
	float getMinValue() override { return 20.f; }
	float getMaxValue() override { return 300.f; }
	float getDefaultValue() override { return 120.f; }
	std::string getLabel() override { return "BPM"; }
	int getDisplayPrecision() override { return 0; }
};

struct BpmSlider : ui::Slider {
	explicit BpmSlider(float* bpm) {
		box.size.x = 200;
		quantity = new BpmQuantity(bpm);
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
		menu->addChild(new BpmSlider(&m->bpm));
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

void init(Plugin* p) {
	pluginInstance = p;
	loadSettings();
	p->addModel(modelFilo = createModel<Filo, FiloWidget>("Filo"));
	p->addModel(createModel<Icing, IcingWidget>("Icing"));
	p->addModel(createModel<Lattice, LatticeWidget>("Lattice"));
}
