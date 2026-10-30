// =============================================================================
// sim_platform.cpp - the PC side of the stand-ins in sim/stubs (simulator only).
// Simulated time, files, downloads, the one background task, time zones.
// See sim/README.md.
// =============================================================================

#include <Arduino.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_system.h>
#include <soc/spi_struct.h>
#include <freertos/task.h>
#include <Arduino_GFX_Library.h>
#include "sim_platform.h"

#include <filesystem>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include <atomic>
#include <map>

namespace stdfs = std::filesystem;

// ── Simulated time ────────────────────────────────────────────────────────────
static std::atomic<unsigned long> g_simMillis{0};
unsigned long millis() { return g_simMillis; }
unsigned long micros() { return g_simMillis * 1000UL; }
void delay(unsigned long ms) { g_simMillis += ms; }
// Sim::advanceMillis: below, with the background task it has to wait for.

// ── Startup and network timing, as measured on the real clock ─────────────────
// Each run draws its times at random within these ranges: the shortest and
// longest seen on the clock (2026-10-18: 11-12 restarts, its own log over
// USB - sim/measure_boot.py), widened by 30% down and 50% up, to cover
// plausible times not seen yet.
//                        seen on the clock      range used
//   setup() done         3.86 - 4.33 s          2.7 - 6.5 s   (saved calendar loaded; 4.33 since images preload, 2026-10-18)
//   Wi-Fi up             9.8 - 13.5 s           6.9 - 20.2 s
//   time sync after it   0.5 - 5.2 s            0.35 - 7.8 s  (~0.6 or ~5.1: one retry)
//   location lookup      0.08 - 2.1 s           0.06 - 3.2 s
//   main Hebcal download 1.9 - 4.0 s            1.4 - 6.1 s
//   Hebcal zmanim        3.85 - 4.9 s           2.7 - 7.4 s
//
// Each value is computed from (seed, what it is for, which occurrence) -
// e.g. "the 2nd zmanim download" - not taken from one running sequence, so
// a run's values don't shift when the code makes one more or one fewer
// download somewhere. Names: setup, wifi, ntp, geo#N, main#N, zmanim#N.
// Every value used is printed; --seed repeats them all, --timing pins any
// of them (Sim::pinTiming).
Sim::Range Sim::setupTime{2700, 6500}, Sim::wifiUp{6900, 20200}, Sim::ntpAfterWifi{350, 7800};
Sim::Range Sim::netMain{1400, 6100}, Sim::netZmanim{2700, 7400}, Sim::netGeo{60, 3200};
unsigned long Sim::setupMs = 0, Sim::wifiUpMs = 0, Sim::ntpSyncMs = 0;
static uint32_t g_seed = 0;
static std::map<std::string, unsigned long> g_pinned;      // "main#2" -> ms, or "main" -> ms for every one
static std::map<std::string, int> g_occurrences;
static std::mutex g_drawLock;
static uint64_t mix64(uint64_t x) {   // splitmix64 finalizer
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}
static unsigned long draw(const Sim::Range& r, const std::string& kind) {
    std::lock_guard<std::mutex> lk(g_drawLock);
    bool counted = kind == "geo" || kind == "main" || kind == "zmanim";
    std::string name = counted ? kind + "#" + std::to_string(++g_occurrences[kind]) : kind;
    unsigned long v;
    auto pin = g_pinned.find(name);
    if (pin == g_pinned.end()) pin = g_pinned.find(kind);
    if (pin != g_pinned.end()) v = pin->second;
    else {
        uint64_t h = mix64(g_seed);
        for (char c : name) h = mix64(h ^ (uint8_t)c);
        v = r.hi <= r.lo ? r.lo : r.lo + (unsigned long)(h % (r.hi - r.lo + 1));
    }
    fprintf(stderr, "timing %s = %lu ms%s\n", name.c_str(), v, pin != g_pinned.end() ? " (pinned)" : "");
    return v;
}
void Sim::pinTiming(const std::string& name, unsigned long ms) { g_pinned[name] = ms; }
void Sim::startTiming(uint32_t seed) {
    g_seed = seed;
    setupMs   = draw(setupTime, "setup");
    wifiUpMs  = std::max(draw(wifiUp, "wifi"), setupMs);   // Wi-Fi starts in setup(), can't be up before it ends
    ntpSyncMs = wifiUpMs + draw(ntpAfterWifi, "ntp");      // "ntp": time from Wi-Fi up to the first sync
}
static unsigned long latencyFor(const std::string& url) {
    if (url.find("hebcal.com/zmanim") != std::string::npos) return draw(Sim::netZmanim, "zmanim");
    if (url.find("hebcal.com") != std::string::npos)        return draw(Sim::netMain, "main");
    return draw(Sim::netGeo, "geo");
}

