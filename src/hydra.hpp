#pragma once
// Hydra (Icing) graphics. GLSL conversion, the audio loudness and the frame exchange with Filo's JS thread are pure
// and unit-tested; Analyser::analyse (Rack's FFT), Renderer and Window are GL/Rack and live in hydra.cpp.
// No Rack or GL includes here, so test/ and engine.hpp can include this header.
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

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

// --- a.fft: hydra-synth's Audio reads Meyda's loudness feature, which Icing computes from its AUDIO input ---

constexpr int kBarkBands = 24; // Meyda's numberOfBarkBands
constexpr int kFftSize = 512;  // Meyda's default bufferSize, the one hydra-synth's analyser uses

/** Meyda's loudness (extractors/loudness.ts with createBarkScale) of `amp`, the n = bufferSize / 2 bins of |FFT|:
the specific loudness of 24 bands of equal width in Bark; returns the total. Float32 where Meyda stores Float32. */
inline float loudness(const float* amp, int n, float sampleRate, float* specific) {
	std::vector<float> bark(n);
	for (int i = 0; i < n; i++) {
		double f = (double)i * sampleRate / (2 * n);
		bark[i] = (float)(13 * std::atan(f / 1315.8) + 3.5 * std::atan(std::pow(f / 7518, 2)));
	}
	int limits[kBarkBands + 1] = {};
	double bandEnd = bark[n - 1] / (double)kBarkBands;
	int band = 1;
	for (int i = 0; i < n; i++)
		while (bark[i] > bandEnd && band < kBarkBands) { // Meyda's Int32Array drops writes past its end
			limits[band++] = i;
			bandEnd = band * (double)bark[n - 1] / kBarkBands;
		}
	limits[kBarkBands] = n - 1;
	double total = 0;
	for (int b = 0; b < kBarkBands; b++) {
		double sum = 0;
		for (int j = limits[b]; j < limits[b + 1]; j++)
			sum += amp[j];
		specific[b] = (float)std::pow(sum, 0.23);
		total += specific[b];
	}
	return (float)total;
}

/** Icing's AUDIO samples (UI thread, drained from the audio thread's ring) -> Meyda's loudness of the last
kFftSize. */
class Analyser {
  public:
	void push(float x) { history_[pos_++ % kFftSize] = x; }
	/** Meyda's Hann window, |FFT| (rack::dsp::RealFFT, hence in hydra.cpp: the tests don't link pffft), loudness();
	returns the total. */
	float analyse(float sampleRate, float* specific) const;

  private:
	float history_[kFftSize] = {};
	unsigned pos_ = 0;
};

// --- Frames ---

/** Icing's UI thread <-> its Filo's JS thread, which owns Hydra (plan item 8). Neither side waits for the other:
each slot holds only the latest value, behind a mutex held for copies. The UI posts a request per frame; the JS
thread wakes up for it, evaluates the outputs' uniforms (H(), a.fft, ...) and publishes them; the UI renders the
latest published frame (one frame behind). Shaders and source images travel only when they change, as hydra-synth's
source. */
class Exchange {
  public:
	static constexpr int kOutputs = 4; // o0-o3
	static constexpr int kSources = 4; // s0-s3
	struct Request {
		double time = 0; // s, Icing's clock
		float specific[kBarkBands] = {};
		float total = 0;
	};
	struct Program {
		uint32_t id = 0;                   // 0: none
		std::string frag;                  // GLSL ES, as hydra-synth generated it
		std::vector<std::string> uniforms; // the ones a Frame sets, in its order
		// Which are sampler2D: the value is an output (0-3: its other buffer) or kOutputs + a source (4-7); -1: none.
		std::vector<bool> samplers;
	};
	/** initImage()'s image, as a local file the UI thread loads. */
	struct Source {
		uint32_t id = 0;  // 0: none
		std::string path; // the disk cache's file, or res/'s, or the samples folder's; "": none (clear())
		std::string url;  // as the code gave it, for messages
	};
	struct Frame {
		uint64_t seq = 0; // 0: none
		int display = 0;  // -1: all four (render()), 0-3: that output (render(oN))
		int width = 1280, height = 720;
		float time = 0;                  // Hydra's time uniform
		uint32_t program[kOutputs] = {}; // what each output's values are for (0: it has none)
		std::vector<float> values[kOutputs];
	};

	// --- UI thread ---
	void request(const Request& r) {
		std::lock_guard<std::mutex> lock(m_);
		request_ = r;
		pending_ = true;
		cv_.notify_one();
	}
	/** The latest frame, if newer than `after`. */
	bool frame(uint64_t after, Frame& out) const {
		std::lock_guard<std::mutex> lock(m_);
		if (frame_.seq <= after)
			return false;
		out = frame_;
		return true;
	}
	/** Output `output`'s program, if it is not `have` (id 0: it has none any more). */
	bool program(int output, uint32_t have, Program& out) const {
		std::lock_guard<std::mutex> lock(m_);
		if (programs_[output].id == have)
			return false;
		out = programs_[output];
		return true;
	}
	/** Source `source`'s image, if it is not `have` (id 0: it has none any more). */
	bool source(int source, uint32_t have, Source& out) const {
		std::lock_guard<std::mutex> lock(m_);
		if (sources_[source].id == have)
			return false;
		out = sources_[source];
		return true;
	}

