#include "hydra.hpp"

#include <rack.hpp>

#if defined ARCH_MAC
#include <objc/message.h>
extern "C" void* glfwGetCocoaWindow(GLFWwindow* window); // glfw3native.h's `id` typedef clashes with objc's
#endif

namespace hydra {

// --- a.fft ---

float Analyser::analyse(float sampleRate, float* specific) const {
	static rack::dsp::RealFFT fft(kFftSize); // UI thread only, like its buffers
	alignas(16) static float in[kFftSize], out[2 * kFftSize];
	for (int i = 0; i < kFftSize; i++) {
		float hann = 0.5f - 0.5f * (float)std::cos(2 * M_PI * i / (kFftSize - 1)); // Meyda's windowing.ts hanning
		in[i] = history_[(pos_ + i) % kFftSize] * hann;
	}
	fft.rfft(in, out); // out[0] = DC, out[1] = Nyquist, then re/im pairs: unnormalized, like Meyda's fftjs
	float amp[kFftSize / 2];
	amp[0] = std::fabs(out[0]);
	for (int i = 1; i < kFftSize / 2; i++)
		amp[i] = std::hypot(out[2 * i], out[2 * i + 1]);
	return loudness(amp, kFftSize / 2, sampleRate, specific);
}

// --- GL ---

// ponytail: canvas sizes beyond this (setResolution()) are clamped; GL 2.0 guarantees less, real GPUs more.
static const int kMaxSize = 4096;

// hydra-synth's output.js vertex shader and big triangle, verbatim (uv = position, so fragment code ports unchanged).
static const char* kVert = R"(precision mediump float;
attribute vec2 position;
varying vec2 uv;
void main () {
  uv = position;
  gl_Position = vec4(2.0 * position - 1.0, 0, 1);
}
)";
static const float kTriangle[] = {-2, 0, 0, -2, 2, 2};

// hydra-synth.js _initRegl(): renderFbo (render(oN)) and renderAll (render()), verbatim, so the image is oriented as
// on hydra's canvas.
static const char* kCompositeVert = R"(precision mediump float;
attribute vec2 position;
varying vec2 uv;
void main () {
  uv = position;
  gl_Position = vec4(1.0 - 2.0 * position, 0, 1);
}
)";
static const char* kRenderFbo = R"(precision mediump float;
varying vec2 uv;
uniform vec2 resolution;
uniform sampler2D tex0;
void main () {
  gl_FragColor = texture2D(tex0, vec2(1.0 - uv.x, uv.y));
}
)";
static const char* kRenderAll = R"(precision mediump float;
varying vec2 uv;
uniform sampler2D tex0;
uniform sampler2D tex1;
uniform sampler2D tex2;
uniform sampler2D tex3;
void main () {
  vec2 st = vec2(1.0 - uv.x, uv.y);
  st*= vec2(2);
  vec2 q = floor(st).xy*(vec2(2.0, 1.0));
  int quad = int(q.x) + int(q.y);
  st.x += step(1., mod(st.y,2.0));
  st.y += step(1., mod(st.x,2.0));
  st = fract(st);
  if(quad==0){
    gl_FragColor = texture2D(tex0, st);
  } else if(quad==1){
    gl_FragColor = texture2D(tex1, st);
  } else if (quad==2){
    gl_FragColor = texture2D(tex2, st);
  } else {
    gl_FragColor = texture2D(tex3, st);
  }
}
)";

static const char* kBlitFrag = R"(precision mediump float;
uniform sampler2D tex;
varying vec2 uv;
void main () {
  gl_FragColor = texture2D(tex, uv);
}
)";

