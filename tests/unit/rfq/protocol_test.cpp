// Unit tests for alphaflow::rfq::protocol.
//
// The wire protocol is a length-prefixed, little-endian, versioned binary
// format. It is pure and deterministic, so unlike the concurrency primitives
// every property here is directly testable.
//
// Covered: request/response round-trips (including exact double bits), the
// on-the-wire byte layout, rejection of malformed payloads, framing, a frame
// split across many small reads, several frames in one read, partial headers,
// and rejection of a frame whose declared length exceeds the maximum.

#include <alphaflow/rfq/protocol.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

using alphaflow::rfq::protocol::FrameDecoder;
using alphaflow::rfq::Request;
using alphaflow::rfq::Response;
using alphaflow::rfq::Status;

std::array<std::byte, 4> le32(std::uint32_t value) {
    return {static_cast<std::byte>(value & 0xFFU),
            static_cast<std::byte>((value >> 8U) & 0xFFU),
            static_cast<std::byte>((value >> 16U) & 0xFFU),
            static_cast<std::byte>((value >> 24U) & 0xFFU)};
}

}  // namespace

TEST_CASE("a request round-trips", "[rfq][protocol]") {
    const Request original{7, 42};

    std::array<std::byte, 32> buffer{};
    const auto written = alphaflow::rfq::protocol::encode(original, buffer);
    REQUIRE(written.has_value());
    REQUIRE(*written == 10);

    const auto decoded =
        alphaflow::rfq::protocol::decode_request(std::span(buffer.data(), *written));
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == original);
}

TEST_CASE("a response round-trips exactly, including the double bits",
          "[rfq][protocol]") {
    const Response original{9, Status::Stale, 12345, 3.141592653589793};

    std::array<std::byte, 64> buffer{};
    const auto written = alphaflow::rfq::protocol::encode(original, buffer);
    REQUIRE(written.has_value());
    REQUIRE(*written == 19);

    const auto decoded =
        alphaflow::rfq::protocol::decode_response(std::span(buffer.data(), *written));
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == original);
}

TEST_CASE("the request layout is little-endian kind, version, ids",
          "[rfq][protocol]") {
    std::array<std::byte, 16> buffer{};

    const auto written = alphaflow::rfq::protocol::encode(Request{0x11223344, 0x55667788}, buffer);
    REQUIRE(written.has_value());

    REQUIRE(buffer[0] == std::byte{0});        // kind: request
    REQUIRE(buffer[1] == std::byte{1});        // version
    REQUIRE(buffer[2] == std::byte{0x44});     // request_id, least significant first
    REQUIRE(buffer[3] == std::byte{0x33});
    REQUIRE(buffer[6] == std::byte{0x88});     // instrument_id
    REQUIRE(buffer[9] == std::byte{0x55});
}

TEST_CASE("decoding rejects malformed payloads", "[rfq][protocol]") {
    std::array<std::byte, 32> buffer{};
    const auto written = alphaflow::rfq::protocol::encode(Request{1, 2}, buffer);
    REQUIRE(written.has_value());

    SECTION("truncated") {
        REQUIRE_FALSE(
            alphaflow::rfq::protocol::decode_request(std::span(buffer.data(), *written - 1)));
    }
    SECTION("trailing bytes") {
        buffer[*written] = std::byte{0};
        REQUIRE_FALSE(
            alphaflow::rfq::protocol::decode_request(std::span(buffer.data(), *written + 1)));
    }
    SECTION("wrong kind") {
        std::array<std::byte, 32> response_buffer{};
        const auto response_written =
            alphaflow::rfq::protocol::encode(Response{}, response_buffer);
        REQUIRE(response_written.has_value());
        REQUIRE_FALSE(alphaflow::rfq::protocol::decode_request(
            std::span(response_buffer.data(), *response_written)));
    }
    SECTION("wrong version") {
        buffer[1] = std::byte{2};
        REQUIRE_FALSE(
            alphaflow::rfq::protocol::decode_request(std::span(buffer.data(), *written)));
    }
    SECTION("out-of-range status") {
        std::array<std::byte, 32> response_buffer{};
        const auto response_written =
            alphaflow::rfq::protocol::encode(Response{}, response_buffer);
        REQUIRE(response_written.has_value());
        response_buffer[6] = std::byte{99};  // beyond Status::Overloaded
        REQUIRE_FALSE(alphaflow::rfq::protocol::decode_response(
            std::span(response_buffer.data(), *response_written)));
    }
}

