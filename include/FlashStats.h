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
    uint32_t count[OPS] = {0, 0, 0};
    uint32_t bytes[OPS] = {0, 0, 0};
    uint32_t us[OPS]    = {0, 0, 0};
    uint32_t maxUs[OPS] = {0, 0, 0};
};
Totals totals();   // since boot

// One slow operation (>= SLOW_OP_US, FlashStats.cpp), newest last.
struct SlowOp {
    uint32_t atMs;
    uint32_t us;
    uint32_t bytes;
    uint8_t  op;
    char     task[12];
};
int slowOps(SlowOp* out, int max);   // copies up to max, returns how many

} // namespace FlashStats
