// make glcheck: Icing's GL path on this machine's driver, which `make test` never reaches (CI runners may have no
// GL). Rack's context (hydra.cpp's window hints; libRack's GLFW, GLEW and NanoVG) in a hidden window, and the plugin's
// own objects: the real Engine evaluates each sketch, hydra::Renderer compiles and renders its frames at 60 frames/s
// of Icing time, present() draws the displayed image into an FBO that is read back. Prints every shader's compile log
// and what each image looks like; exits 1 when a shader fails or an image isn't what hydra draws. Then Filo's editor
// window (editor::Window): a second context sharing the first, its own NanoVG context and fonts, a frame read back.
#include <rack.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include <stb_image_write.h>

#include "editor/view.hpp"
#include "engine.hpp"
#include "lattice.hpp"

rack::plugin::Plugin* pluginInstance = nullptr; // view.cpp's (plugin.cpp's in the plugin): no docs here

namespace {

double now() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** The displayed image as glReadPixels gives it: RGBA, rows from the bottom. */
struct Image {
	int w = 0, h = 0;
	std::vector<uint8_t> px;
	const uint8_t* at(int x, int y) const { return &px[4 * ((size_t)y * w + x)]; }
	/** Share of the pixels in [x0, x1) x [y0, y1) with an RGB channel above 16 (of 255). */
	double lit(int x0, int y0, int x1, int y1) const {
		size_t n = 0;
		for (int y = y0; y < y1; y++)
			for (int x = x0; x < x1; x++)
				n += at(x, y)[0] > 16 || at(x, y)[1] > 16 || at(x, y)[2] > 16;
		return (double)n / ((x1 - x0) * (y1 - y0));
	}
	double lit() const { return lit(0, 0, w, h); }
	/** Mean |RGB difference| (of 255) between neighbouring pixels along x (dx = 1) or y (dy = 1). */
	double ripple(int dx, int dy) const {
		double sum = 0;
		for (int y = 0; y + dy < h; y++)
			for (int x = 0; x + dx < w; x++)
				for (int c = 0; c < 3; c++)
					sum += std::abs(at(x, y)[c] - at(x + dx, y + dy)[c]);
		return sum / (3.0 * (w - dx) * (h - dy));
	}
};

struct Case {
	const char* name;
	std::string code;                               // after initHydra() and hush()
	std::function<std::string(const Image&)> check; // "" when it looks like hydra's, else what is wrong
	int frames = 30;
	const char* error = nullptr; // the Renderer's error line this case must give (part of it)
};

std::string atLeast(const Image& im, double lit) {
	return im.lit() >= lit ? "" : "less than " + std::to_string((int)(lit * 100)) + " % lit";
}

const char kLibColorUrl[] = "https://metagrowing.org/extra-shaders-for-hydra/lib-color.js";
// test/fixtures/quadrants.png: 2x2, red top left, green top right, blue bottom left, white bottom right. Not an
// image: lib-color.js. Missing: the download fails.
const char kQuadrantsUrl[] = "https://example.com/quadrants.png";
const char kNotAnImageUrl[] = "https://example.com/lib-color.png";
const char kMissingUrl[] = "https://example.com/missing.png";

using Rgba = std::array<int, 4>;
const Rgba kRed{255, 0, 0, 255}, kGreen{0, 255, 0, 255}, kBlue{0, 0, 255, 255}, kWhite{255, 255, 255, 255};
struct Probe {
	double x, y; // hydra's st: x to the right, y down from the canvas top
	Rgba rgba;
};
/** The pixels hydra-synth's own WebGL renderer gives for the same sketch (Chrome, read back with regl.read()). */
std::function<std::string(const Image&)> pixels(std::vector<Probe> want) {
	return [=](const Image& im) {
		for (const Probe& p : want) {
			const uint8_t* got = im.at((int)(p.x * im.w), im.h - 1 - (int)(p.y * im.h));
			if (!std::equal(got, got + 4, p.rgba.begin())) {
				char s[128];
				std::snprintf(s, sizeof s, "at %g, %g: %d %d %d %d, hydra: %d %d %d %d", p.x, p.y, got[0], got[1],
							  got[2], got[3], p.rgba[0], p.rgba[1], p.rgba[2], p.rgba[3]);
				return std::string(s);
			}
		}
		return std::string();
	};
}
/** sN.initImage(url) as hydra.ojack.xyz writes it, the image in the disk cache already, as after a first run: the
frames don't wait for a download (test_engine.cpp's engine_hydra_image_source does). */
std::string initImage(int n, const char* url) {
	return std::string("await fetch('") + url + "')\ns" + std::to_string(n) + ".initImage(\"" + url + "\")\n";
}
std::string transparent(const Image& im) {
	for (uint8_t c : im.px)
		if (c)
			return "not transparent black everywhere";
	return "";
}

std::vector<Case> cases() {
	auto lit = [](double share) { return [=](const Image& im) { return atLeast(im, share); }; };
	auto stripes = [](const Image& im) { // osc(): varies along x only
		return im.ripple(1, 0) > 0.1 && im.ripple(0, 1) < 0.05 * im.ripple(1, 0) ? atLeast(im, 0.5)
																				 : "no vertical stripes";
	};
	return {
		// A user's sketch, first (Hydra's time starts at 0): feedback through three samplers of o0, smooth() NaN
		// for its first 2 s (black). At hydra-synth's bpm 30 bright bursts build up every 8 s: its own WebGL renderer
		// (Chrome, same GPU) gives 38 % lit at 10 s. At bpm 120 (Filo's default tempo, which Icing once forced on bpm)
		// they never do: 5 %.
		{"voronoi feedback (user report)",
		 "voronoi(350,0.15)\n"
		 "      .modulateScale(osc(8).rotate(Math.sin(time)),.5)\n"
		 "      .thresh(.8)\n"
		 "    .modulateRotate(osc(7),.4)\n"
		 "    .thresh(.7)\n"
		 "      .diff(src(o0).scale(1.8))\n"
		 "    .modulateScale(osc(2).modulateRotate(o0,.74))\n"
		 "    .diff(src(o0).rotate([-.012,.01,-.002,0]).scrollY(0,[-1/199800,0].fast(0.7)))\n"
		 "    .brightness([-.02,-.17].smooth().fast(.5))\n"
		 "    .out()",
		 lit(0.2), 600},
		// The ten Hydra docs examples of test/hydra.test.mjs.
		{"docs: osc rotate", "osc(60, 0.1, 1.2).rotate(0, 0.1).out()", lit(0.5)},
		{"docs: noise color", "noise(10, 0.1).color(1, 0.5, 0.2).out()", lit(0.3)},
		{"docs: voronoi modulate", "voronoi(5, 0.3, 0.3).modulate(osc(8), 0.2).out()", lit(0.5)},
		{"docs: osc blend shape", "osc(20, 0.1, 0.8).blend(shape(4, 0.3), 0.5).out()", lit(0.5)},
		{"docs: feedback layer mask",
		 "src(o0).modulateHue(src(o0).scale(1.01), 1).layer(osc(10, 0.1, 2).mask(shape(3, 0.2))).out(o0)", lit(0.05)},
		{"docs: kaleid", "osc(25, 0.1, 0.5).kaleid(50).out()", lit(0.3)},
		{"docs: shape array repeat", "shape([3, 4, 5].fast(2), 0.4).repeat(3, 3).out()", lit(0.1)},
		{"docs: osc a.fft", "osc(10, 0.1, () => a.fft[0] * 4).out()", stripes},
		{"docs: render(o2)", "gradient(1).colorama(0.4).out(o1)\nsolid(1, 0, 0).diff(o1).out(o2)\nrender(o2)",
		 lit(0.5)},
		{"docs: render() all four", "osc().out(o0)\nnoise().out(o1)\nvoronoi().out(o2)\nshape().out(o3)\nrender()",
		 [](const Image& im) { // hydra's renderAll: o0 top left, o1 bottom left, o2 top right, o3 bottom right
			 int w = im.w / 2, h = im.h / 2;
			 double o0 = im.lit(0, h, w, im.h), o1 = im.lit(0, 0, w, h), o2 = im.lit(w, h, im.w, im.h),
					o3 = im.lit(w, 0, im.w, h);
			 if (o0 < 0.5 || o1 < 0.2 || o2 < 0.5 || o3 < 0.03 || o3 > 0.5)
				 return "quadrants lit " + std::to_string(o0) + " " + std::to_string(o1) + " " + std::to_string(o2) +
						" " + std::to_string(o3);
			 return std::string();
		 }},
		// Orientation: hydra's canvas shows st.y = 0 at the top (its renderFbo pass), glReadPixels' rows are bottom-up.
		{"render(o0) orientation", "gradient().out()",
		 [](const Image& im) {
			 const uint8_t *bottomLeft = im.at(2, 2), *topRight = im.at(im.w - 3, im.h - 3);
			 bool ok = bottomLeft[0] < 8 && bottomLeft[1] > 247 && topRight[0] > 247 && topRight[1] < 8;
			 return ok ? "" : "gradient() not as on hydra's canvas (red right, green down)";
		 }},
		// loadScript() of an extension (served from test/fixtures/lib-color.js), a setFunction() transform.
		{"loadScript colreflect",
		 std::string("await loadScript(\"") + kLibColorUrl +
			 "\")\nosc(3, 0.5, 1).rotate(() => -0.17 * time).colreflect(osc(12, 0.23, 1).rotate(() => 0.1 * time), "
			 "0.3).out(o0)\nrender(o0)",
		 lit(0.5)},
		// initImage(): upright as on hydra's canvas, stretched over it, nearest; its texture where src(s0) reads.
		{"initImage src(s0)", initImage(0, kQuadrantsUrl) + "src(s0).out()",
		 pixels({{0.01, 0.01, kRed},
				 {0.99, 0.01, kGreen},
				 {0.01, 0.99, kBlue},
				 {0.99, 0.99, kWhite},
				 {0.3, 0.3, kRed},
				 {0.45, 0.45, kRed},
				 {0.55, 0.55, kWhite},
				 {0.7, 0.7, kWhite},
				 {0.49, 0.25, kRed},
				 {0.51, 0.25, kGreen}})},
		// modulate: st + 0.05 (sin(5 st.x) + 1) in x and y, wrapped by src()'s fract() (red in the blue quadrant).
		{"initImage modulate", initImage(0, kQuadrantsUrl) + "src(s0).modulate(osc(5, 0), .1).out()",
		 pixels({{0.45, 0.25, kGreen},
				 {0.3, 0.95, kRed},
				 {0.1, 0.75, kBlue},
				 {0.7, 0.7, kWhite},
				 {0.38, 0.25, kRed},
				 {0.43, 0.25, kGreen}})},
		// Before (or without) an image a source is hydra-synth's 1x1 texture of zeros; after clear() too.
		{"initImage missing", std::string("s1.initImage(\"") + kMissingUrl + "\")\nsrc(s1).out()", transparent},
		{"initImage clear()", // after the image: initImage()'s fetch settles first
		 initImage(0, kQuadrantsUrl) + "src(s0).out()\nawait fetch('" + kQuadrantsUrl + "')\ns0.clear()", transparent},
		{"initImage not an image", initImage(2, kNotAnImageUrl) + "src(s2).out()", transparent, 30,
		 "s2.initImage(): https://example.com/lib-color.png: not an image"},
	};
}

/** Rack's audio thread, unpaced: the engine's timeline goes on. */
void runAudio(filo::Engine& e, int frames) {
	filo::Sequencer::Inputs in;
	filo::Sequencer::Outputs out;
	float l, r;
	for (int i = 0; i < frames; i++)
		e.process(in, out, l, r);
}

/** present() into an FBO of the frame's size, read back. */
Image readBack(const hydra::Renderer& renderer, int w, int h) {
	GLuint tex = 0, fbo = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	glViewport(0, 0, w, h);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	renderer.present(w, h);
	Image im;
	im.w = w, im.h = h;
	im.px.resize(4 * (size_t)w * h);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, im.px.data());
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	return im;
}

