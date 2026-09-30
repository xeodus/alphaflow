#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace alphaflow::rfq {

/// Outcome of an RFQ. Mirrors the response contract of ARCHITECTURE.md §10.1.
/// The numeric values are part of the wire format; do not renumber.
enum class Status : std::uint8_t {
    Ok = 0,
    Stale = 1,
    WarmingUp = 2,
    UnknownInstrument = 3,
    Overloaded = 4,
};

struct Request {
    std::uint32_t request_id{};
    std::uint32_t instrument_id{};

    bool operator==(const Request&) const = default;
};

struct Response {
    std::uint32_t request_id{};
    Status status{Status::Ok};
    std::uint32_t generation{};  // snapshot generation the price came from
    double price{};

    bool operator==(const Response&) const = default;
};

namespace protocol {

/// Protocol version. A message with a different version is rejected.
inline constexpr std::uint8_t kVersion = 1;

/// Bytes of the little-endian length prefix that precedes every payload.
inline constexpr std::size_t kHeaderBytes = 4;

/// Largest payload the decoder will buffer. A declared length beyond this is
/// a protocol error; this bounds memory against a garbled or hostile length.
inline constexpr std::size_t kMaxPayloadBytes = 4096;

/// Encode a message payload into `out`. Returns the number of bytes written,
/// or std::nullopt if `out` is too small. Never allocates.
[[nodiscard]] std::optional<std::size_t> encode(const Request& request,
                                                std::span<std::byte> out) noexcept;
[[nodiscard]] std::optional<std::size_t> encode(const Response& response,
                                                std::span<std::byte> out) noexcept;

/// Decode a payload. Returns std::nullopt if it is malformed, of the wrong
/// kind, of an unknown version, or carries an out-of-range status.
[[nodiscard]] std::optional<Request> decode_request(std::span<const std::byte> payload) noexcept;
[[nodiscard]] std::optional<Response> decode_response(std::span<const std::byte> payload) noexcept;

/// Prefix `payload` with its little-endian length. Returns bytes written, or
/// std::nullopt if `out` is too small. Never allocates.
[[nodiscard]] std::optional<std::size_t> frame(std::span<const std::byte> payload,
                                               std::span<std::byte> out) noexcept;

/// Incremental decoder for a byte stream: feed whatever a socket hands you,
/// extract complete payloads. Fixed capacity, no allocation, bounded.
class FrameDecoder {
public:
    enum class Result {
        Frame,     ///< `out` views a complete payload (valid until the next feed/next)
        NeedMore,  ///< not enough bytes for a whole frame yet
        Error,     ///< a frame declared a length beyond kMaxPayloadBytes
    };

    /// Append bytes received from the socket. Returns false if they cannot be
    /// buffered (a full buffer with no complete frame, i.e. protocol overrun).
    [[nodiscard]] bool feed(std::span<const std::byte> bytes) noexcept;

    /// Extract the next complete payload. `out` points into the decoder's
    /// buffer and is valid only until the next call to feed() or next().
    [[nodiscard]] Result next(std::span<const std::byte>& out) noexcept;

    [[nodiscard]] std::size_t buffered() const noexcept { return size_ - read_; }

private:
    static constexpr std::size_t kCapacity = kMaxPayloadBytes + kHeaderBytes + 64;

    void compact() noexcept;

    std::array<std::byte, kCapacity> buffer_{};
    std::size_t read_{0};
    std::size_t size_{0};
    bool error_{false};
};

}  // namespace protocol
}  // namespace alphaflow::rfq