	// --- JS thread ---
	/** Takes the pending request, waiting for one until `until` at most. */
	bool wait(std::chrono::steady_clock::time_point until, Request& r) {
		std::unique_lock<std::mutex> lock(m_);
		cv_.wait_until(lock, until, [&] { return pending_; });
		if (!pending_)
			return false;
		r = request_;
		pending_ = false;
		return true;
	}
	/** Returns the program's id: a new one for every program, also across clear(). */
	uint32_t setProgram(int output, Program p) {
		std::lock_guard<std::mutex> lock(m_);
		p.id = ++lastId_;
		programs_[output] = std::move(p);
		return lastId_;
	}
	/** As setProgram(), from the same ids. */
	uint32_t setSource(int source, Source s) {
		std::lock_guard<std::mutex> lock(m_);
		s.id = ++lastId_;
		sources_[source] = std::move(s);
		return lastId_;
	}
	void publish(Frame f) {
		std::lock_guard<std::mutex> lock(m_);
		f.seq = ++seq_;
		frame_ = std::move(f);
	}
	/** A new JS runtime: no programs, no images, no frame. */
	void clear() {
		std::lock_guard<std::mutex> lock(m_);
		for (Program& p : programs_)
			p = Program();
		for (Source& s : sources_)
			s = Source();
		frame_ = Frame();
		pending_ = false;
	}

  private:
	mutable std::mutex m_;
	std::condition_variable cv_;
	Request request_;
	bool pending_ = false;
	Program programs_[kOutputs];
	Source sources_[kSources];
	Frame frame_;
	uint32_t lastId_ = 0;
	uint64_t seq_ = 0;
};

// --- GL (hydra.cpp) ---

/** Output window geometry in screen coordinates, as saved in the patch. */
struct WindowGeometry {
	int x = 100, y = 100, w = 640, h = 360;
	bool fullscreen = false;
};

/** Hydra's outputs as GL objects in Rack's context: two FBOs per output (ping-pong: src(o0) reads the other one,
hydra-synth's getTexture()), a texture per source image, and the displayed image, one output or the 2x2 grid. UI
thread, Rack's context current.
*/
class Renderer {
  public:
	using Error = std::function<void(const std::string& line)>;
	/** Compiles the programs and loads the source images that changed (failures go to `error`) and renders the
	exchange's frame if it is new. Restores the GL state it touches. */
	void update(NVGcontext* vg, const Exchange& x, const Error& error);
	void drawPreview(NVGcontext* vg, float x, float y, float w, float h);
	/** Draws the displayed image into the bound framebuffer (Window's, in its context: programs are shared). */
	void present() const;
	/** Black, no programs, no images: for another Filo (or none) from here on. */
	void reset();
	/** Deletes the GL objects; the next update() recreates them (e.g. in a new context). */
	void destroy();

  private:
	struct Program {
		uint32_t id = 0;
		unsigned gl = 0; // 0: none, or it did not compile
		int time = -1, resolution = -1, prevBuffer = -1;
		std::vector<int> locations; // of Exchange::Program::uniforms
		std::vector<bool> samplers;
	};
	struct Source {
		uint32_t id = 0;
		std::string path;     // the file `texture` holds
		unsigned texture = 0; // 0: none
	};
	void compile(int output, const Exchange::Program& p, const Error& error);
	void load(int source, const Exchange::Source& s, const Error& error);
	void render(NVGcontext* vg, const Exchange::Frame& f);
	void clearImages();
	unsigned other(int output) const; // the buffer src(oN) reads
	NVGLUframebuffer* fbo_[Exchange::kOutputs][2] = {};
	NVGLUframebuffer* display_ = nullptr; // display_->image: NanoVG preview; display_->texture: sampled by Window
	int ping_[Exchange::kOutputs] = {};   // which of fbo_[i] holds output i's latest frame
	Program programs_[Exchange::kOutputs];
	Source sources_[Exchange::kSources];
	unsigned none_ = 0; // what a source without image samples: hydra-synth's regl.texture({shape: [1, 1]}), 0 0 0 0
	unsigned one_ = 0, all_ = 0; // hydra-synth's renderFbo (render(oN)) and renderAll (render()) passes
	unsigned blit_ = 0;          // present()
	int w_ = 0, h_ = 0;
	uint64_t seq_ = 0; // the last frame rendered
	bool failed_ = false;
};

/** Separate output window. Its context shares Rack's, but FBOs aren't shared: it only samples Renderer's texture. */
struct Window {
	GLFWwindow* win = nullptr;
	bool fullscreen = false; // applied state; WindowGeometry::fullscreen is the wanted one

	/** Reconciles the window with `open`/`g` and presents `image` (null: black). Title-bar close and Esc write back
	into them. Must run outside GLFW callbacks (step()), so menus and keys only flip flags. Leaves `rack`'s context
	current.
	*/
	void update(GLFWwindow* rack, const Renderer* image, WindowGeometry& g, bool& open);
	/** Hides the window for collectWindows() to destroy: safe inside GLFW callbacks, where Rack deletes widgets. */
	void destroy();
};

/** Destroys the windows Window::destroy() hid. Call outside GLFW callbacks (step()). */
void collectWindows();

} // namespace hydra
