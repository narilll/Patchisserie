#include "check.hpp"
#include "hydra.hpp"

#include <algorithm>

using hydra::glslEsToDesktop;

static const std::string kHeader = hydra::kDesktopHeader;

static size_t lines(const std::string& s) {
	return std::count(s.begin(), s.end(), '\n');
}

TEST(glsl_header_first_with_empty_precision_defines) {
	std::string out = glslEsToDesktop("void main () {}\n");
	CHECK(out.compare(0, 13, "#version 120\n") == 0);
	CHECK(out.find("#define lowp\n") != std::string::npos);
	CHECK(out.find("#define mediump\n") != std::string::npos);
	CHECK(out.find("#define highp\n") != std::string::npos);
}

TEST(glsl_precision_lines_blanked_keeping_line_numbers) {
	std::string src = "precision mediump float;\n"
					  "  \tprecision highp int;\n"
					  "uniform float time;\n"
					  "    precision lowp sampler2D;\n";
	std::string out = glslEsToDesktop(src);
	CHECK(out.find("precision") == std::string::npos);
	CHECK(out == kHeader + "\n  \t\nuniform float time;\n    \n");
	CHECK(lines(out) == lines(kHeader) + lines(src)); // compile error line N -> source line N - 4
}

TEST(glsl_other_lines_unchanged) {
	std::string src = "// precision mediump float;\n"
					  "float precisionX = 1.0;\n"
					  "uniform highp vec2 resolution;\n"
					  "void main () { gl_FragColor = vec4(1.0); }"; // no trailing newline
	CHECK(glslEsToDesktop(src) == kHeader + src);
}

TEST(glsl_precision_statement_sharing_a_line) {
	CHECK(glslEsToDesktop("precision mediump float; uniform float time;\n") == kHeader + " uniform float time;\n");
}

TEST(glsl_conversion_idempotent) {
	std::string once = glslEsToDesktop("precision mediump float;\nvoid main () {}\n");
	CHECK(glslEsToDesktop(once) == once);
	CHECK(glslEsToDesktop("") == kHeader);
}

// --- a.fft: Meyda's loudness ---

// Meyda 5.6.3 (hydra-synth's), node: amp[i] = 1 + (i % 7) * 0.5 for 256 bins, Meyda.extract() with bufferSize 512 for
// its barkScale, then featureExtractors.loudness({ ampSpectrum, barkScale }).
TEST(loudness_matches_meyda) {
	float amp[256];
	for (int i = 0; i < 256; i++)
		amp[i] = 1 + (i % 7) * 0.5f;
	struct {
		float sr;
		double specific[hydra::kBarkBands], total;
	} cases[] = {
		{48000,
		 {1.23459983, 1.17283499, 1.23459983, 1.28747225, 1.33393800, 1.44798183, 1.09774375, 1.41331470,
		  1.53804862, 1.44798183, 1.33393800, 1.65758491, 1.53804862, 1.73588252, 1.73588252, 1.86423647,
		  1.99175954, 2.07705784, 2.07705784, 2.24264383, 2.31555963, 2.51352620, 2.87737250, 3.55569959},
		 42.7247656584},
		{44100,
		 {1.23459983, 1.17283499, 1.23459983, 1.28747225, 1.58951306, 1.00000000, 1.33393800, 1.48007393,
		  1.58951306, 1.23459983, 1.41331470, 1.71740818, 1.56448913, 1.75372100, 1.78767800, 1.93151796,
		  2.02516699, 2.03590369, 2.19476962, 2.20299339, 2.35586858, 2.53426027, 2.87737250, 3.49647760},
		 43.0480864048},
	};
	for (auto& c : cases) {
		float specific[hydra::kBarkBands];
		float total = hydra::loudness(amp, 256, c.sr, specific);
		for (int b = 0; b < hydra::kBarkBands; b++)
			CHECK_NEAR(specific[b], c.specific[b], 1e-6);
		CHECK_NEAR(total, c.total, 1e-5);
	}
}

TEST(loudness_of_silence_is_zero) {
	float amp[256] = {}, specific[hydra::kBarkBands];
	CHECK(hydra::loudness(amp, 256, 48000, specific) == 0);
	for (float s : specific)
		CHECK(s == 0);
}

// --- Frame exchange ---

using hydra::Exchange;

TEST(exchange_request_latest_wins_and_wakes) {
	Exchange x;
	Exchange::Request r;
	auto soon = [] { return std::chrono::steady_clock::now() + std::chrono::milliseconds(1); };
	CHECK(!x.wait(soon(), r)); // nothing pending: times out
	for (int i = 1; i <= 3; i++) {
		Exchange::Request q;
		q.time = i;
		q.specific[23] = (float)i;
		x.request(q);
	}
	CHECK(x.wait(soon(), r));
	CHECK(r.time == 3 && r.specific[23] == 3);
	CHECK(!x.wait(soon(), r)); // taken
}

TEST(exchange_frames_are_new_once) {
	Exchange x;
	Exchange::Frame f;
	CHECK(!x.frame(0, f)); // none yet
	Exchange::Frame g;
	g.display = 2;
	g.values[1] = {1, 2};
	x.publish(g);
	CHECK(x.frame(0, f));
	CHECK(f.seq == 1 && f.display == 2 && f.values[1].size() == 2);
	CHECK(!x.frame(f.seq, f));
	x.publish(g);
	x.publish(g);
	CHECK(x.frame(1, f) && f.seq == 3); // the latest only
	x.clear();                          // a new JS runtime: none, and the next one is still newer
	CHECK(!x.frame(0, f));
	x.publish(g);
	CHECK(x.frame(3, f) && f.seq == 4);
}

TEST(exchange_programs_change_ids_and_clear) {
	Exchange x;
	Exchange::Program p, got;
	CHECK(!x.program(0, 0, got)); // none, as the UI has
	p.frag = "a";
	uint32_t a = x.setProgram(0, p);
	CHECK(a != 0);
	CHECK(x.program(0, 0, got) && got.id == a && got.frag == "a");
	CHECK(!x.program(0, a, got)); // compiled already
	CHECK(!x.program(1, 0, got));
	p.frag = "b";
	uint32_t b = x.setProgram(0, p);
	CHECK(b != a && x.program(0, a, got) && got.frag == "b");
	x.clear();
	CHECK(x.program(0, b, got) && got.id == 0 && got.frag.empty()); // the UI drops it
	CHECK(x.setProgram(0, p) > b);                                  // never an id the UI may still hold
}

TEST(exchange_sources_change_ids_and_clear) {
	Exchange x;
	Exchange::Source s, got;
	CHECK(!x.source(0, 0, got)); // none, as the UI has
	s.path = "/cache/example.com/a.png";
	s.url = "https://example.com/a.png";
	uint32_t a = x.setSource(0, s);
	CHECK(x.source(0, 0, got) && got.id == a && got.path == s.path && got.url == s.url);
	CHECK(!x.source(0, a, got)); // loaded already
	CHECK(!x.source(1, 0, got));
	Exchange::Program p;
	CHECK(x.setProgram(0, p) > a); // one id sequence: never a source's
	uint32_t b = x.setSource(0, Exchange::Source());
	CHECK(x.source(0, a, got) && got.id == b && got.path.empty()); // clear(): the UI drops the image
	x.clear();                                                     // a new JS runtime
	CHECK(x.source(0, b, got) && got.id == 0 && got.path.empty());
	CHECK(x.setSource(0, s) > b);
}
