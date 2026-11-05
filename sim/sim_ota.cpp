// =============================================================================
// sim_ota.cpp - the full simulator's firmware slots (jcclock): the two app
// partitions and their states, kept as the ESP-IDF bootloader keeps them with
// rollback on. <state>/flash/: slot0.bin, slot1.bin (the firmware images
// uploaded - slot 0 may have none: the version first started), ota.json
// (which slot boots, and each one's version and state).
//
// The bootloader's part (which slot to run, rolling back a firmware that
// restarted while still on trial) is in sim_clock_main.cpp's supervisor; the
// app's part (Update, esp_ota_*) is here. A slot runs as the simulator build
// of its version (sim/README.md: versions\<version>\jcclock.exe), so an update
// really moves to the new version's code.
// =============================================================================

#include <Update.h>
#include <esp_ota_ops.h>
#include <ArduinoJson.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include "sim_clock.h"

namespace stdfs = std::filesystem;

std::string simFlashDir() { return Sim::stateDir + "/flash"; }

// ── ota.json ─────────────────────────────────────────────────────────────────
struct SimOta { int boot = 0; std::string version[2]; std::string state[2]; };   // state: valid, new, pending, invalid, aborted, "" (empty slot)

bool simOtaLoad(SimOta& o) {
    std::ifstream f(simFlashDir() + "/ota.json", std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    JsonDocument d;
    if (deserializeJson(d, ss.str())) return false;
    o.boot = d["boot"] | 0;
    for (int i = 0; i < 2; i++) { o.version[i] = d["slots"][i]["version"] | ""; o.state[i] = d["slots"][i]["state"] | ""; }
    return true;
}
void simOtaSave(const SimOta& o) {
    std::error_code ec; stdfs::create_directories(simFlashDir(), ec);
    JsonDocument d;
    d["boot"] = o.boot;
    for (int i = 0; i < 2; i++) { d["slots"][i]["version"] = o.version[i]; d["slots"][i]["state"] = o.state[i]; }
    std::string s; serializeJsonPretty(d, s);
    std::ofstream f(simFlashDir() + "/ota.json.tmp", std::ios::binary); f << s; f.close();
    stdfs::rename(simFlashDir() + "/ota.json.tmp", simFlashDir() + "/ota.json", ec);
}

// The JCVER= marker in a firmware image (scripts/gen_version.py), as
// utils/make_update_package.py reads it.
std::string simImageVersion(const std::string& bytes) {
    size_t p = bytes.find("JCVER=");
    if (p == std::string::npos) return "";
    size_t e = bytes.find(';', p);
    return e == std::string::npos || e - p > 50 ? "" : bytes.substr(p + 6, e - p - 6);
}

static int g_runningSlot = 0;   // set by sim_clock_main.cpp from the supervisor
void simSetRunningSlot(int s) { g_runningSlot = s; }
static esp_partition_t g_parts[2] = { {0, "app0", 0x300000}, {1, "app1", 0x300000} };

const esp_partition_t* esp_ota_get_running_partition() { return &g_parts[g_runningSlot]; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t* p, esp_ota_img_states_t* st) {
    SimOta o; if (!p || !simOtaLoad(o)) return ESP_FAIL;
    const std::string& s = o.state[p->slot];
    *st = s == "valid" ? ESP_OTA_IMG_VALID : s == "new" ? ESP_OTA_IMG_NEW : s == "pending" ? ESP_OTA_IMG_PENDING_VERIFY
        : s == "invalid" ? ESP_OTA_IMG_INVALID : s == "aborted" ? ESP_OTA_IMG_ABORTED : ESP_OTA_IMG_UNDEFINED;
    return s.empty() ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback() {
    SimOta o; if (!simOtaLoad(o)) return ESP_FAIL;
    o.state[g_runningSlot] = "valid";
    simOtaSave(o);
    return ESP_OK;
}
void simRestart(int resetReason);   // sim_clock_main.cpp
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot() {
    SimOta o; if (!simOtaLoad(o)) return ESP_FAIL;
    int other = 1 - g_runningSlot;
    if (o.state[other] != "valid") return ESP_FAIL;   // nothing to go back to: returns, as on the clock
    o.state[g_runningSlot] = "invalid";
    o.boot = other;
    simOtaSave(o);
    simRestart(3);   // ESP_RST_SW
    return ESP_OK;
}

// FlashStats.cpp's wrapped partition calls - nothing in the simulator calls them.
extern "C" esp_err_t __real_esp_partition_read(const esp_partition_t*, size_t, void*, size_t) { return ESP_OK; }
extern "C" esp_err_t __real_esp_partition_write(const esp_partition_t*, size_t, const void*, size_t) { return ESP_OK; }
extern "C" esp_err_t __real_esp_partition_erase_range(const esp_partition_t*, size_t, size_t) { return ESP_OK; }

// ── Update (the Arduino core's UpdateClass) ───────────────────────────────────
UpdateClass Update;
static std::string g_img, g_err = "No Error";
static size_t g_size = 0; static bool g_open = false;

bool UpdateClass::begin(size_t size) {
    if (size == 0 || size > g_parts[0].size) { g_err = "Bad Size Given"; return false; }
    g_img.clear(); g_img.reserve(size); g_size = size; g_open = true; g_err = "No Error";
    return true;
}
size_t UpdateClass::write(uint8_t* data, size_t len) {
    if (!g_open) { g_err = "Not Started"; return 0; }
    if (g_img.size() + len > g_size) len = g_size - g_img.size();
    if (g_img.empty() && len && data[0] != 0xE9) { g_err = "Wrong Magic Byte"; g_open = false; return 0; }
    g_img.append((const char*)data, len);
    return len;
}
bool UpdateClass::end(bool evenIfRemaining) {
    if (!g_open) { g_err = "Not Started"; return false; }
    if (g_img.size() < g_size && !evenIfRemaining) { g_err = "Premature End"; return false; }
    if (g_img.size() < 1024) { g_err = "Premature End"; g_open = false; return false; }
    g_open = false;
    int target = 1 - g_runningSlot;
    std::error_code ec; stdfs::create_directories(simFlashDir(), ec);
    { std::ofstream f(simFlashDir() + "/slot" + std::to_string(target) + ".bin", std::ios::binary); f << g_img; }
    SimOta o; simOtaLoad(o);
    o.version[target] = simImageVersion(g_img);
    o.state[target] = "new";
    o.boot = target;
    simOtaSave(o);
    g_img.clear(); g_img.shrink_to_fit();
    return true;
}
void UpdateClass::abort() { g_open = false; g_img.clear(); g_err = "Aborted"; }
const char* UpdateClass::errorString() { return g_err.c_str(); }
