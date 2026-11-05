// =============================================================================
// Updater.cpp - see Updater.h for what this does and why.
// =============================================================================

#include "Updater.h"
#include "Config.h"
#include "Version.h"
#include "DiagnosticLog.h"
#include "StringTools.h"

#include <LittleFS.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <atomic>
#include <vector>

namespace {

// === TUNABLES ===  (rule: see Config.h header)
// Trial of newly installed firmware: it must join Wi-Fi within the deadline
// and then stay connected (and alive) this long before it confirms itself.
// Internet is deliberately not required - Wi-Fi is all it takes to receive
// the next update, and the parents' internet may simply be down that day.
const unsigned long TRIAL_CONNECT_DEADLINE_MS = 5UL * 60UL * 1000UL;
const unsigned long TRIAL_STABLE_MS           = 60UL * 1000UL;

const size_t MIN_FIRMWARE_BYTES = 100000;   // real builds are ~1.3 MB
const uint8_t ESP_IMAGE_MAGIC   = 0xE9;

// Waiting area for a package's changed web files, and the list of ALL the
// web files of its version (the manifest).
const char* PENDING_DIR   = FS_UPDATE_DIR;
const char* MANIFEST_PATH = FS_UPDATE_DIR "/manifest.txt";
// The waiting area ran out of room (an update changing more than the
// pictures' room check leaves space for - ImageLibrary::hasRoomFor()).
// Nothing is installed: the firmware comes last in a package.
const char* NO_ROOM_MESSAGE = "not enough room on the clock for this update's changed files - "
                              "removing some event pictures makes room";
// Never written or removed by a package: the clock's own data -
// isClockDataPath() in Config.h. Every other file the package's version
// doesn't list is removed on install, and folders left empty with it.

// ── Package format (.jcupdate, written by utils/make_update_package.py) ──────
//   "JCUPD1" | u8 versionLen | version | u16 entryCount
//   entryCount x ( u8 type (1=web file, 2=firmware) | u8 pathLen | path |
//                  u32 size | u32 crc32 | size bytes )
//   "JCEND!"
// All integers little-endian. crc32 = standard CRC-32 (same as Python zlib).
enum class PState : uint8_t { MAGIC, VERLEN, VERSION, COUNT, E_TYPE, E_PATHLEN, E_PATH, E_SIZE, E_CRC, E_DATA, TRAILER, DONE, FAILED };
enum class Mode   : uint8_t { NONE, PLAIN_FIRMWARE, PACKAGE };

Mode     s_mode = Mode::NONE;
size_t   s_total = 0;
String   s_message;
bool     s_failed = false;

// Upload under way (AsyncTCP task writes, main loop reads - see
// uploadInProgress()). Stays set after a successful upload: the restart
// follows.
std::atomic<bool>     s_uploading{false};
std::atomic<uint32_t> s_lastChunkMs{0};
// Data still expected: from uploadBegin() until uploadEnd() or a failure.
// An upload that stops arriving (browser closed, PC's Wi-Fi switched) is
// cleaned up by loop() - see there.
std::atomic<bool>     s_receiving{false};
// Milliseconds since the last chunk. Signed: the AsyncTCP task can store a
// chunk time just after the caller read millis(), and an unsigned difference
// then wrapped to ~49 days - "stalled" in the middle of a running upload
// (seen 2026-10-18: an upload rejected 5.6 s in).
int32_t msSinceLastChunk(uint32_t now) { return (int32_t)(now - s_lastChunkMs.load()); }
// === TUNABLES ===
// No data for this long = the upload was abandoned (browser closed, Wi-Fi
// lost) - the screen goes back to normal.
const uint32_t UPLOAD_STALLED_MS = 10000;

// Package parser state (only touched by the upload handler, one upload at a time).
PState   s_ps = PState::MAGIC;
uint8_t  s_fbuf[160];
size_t   s_fneed = 0, s_fhave = 0;
String   s_pkgVersion;
uint16_t s_entryCount = 0, s_entriesDone = 0;
uint8_t  s_eType = 0;
String   s_ePath;
uint32_t s_eSize = 0, s_eCrcWant = 0, s_eCrc = 0, s_eRemain = 0;
File     s_eFile;
bool     s_eSkip = false;          // this web file is unchanged: checked, not stored
uint16_t s_staged = 0;             // web files that changed (stored in the waiting area)
bool     s_haveFirmware = false;
bool     s_fwBegun = false;
std::vector<String> s_webPaths;

// Trial state (main loop only).
bool          s_trial = false;
unsigned long s_bootMs = 0, s_connectedSinceMs = 0;
bool          s_pendingChecked = false;

// ── helpers ──────────────────────────────────────────────────────────────────

uint32_t crc32Update(uint32_t crc, const uint8_t* p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

uint32_t le32(const uint8_t* b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }

void removeTree(const String& path) {
    File dir = LittleFS.open(path);
    if (!dir) return;
    if (!dir.isDirectory()) { dir.close(); LittleFS.remove(path); return; }
    std::vector<String> children;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        children.push_back(String(f.path()));
        f.close();
    }
    dir.close();
    for (const String& c : children) removeTree(c);
    LittleFS.rmdir(path);
}

// (isClockDataPath() checks "/upd" as a folder - "/upd/..." only, not every
// name starting with "/upd": "/update.html" was refused that way, 2026-10-17.)
bool isProtected(const String& p) { return isClockDataPath(p); }

// Whether the clock already has exactly this web file (size and CRC-32) -
// then a package doesn't store it again (2026-10-21): an update needs room
// only for what changed.
bool sameAsInstalled(const String& path, uint32_t size, uint32_t crc) {
    File f = LittleFS.open(path, "r");
    if (!f || f.isDirectory() || f.size() != size) { if (f) f.close(); return false; }
    uint8_t buf[512];
    uint32_t c = 0;
    size_t n;
    while ((n = f.read(buf, sizeof(buf))) > 0) c = crc32Update(c, buf, n);
    f.close();
    return c == crc;
}

void fail(const String& why) {
    if (s_failed) return;
    s_failed = true;
    s_uploading = false;
    s_receiving = false;
    s_ps = PState::FAILED;
    s_message = why;
    if (s_eFile) s_eFile.close();
    if (s_fwBegun) { Update.abort(); s_fwBegun = false; }
    removeTree(PENDING_DIR);
    DiagnosticLog::record("OTA", "update rejected: %s", why.c_str());
}

void need(PState next, size_t n) { s_ps = next; s_fneed = n; s_fhave = 0; }

void nextEntryOrTrailer() {
    if (s_entriesDone >= s_entryCount) need(PState::TRAILER, 6);
    else need(PState::E_TYPE, 1);
}

void finishEntry() {
    if (s_eCrc != s_eCrcWant) { fail("damaged file in the update (" + s_ePath + ") - please download it again"); return; }
    if (s_eType == 1) {
        if (!s_eSkip) { s_eFile.close(); s_staged++; }
        s_webPaths.push_back(s_ePath);   // the full list: what's not in it gets removed on install
    } else {
        if (!Update.end(true)) { fail("the firmware in the update could not be installed (" + String(Update.errorString()) + ")"); return; }
        s_fwBegun = false;
        s_haveFirmware = true;
    }
    s_entriesDone++;
    nextEntryOrTrailer();
}

void startEntry() {
    s_eCrc = 0;
    s_eRemain = s_eSize;
    if (s_eType == 1) {
        if (!s_ePath.startsWith("/") || s_ePath.indexOf("..") >= 0 || isProtected(s_ePath)) { fail("the update contains a file it may not replace: " + s_ePath); return; }
        s_eSkip = sameAsInstalled(s_ePath, s_eSize, s_eCrcWant);
        if (!s_eSkip) {
            s_eFile = LittleFS.open(String(PENDING_DIR) + s_ePath, "w", true);   // true: create folders
            if (!s_eFile) { fail(NO_ROOM_MESSAGE); return; }
        }
    } else if (s_eType == 2) {
        if (s_haveFirmware || s_eSize < MIN_FIRMWARE_BYTES) { fail("the firmware in the update is not valid"); return; }
        if (!Update.begin(s_eSize)) { fail("the firmware could not be started (" + String(Update.errorString()) + ")"); return; }
        s_fwBegun = true;
    } else {
        fail("unknown kind of entry in the update file");
        return;
    }
    s_ps = PState::E_DATA;
    if (s_eRemain == 0) finishEntry();
}

void writeEntryData(const uint8_t* p, size_t n) {
    if (s_eType == 2 && s_eRemain == s_eSize && n && p[0] != ESP_IMAGE_MAGIC) { fail("the firmware in the update is not valid"); return; }
    s_eCrc = crc32Update(s_eCrc, p, n);
    if (s_eType == 1) {
        if (!s_eSkip && s_eFile.write(p, n) != n) { fail(NO_ROOM_MESSAGE); return; }
    } else {
        if (Update.write(const_cast<uint8_t*>(p), n) != n) { fail("writing the firmware failed (" + String(Update.errorString()) + ")"); return; }
    }
}

void onField() {
    switch (s_ps) {
        case PState::MAGIC:
            if (memcmp(s_fbuf, "JCUPD1", 6) != 0) { fail("this is not a clock update file"); return; }
            need(PState::VERLEN, 1); break;
        case PState::VERLEN:
            if (s_fbuf[0] == 0 || s_fbuf[0] > 40) { fail("the update file is damaged"); return; }
            need(PState::VERSION, s_fbuf[0]); break;
        case PState::VERSION:
            s_pkgVersion = String((const char*)s_fbuf).substring(0, s_fhave);
            need(PState::COUNT, 2); break;
        case PState::COUNT:
            s_entryCount = (uint16_t)(s_fbuf[0] | (s_fbuf[1] << 8));
            if (s_entryCount == 0) { fail("the update file is empty"); return; }
            nextEntryOrTrailer(); break;
        case PState::E_TYPE:
            s_eType = s_fbuf[0];
            need(PState::E_PATHLEN, 1); break;
        case PState::E_PATHLEN:
            if (s_fbuf[0] == 0 || s_fbuf[0] > 120) { fail("the update file is damaged"); return; }
            need(PState::E_PATH, s_fbuf[0]); break;
        case PState::E_PATH:
            s_ePath = String((const char*)s_fbuf).substring(0, s_fhave);
            need(PState::E_SIZE, 4); break;
        case PState::E_SIZE:
            s_eSize = le32(s_fbuf);
            need(PState::E_CRC, 4); break;
        case PState::E_CRC:
            s_eCrcWant = le32(s_fbuf);
            startEntry(); break;
        case PState::TRAILER:
            if (memcmp(s_fbuf, "JCEND!", 6) != 0) { fail("the update file is incomplete - please download it again"); return; }
            s_ps = PState::DONE; break;
        default: break;
    }
}

void feedPackage(const uint8_t* p, size_t n) {
    while (n && s_ps != PState::DONE && s_ps != PState::FAILED) {
        if (s_ps == PState::E_DATA) {
            size_t take = n < s_eRemain ? n : s_eRemain;
            writeEntryData(p, take);
            if (s_failed) return;
            p += take; n -= take; s_eRemain -= take;
            if (s_eRemain == 0) finishEntry();
            continue;
        }
        size_t take = s_fneed - s_fhave;
        if (take > n) take = n;
        memcpy(s_fbuf + s_fhave, p, take);
        s_fhave += take; p += take; n -= take;
        if (s_fhave == s_fneed) {
            s_fbuf[s_fhave < sizeof(s_fbuf) ? s_fhave : sizeof(s_fbuf) - 1] = 0;
            onField();
        }
    }
}

bool writeManifest() {
    File m = LittleFS.open(MANIFEST_PATH, "w", true);
    if (!m) return false;
    m.print(s_pkgVersion); m.print('\n');
    for (const String& p : s_webPaths) { m.print(p); m.print('\n'); }
    m.close();
    return true;
}

// Puts a package's waiting web files in place - only for the firmware they
// came with. Called once the running firmware is confirmed (not on trial).
void installPendingWebFiles() {
    File m = LittleFS.open(MANIFEST_PATH, "r");
    if (!m) { if (LittleFS.exists(PENDING_DIR)) removeTree(PENDING_DIR); return; }
    String version = m.readStringUntil('\n');
    std::vector<String> paths;
    while (m.available()) {
        String p = m.readStringUntil('\n');
        if (p.length()) paths.push_back(p);
    }
    m.close();

    if (version != FIRMWARE_VERSION) {
        // e.g. the firmware they came with failed its trial and was rolled back.
        DiagnosticLog::record("OTA", "discarded waiting web files of version %s (running %s)", version.c_str(), FIRMWARE_VERSION);
        removeTree(PENDING_DIR);
        return;
    }

    // The changed files (only those are waiting; the rest are already right).
    int moved = 0;
    for (const String& p : paths) {
        String from = String(PENDING_DIR) + p;
        if (!LittleFS.exists(from)) continue;
        int slash = p.lastIndexOf('/');
        if (slash > 0) LittleFS.mkdir(p.substring(0, slash));
        LittleFS.remove(p);
        if (LittleFS.rename(from, p)) moved++;
        else DiagnosticLog::record("OTA", "could not install web file %s", p.c_str());
    }
    removeTree(PENDING_DIR);
    // Every other web file the version doesn't list was removed from Data/ -
    // remove it here too, and folders left empty (not the clock's own data).
    int removed = 0;
    std::vector<String> files, dirs;
    std::vector<String> todo{ "/" };
    while (!todo.empty()) {
        String d = todo.back(); todo.pop_back();
        File dir = LittleFS.open(d);
        if (!dir || !dir.isDirectory()) continue;
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            String fp = f.path();
            if (!isClockDataPath(fp)) {
                if (f.isDirectory()) { dirs.push_back(fp); todo.push_back(fp); }
                else files.push_back(fp);
            }
            f.close();
        }
        dir.close();
    }
    // Only with a believable list: one without the page itself would wipe it.
    bool listOk = false;
    for (const String& p : paths) if (p == "/index.html" || p == "/index.html.gz") listOk = true;   // gzipped since 2026-11-04
    if (!listOk) { DiagnosticLog::record("OTA", "web file list of %s has no /index.html - nothing removed", version.c_str()); files.clear(); dirs.clear(); }
    for (const String& fp : files) {
        bool listed = false;
        for (const String& p : paths) if (p == fp) { listed = true; break; }
        if (!listed && LittleFS.remove(fp)) { removed++; DiagnosticLog::record("OTA", "removed %s (not in version %s)", fp.c_str(), FIRMWARE_VERSION); }
    }
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {   // deepest first
        File d = LittleFS.open(*it);
        bool empty = d && d.isDirectory() && !d.openNextFile();
        if (d) d.close();
        if (empty && LittleFS.rmdir(*it)) DiagnosticLog::record("OTA", "removed empty folder %s", it->c_str());
    }
    DiagnosticLog::record("OTA", "installed %d changed web files (removed %d old) for version %s - restarting", moved, removed, FIRMWARE_VERSION);
    delay(200);
    ESP.restart();
}

} // namespace

