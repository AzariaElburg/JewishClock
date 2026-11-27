#pragma once
// =============================================================================
// FlashStats - every flash read/write/erase LittleFS does, counted and timed.
// While the flash is written or erased, code running from flash stops on both
// cores - the screen included (CLAUDE.md, "Writing flash pauses everything").
// The partition functions LittleFS calls are wrapped at link time
// (pioarduino.ini: -Wl,--wrap=esp_partition_...), so this sees all of them.
// For GET /api/debug/flash.
// =============================================================================
#include <Arduino.h>

namespace FlashStats {

enum Op { READ = 0, WRITE = 1, ERASE = 2, OPS = 3 };

struct Totals {
    // 64-bit: the clock runs for years (32-bit microseconds would fill up
    // after 71 minutes of flash time, bytes after 4 GB).
    uint64_t count[OPS] = {0, 0, 0};
    uint64_t bytes[OPS] = {0, 0, 0};
    uint64_t us[OPS]    = {0, 0, 0};
    uint32_t maxUs[OPS] = {0, 0, 0};
};
Totals totals();   // since boot

// One slow operation (>= SLOW_OP_US, FlashStats.cpp), newest last.
struct SlowOp {
    int64_t  atMs;     // Uptime::ms()
    uint32_t us;
    uint32_t bytes;
    uint8_t  op;
    char     task[12];
};
int slowOps(SlowOp* out, int max);   // copies up to max, returns how many

} // namespace FlashStats
