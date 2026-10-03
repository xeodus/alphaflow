// Unit tests for alphaflow::rfq::Responder.
//
// The responder is pure: a request and the current snapshot state map to a
// response. Keeping it free of sockets means the response contract of
// ARCHITECTURE.md §10.1 is tested exhaustively and cheaply.

#include <alphaflow/rfq/responder.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {

using alphaflow::platform::Nanos;

/// A minimal snapshot for exercising the responder without the real curve.
struct StubSnapshot {
    std::uint32_t generation{0};
    Nanos published_at_ns{0};
    double par_rate{0.0};
};

using alphaflow::rfq::InstrumentUniverse;
using alphaflow::rfq::Request;
using alphaflow::rfq::Responder;
using alphaflow::rfq::Response;
using alphaflow::rfq::Status;

constexpr Nanos kStalenessThresholdNs = 1'000'000;  // 1 ms

double stub_price(std::uint32_t instrument_id, const StubSnapshot& snapshot) noexcept {
    return snapshot.par_rate * (1.0 + static_cast<double>(instrument_id));
}

Responder<StubSnapshot> make_responder() {
    return Responder<StubSnapshot>(InstrumentUniverse{3}, kStalenessThresholdNs, &stub_price);
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
    const StubSnapshot snapshot{5, 0, 0.02};

    const Response response = responder.respond(Request{2, 99}, &snapshot, 0);

    REQUIRE(response.request_id == 2);
    REQUIRE(response.status == Status::UnknownInstrument);
    REQUIRE(response.generation == 5);  // still reports which curve it saw
}

TEST_CASE("responder quotes a fresh snapshot as OK", "[rfq][responder]") {
    const auto responder = make_responder();
    const StubSnapshot snapshot{7, 1'000, 0.02};

    const Response response = responder.respond(Request{3, 1}, &snapshot, 1'500);

    REQUIRE(response.status == Status::Ok);
    REQUIRE(response.request_id == 3);
    REQUIRE(response.generation == 7);
    REQUIRE(response.price == 0.04);  // par_rate * (1 + id)
}

TEST_CASE("responder labels a quote stale once past the threshold",
          "[rfq][responder]") {
    const auto responder = make_responder();
    const StubSnapshot snapshot{7, 1'000, 0.02};

    const Response response =
        responder.respond(Request{4, 1}, &snapshot, 1'000 + kStalenessThresholdNs + 1);

    REQUIRE(response.status == Status::Stale);
    REQUIRE(response.price == 0.04);  // a stale quote is still a quote
}

TEST_CASE("responder treats the threshold boundary as fresh", "[rfq][responder]") {
    const auto responder = make_responder();
    const StubSnapshot snapshot{7, 1'000, 0.02};

    const Response response =
        responder.respond(Request{5, 1}, &snapshot, 1'000 + kStalenessThresholdNs);

    REQUIRE(response.status == Status::Ok);
}