static GLuint compile(GLenum type, const std::string& es, std::string& log) {
	std::string src = glslEsToDesktop(es);
	const char* p = src.c_str();
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &p, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char text[1024] = "";
		glGetShaderInfoLog(s, sizeof(text), NULL, text);
		log = text;
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static GLuint link(const char* vert, const std::string& frag, std::string& log) {
	GLuint v = compile(GL_VERTEX_SHADER, vert, log), f = v ? compile(GL_FRAGMENT_SHADER, frag, log) : 0, p = 0;
	if (v && f) {
		p = glCreateProgram();
		glAttachShader(p, v);
		glAttachShader(p, f);
		glBindAttribLocation(p, 0, "position");
		glLinkProgram(p);
		GLint ok = 0;
		glGetProgramiv(p, GL_LINK_STATUS, &ok);
		if (!ok) {
			char text[1024] = "";
			glGetProgramInfoLog(p, sizeof(text), NULL, text);
			log = text;
			glDeleteProgram(p);
			p = 0;
		}
	}
	glDeleteShader(v); // flagged for deletion, freed with the program; 0 is ignored
	glDeleteShader(f);
	return p;
}

static void drawTriangle() {
	// ponytail: client-side vertex array, legal in GL 2.x compatibility contexts. A core profile (Rack 3?) needs a
	// VBO + VAO here.
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, kTriangle);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glDisableVertexAttribArray(0);
}

/** Binds `texture` to the next texture unit for sampler `location` (-1: not in the program, skipped). */
static void bindSampler(GLint location, GLuint texture, int& unit) {
	if (location < 0)
		return;
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, texture);
	glUniform1i(location, unit++);
}

static void setCap(GLenum cap, GLboolean on) {
	if (on)
		glEnable(cap);
	else
		glDisable(cap);
}

/** Rack's NanoVG frame (and other plugins) run after step(): leave the state we touch as we found it. NanoVG
caches its unit-0 texture binding, hence that one too. */
struct SavedGlState {
	GLint fbo, program, buffer, viewport[4], activeTexture, texture;
	GLfloat clear[4];
	GLboolean blend, cull, depth, scissor, stencil;
	SavedGlState() {
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
		glGetIntegerv(GL_CURRENT_PROGRAM, &program);
		glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
		glGetIntegerv(GL_VIEWPORT, viewport);
		glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
		glActiveTexture(GL_TEXTURE0);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
		glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
		blend = glIsEnabled(GL_BLEND);
		cull = glIsEnabled(GL_CULL_FACE);
		depth = glIsEnabled(GL_DEPTH_TEST);
		scissor = glIsEnabled(GL_SCISSOR_TEST);
		stencil = glIsEnabled(GL_STENCIL_TEST);
		for (GLenum cap : {GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_SCISSOR_TEST, GL_STENCIL_TEST})
			glDisable(cap);
	}
	~SavedGlState() {
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glUseProgram(program);
		glBindBuffer(GL_ARRAY_BUFFER, buffer);
		glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, texture);
		glActiveTexture(activeTexture);
		glClearColor(clear[0], clear[1], clear[2], clear[3]);
		setCap(GL_BLEND, blend);
		setCap(GL_CULL_FACE, cull);
		setCap(GL_DEPTH_TEST, depth);
		setCap(GL_SCISSOR_TEST, scissor);
		setCap(GL_STENCIL_TEST, stencil);
	}
};

static void clearTo0(NVGLUframebuffer* fb) {
	glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
	glClearColor(0, 0, 0, 0); // regl's fresh framebuffers
	glClear(GL_COLOR_BUFFER_BIT);
}

static void deleteFb(NVGLUframebuffer*& fb) {
	if (fb)
		nvgluDeleteFramebuffer(fb);
	fb = nullptr;
}

void Renderer::update(NVGcontext* vg, const Exchange& x, const Error& error) {
	if (failed_)
		return;
	if (!blit_) {
		std::string log;
		one_ = link(kCompositeVert, kRenderFbo, log);
		all_ = link(kCompositeVert, kRenderAll, log);
		blit_ = link(kVert, kBlitFrag, log);
		if (!one_ || !all_ || !blit_) {
			WARN("Icing: GL setup failed: %s", log.c_str());
			failed_ = true;
			return;
		}
	}
	for (int i = 0; i < Exchange::kOutputs; i++) {
		Exchange::Program p;
		if (x.program(i, programs_[i].id, p))
			compile(i, p, error);
	}
	Exchange::Frame f;
	if (x.frame(seq_, f)) {
		seq_ = f.seq;
		render(vg, f);
	}
}

