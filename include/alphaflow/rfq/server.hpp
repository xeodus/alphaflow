#pragma once

#include <alphaflow/concurrency/snapshot_ptr.hpp>
#include <alphaflow/metrics/histogram.hpp>
#include <alphaflow/platform/clock.hpp>
#include <alphaflow/platform/socket.hpp>
#include <alphaflow/rfq/protocol.hpp>
#include <alphaflow/rfq/responder.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace alphaflow::rfq {

/// Serves RFQ requests on one connection until the peer closes.
///
/// Generic over the snapshot type, so the curve implementation can change
/// without touching the transport. For each request it leases the current
/// snapshot, asks the responder for a quote, frames it, and sends it. It
/// records the socket-to-socket span -- request bytes read off the socket to
/// response bytes handed to the socket -- into the supplied recorder
/// (ARCHITECTURE.md §11 / ADR-012).
///
/// Nothing here allocates or locks; buffers are fixed and the snapshot lease is
/// an RAII handle.
template <typename Snapshot, std::size_t SnapshotDepth,
          typename Recorder = metrics::LatencyRecorder>
class Server {
public:
    Server(concurrency::SnapshotPool<Snapshot, SnapshotDepth>& pool,
           const Responder<Snapshot>& responder,
           Recorder& latency) noexcept
        : pool_(pool), responder_(responder), latency_(latency) {}

    /// Blocking. Returns when the peer closes or a protocol error occurs.
    void serve(platform::Socket& connection) noexcept {
        protocol::FrameDecoder decoder;
        std::array<std::byte, 512> input{};
        std::array<std::byte, 256> payload{};
        std::array<std::byte, 512> wire{};

        for (;;) {
            const auto received = connection.recv_some(input.data(), input.size());
            if (!received || *received == 0) {
                return;  // peer closed or socket error
            }

            // The span starts when the request bytes are read off the socket.
            const platform::Nanos read_time = platform::Clock::now_ns();

            if (!decoder.feed(std::span(input.data(), *received))) {
                return;  // protocol overrun
            }

            for (;;) {
                std::span<const std::byte> frame;
                const auto result = decoder.next(frame);
                if (result == protocol::FrameDecoder::Result::NeedMore) {
                    break;
                }
                if (result == protocol::FrameDecoder::Result::Error) {
                    return;
                }

                const auto request = protocol::decode_request(frame);
                if (!request) {
                    continue;  // ignore a malformed request, keep the connection
                }

                auto lease = pool_.acquire();
                const Response response = responder_.respond(
                    *request, lease ? lease.get() : nullptr, platform::Clock::now_ns());

                const auto payload_size = protocol::encode(response, payload);
                if (!payload_size) {
                    return;
                }
                const auto wire_size =
                    protocol::frame(std::span(payload.data(), *payload_size), wire);
                if (!wire_size) {
                    return;
                }
                if (!connection.send_all(wire.data(), *wire_size)) {
                    return;
                }

                latency_.record_elapsed(read_time, platform::Clock::now_ns());
            }
        }
    }

private:
    concurrency::SnapshotPool<Snapshot, SnapshotDepth>& pool_;
    const Responder<Snapshot>& responder_;
    Recorder& latency_;
};

}  // namespace alphaflow::rfq
