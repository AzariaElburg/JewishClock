#pragma once

#include <vector>
#include <new>
#include "esp_heap_caps.h"

// A std::allocator that draws from PSRAM (heap_caps_malloc with
// MALLOC_CAP_SPIRAM) instead of the default allocator's regular SRAM heap.
// Used for data whose size scales with user content (e.g. personal events)
// so it never competes with the SRAM that WiFi/TLS/the web server/display
// need - see the PersonalEvent events-list redesign for why. Deliberately
// NOT used for small, fixed-size, frequently-touched data (that's what
// internal SRAM is actually faster for) - this is specifically for
// larger, dynamically-sized collections.
template <typename T>
struct PsramAllocator {
    using value_type = T;

    PsramAllocator() noexcept = default;
    template <typename U> PsramAllocator(const PsramAllocator<U>&) noexcept {}

    T* allocate(size_t n) {
        if (n > (size_t)-1 / sizeof(T)) throw std::bad_alloc();
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM);
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t) noexcept { heap_caps_free(p); }
};

template <typename T, typename U>
bool operator==(const PsramAllocator<T>&, const PsramAllocator<U>&) { return true; }
template <typename T, typename U>
bool operator!=(const PsramAllocator<T>&, const PsramAllocator<U>&) { return false; }

template <typename T>
using PsramVector = std::vector<T, PsramAllocator<T>>;