/** The driver's log for a fragment shader as the Renderer compiles it ("" when it has nothing to say). */
std::string compileLog(const std::string& es, bool& ok) {
	std::string src = hydra::glslEsToDesktop(es);
	const char* p = src.c_str();
	GLuint s = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(s, 1, &p, NULL);
	glCompileShader(s);
	GLint status = 0, n = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &status);
	glGetShaderiv(s, GL_INFO_LOG_LENGTH, &n);
	std::string log(std::max(n, 1), '\0');
	glGetShaderInfoLog(s, n, NULL, &log[0]);
	glDeleteShader(s);
	ok = status;
	return log.c_str();
}

/** Filo's editor window, as FiloWidget opens it: a frame of a Doc drawn by its own NanoVG context with its own fonts,
read back before the swap, in the editor theme `theme`. Text in the first line, the theme's background where there is
none. "" when it is so. */
std::string editorWindow(GLFWwindow* rack, const std::string& theme) {
	editor::Doc doc("$: s(\"bd sd\").bank(\"tr909\")\n");
	hydra::WindowGeometry g;
	bool open = true;
	editor::Window w(doc, nullptr, g, open);
	const std::vector<std::string>& names = editor::themeNames();
	if (std::find(names.begin(), names.end(), theme) == names.end())
		return "no editor theme " + theme + " (res/editor-themes.json)";
	const editor::EditorTheme& colors = editor::themeNamed(theme);
	static_cast<editor::View*>(w.view)->theme = &colors;
	std::string png = "build/glcheck-out/editor" + (theme == names[0] ? "" : "-" + theme) + ".png";
	w.update(rack, "glcheck: editor window"); // creates, shows and draws it
	std::string why = !w.window.win            ? "no window"
					  : !w.vg                  ? "no NanoVG context"
					  : w.mono < 0 || w.ui < 0 ? "fonts not loaded from " + rack::asset::system() +
													 " (set RACK_SYSTEM_DIR to Rack's res/ folder's)"
											   : "";
	if (why.empty()) {
		glfwMakeContextCurrent(w.window.win);
		int fw, fh;
		glfwGetFramebufferSize(w.window.win, &fw, &fh);
		glViewport(0, 0, fw, fh);
		w.draw(fw, fh);
		Image im;
		im.w = fw, im.h = fh;
		im.px.resize(4 * (size_t)fw * fh);
		glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, im.px.data());
		GLenum error = glGetError();
		glfwMakeContextCurrent(rack);
		stbi_write_png(png.c_str(), im.w, im.h, 4, im.at(0, im.h - 1), -4 * im.w);
		// Share of the pixels in [x0, x1) x [y0, y1) (UI px from the top left) that aren't the background.
		float px = fw / w.root->box.size.x;
		int bg[3] = {(int)std::lround(colors.background.r * 255), (int)std::lround(colors.background.g * 255),
					 (int)std::lround(colors.background.b * 255)};
		auto drawn = [&](float x0, float y0, float x1, float y1) {
			size_t n = 0, all = 0;
			for (int y = (int)(im.h - y1 * px); y < (int)(im.h - y0 * px); y++)
				for (int x = (int)(x0 * px); x < (int)(x1 * px); x++, all++) {
					const uint8_t* p = im.at(x, y);
					n += std::abs(p[0] - bg[0]) + std::abs(p[1] - bg[1]) + std::abs(p[2] - bg[2]) > 48;
				}
			return (double)n / all;
		};
		double text = drawn(0, 4, 300, 25), none = drawn(320, 100, 640, 300); // line 1; right of line 5 and on
		const uint8_t* p = im.at((int)(500 * px), (int)(im.h - 200 * px));    // empty: right of line 5 and on
		bool exact = std::abs(p[0] - bg[0]) <= 2 && std::abs(p[1] - bg[1]) <= 2 && std::abs(p[2] - bg[2]) <= 2;
		std::printf("  %dx%d px (%s, %s): line 1 %.1f %% drawn, empty part %.1f %%\n", fw, fh, theme.c_str(),
					png.c_str(), 100 * text, 100 * none);
		why = error                ? "GL error " + std::to_string(error)
			  : text < 0.05        ? "no text"
			  : none > 0 || !exact ? "not the background"
								   : "";
	}
	w.destroy();
	hydra::collectWindows();
	return why;
}