// The Arduino core confirms ANY newly installed firmware by itself, before
// setup() even runs (esp32-hal-misc.c, initArduino()), unless the program
// overrides this weak hook to return true. Without it the trial below never
// got a say - and the clock had no rollback protection at all, despite an
// older main.cpp comment assuming it did (found 2026-10-17).
extern "C" bool verifyRollbackLater() { return true; }

// The JCVER= marker utils/make_update_package.py reads out of firmware.bin:
// src/Version.cpp (written by scripts/gen_version.py).

namespace Updater {

void begin() {
    // A real reference, or the linker discards the marker as unused
    // (__attribute__((used)) alone doesn't survive --gc-sections).
    (void)*(volatile const char*)kJcVersionMarker;
    s_bootMs = millis();
    esp_ota_img_states_t st;
    const esp_partition_t* running = esp_ota_get_running_partition();
    s_trial = running && esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    if (s_trial) DiagnosticLog::record("OTA", "new firmware %s on trial: needs Wi-Fi within %lu s, then %lu s up", FIRMWARE_VERSION,
                                       TRIAL_CONNECT_DEADLINE_MS / 1000, TRIAL_STABLE_MS / 1000);
}

void loop() {
    unsigned long now = millis();
    // An upload that stopped halfway (seen 2026-10-18: the PC switched to the
    // clock's own Wi-Fi mid-upload): drop the half-written firmware and web
    // files now - before this, the status said "installing" until the next
    // restart or upload, and the screen waited on the stall timeout.
    if (s_receiving && msSinceLastChunk(now) >= (int32_t)UPLOAD_STALLED_MS) {
        fail("the upload stopped before it was complete - please try again");
    }
    if (s_trial) {
        if (WiFi.status() == WL_CONNECTED) {
            if (!s_connectedSinceMs) s_connectedSinceMs = now ? now : 1;
            if (now - s_connectedSinceMs >= TRIAL_STABLE_MS) {
                esp_ota_mark_app_valid_cancel_rollback();
                s_trial = false;
                DiagnosticLog::record("OTA", "new firmware %s passed its trial - kept", FIRMWARE_VERSION);
            }
        } else {
            s_connectedSinceMs = 0;
        }
        if (s_trial && now - s_bootMs >= TRIAL_CONNECT_DEADLINE_MS + TRIAL_STABLE_MS) {
            DiagnosticLog::record("OTA", "new firmware %s failed its trial (no stable Wi-Fi) - going back to the previous version", FIRMWARE_VERSION);
            delay(200);
            esp_ota_mark_app_invalid_rollback_and_reboot();
            // Only returns if there is no previous version to go back to.
            DiagnosticLog::record("OTA", "no previous firmware to go back to - keeping %s", FIRMWARE_VERSION);
            esp_ota_mark_app_valid_cancel_rollback();
            s_trial = false;
        }
        return;
    }
    if (!s_pendingChecked) {
        s_pendingChecked = true;
        if (LittleFS.exists(PENDING_DIR)) installPendingWebFiles();
    }
}

const char* stateName() {
    if (s_trial) return "trial";
    return LittleFS.exists(PENDING_DIR) ? "installing" : "ok";
}

void uploadBegin(const String& filename, size_t totalBytes) {
    s_mode = Mode::NONE;
    s_total = totalBytes;
    s_failed = false;
    s_message = "";
    s_ps = PState::MAGIC; s_fneed = 6; s_fhave = 0;
    s_pkgVersion = ""; s_entryCount = s_entriesDone = 0;
    // Anything an earlier, unfinished upload left open: an open file can't be
    // deleted ("Failed to unlink ... Has open FD", seen 2026-10-18).
    if (s_eFile) s_eFile.close();
    if (s_fwBegun) Update.abort();
    s_haveFirmware = false; s_fwBegun = false;
    s_webPaths.clear();
    s_staged = 0;
    if (LittleFS.exists(PENDING_DIR)) removeTree(PENDING_DIR);
    s_lastChunkMs = millis();
    s_uploading = true;
    s_receiving = true;
    DiagnosticLog::record("OTA", "upload started: %s, %u bytes", filename.c_str(), (unsigned)totalBytes);
}

bool uploadInProgress() {
    return s_uploading && msSinceLastChunk(millis()) < (int32_t)UPLOAD_STALLED_MS;
}

void uploadChunk(const uint8_t* data, size_t len) {
    s_lastChunkMs = millis();
    if (s_failed || !len) return;
    if (s_mode == Mode::NONE) {
        if (data[0] == ESP_IMAGE_MAGIC) {
            // A plain firmware file: install directly (no web files).
            if (s_total < MIN_FIRMWARE_BYTES) { fail("this is not a valid firmware file"); return; }
            if (!Update.begin(s_total)) { fail("the firmware could not be started (" + String(Update.errorString()) + ")"); return; }
            s_fwBegun = true;
            s_mode = Mode::PLAIN_FIRMWARE;
        } else if (data[0] == 'J') {
            s_mode = Mode::PACKAGE;
        } else {
            fail("this is not a clock update file");
            return;
        }
    }
    if (s_mode == Mode::PLAIN_FIRMWARE) {
        if (Update.write(const_cast<uint8_t*>(data), len) != len) fail("writing the firmware failed (" + String(Update.errorString()) + ")");
    } else {
        feedPackage(data, len);
    }
}

bool uploadEnd() {
    s_receiving = false;
    if (s_failed) return false;
    if (s_mode == Mode::PLAIN_FIRMWARE) {
        // Multipart framing makes the upload a little longer than the file -
        // end(true) accepts the image as long as it is complete and valid.
        if (!Update.end(true)) { fail("the firmware could not be installed (" + String(Update.errorString()) + ")"); return false; }
        s_fwBegun = false;
        s_message = "Update OK. Installing firmware only. Rebooting...";
        DiagnosticLog::record("OTA", "plain firmware installed, rebooting");
        return true;
    }
    if (s_mode == Mode::PACKAGE) {
        if (s_ps != PState::DONE) { fail("the update file is incomplete - please download it again"); return false; }
        if (!s_haveFirmware) { fail("the update file has no firmware in it"); return false; }
        if (!writeManifest()) { fail(NO_ROOM_MESSAGE); return false; }
        s_message = "Update OK. Installing version " + s_pkgVersion + ". Rebooting...";
        DiagnosticLog::record("OTA", "package %s received: firmware + %u of its %u web files changed and waiting; rebooting into trial",
                              s_pkgVersion.c_str(), (unsigned)s_staged, (unsigned)s_webPaths.size());
        return true;
    }
    fail("nothing was uploaded");
    return false;
}

String uploadMessage() {
    return s_failed ? ("Update FAILED - " + s_message + ".") : s_message;
}

const char EMERGENCY_UPDATE_PAGE[] = R"html(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Clock update</title></head>
<body style="font-family:sans-serif;max-width:420px;margin:30px auto;padding:0 16px">
<h2>Clock update (basic page)</h2>
<p>Choose the update file and press Install. The clock restarts by itself; this takes a few minutes.</p>
<form method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="firmware" accept=".jcupdate,.bin"><br><br><button type="submit">Install</button>
</form></body></html>)html";

} // namespace Updater
