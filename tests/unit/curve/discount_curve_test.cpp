// Unit tests for alphaflow::curve::DiscountCurve.
//
// The curve is log-linear in discount factors, which gives piecewise-constant
// forward rates and local support -- the property that makes incremental
// rebuild correct. These tests pin exactness at pillars, the interpolation
// between them, the forward rate, extrapolation, and local support.

#include <alphaflow/curve/discount_curve.hpp>

#include <alphaflow/core/date.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace {

using alphaflow::core::Date;
using alphaflow::curve::DiscountCurve;

DiscountCurve make_curve() {
    DiscountCurve curve;
    curve.reference = Date{2024, 1, 1};
    curve.count = 3;
    curve.times = {0.0, 1.0, 2.0};
    curve.log_dfs = {0.0, -0.05, -0.08};  // forwards 5% then 3%
    return curve;
}

}  // namespace

TEST_CASE("the curve is exact at its pillars", "[curve][discount]") {
    const DiscountCurve curve = make_curve();

    REQUIRE(curve.discount(0.0) == Catch::Approx(1.0).epsilon(1e-12));
    REQUIRE(curve.discount(1.0) == Catch::Approx(std::exp(-0.05)).epsilon(1e-12));
    REQUIRE(curve.discount(2.0) == Catch::Approx(std::exp(-0.08)).epsilon(1e-12));
    REQUIRE(curve.count == 3);
}

TEST_CASE("log-linear interpolation is geometric between pillars", "[curve][discount]") {
    const DiscountCurve curve = make_curve();

    REQUIRE(curve.discount(0.5) == Catch::Approx(std::exp(-0.025)).epsilon(1e-12));
    REQUIRE(curve.discount(1.5) == Catch::Approx(std::exp(-0.065)).epsilon(1e-12));
}

TEST_CASE("zero rates and forwards follow the segments", "[curve][discount]") {
    const DiscountCurve curve = make_curve();

    REQUIRE(curve.zero_rate(1.0) == Catch::Approx(0.05).epsilon(1e-12));
    REQUIRE(curve.zero_rate(2.0) == Catch::Approx(0.04).epsilon(1e-12));

    // Piecewise-constant forward rates: 5% then 3%.
    REQUIRE(curve.forward(0.0, 1.0) == Catch::Approx(0.05).epsilon(1e-12));
    REQUIRE(curve.forward(1.0, 2.0) == Catch::Approx(0.03).epsilon(1e-12));
}

TEST_CASE("discount factors are positive and decreasing", "[curve][discount]") {
    const DiscountCurve curve = make_curve();

    for (double t = 0.0; t <= 2.0; t += 0.1) {
        REQUIRE(curve.discount(t) > 0.0);
    }
    REQUIRE(curve.discount(0.5) > curve.discount(1.0));
    REQUIRE(curve.discount(1.0) > curve.discount(1.5));
}

TEST_CASE("interpolation has local support", "[curve][discount]") {
    DiscountCurve curve = make_curve();
    const double before_low = curve.discount(0.5);   // inside [t0, t1]
    const double before_high = curve.discount(1.5);  // inside [t1, t2]

    // Move the last pillar: [t1, t2] changes, [t0, t1] does not.
    curve.log_dfs[2] = -0.20;

    REQUIRE(curve.discount(0.5) == Catch::Approx(before_low).epsilon(1e-12));
    REQUIRE(curve.discount(1.5) != Catch::Approx(before_high).epsilon(1e-6));
}

TEST_CASE("extrapolation extends the boundary segment", "[curve][discount]") {
    const DiscountCurve curve = make_curve();

    // Beyond the last pillar the final segment's slope continues (3%).
    REQUIRE(curve.discount(3.0) == Catch::Approx(std::exp(-0.11)).epsilon(1e-12));
}

TEST_CASE("a date maps to a year fraction on the curve's reference", "[curve][discount]") {
    const DiscountCurve curve = make_curve();

    REQUIRE(curve.discount(Date{2024, 1, 1}) == Catch::Approx(1.0).epsilon(1e-12));
    // 2024 is a leap year: 366 days from the reference.
    const double t = 366.0 / 365.0;
    REQUIRE(curve.discount(Date{2025, 1, 1}) == Catch::Approx(curve.discount(t)).epsilon(1e-12));
}
