// Global allocation interception for the scoped allocation guard.
//
// Compiled into the test executable only when no sanitizer is active; ASan and
// TSan intercept the global allocator themselves, so replacing it under them
// would disable their tracking.

#include "support/allocation_guard.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

thread_local bool g_guard_active = false;
thread_local bool g_allocated = false;

void* allocate(std::size_t size) {
    if (g_guard_active) {
        g_allocated = true;
    }
    return std::malloc(size == 0 ? 1 : size);
}

}  // namespace

namespace alphaflow::test {

void begin_allocation_guard() noexcept {
    g_allocated = false;
    g_guard_active = true;
}

bool end_allocation_guard() noexcept {
    g_guard_active = false;
    return g_allocated;
}

}  // namespace alphaflow::test

// --- Global allocation functions -------------------------------------------

void* operator new(std::size_t size) {
    void* pointer = allocate(size);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    return pointer;
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}

// C++17 over-aligned allocation (e.g. cache-line-aligned snapshot slots).
void* operator new(std::size_t size, std::align_val_t alignment) {
    if (g_guard_active) {
        g_allocated = true;
    }
    const std::size_t align = static_cast<std::size_t>(alignment);
    const std::size_t rounded = ((size == 0 ? 1 : size) + align - 1) / align * align;
    void* pointer = std::aligned_alloc(align, rounded);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    return pointer;
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void* pointer, std::align_val_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::align_val_t) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    std::free(pointer);
}
