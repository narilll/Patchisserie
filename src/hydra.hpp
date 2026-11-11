#pragma once
// Hydra (Icing) graphics. GLSL conversion is pure and unit-tested; Renderer/Window are GL and live in hydra.cpp.
// No Rack or GL includes here, so test/ can include this header.
#include <cctype>
#include <string>

struct GLFWwindow;
struct NVGcontext;
struct NVGLUframebuffer;

namespace hydra {

const char kDesktopHeader[] = "#version 120\n#define lowp\n#define mediump\n#define highp\n";

/** GLSL ES 1.0 (WebGL, what hydra-synth emits) -> desktop GLSL 1.20 (Rack's GL 2.0 context).
Precision statements are blanked, not removed, so desktop line N + 4 is source line N.
*/
inline std::string glslEsToDesktop(const std::string& src) {
	// ponytail: guards only against our own header (idempotence); a source with its own #version is not handled.
	if (src.compare(0, sizeof(kDesktopHeader) - 1, kDesktopHeader) == 0)
		return src;
	std::string out = kDesktopHeader;
	size_t i = 0;
	while (i < src.size()) {
		size_t end = src.find('\n', i);
		end = end == std::string::npos ? src.size() : end + 1;
		std::string line = src.substr(i, end - i);
		size_t s = line.find_first_not_of(" \t");
		size_t semi = line.find(';', s);
		// ponytail: line-based; a precision statement split across lines is left alone.
		if (s != std::string::npos && line.compare(s, 9, "precision") == 0 && s + 9 < line.size() &&
			std::isspace((unsigned char)line[s + 9]) && semi != std::string::npos)
			line.erase(s, semi + 1 - s);
		out += line;
		i = end;
	}
	return out;
}

/** Output window geometry in screen coordinates, as saved in the patch. */
struct WindowGeometry {
	int x = 100, y = 100, w = 640, h = 360;
	bool fullscreen = false;
};

/** GL objects in Rack's context. Call only on the UI thread with Rack's context current. */
struct Renderer {
	NVGLUframebuffer* fb = nullptr; // fb->image: NanoVG preview; fb->texture: sampled by Window
	unsigned prog = 0, blitProg = 0;
	int timeLoc = -1, resolutionLoc = -1;
	bool failed = false;

	/** Renders one frame into the FBO, creating GL objects on first use. Restores the GL state it touches. */
	void render(NVGcontext* vg, float time);
	void drawPreview(NVGcontext* vg, float x, float y, float w, float h);
	/** Deletes GL objects; next render() recreates them (e.g. in a new context). */
	void destroy();
};

/** Separate output window. Its context shares Rack's, but FBOs aren't shared: it only samples Renderer's texture. */
struct Window {
	GLFWwindow* win = nullptr;
	bool fullscreen = false; // applied state; WindowGeometry::fullscreen is the wanted one

	/** Reconciles the window with `open`/`g` and presents a frame. Title-bar close and Esc write back into them.
	Must run outside GLFW callbacks (step()), so menus and keys only flip flags. Leaves `rack`'s context current.
	*/
	void update(GLFWwindow* rack, const Renderer& r, WindowGeometry& g, bool& open);
	void destroy();
};

} // namespace hydra
