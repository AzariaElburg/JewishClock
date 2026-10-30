// =============================================================================
// sim_main.cpp - JewishClock on the PC: the clock's own calendar, message and
// drawing code, driven on simulated time, writing the screen as images.
// Startup and loop follow src/main.cpp (keep in step with it). See sim/README.md.
//
//   jcsim --fs DIR --time "2026-10-16 12:00" [--seconds 90] --out DIR
//         [--live] [--speed 1] [--verbose] [--http-cache DIR]
//         [--epoch UTC_SECONDS] [--match DIR] [--print-epoch]
//         [--seed N] [--timing NAME=MS,...] [--instant-boot] [--demo-at SECONDS]
//         [--calibration content|pattern] [--setup-mode] [--every-message]
//
// --frame-diff FILE: per new frame, what changed in the window since the
// previous one: "changed rows<TAB>first..last row span<TAB>pixels from each
// row's first to last change<TAB>1 if a picture is on screen".
//
// --every-message: a slides.txt line for every new message, also when its
// text is the same as before (e.g. a status line starting over).
//
// --setup-mode: the clock's setup screen (as with no Wi-Fi known).
//
// --calibration: the clock's calibration mode (Settings > Advanced) - the
// window frame on the normal content, or on the numbered test pattern.
//
// --time/--epoch is the moment of POWER ON. Like the clock, the simulator
// then connects, syncs its time and downloads, each taking a random time
// within the ranges measured on the real clock (sim_platform.cpp). Every
// value is printed by name ("timing main#1 = 3912 ms"): --seed repeats the
// whole run, --timing pins chosen values (setup, wifi, ntp, geo#N, main#N,
// zmanim#N; without #N: every one of that kind) to recreate one situation.
// --instant-boot: all zero, for checks of content only.
//
// Default mode: runs --seconds of simulated time and saves one BMP per slide
// shown (out/NNN.bmp) plus out/slides.txt ("NNN <local time> <scrollX> <text>").
// --live: runs in real time (x --speed), keeping out/latest.bmp + latest.txt
// current, for sim/viewer.py.
// =============================================================================

#include <Arduino.h>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <filesystem>
#include "Config.h"
#include "Storage.h"
#include "Display.h"
#include "Calendar.h"
#include "MessageAggregator.h"
#include "NTPManager.h"
#include "WiFiManager.h"
#include "Location.h"
#include "LogLevel.h"
#include "Font.h"
#include "OnScreenElement.h"
#include "sim_platform.h"

namespace stdfs = std::filesystem;

static AppConfig g_cfg;
static EventList g_events;

static const unsigned long STEP_MS = 16;   // Display::update()'s own frame interval

static std::string elementsText(const std::vector<OnScreenElement>& els) {
    std::string t;
    for (size_t i = 0; i < els.size(); i++) {
        if (i) t += " | ";
        t += els[i].type == OnScreenElementType::TEXT ? std::string(els[i].content.c_str()) : "[" + std::string(els[i].content.c_str()) + "]";
    }
    return t;
}

// Same BMP layout as the clock's /api/debug/screenshot (24-bit, bottom-up).
static bool writeBmp(const std::string& path, const uint16_t* fb) {
    const int W = TFT_WIDTH, H = TFT_HEIGHT, ROW = W * 3;
    const uint32_t total = 54 + ROW * H;
    std::vector<uint8_t> out(total, 0);
    auto put32 = [&out](int at, uint32_t v) { for (int k = 0; k < 4; k++) out[at + k] = (uint8_t)(v >> (8 * k)); };
    out[0] = 'B'; out[1] = 'M'; put32(2, total); put32(10, 54); put32(14, 40); put32(18, W); put32(22, H);
    out[26] = 1; out[28] = 24; put32(34, ROW * H);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint16_t c = fb[y * W + x];
        uint8_t* px = &out[54 + (H - 1 - y) * ROW + x * 3];
        px[0] = (uint8_t)((c & 0x1F) << 3); px[1] = (uint8_t)(((c >> 5) & 0x3F) << 2); px[2] = (uint8_t)(((c >> 11) & 0x1F) << 3);
    }
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    std::error_code ec;
    stdfs::rename(tmp, path, ec);
    return !ec;
}