void Renderer::compile(int output, const Exchange::Program& p, const Error& error) {
	Program& q = programs_[output];
	if (q.gl)
		glDeleteProgram(q.gl);
	q = Program();
	q.id = p.id;
	if (!p.id)
		return;
	std::string log;
	q.gl = link(kVert, p.frag, log);
	if (!q.gl) { // the output keeps its last image
		error("[hydra] o" + std::to_string(output) + ": " + log);
		return;
	}
	q.time = glGetUniformLocation(q.gl, "time");
	q.resolution = glGetUniformLocation(q.gl, "resolution");
	q.prevBuffer = glGetUniformLocation(q.gl, "prevBuffer");
	for (const std::string& name : p.uniforms)
		q.locations.push_back(glGetUniformLocation(q.gl, name.c_str()));
	q.samplers = p.samplers;
	q.samplers.resize(q.locations.size());
}

unsigned Renderer::other(int output) const {
	return fbo_[output][ping_[output] ^ 1]->texture;
}

void Renderer::render(NVGcontext* vg, const Exchange::Frame& f) {
	int w = rack::math::clamp(f.width, 1, kMaxSize), h = rack::math::clamp(f.height, 1, kMaxSize);
	if (!display_ || w != w_ || h != h_) { // first frame, setResolution()
		for (auto& pair : fbo_)
			for (NVGLUframebuffer*& fb : pair)
				deleteFb(fb);
		deleteFb(display_);
		bool ok = true;
		for (auto& pair : fbo_)
			for (NVGLUframebuffer*& fb : pair)
				ok &= (fb = nvgluCreateFramebuffer(vg, w, h, NVG_IMAGE_NEAREST)) != nullptr; // hydra's mag: 'nearest'
		ok &= (display_ = nvgluCreateFramebuffer(vg, w, h, 0)) != nullptr;
		if (!ok) {
			WARN("Icing: cannot create %dx%d framebuffers", w, h);
			failed_ = true;
			return;
		}
		w_ = w, h_ = h;
		clearImages();
	}
	SavedGlState saved;
	glViewport(0, 0, w_, h_);
	// hydra-synth's tick(): each output with a shader draws into its other buffer, then the display pass.
	for (int i = 0; i < Exchange::kOutputs; i++) {
		const Program& p = programs_[i];
		const std::vector<float>& v = f.values[i];
		if (!p.gl || f.program[i] != p.id || v.size() != p.locations.size())
			continue;
		ping_[i] ^= 1;
		glBindFramebuffer(GL_FRAMEBUFFER, fbo_[i][ping_[i]]->fbo);
		glUseProgram(p.gl);
		glUniform1f(p.time, f.time);
		glUniform2f(p.resolution, (float)w_, (float)h_);
		int unit = 0;
		// ponytail: prev() reads the previous frame; hydra-synth binds the buffer being drawn (a WebGL feedback loop).
		bindSampler(p.prevBuffer, other(i), unit);
		for (size_t k = 0; k < v.size(); k++) {
			if (!p.samplers[k]) {
				glUniform1f(p.locations[k], v[k]); // -1 (optimized out) is ignored
				continue;
			}
			bool output = v[k] >= 0 && v[k] < Exchange::kOutputs; // false for NaN
			bindSampler(p.locations[k], output ? other((int)v[k]) : 0, unit);
		}
		drawTriangle();
	}
	glBindFramebuffer(GL_FRAMEBUFFER, display_->fbo);
	int unit = 0;
	if (f.display >= 0 && f.display < Exchange::kOutputs) {
		glUseProgram(one_);
		bindSampler(glGetUniformLocation(one_, "tex0"), fbo_[f.display][ping_[f.display]]->texture, unit);
	} else {
		glUseProgram(all_);
		for (int i = 0; i < Exchange::kOutputs; i++) {
			std::string name = "tex" + std::to_string(i);
			bindSampler(glGetUniformLocation(all_, name.c_str()), fbo_[i][ping_[i]]->texture, unit);
		}
	}
	drawTriangle();
	glFlush(); // Apple's shared-context rule: flush the writer, rebind in the reader (Window::update)
}