// ── Globals the clock's code expects ──────────────────────────────────────────
HardwareSerial Serial;
EspClass ESP;
WiFiClass WiFi;
LittleFSFS LittleFS;
spi_dev_t GPSPI2, GPSPI3;
bool Sim::quiet = true;

size_t HardwareSerial::write(uint8_t c) { if (!Sim::quiet) fputc(c, stderr); return 1; }
size_t HardwareSerial::write(const uint8_t* buf, size_t n) { if (!Sim::quiet) fwrite(buf, 1, n, stderr); return n; }

uint32_t esp_random() {   // deterministic: event ids etc. repeat run to run
    static uint32_t x = 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

// ── Graphics: the library's own line/rectangle algorithms (Arduino_GFX.cpp) ──
void Arduino_GFX::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    drawLine(x, y, x + w - 1, y, color);
    drawLine(x, y + h - 1, x + w - 1, y + h - 1, color);
    drawLine(x, y, x, y + h - 1, color);
    drawLine(x + w - 1, y, x + w - 1, y + h - 1, color);
}
void Arduino_GFX::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
    if (x0 == x1) { if (y0 > y1) std::swap(y0, y1); for (int y = y0; y <= y1; y++) drawPixel(x0, (int16_t)y, color); return; }
    if (y0 == y1) { if (x0 > x1) std::swap(x0, x1); for (int x = x0; x <= x1; x++) drawPixel((int16_t)x, y0, color); return; }
    // writeSlashLine: Bresenham, exactly as the library does it.
    bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
    if (steep) { std::swap(x0, y0); std::swap(x1, y1); }
    if (x0 > x1) { std::swap(x0, x1); std::swap(y0, y1); }
    int16_t dx = x1 - x0, dy = (int16_t)std::abs(y1 - y0), err = dx >> 1, step = (y0 < y1) ? 1 : -1;
    for (; x0 <= x1; x0++) {
        if (steep) drawPixel(y0, x0, color); else drawPixel(x0, y0, color);
        err -= dy;
        if (err < 0) { y0 += step; err += dx; }
    }
}

// ── Files: "/path" on the clock = <fsRoot>/path here ──────────────────────────
std::string Sim::fsRoot = "sim_fs";
static std::string pcPath(const char* clockPath) {
    std::string p = clockPath ? clockPath : "/";
    if (p.empty() || p[0] != '/') p = "/" + p;
    return Sim::fsRoot + p;
}

