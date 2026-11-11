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