// Reads a 24-bit BMP written by writeBmp() or the clock's screenshot into
// top-down RGB rows (3 bytes per pixel).
static bool readBmp(const std::string& path, std::vector<uint8_t>& rgb) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> in;
    uint8_t buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) in.insert(in.end(), buf, buf + n);
    fclose(f);
    const int W = TFT_WIDTH, H = TFT_HEIGHT, ROW = W * 3;
    if (in.size() < 54 + (size_t)ROW * H || in[0] != 'B' || in[1] != 'M') return false;
    uint32_t off = in[10] | in[11] << 8 | in[12] << 16 | (uint32_t)in[13] << 24;
    rgb.assign((size_t)ROW * H, 0);
    for (int y = 0; y < H; y++) memcpy(&rgb[(size_t)y * ROW], &in[off + (size_t)(H - 1 - y) * ROW], ROW);
    return true;
}

static int pixelDiff(const std::vector<uint8_t>& rgb, const uint16_t* fb) {
    int diff = 0;
    for (int i = 0; i < TFT_WIDTH * TFT_HEIGHT; i++) {
        uint16_t c = fb[i];
        const uint8_t* p = &rgb[(size_t)i * 3];   // R, G, B in BMP order B, G, R
        if (p[0] != (uint8_t)((c & 0x1F) << 3) || p[1] != (uint8_t)(((c >> 5) & 0x3F) << 2) || p[2] != (uint8_t)(((c >> 11) & 0x1F) << 3)) diff++;
    }
    return diff;
}

// --match DIR: DIR/index.txt lists the clock's screenshots, one per line:
// "<file>\t<scrollX>\t<screen text>". Every simulated frame showing the same
// text at the same scroll position is compared pixel by pixel; the result
// goes to DIR/match.txt ("<file>\t<result>\t<best different pixels>\t...")
// and the closest simulated frame to DIR/sim_<file>.
struct MatchTarget {
    std::string file, text; int scrollX = 0;
    std::vector<uint8_t> rgb;
    int best = -1;                 // fewest different pixels so far (-1 = never compared)
    bool textSeen = false; int nearestScroll = 1 << 30;
    std::vector<uint16_t> bestFrame;
};

static std::vector<MatchTarget> loadTargets(const std::string& dir) {
    std::vector<MatchTarget> out;
    FILE* f = fopen((dir + "/index.txt").c_str(), "rb");
    if (!f) { fprintf(stderr, "no %s/index.txt\n", dir.c_str()); std::exit(2); }
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t a = s.find('\t'), b = a == std::string::npos ? a : s.find('\t', a + 1);
        if (b == std::string::npos) continue;
        MatchTarget t;
        t.file = s.substr(0, a); t.scrollX = atoi(s.substr(a + 1, b - a - 1).c_str()); t.text = s.substr(b + 1);
        if (!readBmp(dir + "/" + t.file, t.rgb)) { fprintf(stderr, "cannot read %s\n", t.file.c_str()); continue; }
        out.push_back(std::move(t));
    }
    fclose(f);
    return out;
}