TEST_CASE("encoding reports insufficient output space", "[rfq][protocol]") {
    std::array<std::byte, 4> too_small{};
    REQUIRE_FALSE(alphaflow::rfq::protocol::encode(Request{1, 2}, too_small).has_value());
    REQUIRE_FALSE(alphaflow::rfq::protocol::encode(Response{}, too_small).has_value());
}

TEST_CASE("a framed payload round-trips through the decoder", "[rfq][protocol]") {
    std::array<std::byte, 32> payload{};
    const auto payload_written = alphaflow::rfq::protocol::encode(Request{5, 6}, payload);
    REQUIRE(payload_written.has_value());

    std::array<std::byte, 64> wire{};
    const auto frame_written =
        alphaflow::rfq::protocol::frame(std::span(payload.data(), *payload_written), wire);
    REQUIRE(frame_written.has_value());
    REQUIRE(*frame_written == 4 + *payload_written);

    FrameDecoder decoder;
    REQUIRE(decoder.feed(std::span(wire.data(), *frame_written)));

    std::span<const std::byte> out;
    REQUIRE(decoder.next(out) == FrameDecoder::Result::Frame);
    REQUIRE(alphaflow::rfq::protocol::decode_request(out) == Request{5, 6});

    REQUIRE(decoder.next(out) == FrameDecoder::Result::NeedMore);
}

TEST_CASE("a frame split across single-byte reads reassembles", "[rfq][protocol]") {
    std::array<std::byte, 32> payload{};
    const auto payload_written = alphaflow::rfq::protocol::encode(Request{8, 9}, payload);
    REQUIRE(payload_written.has_value());

    std::array<std::byte, 64> wire{};
    const auto frame_written =
        alphaflow::rfq::protocol::frame(std::span(payload.data(), *payload_written), wire);
    REQUIRE(frame_written.has_value());

    FrameDecoder decoder;
    std::span<const std::byte> out;

    for (std::size_t i = 0; i + 1 < *frame_written; ++i) {
        REQUIRE(decoder.feed(std::span(&wire[i], 1)));
        REQUIRE(decoder.next(out) == FrameDecoder::Result::NeedMore);
    }
    REQUIRE(decoder.feed(std::span(&wire[*frame_written - 1], 1)));
    REQUIRE(decoder.next(out) == FrameDecoder::Result::Frame);
    REQUIRE(alphaflow::rfq::protocol::decode_request(out) == Request{8, 9});
}

TEST_CASE("two frames arriving together are both delivered", "[rfq][protocol]") {
    std::array<std::byte, 32> payload_a{};
    std::array<std::byte, 32> payload_b{};
    const auto written_a = alphaflow::rfq::protocol::encode(Request{1, 1}, payload_a);
    const auto written_b = alphaflow::rfq::protocol::encode(Request{2, 2}, payload_b);
    REQUIRE(written_a.has_value());
    REQUIRE(written_b.has_value());

    std::array<std::byte, 128> wire{};
    auto frame_a = alphaflow::rfq::protocol::frame(std::span(payload_a.data(), *written_a), wire);
    REQUIRE(frame_a.has_value());
    auto frame_b = alphaflow::rfq::protocol::frame(
        std::span(payload_b.data(), *written_b),
        std::span(wire.data() + *frame_a, wire.size() - *frame_a));
    REQUIRE(frame_b.has_value());

    FrameDecoder decoder;
    REQUIRE(decoder.feed(std::span(wire.data(), *frame_a + *frame_b)));

    std::span<const std::byte> out;
    REQUIRE(decoder.next(out) == FrameDecoder::Result::Frame);
    REQUIRE(alphaflow::rfq::protocol::decode_request(out) == Request{1, 1});
    REQUIRE(decoder.next(out) == FrameDecoder::Result::Frame);
    REQUIRE(alphaflow::rfq::protocol::decode_request(out) == Request{2, 2});
    REQUIRE(decoder.next(out) == FrameDecoder::Result::NeedMore);
}

TEST_CASE("a partial header yields NeedMore", "[rfq][protocol]") {
    FrameDecoder decoder;
    const std::array<std::byte, 2> partial{std::byte{0x01}, std::byte{0x00}};
    REQUIRE(decoder.feed(partial));

    std::span<const std::byte> out;
    REQUIRE(decoder.next(out) == FrameDecoder::Result::NeedMore);
}

TEST_CASE("a frame declaring an excessive length is rejected before buffering",
          "[rfq][protocol]") {
    FrameDecoder decoder;
    const auto header = le32(5000);  // exceeds kMaxPayloadBytes (4096)
    REQUIRE(decoder.feed(header));

    std::span<const std::byte> out;
    REQUIRE(decoder.next(out) == FrameDecoder::Result::Error);
}
