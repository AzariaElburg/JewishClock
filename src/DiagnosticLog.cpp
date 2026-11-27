#include "DiagnosticLog.h"
#include "Uptime.h"
#include "Config.h"
#include "StringTools.h"
#include "DebugTime.h"
#include "Display.h"
#include "Texts.h"

#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <cstdarg>
#include <algorithm>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

const char* LOG_PATH       = FS_LOG_DIR "/diagnostic_log.txt";
const char* LOG_OLD_PATH   = FS_LOG_DIR "/diagnostic_log.old.txt";

// === TUNABLES ===  (rule: see Config.h header)
const size_t   MAX_LOG_BYTES        = DiagnosticLog::MAX_FILE_BYTES;   // per file (DiagnosticLog.h)
const size_t   LINE_MAX_BYTES       = 220;     // one log line at most (longer: cut)
const size_t   PENDING_BYTES        = 3072;    // lines waiting for the file (RTC memory, see s_pending)
const size_t   PENDING_FLUSH_BYTES  = 2048;    // written out at once from this much waiting
// A line fits the waiting room many times; the waiting
// room is written out before it's full; a file holds many writes.
static_assert(LINE_MAX_BYTES * 4 < PENDING_FLUSH_BYTES && PENDING_FLUSH_BYTES < PENDING_BYTES && PENDING_BYTES * 8 < DiagnosticLog::MAX_FILE_BYTES,
              "log buffer sizes out of proportion");

// Lines wait here, and are written to the file together when the screen
// stands still (loop(), DisplayManager::goodMomentToWrite() - which also
// says how long at most). Each write to flash is a short screen pause (up
// to ~90 ms measured per append, 287 ms with open/close per line,
// 2026-10-25, /api/status logWrites) - unseen on a still screen, and one
// per batch instead of one per line.
// In RTC memory, which a restart, crash or watchdog reset leaves alone:
// begin() writes what's left to the file first thing. Only a power cut
// loses the lines still waiting.
struct Pending {
    uint32_t magic;
    uint32_t len;
    uint32_t sum;        // of data[0..len) - RTC memory holds noise after power-on
    char     data[PENDING_BYTES];
};
RTC_NOINIT_ATTR Pending s_pending;
const uint32_t PENDING_MAGIC = 0x4C4F4731;   // "LOG1"
int64_t  s_oldestPendingMs = 0;              // Uptime::ms() of the oldest waiting line, 0 = none this boot

uint32_t pendingSum() {
    uint32_t s = 0;
    for (uint32_t i = 0; i < s_pending.len; i++) s = s * 31 + (uint8_t)s_pending.data[i];
    return s;
}
bool pendingValid() {
    return s_pending.magic == PENDING_MAGIC && s_pending.len <= PENDING_BYTES && s_pending.sum == pendingSum();
}
void pendingClear() { s_pending.len = 0; s_pending.sum = pendingSum(); s_pending.magic = PENDING_MAGIC; s_oldestPendingMs = 0; }

// record()/sendTo() are called from three independent FreeRTOS tasks: the
// main loop() task, the WiFi event task (WiFiManager's WiFi.onEvent), and
// the AsyncTCP task (every WebUI.cpp HTTP handler). s_mutex guards s_pending
// (held only for a memcpy); s_fileMutex the log files - two tasks racing on
// the same file corrupt LittleFS's internal state silently (open() just
// starts failing, no crash) - this is what got the log stuck right after the
// one single-threaded BOOT line written before WiFi/AsyncTCP existed.
SemaphoreHandle_t s_mutex = nullptr;
SemaphoreHandle_t s_fileMutex = nullptr;
DiagnosticLog::WriteStats s_stats;   // under s_fileMutex

void rotateIfNeeded() {
    File f = LittleFS.open(LOG_PATH, "r");
    size_t sz = f ? f.size() : 0;
    if (f) f.close();
    if (sz < MAX_LOG_BYTES) return;

    if (LittleFS.exists(LOG_OLD_PATH)) LittleFS.remove(LOG_OLD_PATH);
    LittleFS.rename(LOG_PATH, LOG_OLD_PATH);
}

// Moves every waiting line into the file, as one append.
void flush() {
    if (!s_mutex) return;
    static char batch[PENDING_BYTES];
    xSemaphoreTake(s_fileMutex, portMAX_DELAY);   // first: one flush at a time (batch is shared)
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t len = s_pending.len;
    memcpy(batch, s_pending.data, len);
    pendingClear();
    xSemaphoreGive(s_mutex);
    if (len) {
        int64_t t0 = Uptime::us();
        rotateIfNeeded();
        File f = LittleFS.open(LOG_PATH, "a");
        if (f) { f.write((const uint8_t*)batch, len); f.close(); }
        uint32_t us = (uint32_t)(Uptime::us() - t0);
        s_stats.writes++;
        if (us > 50000)  s_stats.over50ms++;
        if (us > 100000) s_stats.over100ms++;
        s_stats.maxUs = std::max(s_stats.maxUs, us);
    }
    xSemaphoreGive(s_fileMutex);
}

} // namespace

