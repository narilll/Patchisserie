#pragma once
// ponytail: minimal test runner (TEST/CHECK/CHECK_NEAR). Swap for doctest if tests ever need fixtures or params.
#include <cmath>
#include <cstdio>
#include <vector>

namespace check {
struct Test {
	const char* name;
	void (*fn)();
};
inline std::vector<Test>& registry() {
	static std::vector<Test> r;
	return r;
}
inline int& failures() {
	static int f = 0;
	return f;
}
struct Reg {
	Reg(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};
inline void fail(const char* file, int line, const char* expr) {
	std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, expr);
	failures()++;
}
} // namespace check

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)
#define TEST(name)                                                                                                     \
	static void name();                                                                                                \
	static check::Reg CHECK_CAT(reg_, name)(#name, name);                                                              \
	static void name()
#define CHECK(expr) ((expr) ? (void)0 : check::fail(__FILE__, __LINE__, #expr))
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((double)(a) - (double)(b)) <= (double)(eps))