static std::string localStamp() {
    time_t t = NTPManager::getTime();
    struct tm tm; localtime_r(&t, &tm);
    char b[32]; strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

static void setupLikeTheClock() {   // src/main.cpp setup(), minus hardware
    Storage::begin();
    Storage::loadConfig(g_cfg);
    Logging::setLevel(Logging::parseLevelName(g_cfg.logLevel, LogLevel::INFO));
    NTPManager::setDebugOffset(0);   // the simulator sets "now" itself (--time)
    Storage::loadEvents(g_events);
    DisplayManager::begin();
    DisplayManager::setWindow(g_cfg.calibX, g_cfg.calibY, g_cfg.calibW, g_cfg.calibH);
    DisplayManager::setTearUs(g_cfg.calibTearUs);
    DisplayManager::preloadImages();
    DisplayManager::setScrollSpeed(g_cfg.scrollSpeed);
    fontSetSpacing((uint8_t)g_cfg.fontCharGapPx, (uint8_t)g_cfg.fontWordGapPx);
    fontSetWidthScale((uint8_t)g_cfg.fontWidthScalePct);
    NTPManager::begin(g_cfg.ntpServerCount > 0 ? g_cfg.ntpServers[0] : NTP_SERVER_1, g_cfg.tzPOSIX);
    Calendar::begin(g_cfg);
    MessageAggregator::begin(g_cfg, g_events);
    WiFiManager::begin(g_cfg);
    // The clock's setup() takes ~3.9 s (display, file system, Wi-Fi start);
    // the simulator's takes none - so wait out the clock's, drawn like the
    // other startup times (Sim::setupTime, sim_platform.cpp).
    Sim::advanceMillis(Sim::setupMs);
}

static uint32_t g_lastMessageId = 0;
static long g_demoAtMs = -1;   // --demo-at: press the web page's Demo button at this simulated ms
static int  g_calibration = 0; // --calibration: 0 off, 1 content, 2 pattern
static bool g_everyMessage = false; // --every-message
static std::string g_frameDiffPath; // --frame-diff FILE
static const unsigned long BOOT_OVERLAY_MS = 3UL * 60UL * 1000UL;   // = src/main.cpp's (keep in step)
static void loopLikeTheClock() {    // src/main.cpp loop(), minus hardware
    NTPManager::loop();
    WiFiManager::loop();
    Location::loop(g_cfg);
    bool hasTime = NTPManager::isSynced() || Calendar::getToday().hYear != 0;
    bool showOverlay = (millis() < BOOT_OVERLAY_MS) || (!hasTime && WiFiManager::isApActive());
    if (g_demoAtMs >= 0 && (long)millis() >= g_demoAtMs) { MessageAggregator::requestDemo(true); g_demoAtMs = -1; }
    Calendar::update(g_cfg, g_events);
    Sim::waitForTasks();             // the download task is idle, or asleep until its answer is due
    MessageAggregator::loop(g_cfg, showOverlay);
    DisplayManager::setScrollSpeed(g_cfg.scrollSpeed);   // night-mode factor not simulated
    uint32_t msgId = MessageAggregator::getCurrentMessageId();
    if (msgId != g_lastMessageId) {
        DisplayManager::setMessage(MessageAggregator::getMessageElements(), MessageAggregator::getCurrentColor(), g_cfg);
        g_lastMessageId = msgId;
    }
    DisplayManager::update(g_cfg);
    Storage::loop(g_cfg);
    Sim::advanceMillis(STEP_MS);
}

static time_t parseLocalTime(const std::string& s, bool utc) {
    struct tm tm{}; int sec = 0;
    if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &sec) < 5) {
        fprintf(stderr, "bad --time \"%s\" (expected \"YYYY-MM-DD HH:MM[:SS]\")\n", s.c_str()); std::exit(2);
    }
    tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_sec = sec;
    time_t wall = sim_timegm(&tm);
    if (utc) return wall;
    // Local -> UTC: two passes handle the DST edge correctly.
    time_t guess = wall - Sim::utcOffsetAt(wall);
    return wall - Sim::utcOffsetAt(guess);
}

