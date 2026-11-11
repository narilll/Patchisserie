#include "check.hpp"

#ifdef _WIN32
// Rack's common.hpp redirects fopen to libRack's UTF-8 wrapper, and the tests don't link libRack.
// ponytail: plain fopen, ASCII paths only (the tests' temp dirs); convert to UTF-16 + _wfopen if a test needs more.
extern "C" FILE* fopen_u8(const char* filename, const char* mode) {
	return std::fopen(filename, mode);
}
#endif

int main() {
	for (const check::Test& t : check::registry()) {
		int before = check::failures();
		t.fn();
		std::printf("%s %s\n", check::failures() == before ? "ok  " : "FAIL", t.name);
	}
	std::printf("%zu tests, %d failures\n", check::registry().size(), check::failures());
	return check::failures() ? 1 : 0;
}
