// 0.1.4's "first trivial test": proves the harness runs green and can fail
// red. Real engine tests start with core at 0.2.1 (handles, math, ...).
#include <doctest/doctest.h>

TEST_CASE("harness: doctest executes and evaluates checks") {
    CHECK(2 + 2 == 4);
    CHECK(1 + 1 != 3);
}
