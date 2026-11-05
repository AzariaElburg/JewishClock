// =============================================================================
// sim_heap.cpp - the full simulator's PSRAM (jcclock): what the clock's code
// puts there (heap_caps_malloc(..., MALLOC_CAP_SPIRAM): pictures, events,
// the calendar JSON) is counted against the clock's 8 MB less what its system
// takes, and refused at SIM_PSRAM_MARGIN of that - a little before the clock
// would run out, so the simulator fails first. Internal RAM: the clock's own
// measured numbers (sim_clock_main.cpp). Fragmentation is not modelled.
// =============================================================================

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <mutex>
#include <unordered_map>

// === TUNABLES ===
static const size_t PSRAM_TOTAL_BYTES  = 8u * 1024u * 1024u;
static const size_t PSRAM_SYSTEM_BYTES = 512u * 1024u;   // Wi-Fi/lwIP buffers etc. moved to PSRAM - an estimate
static const double SIM_PSRAM_MARGIN   = 0.90;           // refuse beyond 90% of what's left for the clock's code

static std::mutex g_m;
static std::unordered_map<void*, size_t> g_sizes;
static std::atomic<size_t> g_used{0}, g_peak{0};
static size_t limitBytes() { return (size_t)((PSRAM_TOTAL_BYTES - PSRAM_SYSTEM_BYTES) * SIM_PSRAM_MARGIN); }

size_t sim_psram_used() { return g_used; }

void* heap_caps_malloc(size_t n, uint32_t caps) {
    if (!(caps & MALLOC_CAP_SPIRAM)) return std::malloc(n);
    std::lock_guard<std::mutex> lk(g_m);
    if (g_used + n > limitBytes()) return nullptr;   // out of PSRAM
    void* p = std::malloc(n);
    if (p) { g_sizes[p] = n; g_used += n; if (g_used > g_peak) g_peak = g_used.load(); }
    return p;
}
void* heap_caps_calloc(size_t n, size_t s, uint32_t caps) {
    void* p = heap_caps_malloc(n * s, caps);
    if (p) memset(p, 0, n * s);
    return p;
}
void heap_caps_free(void* p) {
    if (!p) return;
    {
        std::lock_guard<std::mutex> lk(g_m);
        auto it = g_sizes.find(p);
        if (it != g_sizes.end()) { g_used -= it->second; g_sizes.erase(it); }
    }
    std::free(p);
}
void* heap_caps_realloc(void* p, size_t n, uint32_t caps) {
    if (!p) return heap_caps_malloc(n, caps);
    size_t old = 0;
    { std::lock_guard<std::mutex> lk(g_m); auto it = g_sizes.find(p); if (it == g_sizes.end()) return std::realloc(p, n); old = it->second; }
    void* q = heap_caps_malloc(n, caps);
    if (!q) return nullptr;
    memcpy(q, p, old < n ? old : n);
    heap_caps_free(p);
    return q;
}
size_t heap_caps_get_total_size(uint32_t caps) { return caps & MALLOC_CAP_SPIRAM ? PSRAM_TOTAL_BYTES : 320u * 1024u; }
size_t heap_caps_get_free_size(uint32_t caps) {
    if (!(caps & MALLOC_CAP_SPIRAM)) return ESP.getFreeHeap();
    size_t lim = limitBytes();
    return g_used >= lim ? 0 : lim - g_used;
}
size_t heap_caps_get_largest_free_block(uint32_t caps) { return caps & MALLOC_CAP_SPIRAM ? heap_caps_get_free_size(caps) : ESP.getMaxAllocHeap(); }
size_t heap_caps_get_minimum_free_size(uint32_t caps) { return caps & MALLOC_CAP_SPIRAM ? limitBytes() - g_peak : ESP.getMinFreeHeap(); }
uint32_t EspClass::getFreePsram() { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
