#pragma once
// PC stand-in (simulator only): "PSRAM" is just the PC's memory.
#include <cstdlib>
#include <cstddef>
#define MALLOC_CAP_SPIRAM  (1 << 10)
#define MALLOC_CAP_8BIT    (1 << 2)
#define MALLOC_CAP_DEFAULT (1 << 12)
inline void* heap_caps_malloc(size_t n, uint32_t) { return std::malloc(n); }
inline void* heap_caps_calloc(size_t n, size_t s, uint32_t) { return std::calloc(n, s); }
inline void* heap_caps_realloc(void* p, size_t n, uint32_t) { return std::realloc(p, n); }
inline void heap_caps_free(void* p) { std::free(p); }
inline size_t heap_caps_get_free_size(uint32_t) { return 7u * 1024u * 1024u; }
inline size_t heap_caps_get_total_size(uint32_t) { return 8u * 1024u * 1024u; }
inline size_t heap_caps_get_largest_free_block(uint32_t) { return 4u * 1024u * 1024u; }