void Renderer::drawPreview(NVGcontext* vg, float x, float y, float w, float h) {
	nvgBeginPath(vg);
	nvgRect(vg, x, y, w, h);
	nvgFillColor(vg, nvgRGB(0, 0, 0)); // under a transparent image, as hydra's black page under its canvas
	nvgFill(vg);
	if (!display_ || failed_)
		return;
	int iw, ih;
	nvgImageSize(vg, display_->image, &iw, &ih);
	float fw = std::min(w, h * iw / ih), fh = fw * ih / iw; // the canvas's aspect, centred; the rest stays black
	x += (w - fw) / 2;
	y += (h - fh) / 2;
	nvgBeginPath(vg);
	nvgRect(vg, x, y, fw, fh);
	nvgFillPaint(vg, nvgImagePattern(vg, x, y, fw, fh, 0.f, display_->image, 1.f)); // nvglu image is already FLIPY
	nvgFill(vg);
}

void Renderer::present() const {
	if (!display_ || failed_)
		return;
	glUseProgram(blit_);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, display_->texture); // rebind every frame to see the Rack context's writes
	drawTriangle();
}

void Renderer::reset() {
	for (Program& p : programs_) {
		glDeleteProgram(p.gl); // 0 is ignored
		p = Program();
	}
	seq_ = 0;
	clearImages();
}

void Renderer::clearImages() {
	for (int& p : ping_)
		p = 0;
	if (!display_)
		return;
	SavedGlState saved;
	for (auto& pair : fbo_)
		for (NVGLUframebuffer* fb : pair)
			clearTo0(fb);
	clearTo0(display_);
}

void Renderer::destroy() {
	for (auto& pair : fbo_)
		for (NVGLUframebuffer*& fb : pair)
			deleteFb(fb);
	deleteFb(display_);
	for (const Program& p : programs_)
		glDeleteProgram(p.gl); // 0 is ignored
	for (unsigned p : {one_, all_, blit_})
		glDeleteProgram(p);
	*this = Renderer();
}

// --- Output window ---

static void setLevelAboveMenuBar(GLFWwindow* win, bool above) {
#if defined ARCH_MAC
	// Borderless windows sit under the menu bar and Dock; GLFW's own fullscreen uses NSMainMenuWindowLevel + 1.
	const long kNSNormalWindowLevel = 0, kNSMainMenuWindowLevel = 24;
	((void (*)(void*, SEL, long))objc_msgSend)(glfwGetCocoaWindow(win), sel_registerName("setLevel:"),
											   above ? kNSMainMenuWindowLevel + 1 : kNSNormalWindowLevel);
#endif
}

static GLFWmonitor* monitorAt(int x, int y) {
	int n = 0;
	GLFWmonitor** monitors = glfwGetMonitors(&n);
	for (int i = 0; i < n; i++) {
		int mx, my;
		glfwGetMonitorPos(monitors[i], &mx, &my);
		const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
		if (mode && x >= mx && y >= my && x < mx + mode->width && y < my + mode->height)
			return monitors[i];
	}
	return NULL;
}

