// make glcheck: Icing's GL path on this machine's driver, which `make test` never reaches (CI runners may have no
// GL). Rack's context (hydra.cpp's window hints; libRack's GLFW, GLEW and NanoVG) in a hidden window, and the plugin's
// own objects: the real Engine evaluates each sketch, hydra::Renderer compiles and renders its frames at 60 frames/s
// of Icing time, present() draws the displayed image into an FBO that is read back. Prints every shader's compile log
// and what each image looks like; exits 1 when a shader fails or an image isn't what hydra draws.
#include <rack.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

#include <stb_image_write.h>

#include "engine.hpp"

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
};

std::string atLeast(const Image& im, double lit) {
	return im.lit() >= lit ? "" : "less than " + std::to_string((int)(lit * 100)) + " % lit";
}

const char kLibColorUrl[] = "https://metagrowing.org/extra-shaders-for-hydra/lib-color.js";

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
	renderer.present();
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

	std::string cache = "build/glcheck-out/cache";
	rack::system::removeRecursively(cache);
	fetch::Worker web(cache, [](const std::string& url, const std::string& file) {
		return url == kLibColorUrl && rack::system::copy("test/fixtures/lib-color.js", file);
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
		bool shaderFailed = false;
		for (int i = 0; i < k.frames; i++) {
			hydra::Exchange::Request q;
			q.time = t += 1.0 / 60;
			engine.hydra.request(q);
			for (double until = now() + 2; !engine.hydra.frame(seen, f) && now() < until;)
				runAudio(engine, 64);
			seen = f.seq;
			renderer.update(vg, engine.hydra, [&](const std::string& line) {
				std::printf("  FAIL %s\n", line.c_str());
				shaderFailed = true;
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
		std::string why = error ? "GL error " + std::to_string(error) : k.check(im);
		std::string png = "build/glcheck-out/" + std::to_string(&k - &all[0]) + ".png";
		stbi_write_png(png.c_str(), im.w, im.h, 4, im.at(0, im.h - 1), -4 * im.w); // top row first
		std::printf("  %dx%d after %d frames (%s): %.1f %% lit, ripple x %.2f y %.2f -> %s\n", im.w, im.h, k.frames,
					png.c_str(), 100 * im.lit(), im.ripple(1, 0), im.ripple(0, 1),
					why.empty() ? "ok" : ("FAIL " + why).c_str());
		failures += shaderFailed || !why.empty();
	}
	renderer.destroy();
	nvgDeleteGL2(vg);
	glfwDestroyWindow(win);
	glfwTerminate();
	std::printf("glcheck: %d failures\n", failures);
	return failures ? 1 : 0;
}
