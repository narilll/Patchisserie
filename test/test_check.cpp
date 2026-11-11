#include "check.hpp"

// Guards the runner itself: if CHECK_NEAR breaks, every numeric test silently lies.
TEST(check_near_tolerates_float_error) {
	CHECK_NEAR(0.1 + 0.2, 0.3, 1e-12);
	CHECK(!(std::fabs(0.1 - 0.2) <= 1e-12));
}
