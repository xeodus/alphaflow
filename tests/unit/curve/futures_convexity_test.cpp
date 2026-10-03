// Unit tests for alphaflow::curve futures convexity.

#include <alphaflow/curve/futures_convexity.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

using alphaflow::curve::adjusted_futures_rate;
using alphaflow::curve::futures_convexity_adjustment;
using alphaflow::curve::futures_implied_rate;

}  // namespace

TEST_CASE("a futures price implies a rate", "[curve][futures]") {
    REQUIRE(futures_implied_rate(94.0) == Catch::Approx(0.06));
    REQUIRE(futures_implied_rate(95.5) == Catch::Approx(0.045));
}

TEST_CASE("the convexity adjustment is positive and grows with volatility",
          "[curve][futures]") {
    const double low = futures_convexity_adjustment(0.01, 1.0, 1.25);
    const double high = futures_convexity_adjustment(0.02, 1.0, 1.25);
    REQUIRE(low > 0.0);
    REQUIRE(high > low);
}

TEST_CASE("the adjusted forward exceeds the raw implied rate", "[curve][futures]") {
    const double implied = futures_implied_rate(94.0);
    const double adjusted = adjusted_futures_rate(94.0, 0.01, 1.0, 1.25);
    REQUIRE(adjusted > implied);
}