namespace fs {
File::File(FILE* fp, const std::string& clockPath, const std::string& pc)
    : fp_(fp, [](FILE* f) { if (f) fclose(f); }), clockPath_(clockPath), pcPath_(pc) {}
File::File(const std::string& clockPath, const std::string& pc) : isDir_(true), clockPath_(clockPath), pcPath_(pc) {
    entries_ = std::make_shared<std::vector<std::string>>();
    nextEntry_ = std::make_shared<size_t>(0);
    std::error_code ec;
    for (auto& e : stdfs::directory_iterator(pc, ec)) entries_->push_back(e.path().filename().string());
    std::sort(entries_->begin(), entries_->end());
}
File::~File() {}
size_t File::write(uint8_t c) { return fp_ ? fwrite(&c, 1, 1, fp_.get()) : 0; }
size_t File::write(const uint8_t* buf, size_t n) { return fp_ ? fwrite(buf, 1, n, fp_.get()) : 0; }
int File::available() { if (!fp_) return 0; long here = ftell(fp_.get()); return (int)((long)size() - here); }
int File::read() { if (!fp_) return -1; int c = fgetc(fp_.get()); return c == EOF ? -1 : c; }
int File::peek() { if (!fp_) return -1; int c = fgetc(fp_.get()); if (c != EOF) ungetc(c, fp_.get()); return c == EOF ? -1 : c; }
size_t File::read(uint8_t* buf, size_t n) { return fp_ ? fread(buf, 1, n, fp_.get()) : 0; }
bool File::seek(uint32_t pos) { return fp_ && fseek(fp_.get(), (long)pos, SEEK_SET) == 0; }
size_t File::position() const { return fp_ ? (size_t)ftell(fp_.get()) : 0; }
size_t File::size() const {
    if (!fp_) return 0;
    fflush(fp_.get());
    std::error_code ec; auto s = stdfs::file_size(pcPath_, ec);
    return ec ? 0 : (size_t)s;
}
void File::close() { fp_.reset(); isDir_ = false; }
void File::flush() { if (fp_) fflush(fp_.get()); }
const char* File::name() const {
    size_t slash = clockPath_.find_last_of('/');
    return slash == std::string::npos ? clockPath_.c_str() : clockPath_.c_str() + slash + 1;
}
File File::openNextFile(const char* mode) {
    if (!isDir_ || !entries_ || *nextEntry_ >= entries_->size()) return File();
    std::string child = clockPath_ + (clockPath_.size() > 1 ? "/" : "") + (*entries_)[(*nextEntry_)++];
    return LittleFS.open(child.c_str(), mode);
}

File FS::open(const char* path, const char* mode, bool create) {
    std::string pc = pcPath(path);
    std::error_code ec;
    if (stdfs::is_directory(pc, ec)) return File(path, pc);
    bool writing = mode && (mode[0] == 'w' || mode[0] == 'a');
    if (writing) stdfs::create_directories(stdfs::path(pc).parent_path(), ec);
    std::string m = std::string(mode ? mode : "r") + "b";
    FILE* fp = fopen(pc.c_str(), m.c_str());
    if (!fp) return File();
    return File(fp, path, pc);
}
bool FS::exists(const char* path) { std::error_code ec; return stdfs::exists(pcPath(path), ec); }
bool FS::remove(const char* path) { std::error_code ec; return stdfs::remove(pcPath(path), ec); }
bool FS::rename(const char* from, const char* to) {
    std::error_code ec; stdfs::rename(pcPath(from), pcPath(to), ec); return !ec;
}
bool FS::mkdir(const char* path) { std::error_code ec; stdfs::create_directories(pcPath(path), ec); return !ec; }
bool FS::rmdir(const char* path) { std::error_code ec; return stdfs::remove(pcPath(path), ec); }
size_t FS::usedBytes() {
    size_t total = 0; std::error_code ec;
    for (auto& e : stdfs::recursive_directory_iterator(Sim::fsRoot, ec)) if (e.is_regular_file(ec)) total += (size_t)e.file_size(ec);
    return total;
}
} // namespace fs

// ── Downloads: curl.exe, cached on disk by URL ────────────────────────────────
std::string Sim::httpCacheDir = "sim_http_cache";
static void simSleepUntil(unsigned long targetMs);   // below, with the task
int HTTPClient::GET() {
    const unsigned long startMs = millis();
    struct Done { unsigned long at; ~Done() { simSleepUntil(at); } } done{0};   // answer arrives after the clock's measured time
    body_ = String();
    std::string url = url_.c_str();
    if (url.empty()) return -1;
    done.at = startMs + latencyFor(url);
    uint64_t h = 1469598103934665603ULL;   // FNV-1a of the URL = cache file name
    for (unsigned char c : url) { h ^= c; h *= 1099511628211ULL; }
    char name[32]; snprintf(name, sizeof(name), "%016llx.txt", (unsigned long long)h);
    std::error_code ec;
    stdfs::create_directories(Sim::httpCacheDir, ec);
    std::string file = Sim::httpCacheDir + "/" + name;
    if (!stdfs::exists(file, ec) || stdfs::file_size(file, ec) == 0) {
        std::string cmd = "curl.exe -s -f -m 30 -o \"" + file + "\" \"" + url + "\"";
        if (std::system(cmd.c_str()) != 0) { stdfs::remove(file, ec); return -1; }
        FILE* idx = fopen((Sim::httpCacheDir + "/index.txt").c_str(), "ab");
        if (idx) { fprintf(idx, "%s %s\n", name, url.c_str()); fclose(idx); }
    }
    FILE* fp = fopen(file.c_str(), "rb");
    if (!fp) return -1;
    std::string data; char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) data.append(buf, n);
    fclose(fp);
    body_ = String(data);
    return 200;
}

