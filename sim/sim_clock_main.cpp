// =============================================================================
// sim_clock_main.cpp - the full simulator (jcclock): the clock's whole program
// (src/*.cpp unchanged, main.cpp's setup()/loop() included) on the PC, in real
// time, with its web pages, Wi-Fi, updates, backups, factory reset, restarts.
// See sim/README.md.
//
//   jcclock --fs DIR [--state DIR] [--home-port 8080] [--ap-port 8081]
//           [--control-port 8090] [--verbose]
//
// Two processes, as on the clock two layers:
//  - the SUPERVISOR (this program as started) plays the power supply and the
//    bootloader: which firmware slot to run (rolling back one that restarted
//    while still on trial), which build that is (the version in the slot:
//    C:/pio3/jc_clean/sim/versions/<version>/jcclock.exe, else this one if it is
//    that version), and starting it again after a restart, crash or watchdog
//    reset with the matching reset reason;
//  - the CHILD is the simulated clock: setup(), then loop() for ever.
// The control port (127.0.0.1, child) is for tests, not part of the clock:
//   GET /state  POST /power-cut  POST /reset  POST /stop
//   GET|POST /world (sim_wifi.cpp)  POST /light?lux=N[&sensor=0|1]
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <Wire.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <random>
#include "Config.h"
#include "Storage.h"
#include "Version.h"
#include "Display.h"
#include "OnScreenElement.h"
#include "sim_clock.h"

namespace stdfs = std::filesystem;
void setup();
void loop();

// Exit codes child -> supervisor.
enum : int { EXIT_STOP = 0, EXIT_RESTART = 3, EXIT_POWER_CUT = 40, EXIT_WDT = 41, EXIT_EXT_RESET = 42 };

std::string Sim::stateDir;
std::string Sim::firmwareVersion = FIRMWARE_VERSION;
static int g_controlPort = 8090;
TwoWire Wire;
MDNSResponder MDNS;

// ── One lock for the main loop and the web server's handlers ──────────────────
std::recursive_mutex& Sim::clockLock() { static std::recursive_mutex m; return m; }

// ── FreeRTOS locks that really lock (clock_stubs/freertos/semphr.h) ───────────
namespace {
struct SimMutex { std::recursive_timed_mutex m; std::atomic<void*> owner{nullptr}; int depth = 0; };
thread_local int t_taskId = 0;
std::atomic<int> g_nextTaskId{1};
std::mutex g_critical;
}
TaskHandle_t xTaskGetCurrentTaskHandle() {
    if (!t_taskId) t_taskId = g_nextTaskId++;
    return (TaskHandle_t)(intptr_t)t_taskId;
}
static std::thread::id g_mainThread;
const char* pcTaskGetName(TaskHandle_t) { return std::this_thread::get_id() == g_mainThread ? "loopTask" : "async_tcp"; }
SemaphoreHandle_t xSemaphoreCreateMutex() { return new SimMutex(); }
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return new SimMutex(); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t wait) {
    auto* s = (SimMutex*)h;
    bool ok = wait == portMAX_DELAY ? (s->m.lock(), true) : s->m.try_lock_for(std::chrono::milliseconds(wait));
    if (!ok) return pdFALSE;
    s->owner = xTaskGetCurrentTaskHandle(); s->depth++;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t h) {
    auto* s = (SimMutex*)h;
    if (--s->depth == 0) s->owner = nullptr;
    s->m.unlock();
    return pdTRUE;
}
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t h, TickType_t w) { return xSemaphoreTake(h, w); }
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t h) { return xSemaphoreGive(h); }
TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t h) { return ((SimMutex*)h)->owner.load(); }
void sim_enter_critical() { g_critical.lock(); }
void sim_exit_critical() { g_critical.unlock(); }

// ── Memory, as the clock reports it ───────────────────────────────────────────
// Internal RAM: the clock's own numbers (/api/status, 2026-11-05, 2.4 h up).
// PSRAM: counted (esp_heap_caps.h in clock_stubs) against the clock's 8 MB,
// minus what its system takes, and refused a little earlier than the clock
// would (SIM_PSRAM_MARGIN) - so the simulator runs out first.
uint32_t EspClass::getFreeHeap() { return 172088; }
uint32_t EspClass::getMaxAllocHeap() { return 90100; }
uint32_t EspClass::getMinFreeHeap() { return 118408; }
size_t sim_psram_used();   // sim_heap.cpp, with getFreePsram()
uint32_t EspClass::getPsramSize() { return 8u * 1024u * 1024u; }

