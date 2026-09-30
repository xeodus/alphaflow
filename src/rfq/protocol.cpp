#include <alphaflow/rfq/protocol.hpp>

#include <bit>
#include <cstring>

namespace alphaflow::rfq::protocol {
namespace {

constexpr std::byte kKindRequest{0};
constexpr std::byte kKindResponse{1};

constexpr std::size_t kRequestPayloadBytes = 2 + 4 + 4;         // kind, version, ids
constexpr std::size_t kResponsePayloadBytes = 2 + 4 + 1 + 4 + 8;  // + status, generation, price

void put_u32(std::byte* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::byte>(value & 0xFFU);
    out[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    out[2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    out[3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

void put_u64(std::byte* out, std::uint64_t value) noexcept {
    for (unsigned int i = 0; i < 8U; ++i) {
        out[i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
}

std::uint32_t get_u32(const std::byte* in) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[3])) << 24U);
}

std::uint64_t get_u64(const std::byte* in) noexcept {
    std::uint64_t value = 0;
    for (unsigned int i = 0; i < 8U; ++i) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(in[i])) << (8U * i);
    }
    return value;
}

}  // namespace

std::optional<std::size_t> encode(const Request& request, std::span<std::byte> out) noexcept {
    if (out.size() < kRequestPayloadBytes) {
        return std::nullopt;
    }
    out[0] = kKindRequest;
    out[1] = static_cast<std::byte>(kVersion);
    put_u32(out.data() + 2, request.request_id);
    put_u32(out.data() + 6, request.instrument_id);
    return kRequestPayloadBytes;
}

std::optional<std::size_t> encode(const Response& response, std::span<std::byte> out) noexcept {
    if (out.size() < kResponsePayloadBytes) {
        return std::nullopt;
    }
    out[0] = kKindResponse;
    out[1] = static_cast<std::byte>(kVersion);
    put_u32(out.data() + 2, response.request_id);
    out[6] = static_cast<std::byte>(response.status);
    put_u32(out.data() + 7, response.generation);
    put_u64(out.data() + 11, std::bit_cast<std::uint64_t>(response.price));
    return kResponsePayloadBytes;
}

std::optional<Request> decode_request(std::span<const std::byte> payload) noexcept {
    if (payload.size() != kRequestPayloadBytes) {
        return std::nullopt;
    }
    if (payload[0] != kKindRequest || payload[1] != static_cast<std::byte>(kVersion)) {
        return std::nullopt;
    }
    Request request;
    request.request_id = get_u32(payload.data() + 2);
    request.instrument_id = get_u32(payload.data() + 6);
    return request;
}

std::optional<Response> decode_response(std::span<const std::byte> payload) noexcept {
    if (payload.size() != kResponsePayloadBytes) {
        return std::nullopt;
    }
    if (payload[0] != kKindResponse || payload[1] != static_cast<std::byte>(kVersion)) {
        return std::nullopt;
    }

    const auto status_value = std::to_integer<std::uint8_t>(payload[6]);
    if (status_value > static_cast<std::uint8_t>(Status::Overloaded)) {
        return std::nullopt;
    }

    Response response;
    response.request_id = get_u32(payload.data() + 2);
    response.status = static_cast<Status>(status_value);
    response.generation = get_u32(payload.data() + 7);
    response.price = std::bit_cast<double>(get_u64(payload.data() + 11));
    return response;
}

std::optional<std::size_t> frame(std::span<const std::byte> payload,
                                 std::span<std::byte> out) noexcept {
    if (payload.size() > kMaxPayloadBytes || out.size() < kHeaderBytes + payload.size()) {
        return std::nullopt;
    }
    put_u32(out.data(), static_cast<std::uint32_t>(payload.size()));
    if (!payload.empty()) {
        std::memcpy(out.data() + kHeaderBytes, payload.data(), payload.size());
    }
    return kHeaderBytes + payload.size();
}

bool FrameDecoder::feed(std::span<const std::byte> bytes) noexcept {
    if (error_) {
        return false;
    }
    if (read_ > 0 && size_ + bytes.size() > buffer_.size()) {
        compact();
    }
    if (size_ + bytes.size() > buffer_.size()) {
        error_ = true;
        return false;
    }
    if (!bytes.empty()) {
        std::memcpy(buffer_.data() + size_, bytes.data(), bytes.size());
        size_ += bytes.size();
    }
    return true;
}

FrameDecoder::Result FrameDecoder::next(std::span<const std::byte>& out) noexcept {
    if (error_) {
        return Result::Error;
    }
    if (size_ - read_ < kHeaderBytes) {
        return Result::NeedMore;
    }

    const std::uint32_t length = get_u32(buffer_.data() + read_);
    if (length > kMaxPayloadBytes) {
        error_ = true;
        return Result::Error;
    }

    const std::size_t total = kHeaderBytes + length;
    if (size_ - read_ < total) {
        return Result::NeedMore;
    }

    out = std::span<const std::byte>(buffer_.data() + read_ + kHeaderBytes, length);
    read_ += total;
    if (read_ == size_) {
        read_ = 0;
        size_ = 0;
    }
    return Result::Frame;
}

void FrameDecoder::compact() noexcept {
    if (read_ == 0) {
        return;
    }
    const std::size_t remaining = size_ - read_;
    if (remaining > 0) {
        std::memmove(buffer_.data(), buffer_.data() + read_, remaining);
    }
    size_ = remaining;
    read_ = 0;
}

}  // namespace alphaflow::rfq::protocol