// ── The one background task (Calendar's downloads) ────────────────────────────
// A download takes the clock's measured time in SIMULATED time (latencyFor),
// however long the PC really needs: the task sleeps until the simulated
// clock reaches its finish time. The main loop never runs alongside the task
// otherwise - it waits until the task is idle or asleep (Sim::waitForTasks),
// and when a sleep ends (Sim::advanceMillis) until the task has finished its
// work - so every run is repeatable, whatever the network does.
namespace {
struct SimTask {
    std::mutex m; std::condition_variable cv;
    int notifications = 0; bool busy = false;
    bool sleeping = false; unsigned long wakeAt = 0;
};
SimTask g_task;
thread_local bool t_isTask = false;
bool taskSettled() {   // idle, or asleep until a later simulated moment
    return (!g_task.busy && g_task.notifications == 0) || (g_task.sleeping && g_task.wakeAt > g_simMillis);
}
}
static void simSleepUntil(unsigned long targetMs) {
    if (!t_isTask) return;   // only the background task can wait for simulated time
    std::unique_lock<std::mutex> lk(g_task.m);
    if (g_simMillis >= targetMs) return;
    g_task.sleeping = true; g_task.wakeAt = targetMs;
    g_task.cv.notify_all();
    g_task.cv.wait(lk, [targetMs] { return g_simMillis >= targetMs; });
    g_task.sleeping = false;
}
void Sim::advanceMillis(unsigned long ms) {
    std::unique_lock<std::mutex> lk(g_task.m);
    g_simMillis += ms;
    g_task.cv.notify_all();
    g_task.cv.wait(lk, [] { return taskSettled(); });   // a woken download finishes first
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char*, uint32_t, void* param, UBaseType_t, TaskHandle_t* handle, BaseType_t) {
    if (handle) *handle = &g_task;
    std::thread([fn, param] { t_isTask = true; fn(param); }).detach();
    return pdPASS;
}
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t) {
    std::unique_lock<std::mutex> lk(g_task.m);
    g_task.busy = false;
    g_task.cv.notify_all();
    g_task.cv.wait(lk, [] { return g_task.notifications > 0; });
    g_task.notifications = 0;
    g_task.busy = true;
    return 1;
}
void xTaskNotifyGive(TaskHandle_t) {
    std::lock_guard<std::mutex> lk(g_task.m);
    g_task.notifications++;
    g_task.busy = true;
    g_task.cv.notify_all();
}
void Sim::waitForTasks() {
    std::unique_lock<std::mutex> lk(g_task.m);
    g_task.cv.wait(lk, [] { return taskSettled(); });
}