int main(int argc, char** argv) {
    std::string fsDir = "sim_fs", outDir = "sim_out", when, cacheDir, matchDir;
    double seconds = 90, speed = 1; bool live = false, utc = false, printEpoch = false; long long epoch = -1;
    uint32_t seed = 0; bool seedGiven = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); } return argv[++i]; };
        if (a == "--fs") fsDir = next();
        else if (a == "--out") outDir = next();
        else if (a == "--time") when = next();
        else if (a == "--utc") utc = true;
        else if (a == "--seconds") seconds = atof(next().c_str());
        else if (a == "--live") live = true;
        else if (a == "--speed") speed = atof(next().c_str());
        else if (a == "--verbose") Sim::quiet = false;
        else if (a == "--http-cache") cacheDir = next();
        else if (a == "--epoch") epoch = atoll(next().c_str());
        else if (a == "--match") matchDir = next();
        else if (a == "--print-epoch") printEpoch = true;
        else if (a == "--seed") { seed = (uint32_t)strtoul(next().c_str(), nullptr, 10); seedGiven = true; }
        else if (a == "--timing") {   // "main#1=6000,zmanim=7400,wifi=12000" - pin these values
            std::string list = next();
            size_t pos = 0;
            while (pos < list.size()) {
                size_t end = list.find(',', pos); if (end == std::string::npos) end = list.size();
                std::string item = list.substr(pos, end - pos);
                size_t eq = item.find('=');
                if (eq == std::string::npos) { fprintf(stderr, "--timing: expected name=ms, got \"%s\"\n", item.c_str()); return 2; }
                Sim::pinTiming(item.substr(0, eq), strtoul(item.c_str() + eq + 1, nullptr, 10));
                pos = end + 1;
            }
        }
        else if (a == "--demo-at") g_demoAtMs = (long)(atof(next().c_str()) * 1000);
        else if (a == "--every-message") g_everyMessage = true;
        else if (a == "--frame-diff") g_frameDiffPath = next();
        else if (a == "--setup-mode") { extern bool g_simSetupMode; g_simSetupMode = true; }
        else if (a == "--calibration") {
            std::string m = next();
            if (m == "content") g_calibration = 1; else if (m == "pattern") g_calibration = 2;
            else { fprintf(stderr, "--calibration: content or pattern\n"); return 2; }
        }
        else if (a == "--instant-boot") Sim::setupTime = Sim::wifiUp = Sim::ntpAfterWifi = Sim::netMain = Sim::netZmanim = Sim::netGeo = {0, 0};
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (!seedGiven) seed = (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count();
    if (!printEpoch) fprintf(stderr, "timing: --seed %u\n", seed);
    Sim::startTiming(seed);
    Sim::fsRoot = fsDir;
    Sim::httpCacheDir = cacheDir.empty() ? fsDir + "/../http_cache" : cacheDir;
    std::error_code ec;
    stdfs::create_directories(outDir, ec);

    // The time zone must be known before --time can be read as local time.
    Storage::begin();
    { AppConfig probe; Storage::loadConfig(probe); Sim::setTimezone(probe.tzPOSIX); }
    // (time)(...): the PC's real clock - plain time() is the simulated ESP32 one (sim_force.h)
    Sim::setUtc(epoch >= 0 ? (time_t)epoch : when.empty() ? (time)(nullptr) : parseLocalTime(when, utc));
    if (printEpoch) {   // --time read in the clock's own time zone, as UTC seconds
        printf("%lld\n", (long long)Sim::utcNow());
        return 0;
    }
    setupLikeTheClock();
    if (g_calibration) {   // as the web page's Calibrate switch does
        DisplayManager::setCalibrationPattern(g_calibration == 2);
        DisplayManager::setMode(DisplayMode::Calibration);
    }

    uint16_t* fb = nullptr;
    std::vector<uint16_t> frame((size_t)TFT_WIDTH * TFT_HEIGHT);
    if (live) {
        auto start = std::chrono::steady_clock::now();
        auto lastWrite = start - std::chrono::seconds(1);
        unsigned long simStart = millis();
        for (;;) {
            loopLikeTheClock();
            auto nowReal = std::chrono::steady_clock::now();
            if (nowReal - lastWrite >= std::chrono::milliseconds(50) && DisplayManager::copyFrame(frame.data())) {   // ~20 pictures a second is plenty for the viewer
                lastWrite = nowReal;
                writeBmp(outDir + "/latest.bmp", frame.data());
                FILE* t = fopen((outDir + "/latest.txt.tmp").c_str(), "wb");
                if (t) {
                    std::string line = localStamp() + "\n" + elementsText(DisplayManager::getCurrentMessageElements()) + "\n";
                    fwrite(line.data(), 1, line.size(), t); fclose(t);
                    stdfs::rename(outDir + "/latest.txt.tmp", outDir + "/latest.txt", ec);
                }
            }
            double simElapsed = (millis() - simStart) / 1000.0;
            auto target = start + std::chrono::duration<double>(simElapsed / speed);
            std::this_thread::sleep_until(target);
        }
    }

    std::vector<MatchTarget> targets;
    if (!matchDir.empty()) targets = loadTargets(matchDir);
    FILE* list = fopen((outDir + "/slides.txt").c_str(), "wb");
    std::string lastText; int n = 0; uint32_t lastId = 0;
    FILE* diffOut = g_frameDiffPath.empty() ? nullptr : fopen(g_frameDiffPath.c_str(), "wb");
    std::vector<uint16_t> prevFrame;
    unsigned long endMs = millis() + (unsigned long)(seconds * 1000);
    while (millis() < endMs) {
        loopLikeTheClock();
        std::vector<OnScreenElement> shown; int scrollX = 0;
        if (!DisplayManager::copyFrame(frame.data(), &shown, &scrollX)) continue;
        std::string text = elementsText(shown);
        for (auto& t : targets) {
            if (t.best == 0 || t.text != text) continue;
            t.textSeen = true;
            if (abs(scrollX - t.scrollX) < abs(t.nearestScroll - t.scrollX)) t.nearestScroll = scrollX;
            if (scrollX != t.scrollX) continue;
            int d = pixelDiff(t.rgb, frame.data());
            if (t.best < 0 || d < t.best) { t.best = d; t.bestFrame = frame; }
        }
        if (diffOut) {   // --frame-diff: what changed in the window since the previous frame
            if (!prevFrame.empty() && prevFrame != frame) {
                int x0 = std::max(0, g_cfg.calibX), y0 = std::max(0, g_cfg.calibY);
                int x1 = std::min<int>(TFT_WIDTH, g_cfg.calibX + g_cfg.calibW), y1 = std::min<int>(TFT_HEIGHT, g_cfg.calibY + g_cfg.calibH);
                int rows = 0, first = -1, last = -1; long px = 0;
                for (int y = y0; y < y1; y++) {
                    const uint16_t* a = &frame[(size_t)y * TFT_WIDTH]; const uint16_t* b = &prevFrame[(size_t)y * TFT_WIDTH];
                    int l = -1, r = -1;
                    for (int x = x0; x < x1; x++) if (a[x] != b[x]) { if (l < 0) l = x; r = x; }
                    if (l < 0) continue;
                    rows++; px += r - l + 1; if (first < 0) first = y; last = y;
                }
                bool image = false;
                for (auto& e : shown) image |= e.type != OnScreenElementType::TEXT;
                if (rows) fprintf(diffOut, "%d\t%d\t%ld\t%d\n", rows, last - first + 1, px, image ? 1 : 0);
            }
            prevFrame = frame;
        }
        uint32_t id = MessageAggregator::getCurrentMessageId();
        if (text != lastText || (g_everyMessage && id != lastId)) {   // a new slide is on screen: save its first frame
            lastText = text; lastId = id;
            char name[32]; snprintf(name, sizeof(name), "%03d.bmp", n);
            writeBmp(outDir + "/" + name, frame.data());
            fprintf(list, "%03d\t%s\t%d\t%s\n", n, localStamp().c_str(), scrollX, text.c_str());
            fflush(list);
            n++;
        }
    }
    fclose(list);
    if (diffOut) fclose(diffOut);
    (void)fb;
    printf("%d slide(s) -> %s/slides.txt\n", n, outDir.c_str());
    if (!targets.empty()) {
        FILE* m = fopen((matchDir + "/match.txt").c_str(), "wb");
        int same = 0;
        for (auto& t : targets) {
            const char* result = t.best == 0 ? "SAME" : t.best > 0 ? "DIFFERENT" : t.textSeen ? "NO-FRAME-AT-SCROLL" : "TEXT-NEVER-SHOWN";
            if (t.best == 0) same++;
            fprintf(m, "%s\t%s\t%d\t%d\t%d\t%s\n", t.file.c_str(), result, t.best, t.scrollX,
                    t.textSeen ? t.nearestScroll : 0, t.text.c_str());
            if (t.best > 0) writeBmp(matchDir + "/sim_" + t.file, t.bestFrame.data());
        }
        fclose(m);
        printf("match: %d of %zu screenshots identical -> %s/match.txt\n", same, targets.size(), matchDir.c_str());
    }
    return 0;
}