void DiagnosticLog::begin() {
    s_mutex = xSemaphoreCreateMutex();
    s_fileMutex = xSemaphoreCreateMutex();
    if (!LittleFS.exists(FS_LOG_DIR)) LittleFS.mkdir(FS_LOG_DIR);
    if (!pendingValid()) pendingClear();   // power-on: noise, not lines
    flush();                               // lines from before a restart/crash
}

size_t DiagnosticLog::growthLeft() {
    size_t used = 0;
    if (s_fileMutex) xSemaphoreTake(s_fileMutex, portMAX_DELAY);
    for (const char* p : { LOG_PATH, LOG_OLD_PATH }) {
        File f = LittleFS.open(p, "r");
        if (f) { used += f.size(); f.close(); }
    }
    if (s_fileMutex) xSemaphoreGive(s_fileMutex);
    const size_t most = 2 * MAX_LOG_BYTES + PENDING_BYTES;
    return used < most ? most - used : 0;
}

void DiagnosticLog::loop() {
    if (s_oldestPendingMs && DisplayManager::goodMomentToWrite(s_oldestPendingMs)) flush();
}


void DiagnosticLog::flushNow() { flush(); }

void DiagnosticLog::discardPending() {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    pendingClear();
    xSemaphoreGive(s_mutex);
}

void DiagnosticLog::record(const char* category, const char* fmt, ...) {
    char msgBuf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);

    // Both uptime and wall-clock on every line (even pre-sync) so any later
    // synced line lets you back-calculate the real time of an earlier one.
    //
    // Lock-free (DebugTime.h), NOT NTPManager::isSynced()/getDateTime(): those
    // take NTPLock, and NTPManager logs while already holding it - a DNS
    // failure logged from inside NTPManager::loop() self-deadlocked the main
    // loop until the task watchdog rebooted the clock, every ~25 s for as long
    // as DNS kept failing (found 2026-10-16). Still the time the firmware acts
    // on, debug offset included - same source as CLOCK_LOG's timestamp.
    char clockBuf[24] = "unsynced";
    time_t now = debugAdjustedNow();
    if (now != 0) {
        struct tm tmLocal;
        localtime_r(&now, &tmLocal);
        strftime(clockBuf, sizeof(clockBuf), "%Y-%m-%d %H:%M:%S", &tmLocal);
    }

    char line[LINE_MAX_BYTES];
    int n = snprintf(line, sizeof(line), "uptime=%lld clock=%s [%s] %s\r\n",
                     (long long)Uptime::ms(), clockBuf, category, msgBuf);
    if (n < 0) return;
    if (n >= (int)sizeof(line)) { n = sizeof(line) - 1; line[n - 2] = '\r'; line[n - 1] = '\n'; }

    CLOCK_LOG("DiagnosticLog", "%.*s", n - 2, line);

    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool full = s_pending.len + n > PENDING_BYTES;
    xSemaphoreGive(s_mutex);
    if (full) flush();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int64_t nowMs = Uptime::ms();
    if (s_pending.len + n <= PENDING_BYTES) {
        memcpy(s_pending.data + s_pending.len, line, n);
        s_pending.len += n;
        s_pending.sum = pendingSum();
        if (!s_oldestPendingMs) s_oldestPendingMs = nowMs | 1;
    }
    bool due = s_pending.len >= PENDING_FLUSH_BYTES;   // else loop() writes them
    xSemaphoreGive(s_mutex);
    if (due) flush();
}

DiagnosticLog::WriteStats DiagnosticLog::writeStats() {
    if (!s_fileMutex) return WriteStats();
    xSemaphoreTake(s_fileMutex, portMAX_DELAY);
    WriteStats s = s_stats;
    xSemaphoreGive(s_fileMutex);
    return s;
}

void DiagnosticLog::Streak::fail(const char* category, const char* what, const char* detail) {
    if (++failed == after) record(category, Texts::logFailing, what, detail ? detail : "");
}

void DiagnosticLog::Streak::ok(const char* category, const char* what) {
    if (failed >= after) record(category, Texts::logWorksAgain, what, (unsigned long long)failed);
    failed = 0;
}

void DiagnosticLog::sendTo(AsyncWebServerRequest* req, bool old) {
    // Deliberately streams straight from LittleFS (ESPAsyncWebServer's own
    // AsyncFileResponse, which reads in small chunks on demand) instead of
    // building a String and handing that to send(). The previous version
    // did exactly that - and the very first time the log was large enough
    // for it to matter (tens of KB, no longer silently truncated by the
    // NUL-byte bug this replaced), it crashed the live device: building the
    // combined old+current content into one String, then copying THAT into
    // the response's own buffer, held two full copies in RAM at once and
    // exhausted the heap. The two files go as two requests (old = the
    // rotated .old one), each streamed - never both in RAM.
    flush();   // the waiting lines too
    const char* path = old ? LOG_OLD_PATH : LOG_PATH;
    xSemaphoreTake(s_fileMutex, portMAX_DELAY);
    bool exists = LittleFS.exists(path);
    xSemaphoreGive(s_fileMutex);

    if (!exists) {
        if (old) req->send(200, "text/plain", "");
        else req->send(200, "text/plain", "(no diagnostic log entries yet)");
        return;
    }
    req->send(LittleFS, path, "text/plain");
}
