#pragma once
// PC stand-in (jcclock): PSRAM counted against the clock's 8 MB and refused
// a little before the clock would run out - sim/sim_heap.cpp.
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#define MALLOC_CAP_SPIRAM  (1 << 10)
#define MALLOC_CAP_8BIT    (1 << 2)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DEFAULT (1 << 12)
void* heap_caps_malloc(size_t n, uint32_t caps);
void* heap_caps_calloc(size_t n, size_t s, uint32_t caps);
void* heap_caps_realloc(void* p, size_t n, uint32_t caps);
void heap_caps_free(void* p);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_total_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