/** Lattice's grid window: a GridView over the default grid (stopped) in an editor::Window, a frame read back. Glyphs in
the first row, the background in an empty part of the grid. "" when it is so. */
std::string gridWindow(GLFWwindow* rack) {
	lattice::Machine machine;
	editor::GridEdit edit;
	edit.machine = &machine;
	edit.refresh();
	auto* view = new editor::GridView; // owned by the window
	view->edit = &edit;
	view->machine = &machine;
	view->fontSize = 16;
	view->fit = true;
	hydra::WindowGeometry g;
	bool open = true;
	editor::Window w(view, g, open);
	view->host = &w;
	w.update(rack, "glcheck: grid window");
	std::string why = !w.window.win            ? "no window"
					  : !w.vg                  ? "no NanoVG context"
					  : w.mono < 0 || w.ui < 0 ? "fonts not loaded from " + rack::asset::system() +
													 " (set RACK_SYSTEM_DIR to Rack's res/ folder's)"
											   : "";
	if (why.empty()) {
		glfwMakeContextCurrent(w.window.win);
		int fw, fh;
		glfwGetFramebufferSize(w.window.win, &fw, &fh);
		glViewport(0, 0, fw, fh);
		w.draw(fw, fh);
		Image im;
		im.w = fw, im.h = fh;
		im.px.resize(4 * (size_t)fw * fh);
		glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, im.px.data());
		GLenum error = glGetError();
		glfwMakeContextCurrent(rack);
		stbi_write_png("build/glcheck-out/lattice.png", im.w, im.h, 4, im.at(0, im.h - 1), -4 * im.w);
		// Share of the pixels in [x0, x1) x [y0, y1) (UI px from the top left) that aren't the background.
		float px = fw / w.root->box.size.x;
		auto drawn = [&](float x0, float y0, float x1, float y1) {
			size_t n = 0, all = 0;
			for (int y = (int)(im.h - y1 * px); y < (int)(im.h - y0 * px); y++)
				for (int x = (int)(x0 * px); x < (int)(x1 * px); x++, all++) {
					const uint8_t* p = im.at(x, y);
					n += std::abs(p[0] - 0x1b) + std::abs(p[1] - 0x1c) + std::abs(p[2] - 0x1e) > 48;
				}
			return (double)n / all;
		};
		// The fitted cells: the first 12 columns of row 1 (the arpeggio's "...2C4"; empty dots are not drawn), the "+"
		// marker at column 48, row 24 (so all 57x25 are in), and right of the 57 columns.
		rack::math::Vec cell = view->cellSize();
		constexpr float kPad = 4; // editor/view.cpp's
		double text = drawn(0, kPad, kPad + 12 * cell.x, kPad + cell.y),
			   marker = drawn(kPad + 48 * cell.x, kPad + 24 * cell.y, kPad + 49 * cell.x, kPad + 25 * cell.y),
			   none = drawn(kPad + lattice::kDefaultWidth * cell.x + 2, kPad, w.root->box.size.x,
							kPad + lattice::kDefaultHeight * cell.y);
		std::printf("  %dx%d px (%s), cells %.1fx%.0f: row 1 %.1f %% drawn, marker %.1f %%, empty part %.1f %%\n", fw,
					fh, "build/glcheck-out/lattice.png", cell.x, cell.y, 100 * text, 100 * marker, 100 * none);
		why = error           ? "GL error " + std::to_string(error)
			  : text < 0.09   ? "no grid"
			  : marker < 0.01 ? "the grid doesn't fit the window"
			  : none > 0      ? "not the background"
							  : "";
	}
	w.destroy();
	hydra::collectWindows();
	return why;
}

} // namespace