// ── RTC memory: kept through a restart, not a power cut (esp_attr.h) ─────────
__attribute__((section(".rtcni$a"))) volatile char g_rtcStart[16] = {1};
__attribute__((section(".rtcni$c"))) volatile char g_rtcEnd[16] = {1};
static std::string rtcFile() { return Sim::stateDir + "/rtc.bin"; }
static void saveRtc() {
    const char* a = (const char*)g_rtcStart + sizeof(g_rtcStart);
    const char* b = (const char*)g_rtcEnd;
    if (b <= a) return;
    std::ofstream f(rtcFile(), std::ios::binary); f.write(a, b - a);
}
static void loadRtc() {
    char* a = (char*)g_rtcStart + sizeof(g_rtcStart);
    char* b = (char*)g_rtcEnd;
    std::ifstream f(rtcFile(), std::ios::binary);
    if (!f || b <= a) return;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() == (size_t)(b - a)) memcpy(a, s.data(), s.size());
}

// ── Restarts ──────────────────────────────────────────────────────────────────
static esp_reset_reason_t g_resetReason = ESP_RST_POWERON;
esp_reset_reason_t esp_reset_reason() { return g_resetReason; }
void simRestart(int) { saveRtc(); fflush(stderr); _exit(EXIT_RESTART); }
void EspClass::restart() { simRestart(ESP_RST_SW); for (;;) {} }

// ── The loop watchdog (main.cpp arms it: WDT_TIMEOUT_SEC) ─────────────────────
static std::atomic<long long> g_lastFeedMs{0};
static std::atomic<uint32_t> g_wdtTimeoutMs{0};
static long long realMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t* c) { g_wdtTimeoutMs = c->timeout_ms; return ESP_OK; }
esp_err_t esp_task_wdt_add(void*) { g_lastFeedMs = realMs(); return ESP_OK; }
esp_err_t esp_task_wdt_reset() { g_lastFeedMs = realMs(); return ESP_OK; }
static void watchdogThread() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        uint32_t t = g_wdtTimeoutMs;
        if (t && g_lastFeedMs && realMs() - g_lastFeedMs > (long long)t) {
            fprintf(stderr, "[sim] task watchdog: loop() did not come back within %u s - restarting\n", t / 1000);
            saveRtc(); fflush(stderr); _exit(EXIT_WDT);
        }
    }
}
static LONG WINAPI onCrash(EXCEPTION_POINTERS* e) {
    fprintf(stderr, "[sim] crash (exception 0x%08lX) - restarting, as the clock's panic handler does\n", e->ExceptionRecord->ExceptionCode);
    saveRtc(); fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;   // the supervisor sees the crash code
}

// ── The control port (tests only) ─────────────────────────────────────────────
float sim_parse_float(const std::string& q, const char* key, float def) {
    size_t p = q.find(std::string(key) + "=");
    return p == std::string::npos ? def : (float)atof(q.c_str() + p + strlen(key) + 1);
}
bool simOtaStateJson(std::string& out);
static std::string controlAnswer(const std::string& method, const std::string& path, const std::string& query, const std::string& body, int& code) {
    code = 200;
    std::lock_guard<std::recursive_mutex> lk(Sim::clockLock());
    if (path == "/state") {
        JsonDocument d;
        d["version"] = Sim::firmwareVersion; d["millis"] = millis(); d["resetReason"] = (int)g_resetReason;
        d["wifi"] = WiFi.status(); d["sta"] = Sim::staUp(); d["ip"] = Sim::staIpString(); d["ssid"] = WiFi.SSID();
        d["ap"] = Sim::apUp(); d["internet"] = Sim::wifiInternet(); d["lux"] = Sim::roomLux;
        d["psramUsed"] = sim_psram_used();
        std::string screen;   // what the display shows now, as the screen simulator writes it
        for (auto& e : DisplayManager::getCurrentMessageElements()) {
            if (!screen.empty()) screen += " | ";
            screen += e.type == OnScreenElementType::TEXT ? std::string(e.content.c_str()) : "[" + std::string(e.content.c_str()) + "]";
        }
        d["screen"] = screen;
        std::string ota; if (simOtaStateJson(ota)) d["ota"] = serialized(ota);
        std::string s; serializeJson(d, s); return s;
    }
    if (path == "/world" && method == "GET") return Sim::worldJson();
    if (path == "/world" && method == "POST") { std::string err; if (!Sim::setWorldJson(body, err)) { code = 400; return err; } return "ok"; }
    if (path == "/light") { Sim::roomLux = sim_parse_float(query, "lux", Sim::roomLux); if (query.find("sensor=0") != std::string::npos) Sim::lightSensorPresent = false; return "ok"; }
    if (path == "/power-cut") { fflush(stderr); _exit(EXIT_POWER_CUT); }
    if (path == "/reset") { saveRtc(); fflush(stderr); _exit(EXIT_EXT_RESET); }   // the reset button: RTC memory kept
    if (path == "/stop") { fflush(stderr); _exit(EXIT_STOP); }
    code = 404; return "unknown";
}
static void controlServer() {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)g_controlPort); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOL yes = TRUE; setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&yes, sizeof(yes));
    // The run before this one (a restart) may still hold the port for a moment.
    int tries = 0;
    while (bind(ls, (sockaddr*)&a, sizeof(a)) != 0) {
        if (++tries > 75) { fprintf(stderr, "[sim] control port %d in use\n", g_controlPort); return; }
        Sleep(200);
    }
    if (listen(ls, 8)) { fprintf(stderr, "[sim] control port %d: listen failed\n", g_controlPort); return; }
    for (;;) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        std::string buf; char tmp[8192]; int n; size_t he;
        while ((he = buf.find("\r\n\r\n")) == std::string::npos && (n = recv(c, tmp, sizeof(tmp), 0)) > 0) buf.append(tmp, n);
        if (he == std::string::npos) { closesocket(c); continue; }
        size_t cl = 0, p = buf.find("Content-Length:"); if (p == std::string::npos) p = buf.find("content-length:");
        if (p != std::string::npos && p < he) cl = strtoul(buf.c_str() + p + 15, nullptr, 10);
        while (buf.size() < he + 4 + cl && (n = recv(c, tmp, sizeof(tmp), 0)) > 0) buf.append(tmp, n);
        std::string first = buf.substr(0, buf.find("\r\n"));
        std::string method = first.substr(0, first.find(' '));
        std::string target = first.substr(first.find(' ') + 1); target = target.substr(0, target.find(' '));
        std::string path = target.substr(0, target.find('?')), query = target.find('?') == std::string::npos ? "" : target.substr(target.find('?') + 1);
        int code; std::string body = controlAnswer(method, path, query, buf.substr(he + 4, cl), code);
        std::string out = "HTTP/1.1 " + std::to_string(code) + " X\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\n\r\n" + body;
        send(c, out.data(), (int)out.size(), 0);
        closesocket(c);
    }
}