static GLFWwindow* createWindow(GLFWwindow* rack, WindowGeometry& g) {
	// Same hints as Rack 2.6's own window (checked in libRack's Window::Window), so the contexts can share.
	glfwDefaultWindowHints();
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
	glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	GLFWwindow* win = glfwCreateWindow(g.w, g.h, "Icing", NULL, rack);
	if (!win) {
		WARN("Icing: could not create output window");
		return NULL;
	}
	glfwMakeContextCurrent(win);
	glfwSwapInterval(0); // never block Rack's UI thread on this window's vsync
	glfwMakeContextCurrent(rack);
	glfwSetInputMode(win, GLFW_STICKY_KEYS, GLFW_TRUE); // a quick Esc tap between frames still registers
	if (!monitorAt(g.x + g.w / 2, g.y + g.h / 2)) {
		// Saved on a monitor that is gone (or another machine): don't open off-screen.
		glfwGetMonitorWorkarea(glfwGetPrimaryMonitor(), &g.x, &g.y, NULL, NULL);
		g.x += 50;
		g.y += 50;
	}
	glfwSetWindowPos(win, g.x, g.y);
	return win;
}

static void applyFullscreen(GLFWwindow* win, const WindowGeometry& g) {
	// Borderless window sized to its monitor, no video mode switch (glfwSetWindowMonitor would also iconify on
	// focus loss).
	if (g.fullscreen) {
		GLFWmonitor* mon = monitorAt(g.x + g.w / 2, g.y + g.h / 2);
		if (!mon)
			mon = glfwGetPrimaryMonitor();
		int mx, my;
		glfwGetMonitorPos(mon, &mx, &my);
		const GLFWvidmode* mode = glfwGetVideoMode(mon);
		glfwSetWindowAttrib(win, GLFW_DECORATED, GLFW_FALSE);
		glfwSetWindowSize(win, mode->width, mode->height); // size before pos: Cocoa keeps the top-left
		glfwSetWindowPos(win, mx, my);
	} else {
		glfwSetWindowAttrib(win, GLFW_DECORATED, GLFW_TRUE);
		glfwSetWindowSize(win, g.w, g.h);
		glfwSetWindowPos(win, g.x, g.y);
	}
	setLevelAboveMenuBar(win, g.fullscreen);
}

void Window::update(GLFWwindow* rack, const Renderer* image, WindowGeometry& g, bool& open) {
	if (win && glfwWindowShouldClose(win)) { // title-bar close = hide; Rack state stays
		glfwSetWindowShouldClose(win, GLFW_FALSE);
		open = false;
	}
	if (!open) {
		if (win && glfwGetWindowAttrib(win, GLFW_VISIBLE))
			glfwHideWindow(win);
		return;
	}
	if (!win) {
		win = createWindow(rack, g);
		fullscreen = false;
		if (!win) {
			open = false;
			return;
		}
	}
	if (glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS)
		g.fullscreen = false;
	if (!fullscreen) { // track the windowed geometry for the patch and for leaving fullscreen
		glfwGetWindowPos(win, &g.x, &g.y);
		glfwGetWindowSize(win, &g.w, &g.h);
	}
	if (g.fullscreen != fullscreen) {
		applyFullscreen(win, g);
		fullscreen = g.fullscreen;
	}
	if (!glfwGetWindowAttrib(win, GLFW_VISIBLE)) // after applyFullscreen: no windowed flash when restoring
		glfwShowWindow(win);
	if (glfwGetWindowAttrib(win, GLFW_ICONIFIED))
		return;

	glfwMakeContextCurrent(win);
	int fw, fh;
	glfwGetFramebufferSize(win, &fw, &fh);
	glViewport(0, 0, fw, fh);
	glClear(GL_COLOR_BUFFER_BIT);
	if (image)
		image->present();
	glfwSwapBuffers(win);
	glfwMakeContextCurrent(rack);
}

// UI thread. glfwDestroyWindow is not callback-safe (it pumps Cocoa events), and Rack deletes widgets inside its
// input callbacks (Delete, undo, File > Open).
static std::vector<GLFWwindow*> hidden;

void Window::destroy() {
	if (win) {
		glfwHideWindow(win);
		hidden.push_back(win);
	}
	win = nullptr;
	fullscreen = false;
}

// ponytail: with no Icing left to step, a hidden window waits for the next Icing or for glfwTerminate() at exit.
void collectWindows() {
	for (GLFWwindow* w : hidden)
		glfwDestroyWindow(w);
	hidden.clear();
}

} // namespace hydra
