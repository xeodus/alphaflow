// Unit tests for alphaflow::rfq::Responder.
//
// The responder is pure: a request and the current snapshot state map to a
// response. Keeping it free of sockets means the response contract of
// ARCHITECTURE.md §10.1 is tested exhaustively and cheaply.

#include <alphaflow/rfq/responder.hpp>

#include <alphaflow/curve/curve_snapshot.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {

using alphaflow::curve::CurveSnapshot;
using alphaflow::platform::Nanos;
using alphaflow::rfq::InstrumentUniverse;
using alphaflow::rfq::Request;
using alphaflow::rfq::Responder;
using alphaflow::rfq::Response;
using alphaflow::rfq::Status;

constexpr Nanos kStalenessThresholdNs = 1'000'000;  // 1 ms

double stub_price(std::uint32_t instrument_id, const CurveSnapshot& snapshot) noexcept {
    return snapshot.par_rate * (1.0 + static_cast<double>(instrument_id));
}

Responder<CurveSnapshot> make_responder() {
    return Responder<CurveSnapshot>(InstrumentUniverse{3}, kStalenessThresholdNs, &stub_price);
}

}  // namespace

TEST_CASE("responder reports warming up before anything is published",
          "[rfq][responder]") {
    const auto responder = make_responder();

    const Response response = responder.respond(Request{1, 0}, nullptr, 0);

    REQUIRE(response.request_id == 1);
    REQUIRE(response.status == Status::WarmingUp);
    REQUIRE(response.generation == 0);
}

TEST_CASE("responder rejects an instrument outside the universe", "[rfq][responder]") {
    const auto responder = make_responder();
    const CurveSnapshot snapshot{5, 0, 0.02};

    const Response response = responder.respond(Request{2, 99}, &snapshot, 0);

    REQUIRE(response.request_id == 2);
    REQUIRE(response.status == Status::UnknownInstrument);
    REQUIRE(response.generation == 5);  // still reports which curve it saw
}

TEST_CASE("responder quotes a fresh snapshot as OK", "[rfq][responder]") {
    const auto responder = make_responder();
    const CurveSnapshot snapshot{7, 1'000, 0.02};

    const Response response = responder.respond(Request{3, 1}, &snapshot, 1'500);

    REQUIRE(response.status == Status::Ok);
    REQUIRE(response.request_id == 3);
    REQUIRE(response.generation == 7);
    REQUIRE(response.price == 0.04);  // par_rate * (1 + id)
}

TEST_CASE("responder labels a quote stale once past the threshold",
          "[rfq][responder]") {
    const auto responder = make_responder();
    const CurveSnapshot snapshot{7, 1'000, 0.02};

    const Response response =
        responder.respond(Request{4, 1}, &snapshot, 1'000 + kStalenessThresholdNs + 1);

    REQUIRE(response.status == Status::Stale);
    REQUIRE(response.price == 0.04);  // a stale quote is still a quote
}

TEST_CASE("responder treats the threshold boundary as fresh", "[rfq][responder]") {
    const auto responder = make_responder();
    const CurveSnapshot snapshot{7, 1'000, 0.02};

    const Response response =
        responder.respond(Request{5, 1}, &snapshot, 1'000 + kStalenessThresholdNs);

    REQUIRE(response.status == Status::Ok);
}