// ── The child: the clock ──────────────────────────────────────────────────────
void simSetRunningSlot(int s);
void simDefaultWorld(const char* const ssids[], const char* const passwords[], int n);
uint32_t& sim_random_seed();

static int runClock() {
    g_mainThread = std::this_thread::get_id();
    g_resetReason = (esp_reset_reason_t)atoi(getenv("JC_RESET") ? getenv("JC_RESET") : "1");
    simSetRunningSlot(atoi(getenv("JC_SLOT") ? getenv("JC_SLOT") : "0"));
    if (g_resetReason != ESP_RST_POWERON) loadRtc();
    SetUnhandledExceptionFilter(onCrash);
    sim_random_seed() = (uint32_t)std::random_device{}();
    Sim::setTimezone("UTC0");
    // No PC time: the system clock counts from 1970 until the clock's own
    // NTPManager.cpp sets it from a real time server (sim_ntp.cpp).
    Sim::internetUp = Sim::wifiInternet;
    Sim::liveNetwork = true;   // downloads go out for real, at their real speed
    Sim::fsFull = true;        // LittleFS's 4 MB can run full
    Sim::startTiming((uint32_t)std::random_device{}());

    // The networks around: <state>/world.json, else every known network.
    Sim::loadWorld(Sim::stateDir + "/world.json");
    {
        Storage::begin();
        AppConfig probe; Storage::loadConfig(probe);
        const char* s[WIFI_NETWORKS]; const char* p[WIFI_NETWORKS];
        for (int i = 0; i < WIFI_NETWORKS; i++) { s[i] = wifiSsidAt(probe, i); p[i] = wifiPasswordAt(probe, i); }
        simDefaultWorld(s, p, WIFI_NETWORKS);
    }
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);   // before any socket (the control port's too)
    std::thread(controlServer).detach();
    std::thread(watchdogThread).detach();
    fprintf(stderr, "[sim] JewishClock %s (slot %d, reset reason %d)\n", Sim::firmwareVersion.c_str(), atoi(getenv("JC_SLOT") ? getenv("JC_SLOT") : "0"), (int)g_resetReason);

    const long long start = realMs();
    {
        std::lock_guard<std::recursive_mutex> lk(Sim::clockLock());
        setup();
    }
    for (;;) {
        {
            std::lock_guard<std::recursive_mutex> lk(Sim::clockLock());
            Sim::wifiPumpEvents();
            loop();
        }
        // Real time: millis() follows the PC's clock (a delay() in the clock's
        // code may have run it ahead - then it waits for the PC to catch up).
        long long target = realMs() - start;
        if (target > (long long)millis()) Sim::advanceMillis((unsigned long)(target - (long long)millis()));
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ── The supervisor: power supply and bootloader ───────────────────────────────
struct SimOta { int boot = 0; std::string version[2]; std::string state[2]; };
bool simOtaLoad(SimOta& o);
void simOtaSave(const SimOta& o);
bool simOtaStateJson(std::string& out) {
    std::ifstream f(Sim::stateDir + "/flash/ota.json", std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}

static std::string exeFor(const std::string& version, const std::string& self) {
    if (version == FIRMWARE_VERSION) return self;
    std::string p = "C:/pio3/jc_clean/sim/versions/" + version + "/jcclock.exe";
    return stdfs::exists(p) ? p : std::string();
}

static int supervise(int argc, char** argv) {
    char selfBuf[MAX_PATH]; GetModuleFileNameA(nullptr, selfBuf, MAX_PATH);
    std::string self = selfBuf;
    std::string cmdTail;
    for (int i = 1; i < argc; i++) cmdTail += std::string(" \"") + argv[i] + "\"";
    int reason = ESP_RST_POWERON;
    stdfs::remove(Sim::stateDir + "/rtc.bin");   // a fresh start = power on
    for (;;) {
        // Bootloader: the slot to run.
        SimOta o;
        if (!simOtaLoad(o)) { o.boot = 0; o.version[0] = FIRMWARE_VERSION; o.state[0] = "valid"; simOtaSave(o); }
        int slot = o.boot;
        if (o.state[slot] == "new") { o.state[slot] = "pending"; simOtaSave(o); }
        else if (o.state[slot] == "pending") {
            // Restarted while on trial without confirming: the bootloader rolls back.
            fprintf(stderr, "[sim] bootloader: %s restarted while on trial - rolling back\n", o.version[slot].c_str());
            o.state[slot] = "aborted"; o.boot = 1 - slot; simOtaSave(o); slot = o.boot;
        }
        std::string exe = exeFor(o.version[slot], self);
        if (exe.empty()) {
            fprintf(stderr, "[sim] no simulator build for version \"%s\" (C:/pio3/jc_clean/sim/versions/<version>/jcclock.exe - sim/build_clock.py)\n", o.version[slot].c_str());
            return 2;
        }
        std::string cmd = "\"" + exe + "\"" + cmdTail;
        SetEnvironmentVariableA("JC_CHILD", "1");
        SetEnvironmentVariableA("JC_SLOT", std::to_string(slot).c_str());
        SetEnvironmentVariableA("JC_RESET", std::to_string(reason).c_str());
        STARTUPINFOA si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
        std::vector<char> line(cmd.begin(), cmd.end()); line.push_back(0);
        if (!CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) { fprintf(stderr, "[sim] cannot start %s\n", exe.c_str()); return 2; }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        if (code == EXIT_STOP) return 0;
        if (code == EXIT_RESTART) reason = ESP_RST_SW;
        else if (code == EXIT_EXT_RESET) reason = ESP_RST_EXT;
        else if (code == EXIT_WDT) reason = ESP_RST_TASK_WDT;
        else if (code == EXIT_POWER_CUT) { reason = ESP_RST_POWERON; stdfs::remove(Sim::stateDir + "/rtc.bin"); fprintf(stderr, "[sim] power cut\n"); }
        else { reason = ESP_RST_PANIC; fprintf(stderr, "[sim] crashed (0x%08lX)\n", (unsigned long)code); }
        Sleep(200);
    }
}

int main(int argc, char** argv) {
    std::string fsDir = "C:/pio3/jc_clean/sim/clock/fs", stateDir;
    Sim::quiet = true;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); } return argv[++i]; };
        if (a == "--fs") fsDir = next();
        else if (a == "--state") stateDir = next();
        else if (a == "--home-port") Sim::homePort = atoi(next().c_str());
        else if (a == "--ap-port") Sim::apPort = atoi(next().c_str());
        else if (a == "--control-port") g_controlPort = atoi(next().c_str());
        else if (a == "--verbose") Sim::quiet = false;
        else if (a == "--http-cache") Sim::httpCacheDir = next();
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    Sim::fsRoot = fsDir;
    Sim::stateDir = stateDir.empty() ? (stdfs::path(fsDir).parent_path() / "state").string() : stateDir;
    if (Sim::httpCacheDir == "sim_http_cache") Sim::httpCacheDir = "C:/pio3/jc_clean/sim/work/http_cache";
    std::error_code ec; stdfs::create_directories(Sim::stateDir, ec); stdfs::create_directories(fsDir, ec);
    if (getenv("JC_CHILD")) return runClock();
    return supervise(argc, argv);
}
