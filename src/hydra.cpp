#include "hydra.hpp"

#include <rack.hpp>

#if defined ARCH_MAC
#include <objc/message.h>
extern "C" void* glfwGetCocoaWindow(GLFWwindow* window); // glfw3native.h's `id` typedef clashes with objc's
#endif

namespace hydra {

// ponytail: fixed size = hydra-synth's default canvas; the window stretches it. Follow the window size when M5
// needs setResolution().
static const int W = 1280, H = 720;

// hydra-synth's output vertex shader and big triangle, verbatim (uv = position, so fragment code ports unchanged).
static const char* kVert = R"(precision mediump float;
attribute vec2 position;
varying vec2 uv;
void main () {
  uv = position;
  gl_Position = vec4(2.0 * position - 1.0, 0, 1);
}
)";
static const float kTriangle[] = {-2, 0, 0, -2, 2, 2};

// ponytail: hard-coded spike shader = `osc(60, 0.1, 1.2).rotate(0, 0.1).out()` with hydra's GLSL; M5 swaps in
// hydra-synth's generated source.
static const char* kFrag = R"(precision mediump float;
uniform float time;
uniform vec2 resolution;
varying vec2 uv;

vec4 osc(vec2 _st, float frequency, float sync, float offset) {
  vec2 st = _st;
  float r = sin((st.x - offset / frequency + time * sync) * frequency) * 0.5 + 0.5;
  float g = sin((st.x + time * sync) * frequency) * 0.5 + 0.5;
  float b = sin((st.x + offset / frequency + time * sync) * frequency) * 0.5 + 0.5;
  return vec4(r, g, b, 1.0);
}

vec2 rotate(vec2 _st, float angle, float speed) {
  vec2 xy = _st - vec2(0.5);
  float ang = angle + speed * time;
  xy = mat2(cos(ang), -sin(ang), sin(ang), cos(ang)) * xy;
  return xy + 0.5;
}

void main () {
  vec2 st = gl_FragCoord.xy / resolution.xy;
  gl_FragColor = osc(rotate(st, 0.0, 0.1), 60.0, 0.1, 1.2);
}
)";

static const char* kBlitFrag = R"(precision mediump float;
uniform sampler2D tex;
varying vec2 uv;
void main () {
  gl_FragColor = texture2D(tex, uv);
}
)";

static GLuint compile(GLenum type, const char* es) {
	std::string src = glslEsToDesktop(es);
	const char* p = src.c_str();
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &p, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		WARN("Icing: shader compile failed: %s", log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static GLuint link(const char* frag) {
	GLuint v = compile(GL_VERTEX_SHADER, kVert), f = compile(GL_FRAGMENT_SHADER, frag), p = 0;
	if (v && f) {
		p = glCreateProgram();
		glAttachShader(p, v);
		glAttachShader(p, f);
		glBindAttribLocation(p, 0, "position");
		glLinkProgram(p);
		GLint ok = 0;
		glGetProgramiv(p, GL_LINK_STATUS, &ok);
		if (!ok) {
			char log[1024] = "";
			glGetProgramInfoLog(p, sizeof(log), NULL, log);
			WARN("Icing: shader link failed: %s", log);
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

static void setCap(GLenum cap, GLboolean on) {
	if (on)
		glEnable(cap);
	else
		glDisable(cap);
}

/** Rack's NanoVG frame (and other plugins) run after step(): leave the state we touch as we found it. */
struct SavedGlState {
	GLint fbo, program, buffer, viewport[4];
	GLboolean blend, cull, depth, scissor, stencil;
	SavedGlState() {
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
		glGetIntegerv(GL_CURRENT_PROGRAM, &program);
		glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
		glGetIntegerv(GL_VIEWPORT, viewport);
		blend = glIsEnabled(GL_BLEND);
		cull = glIsEnabled(GL_CULL_FACE);
		depth = glIsEnabled(GL_DEPTH_TEST);
		scissor = glIsEnabled(GL_SCISSOR_TEST);
		stencil = glIsEnabled(GL_STENCIL_TEST);
	}
	~SavedGlState() {
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glUseProgram(program);
		glBindBuffer(GL_ARRAY_BUFFER, buffer);
		glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
		setCap(GL_BLEND, blend);
		setCap(GL_CULL_FACE, cull);
		setCap(GL_DEPTH_TEST, depth);
		setCap(GL_SCISSOR_TEST, scissor);
		setCap(GL_STENCIL_TEST, stencil);
	}
};

void Renderer::render(NVGcontext* vg, float time) {
	if (failed)
		return;
	if (!fb) {
		fb = nvgluCreateFramebuffer(vg, W, H, 0);
		prog = link(kFrag);
		blitProg = link(kBlitFrag);
		if (!fb || !prog || !blitProg) {
			WARN("Icing: GL setup failed (framebuffer %d, programs %u %u)", fb != nullptr, prog, blitProg);
			failed = true;
			return;
		}
		timeLoc = glGetUniformLocation(prog, "time");
		resolutionLoc = glGetUniformLocation(prog, "resolution");
	}
	SavedGlState saved;
	glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
	glViewport(0, 0, W, H);
	for (GLenum cap : {GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_SCISSOR_TEST, GL_STENCIL_TEST})
		glDisable(cap);
	glUseProgram(prog);
	glUniform1f(timeLoc, time);
	glUniform2f(resolutionLoc, W, H);
	drawTriangle();
	glFlush(); // Apple's shared-context rule: flush the writer, rebind in the reader (Window::update)
}

void Renderer::drawPreview(NVGcontext* vg, float x, float y, float w, float h) {
	if (!fb || failed)
		return;
	nvgBeginPath(vg);
	nvgRect(vg, x, y, w, h);
	nvgFillPaint(vg, nvgImagePattern(vg, x, y, w, h, 0.f, fb->image, 1.f)); // nvglu image is already FLIPY
	nvgFill(vg);
}

void Renderer::destroy() {
	if (fb)
		nvgluDeleteFramebuffer(fb);
	if (prog)
		glDeleteProgram(prog);
	if (blitProg)
		glDeleteProgram(blitProg);
	*this = Renderer();
}

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

void Window::update(GLFWwindow* rack, const Renderer& r, WindowGeometry& g, bool& open) {
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
	if (r.fb && !r.failed) {
		glUseProgram(r.blitProg);
		glBindTexture(GL_TEXTURE_2D, r.fb->texture); // rebind every frame to see the Rack context's writes
		drawTriangle();
	}
	glfwSwapBuffers(win);
	glfwMakeContextCurrent(rack);
}

void Window::destroy() {
	// ponytail: GLFW documents glfwDestroyWindow as not callback-safe (it pumps Cocoa events), and Rack deletes
	// widgets inside its input callbacks (Delete, undo, File > Open). Works in practice; if it ever misbehaves,
	// hide here and reuse/destroy the window from the next step().
	if (win)
		glfwDestroyWindow(win);
	win = nullptr;
	fullscreen = false;
}

} // namespace hydra