// ── Time zones: POSIX TZ strings, e.g. "IST-2IDT,M3.4.4/26,M10.5.0" ───────────
namespace {
struct TzRule { int month = 0, week = 0, wday = 0; long secs = 7200; };
struct Tz { long stdOff = 0, dstOff = 0; bool hasDst = false; TzRule start, end; } g_tz;   // offsets: seconds EAST of UTC

long parseOffset(const char*& p) {   // POSIX: "-2" means 2 hours EAST of UTC
    int sign = 1;
    if (*p == '+') p++; else if (*p == '-') { sign = -1; p++; }
    long h = strtol(p, (char**)&p, 10), m = 0, s = 0;
    if (*p == ':') { p++; m = strtol(p, (char**)&p, 10); if (*p == ':') { p++; s = strtol(p, (char**)&p, 10); } }
    return -sign * (h * 3600 + m * 60 + s);
}
void skipName(const char*& p) {
    if (*p == '<') { while (*p && *p != '>') p++; if (*p) p++; return; }
    while (*p && isalpha((unsigned char)*p)) p++;
}
bool parseRule(const char*& p, TzRule& r) {
    if (*p != 'M') return false;
    p++;
    r.month = (int)strtol(p, (char**)&p, 10); if (*p++ != '.') return false;
    r.week  = (int)strtol(p, (char**)&p, 10); if (*p++ != '.') return false;
    r.wday  = (int)strtol(p, (char**)&p, 10);
    r.secs = 7200;
    if (*p == '/') { p++; r.secs = -parseOffset(p); }   // a time, not an offset: undo the sign flip
    return true;
}
long daysFromCivil(long y, unsigned m, unsigned d) {   // days since 1970-01-01
    y -= m <= 2; long era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
void civilFromDays(long z, int& y, int& m, int& d) {
    z += 719468; long era = (z >= 0 ? z : z - 146096) / 146097; unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; long yy = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100); unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1); m = (int)(mp < 10 ? mp + 3 : mp - 9); y = (int)(yy + (m <= 2));
}
int daysInMonth(int y, int m) {
    static const int dm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return dm[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
}
// Local wall-clock moment the rule fires in year y, as seconds since epoch "as if UTC".
// int64_t, not long: long is 32-bit on Windows, and these seconds pass 2^31
// in 2038 - the simulator itself had a year-2038 bug (wrong summer time in
// 2100, found comparing with the 64-bit clock, 2026-10-18).
int64_t ruleLocal(const TzRule& r, int y) {
    long first = daysFromCivil(y, r.month, 1);
    int firstWday = (int)(((first % 7) + 7 + 4) % 7);   // 1970-01-01 was a Thursday (4)
    int day = 1 + (r.wday - firstWday + 7) % 7 + (r.week - 1) * 7;
    while (day > daysInMonth(y, r.month)) day -= 7;     // week 5 = last
    return (int64_t)daysFromCivil(y, r.month, day) * 86400 + r.secs;
}
bool isDst(time_t utc) {
    if (!g_tz.hasDst) return false;
    int y, m, d; civilFromDays((long)((utc + g_tz.stdOff) / 86400), y, m, d);
    int64_t startUtc = ruleLocal(g_tz.start, y) - g_tz.stdOff;   // starts on standard time
    int64_t endUtc   = ruleLocal(g_tz.end, y) - g_tz.dstOff;     // ends on daylight time
    return startUtc < endUtc ? (utc >= startUtc && utc < endUtc) : !(utc >= endUtc && utc < startUtc);
}
}

void Sim::setTimezone(const char* posix) {
    g_tz = Tz();
    if (!posix || !*posix) return;
    const char* p = posix;
    skipName(p);
    g_tz.stdOff = parseOffset(p);
    if (!*p) return;
    skipName(p);
    g_tz.hasDst = true;
    g_tz.dstOff = g_tz.stdOff + 3600;
    if (*p && *p != ',') g_tz.dstOff = parseOffset(p);
    if (*p == ',') { p++; parseRule(p, g_tz.start); }
    if (*p == ',') { p++; parseRule(p, g_tz.end); }
}
long Sim::utcOffsetAt(time_t utc) { return isDst(utc) ? g_tz.dstOff : g_tz.stdOff; }

struct tm* sim_gmtime_r(const time_t* t, struct tm* out) {
    long days = (long)(*t / 86400), rem = (long)(*t % 86400);
    if (rem < 0) { rem += 86400; days--; }
    int y, m, d; civilFromDays(days, y, m, d);
    memset(out, 0, sizeof(*out));
    out->tm_year = y - 1900; out->tm_mon = m - 1; out->tm_mday = d;
    out->tm_hour = (int)(rem / 3600); out->tm_min = (int)(rem % 3600 / 60); out->tm_sec = (int)(rem % 60);
    out->tm_wday = (int)(((days % 7) + 7 + 4) % 7);
    out->tm_yday = (int)(days - daysFromCivil(y, 1, 1));
    return out;
}
struct tm* sim_localtime_r(const time_t* t, struct tm* out) {
    bool dst = isDst(*t);
    time_t local = *t + (dst ? g_tz.dstOff : g_tz.stdOff);
    sim_gmtime_r(&local, out);
    out->tm_isdst = dst ? 1 : 0;
    return out;
}
time_t sim_timegm(struct tm* tm) {
    return (time_t)daysFromCivil(tm->tm_year + 1900, (unsigned)(tm->tm_mon + 1), (unsigned)tm->tm_mday) * 86400
         + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}
int sim_setenv(const char* name, const char* value, int) {
    if (name && strcmp(name, "TZ") == 0) Sim::setTimezone(value);
    return 0;
}
