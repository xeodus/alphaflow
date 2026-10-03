#pragma once

namespace alphaflow::test {

/// Scoped, thread-local heap-allocation detector (test-only).
///
/// `begin_allocation_guard()` arms the guard on the calling thread;
/// `end_allocation_guard()` disarms it and returns true if any heap allocation
/// occurred on that thread in between. It measures one thread, so other threads
/// (feeds, curve, metrics, the test framework) do not interfere.
///
/// The detector is built by replacing the global allocation functions, which is
/// only compiled in when no sanitizer is active (ASan/TSan intercept the
/// allocator themselves).
void begin_allocation_guard() noexcept;
[[nodiscard]] bool end_allocation_guard() noexcept;

}  // namespace alphaflow::test