int main() {
	if (!glfwInit()) {
		std::fprintf(stderr, "glcheck: no GLFW (no display?)\n");
		return 1;
	}
	// hydra.cpp createWindow()'s hints, Rack 2.6's own.
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
	glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	GLFWwindow* win = glfwCreateWindow(64, 64, "glcheck", NULL, NULL);
	if (!win) {
		std::fprintf(stderr, "glcheck: no GL 2.0 context\n");
		return 1;
	}
	glfwMakeContextCurrent(win);
	glewExperimental = GL_TRUE;
	glewInit();
	NVGcontext* vg = nvgCreateGL2(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
	std::printf("GL %s, GLSL %s, %s\n", glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION),
				glGetString(GL_RENDERER));

	// The editor window's widgets use Rack's event state (Widget's destructor), its fonts Rack's res/.
	// ponytail: never freed, glcheck exits.
	static rack::plugin::Plugin self; // the bundled fonts: res/fonts of the working directory
	self.path = ".";
	pluginInstance = &self;
	rack::contextSet(new rack::Context);
	APP->event = new rack::widget::EventState;
	if (const char* dir = std::getenv("RACK_SYSTEM_DIR"))
		rack::asset::systemDir = dir;

	std::string cache = "build/glcheck-out/cache";
	rack::system::removeRecursively(cache);
	fetch::Worker web(cache, [](const std::string& url, const std::string& file) {
		const char* fixture = url == kLibColorUrl || url == kNotAnImageUrl ? "test/fixtures/lib-color.js"
							  : url == kQuadrantsUrl                       ? "test/fixtures/quadrants.png"
																		   : nullptr;
		return fixture && rack::system::copy(fixture, file);
	});
	filo::EngineConfig c;
	c.resDir = "res";
	c.web = &web;
	c.log = [](const std::string& line, bool warning) {
		if (warning && line.find("strudel.b-cdn.net") == std::string::npos) // offline: prebake's sample maps fail
			std::printf("  log: %s\n", line.substr(0, 400).c_str());
	};
	filo::Engine engine(c);
	hydra::Renderer renderer;
	int failures = 0;
	double t = 100; // Icing's clock (s)
	uint64_t seen = 0;
	rack::system::createDirectories("build/glcheck-out");
	const std::vector<Case> all = cases();
	for (const Case& k : all) {
		std::printf("%s\n", k.name);
		renderer.reset();
		uint32_t id = engine.evaluate("await initHydra()\nhush()\n" + k.code, false);
		for (double until = now() + 60; engine.report().id != id && now() < until;) {
			runAudio(engine, 256);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		filo::Engine::Report r = engine.report();
		if (r.id != id || !r.ok) {
			std::printf("  FAIL evaluate: %s\n", r.id == id ? r.error.c_str() : "timed out");
			failures++;
			continue;
		}
		hydra::Exchange::Frame f;
		bool shaderFailed = false, errorSeen = false;
		for (int i = 0; i < k.frames; i++) {
			hydra::Exchange::Request q;
			q.time = t += 1.0 / 60;
			engine.hydra.request(q);
			for (double until = now() + 2; !engine.hydra.frame(seen, f) && now() < until;)
				runAudio(engine, 64);
			seen = f.seq;
			renderer.update(vg, engine.hydra, [&](const std::string& line) {
				bool expected = k.error && line.find(k.error) != std::string::npos;
				std::printf("  %s %s\n", expected ? "error, as expected:" : "FAIL", line.c_str());
				errorSeen |= expected;
				shaderFailed |= !expected;
			});
		}
		for (int o = 0; o < hydra::Exchange::kOutputs; o++) {
			hydra::Exchange::Program p;
			if (!engine.hydra.program(o, 0, p) || p.frag.find("solid(st, 0., 0., 0., 0.)") != std::string::npos)
				continue; // hush()'s
			bool ok = false;
			std::string log = compileLog(p.frag, ok);
			std::printf("  o%d: %s%s%s\n", o, ok ? "compiled" : "FAILED", log.empty() ? "" : ": ", log.c_str());
		}
		Image im = readBack(renderer, f.width, f.height);
		GLenum error = glGetError();
		std::string why = error                   ? "GL error " + std::to_string(error)
						  : k.error && !errorSeen ? std::string("no error \"") + k.error + "\""
												  : k.check(im);
		std::string png = "build/glcheck-out/" + std::to_string(&k - &all[0]) + ".png";
		stbi_write_png(png.c_str(), im.w, im.h, 4, im.at(0, im.h - 1), -4 * im.w); // top row first
		std::printf("  %dx%d after %d frames (%s): %.1f %% lit, ripple x %.2f y %.2f -> %s\n", im.w, im.h, k.frames,
					png.c_str(), 100 * im.lit(), im.ripple(1, 0), im.ripple(0, 1),
					why.empty() ? "ok" : ("FAIL " + why).c_str());
		failures += shaderFailed || !why.empty();
	}
	// The canvas follows its window: the preview's aspect, 16:9, and back. Every size draws and leaves no GL error.
	std::printf("canvas resize\n");
	{
		renderer.reset();
		uint32_t id = engine.evaluate("await initHydra()\nhush()\nosc(10, 0.1, 1).out()", false);
		for (double until = now() + 60; engine.report().id != id && now() < until;) {
			runAudio(engine, 256);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		std::string why;
		for (hydra::CanvasSize size : {hydra::CanvasSize{1280, 1032}, {1920, 1080}, {1280, 1032}, {800, 1200}}) {
			hydra::Exchange::Frame f;
			for (int i = 0; i < 4 && why.empty(); i++) {
				hydra::Exchange::Request q;
				q.time = t += 1.0 / 60;
				q.width = size.w, q.height = size.h;
				engine.hydra.request(q);
				for (double until = now() + 2; !engine.hydra.frame(seen, f) && now() < until;)
					runAudio(engine, 64);
				seen = f.seq;
				renderer.update(vg, engine.hydra, [&](const std::string& line) { why = line; });
			}
			Image im = readBack(renderer, size.w, size.h);
			GLenum error = glGetError();
			std::printf("  %dx%d: frame %dx%d, %.1f %% lit\n", size.w, size.h, f.width, f.height, 100 * im.lit());
			if (why.empty())
				why = error ? "GL error " + std::to_string(error)
					  : f.width != size.w || f.height != size.h
						  ? "the frame is not " + std::to_string(size.w) + "x" + std::to_string(size.h)
					  : im.lit() < 0.2 ? "nothing drawn"
									   : "";
		}
		std::printf("  -> %s\n", why.empty() ? "ok" : ("FAIL " + why).c_str());
		failures += !why.empty();
	}
	std::printf("editor window\n");
	std::string why = editorWindow(win, "Patchisserie");
	std::printf("  -> %s\n", why.empty() ? "ok" : ("FAIL " + why).c_str());
	failures += !why.empty();
	why = editorWindow(win, "githubLight"); // a light theme: #ffffff
	std::printf("  -> %s\n", why.empty() ? "ok" : ("FAIL " + why).c_str());
	failures += !why.empty();
	std::printf("grid window\n");
	why = gridWindow(win);
	std::printf("  -> %s\n", why.empty() ? "ok" : ("FAIL " + why).c_str());
	failures += !why.empty();
	renderer.destroy();
	nvgDeleteGL2(vg);
	glfwDestroyWindow(win);
	glfwTerminate();
	std::printf("glcheck: %d failures\n", failures);
	return failures ? 1 : 0;
}
