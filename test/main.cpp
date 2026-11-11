#include "check.hpp"

int main() {
	for (const check::Test& t : check::registry()) {
		int before = check::failures();
		t.fn();
		std::printf("%s %s\n", check::failures() == before ? "ok  " : "FAIL", t.name);
	}
	std::printf("%zu tests, %d failures\n", check::registry().size(), check::failures());
	return check::failures() ? 1 : 0;
}
