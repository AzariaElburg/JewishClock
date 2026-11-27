#include "FlashStats.h"
#include "StringTools.h"
#include "Uptime.h"
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// === TUNABLES ===  (rule: see Config.h header)
static const uint32_t SLOW_OP_US = 20000;   // an operation this long is kept in the slow list
static const int      SLOW_OPS_KEPT = 32;

static FlashStats::Totals s_totals;
static FlashStats::SlowOp s_slow[SLOW_OPS_KEPT];
static int s_slowNext = 0, s_slowCount = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void note(FlashStats::Op op, size_t bytes, uint32_t us) {
    FlashStats::SlowOp slow = {};
    bool isSlow = us >= SLOW_OP_US;
    if (isSlow) {
        slow.atMs = Uptime::ms(); slow.us = us; slow.bytes = bytes; slow.op = op;
        StringTools::utf8Copy(slow.task, pcTaskGetName(nullptr));
    }
    portENTER_CRITICAL(&s_mux);
    s_totals.count[op]++;
    s_totals.bytes[op] += bytes;
    s_totals.us[op] += us;
    if (us > s_totals.maxUs[op]) s_totals.maxUs[op] = us;
    if (isSlow) {
        s_slow[s_slowNext] = slow;
        s_slowNext = (s_slowNext + 1) % SLOW_OPS_KEPT;
        if (s_slowCount < SLOW_OPS_KEPT) s_slowCount++;
    }
    portEXIT_CRITICAL(&s_mux);
}

extern "C" {
esp_err_t __real_esp_partition_read(const esp_partition_t* p, size_t off, void* dst, size_t n);
esp_err_t __real_esp_partition_write(const esp_partition_t* p, size_t off, const void* src, size_t n);
esp_err_t __real_esp_partition_erase_range(const esp_partition_t* p, size_t off, size_t n);

esp_err_t __wrap_esp_partition_read(const esp_partition_t* p, size_t off, void* dst, size_t n) {
    int64_t t = Uptime::us();
    esp_err_t r = __real_esp_partition_read(p, off, dst, n);
    note(FlashStats::READ, n, (uint32_t)(Uptime::us() - t));
    return r;
}
esp_err_t __wrap_esp_partition_write(const esp_partition_t* p, size_t off, const void* src, size_t n) {
    int64_t t = Uptime::us();
    esp_err_t r = __real_esp_partition_write(p, off, src, n);
    note(FlashStats::WRITE, n, (uint32_t)(Uptime::us() - t));
    return r;
}
esp_err_t __wrap_esp_partition_erase_range(const esp_partition_t* p, size_t off, size_t n) {
    int64_t t = Uptime::us();
    esp_err_t r = __real_esp_partition_erase_range(p, off, n);
    note(FlashStats::ERASE, n, (uint32_t)(Uptime::us() - t));
    return r;
}
}

FlashStats::Totals FlashStats::totals() {
    portENTER_CRITICAL(&s_mux);
    Totals t = s_totals;
    portEXIT_CRITICAL(&s_mux);
    return t;
}

int FlashStats::slowOps(SlowOp* out, int max) {
    portENTER_CRITICAL(&s_mux);
    int n = s_slowCount < max ? s_slowCount : max;
    for (int i = 0; i < n; i++) {
        int idx = (s_slowNext - n + i + SLOW_OPS_KEPT) % SLOW_OPS_KEPT;
        out[i] = s_slow[idx];
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}
