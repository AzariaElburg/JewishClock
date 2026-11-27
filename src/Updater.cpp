// =============================================================================
// Updater.cpp - see Updater.h for what this does and why.
// =============================================================================

#include "FileTools.h"
#include "Updater.h"
#include "Uptime.h"
#include "Config.h"
#include "Version.h"
#include "VersionRule.h"
#include "LogLevel.h"
#include "DiagnosticLog.h"
#include "StringTools.h"
#include "Display.h"
#include "WebUI.h"
#include "UpdateSignature.h"
#include "UpdateKeyState.h"
#include "OnlineUpdate.h"
#include "NTPManager.h"
#include "Tunables.h"
#include "Texts.h"
#include "Restart.h"

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
const int64_t TRIAL_CONNECT_DEADLINE_MS = 5LL * 60 * 1000;
TUNABLE_SHARED(TRIAL_CONNECT_DEADLINE_MS)
const int64_t TRIAL_STABLE_MS           = 60LL * 1000;
TUNABLE_SHARED(TRIAL_STABLE_MS)
const int     UPDATE_FAILED_SCREEN_DAYS = 3;   // a failed update on the clock's screen at most this long (Settings: until seen)
TUNABLE_SHARED(UPDATE_FAILED_SCREEN_DAYS)
const char*   NOT_SIGNED_MESSAGE        = Texts::updNotSigned;
const char*   NO_KEY_MESSAGE            = Texts::updNoKey;
const char*   BAD_SIGNATURE_MESSAGE     = Texts::updBadSignature;

const size_t MIN_FIRMWARE_BYTES = 100000;   // real builds are ~1.3 MB
const uint8_t ESP_IMAGE_MAGIC   = 0xE9;

// Waiting area for a package's changed web files, and the list of ALL the
// web files of its version (the manifest).
const char* PENDING_DIR   = FS_UPDATE_DIR;
const char* MANIFEST_PATH = FS_UPDATE_DIR "/manifest.txt";
// The waiting area ran out of room (an update changing more than the
// pictures' room check leaves space for - ImageLibrary::hasRoomFor()).
// Nothing is installed: the firmware comes last in a package.
const char* NO_ROOM_MESSAGE = Texts::updNoRoom;
// Never written or removed by a package: the clock's own data -
// isClockDataPath() in Config.h. Every other file the package's version
// doesn't list is removed on install, and folders left empty with it.

// ── Package format (.jcupdate, written by utils/make_update_package.py) ──────
//   "JCUPD1" | u8 versionLen | version | u16 entryCount
//   entryCount x ( u8 type (1=web file, 2=firmware) | u8 pathLen | path |
//                  u32 size | u32 crc32 | size bytes )
//   "JCEND!"
//   "JCSIG1" | u16 sigLen | signature (2026-11-14: UpdateSignature.h - SHA-256 over
//   every byte up to and including JCEND!; nothing is installed without it)
// All integers little-endian. crc32 = standard CRC-32 (same as Python zlib).
enum class PState : uint8_t { MAGIC, VERLEN, VERSION, COUNT, E_TYPE, E_PATHLEN, E_PATH, E_SIZE, E_CRC, E_DATA, TRAILER,
                              SIG_MAGIC, SIG_LEN, SIG, DONE, FAILED };
enum class Mode   : uint8_t { NONE, PACKAGE };

Mode     s_mode = Mode::NONE;
size_t   s_total = 0;
String   s_message;
bool     s_failed = false;
bool     s_refusedOlder = false;   // uploadRefusedOlder()

// Upload under way (AsyncTCP task writes, main loop reads - see
// uploadInProgress()). Stays set after a successful upload: the restart
// follows.
std::atomic<bool>     s_uploading{false};
std::atomic<int64_t>  s_lastChunkMs{0};   // Uptime::ms()
// Data still expected: from uploadBegin() until uploadEnd() or a failure.
// An upload that stops arriving (browser closed, PC's Wi-Fi switched) is
// cleaned up by loop() - see there.
std::atomic<bool>     s_receiving{false};
// Milliseconds since the last chunk. Can be a little negative: the AsyncTCP
// task can store a chunk time just after the caller read the time (with
// 32-bit millis() that once read as "stalled" mid-upload, 2026-10-18).
int64_t msSinceLastChunk(int64_t now) { return now - s_lastChunkMs.load(); }
// === TUNABLES ===
// No data for this long = the upload was abandoned (browser closed, Wi-Fi
// lost) - the screen goes back to normal.
const int64_t UPLOAD_STALLED_MS = 10000;
TUNABLE_SHARED(UPLOAD_STALLED_MS)

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
bool     s_haveFirmware = false;   // its firmware received in full (written, not yet made the one to start)
bool     s_fwBegun = false;
// The signature (2026-11-14): SHA-256 over the package up to JCEND!, then checked.
// Firmware is made the one to start (Update.end) only after the check - before,
// a package's firmware was installed the moment its bytes were in.
UpdateSignature::Sha256* s_sha = nullptr;
bool     s_sigOk = false;
// The key this upload is checked with: the setting's (UpdateKeyState.h). None = nothing
// is installed (the user, 2026-11-17: no key is the same as a wrong one).
uint8_t  s_key[128];
size_t   s_keyLen = 0;
// Versions (agreed 2026-11-13): a newer one may skip at most one major version (vN reads
// v(N-1)'s data); an older one only with "erase" - its data can't be read by it.
bool     s_eraseAllowed = false, s_eraseWanted = false;
std::vector<String> s_webPaths;

// Web files being put in place (main loop writes, web task reads).
std::atomic<bool> s_movingWebFiles{false};

// Trial state (main loop only).
bool          s_trial = false;
int64_t s_bootMs = 0;
Timer   s_connected;   // connected without a break since
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
    Updater::debugFocus(Logging::FOCUS_UPDATE_INSTALL, false, "package refused");
}

void need(PState next, size_t n) { s_ps = next; s_fneed = n; s_fhave = 0; }

void nextEntryOrTrailer() {
    if (s_entriesDone >= s_entryCount) need(PState::TRAILER, 6);
    else need(PState::E_TYPE, 1);
}

void finishEntry() {
    if (s_eCrc != s_eCrcWant) { fail(Texts::fill(Texts::updDamagedFile, s_ePath)); return; }
    if (s_eType == 1) {
        if (!s_eSkip) { s_eFile.close(); s_staged++; }
        s_webPaths.push_back(s_ePath);   // the full list: what's not in it gets removed on install
    } else {
        s_haveFirmware = true;   // made the one to start in uploadEnd(), once the signature checks out
    }
    s_entriesDone++;
    nextEntryOrTrailer();
}

void startEntry() {
    s_eCrc = 0;
    s_eRemain = s_eSize;
    if (s_eType == 1) {
        if (!s_ePath.startsWith("/") || s_ePath.indexOf("..") >= 0 || isProtected(s_ePath)) { fail(Texts::fill(Texts::updMayNotReplace, s_ePath)); return; }
        s_eSkip = sameAsInstalled(s_ePath, s_eSize, s_eCrcWant);
        if (!s_eSkip) {
            s_eFile = LittleFS.open(String(PENDING_DIR) + s_ePath, "w", true);   // true: create folders
            if (!s_eFile) { fail(NO_ROOM_MESSAGE); return; }
        }
    } else if (s_eType == 2) {
        if (s_haveFirmware || s_eSize < MIN_FIRMWARE_BYTES) { fail(Texts::updFirmwareInvalid); return; }
        if (!Update.begin(s_eSize)) { fail(Texts::fill(Texts::updFirmwareNotStarted, String(Update.errorString()))); return; }
        s_fwBegun = true;
    } else {
        fail(Texts::updUnknownEntry);
        return;
    }
    s_ps = PState::E_DATA;
    if (s_eRemain == 0) finishEntry();
}

void writeEntryData(const uint8_t* p, size_t n) {
    if (s_eType == 2 && s_eRemain == s_eSize && n && p[0] != ESP_IMAGE_MAGIC) { fail(Texts::updFirmwareInvalid); return; }
    s_eCrc = crc32Update(s_eCrc, p, n);
    if (s_eType == 1) {
        if (!s_eSkip && s_eFile.write(p, n) != n) { fail(NO_ROOM_MESSAGE); return; }
    } else {
        if (Update.write(const_cast<uint8_t*>(p), n) != n) { fail(Texts::fill(Texts::updWriteFailed, String(Update.errorString()))); return; }
    }
}

void onField() {
    switch (s_ps) {
        case PState::MAGIC:
            if (memcmp(s_fbuf, "JCUPD1", 6) != 0) { fail(Texts::updNotAnUpdate); return; }
            need(PState::VERLEN, 1); break;
        case PState::VERLEN:
            if (s_fbuf[0] == 0 || s_fbuf[0] > 40) { fail(Texts::updDamaged); return; }
            need(PState::VERSION, s_fbuf[0]); break;
        case PState::VERSION: {
            String v = String((const char*)s_fbuf).substring(0, s_fneed);
            bool older = VersionRule::newer(FIRMWARE_VERSION, v);
            if (older && !s_eraseAllowed) {
                s_refusedOlder = true;
                fail(Texts::fill(Texts::updOlderVersion, v, FIRMWARE_VERSION));
                return;
            }
            // The new version must read this clock's data - the same rule as a restored file (VersionRule.h).
            if (!older && VersionRule::dataProblem(v, FIRMWARE_VERSION, "The clock's data").length()) {
                fail(Texts::fill(Texts::updCantReadData, v, String(VersionRule::major(v) - 1)));
                return;
            }
            s_eraseWanted = s_eraseAllowed && older;
        }
            s_pkgVersion = String((const char*)s_fbuf).substring(0, s_fhave);
            need(PState::COUNT, 2); break;
        case PState::COUNT:
            s_entryCount = (uint16_t)(s_fbuf[0] | (s_fbuf[1] << 8));
            if (s_entryCount == 0) { fail(Texts::updEmpty); return; }
            nextEntryOrTrailer(); break;
        case PState::E_TYPE:
            s_eType = s_fbuf[0];
            need(PState::E_PATHLEN, 1); break;
        case PState::E_PATHLEN:
            if (s_fbuf[0] == 0 || s_fbuf[0] > 120) { fail(Texts::updDamaged); return; }
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
            if (memcmp(s_fbuf, "JCEND!", 6) != 0) { fail(Texts::updIncomplete); return; }
            need(PState::SIG_MAGIC, 6); break;
        case PState::SIG_MAGIC:
            if (memcmp(s_fbuf, "JCSIG1", 6) != 0) { fail(NOT_SIGNED_MESSAGE); return; }
            need(PState::SIG_LEN, 2); break;
        case PState::SIG_LEN: {
            size_t len = s_fbuf[0] | (s_fbuf[1] << 8);
            if (len < 8 || len >= sizeof(s_fbuf)) { fail(BAD_SIGNATURE_MESSAGE); return; }
            need(PState::SIG, len); break;
        }
        case PState::SIG: {
            uint8_t hash[32];
            s_sha->finish(hash);
            s_sigOk = s_keyLen && UpdateSignature::verify(hash, s_fbuf, s_fneed, s_key, s_keyLen);
            s_ps = PState::DONE;
            break;
        }
        default: break;
    }
}

// The bytes the signature covers: everything before the JCSIG1 part.
bool signed_(PState st) { return st != PState::SIG_MAGIC && st != PState::SIG_LEN && st != PState::SIG; }

void feedPackage(const uint8_t* p, size_t n) {
    while (n && s_ps != PState::DONE && s_ps != PState::FAILED) {
        if (s_ps == PState::E_DATA) {
            size_t take = n < s_eRemain ? n : s_eRemain;
            s_sha->add(p, take);
            writeEntryData(p, take);
            if (s_failed) return;
            p += take; n -= take; s_eRemain -= take;
            if (s_eRemain == 0) finishEntry();
            continue;
        }
        size_t take = s_fneed - s_fhave;
        if (take > n) take = n;
        if (signed_(s_ps)) s_sha->add(p, take);
        memcpy(s_fbuf + s_fhave, p, take);
        s_fhave += take; p += take; n -= take;
        if (s_fhave == s_fneed) {
            s_fbuf[s_fhave < sizeof(s_fbuf) ? s_fhave : sizeof(s_fbuf) - 1] = 0;
            onField();
        }
    }
}

// Whole or not at all (FileTools.h): a list cut off by a full flash would have the install remove
// every web file missing from it - the page gone (2026-11-18).
bool writeManifest() {
    String text = s_pkgVersion + "\n";
    for (const String& p : s_webPaths) text += p + "\n";
    return FileTools::writeTextAtomic(MANIFEST_PATH, text);
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
        // Said on the screen and in Settings (failedUpdate()) until "OK, seen".
        FileTools::writeTextAtomic(UPDATE_FAILED_PATH, version + "\n");
        return;
    }
    LittleFS.remove(UPDATE_FAILED_PATH);   // this update stayed: an older failure is history

    // The changed files (only those are waiting; the rest are already right).
    // Page files asked for meanwhile get "reload in a moment" (WebUI.cpp sendWebFile()).
    s_movingWebFiles = true;
    std::vector<String> touched;   // put in place or removed - icons among them are reloaded below
    int moved = 0;
    for (const String& p : paths) {
        String from = String(PENDING_DIR) + p;
        if (!LittleFS.exists(from)) continue;
        int slash = p.lastIndexOf('/');
        if (slash > 0) LittleFS.mkdir(p.substring(0, slash));
        LittleFS.remove(p);
        if (LittleFS.rename(from, p)) { moved++; touched.push_back(p); }
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
        if (!listed && LittleFS.remove(fp)) { removed++; touched.push_back(fp); DiagnosticLog::record("OTA", "removed %s (not in version %s)", fp.c_str(), FIRMWARE_VERSION); }
    }
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {   // deepest first
        File d = LittleFS.open(*it);
        bool empty = d && d.isDirectory() && !d.openNextFile();
        if (d) d.close();
        if (empty && LittleFS.rmdir(*it)) DiagnosticLog::record("OTA", "removed empty folder %s", it->c_str());
    }
    // In use from now on, without a restart (2026-11-10): the clock's own icons
    // that changed are read again, as at startup, and the web server forgets
    // what it knew about the page files (their gzip and ETag).
    for (const String& p : touched) if (p.startsWith("/IMG/")) DisplayManager::forgetImage(p.c_str());
    DisplayManager::preloadImages();
    WebUI::webFilesChanged();
    s_movingWebFiles = false;
    DiagnosticLog::record("OTA", "installed %d changed web files (removed %d old) for version %s - in use now", moved, removed, FIRMWARE_VERSION);
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

bool justInstalled() {
    esp_ota_img_states_t st;
    const esp_partition_t* running = esp_ota_get_running_partition();
    return running && esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

void begin() {
    // A real reference, or the linker discards the marker as unused
    // (__attribute__((used)) alone doesn't survive --gc-sections).
    (void)*(volatile const char*)kJcVersionMarker;
    s_bootMs = Uptime::ms();
    esp_ota_img_states_t st;
    const esp_partition_t* running = esp_ota_get_running_partition();
    s_trial = running && esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    if (s_trial) DiagnosticLog::record("OTA", "new firmware %s on trial: needs Wi-Fi within %lu s, then %lu s up", FIRMWARE_VERSION,
                                       TRIAL_CONNECT_DEADLINE_MS / 1000, TRIAL_STABLE_MS / 1000);
    if (s_trial) debugFocus(Logging::FOCUS_UPDATE_TRIAL, true, Texts::focusOnTrial);
}

void loop() {
    int64_t now = Uptime::ms();
    // An upload that stopped halfway (seen 2026-10-18: the PC switched to the
    // clock's own Wi-Fi mid-upload): drop the half-written firmware and web
    // files now - before this, the status said "installing" until the next
    // restart or upload, and the screen waited on the stall timeout.
    if (s_receiving && msSinceLastChunk(now) >= UPLOAD_STALLED_MS) {
        fail(Texts::updStalled);
    }
    if (s_trial) {
        if (WiFi.status() == WL_CONNECTED) {
            if (!s_connected.running()) s_connected.startAt(now);
            if (s_connected.passed(TRIAL_STABLE_MS, now)) {
                esp_ota_mark_app_valid_cancel_rollback();
                s_trial = false;
                DiagnosticLog::record("OTA", Texts::updTrialPassed, FIRMWARE_VERSION);
            }
        } else {
            s_connected.stop();
        }
        if (s_trial && now - s_bootMs >= TRIAL_CONNECT_DEADLINE_MS + TRIAL_STABLE_MS) {
            DiagnosticLog::record("OTA", Texts::updTrialFailed, FIRMWARE_VERSION);
            delay(200);
            Restart::rollback();
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
        debugFocus(Logging::FOCUS_UPDATE_TRIAL, false, Texts::focusTrialOver);
    }
}

bool movingWebFiles() { return s_movingWebFiles; }

// UPDATE_FAILED_PATH: "<version>\n[<the local day (since 1970) the screen first showed it>]\n".
// Read by the main loop (the status slide) and the web task - small, and only after an
// update that went back.
static bool readFailed(String& version, String& day) {
    File f = LittleFS.open(UPDATE_FAILED_PATH, "r");
    if (!f) return false;
    version = f.readStringUntil('\n');
    day = f.readStringUntil('\n');
    f.close();
    version.trim(); day.trim();
    if (version.isEmpty() || version.length() > 40) { LittleFS.remove(UPDATE_FAILED_PATH); return false; }   // not ours
    return true;
}

String failedUpdate() {
    String version, day;
    return readFailed(version, day) ? version : String();
}

String failedUpdateOnScreen() {
    String version, day;
    if (!readFailed(version, day)) return "";
    time_t now = NTPManager::getLocalEpoch();
    if (now <= 0) return version;   // the date not known yet
    long today = (long)(now / 86400);
    if (day.isEmpty()) {            // first on the screen today: the days count from here
        FileTools::writeTextAtomic(UPDATE_FAILED_PATH, version + "\n" + String(today) + "\n");
        return version;
    }
    return today - day.toInt() < UPDATE_FAILED_SCREEN_DAYS ? version : String();
}

void failedUpdateSeen() {
    if (LittleFS.remove(UPDATE_FAILED_PATH)) DiagnosticLog::record("OTA", "the failed update's notice: seen (Settings)");
}

const char* stateName() {
    if (s_trial) return "trial";
    return LittleFS.exists(PENDING_DIR) ? "installing" : "ok";
}

bool eraseWanted() { return s_eraseWanted && !s_failed; }

void uploadAbort(const String& why) { fail(why); }

void debugFocus(uint8_t area, bool on, const char* what) {
    bool was = Logging::focus() & area;
    if (was == on) return;
    if (!what) { Logging::setFocus(area, on); return; }   // quietly (the feed check, every 5 min in version A)
    if (on) {
        if (Logging::setFocus(area, true))
            DiagnosticLog::record("Log", Texts::focusOn, what, Logging::levelName(Logging::getLevel()));
        else DiagnosticLog::record("Log", "debug also for: %s", what);
    } else {
        if (Logging::setFocus(area, false))
            DiagnosticLog::record("Log", Texts::focusBack, Logging::levelName(Logging::getLevel()), what);
        else DiagnosticLog::record("Log", "debug ended for: %s (other update work still at debug)", what);
    }
}

void uploadBegin(const String& filename, size_t totalBytes, bool erase) {
    debugFocus(Logging::FOCUS_UPDATE_INSTALL, true, "update package being received");
    s_eraseAllowed = erase;
    s_eraseWanted = false;
    { size_t n; const uint8_t* k = UpdateKeyState::der(n); memcpy(s_key, k, n); s_keyLen = n; }
    s_mode = Mode::NONE;
    s_total = totalBytes;
    s_failed = false;
    s_refusedOlder = false;
    s_message = "";
    s_ps = PState::MAGIC; s_fneed = 6; s_fhave = 0;
    s_pkgVersion = ""; s_entryCount = s_entriesDone = 0;
    // Anything an earlier, unfinished upload left open: an open file can't be
    // deleted ("Failed to unlink ... Has open FD", seen 2026-10-18).
    if (s_eFile) s_eFile.close();
    if (s_fwBegun) Update.abort();
    s_haveFirmware = false; s_fwBegun = false;
    if (!s_sha) s_sha = new UpdateSignature::Sha256();
    s_sha->reset();
    s_sigOk = false;
    s_webPaths.clear();
    s_staged = 0;
    if (LittleFS.exists(PENDING_DIR)) removeTree(PENDING_DIR);
    s_lastChunkMs = Uptime::ms();
    s_uploading = true;
    s_receiving = true;
    DiagnosticLog::record("OTA", "upload started: %s, %u bytes", filename.c_str(), (unsigned)totalBytes);
}

bool uploadInProgress() {
    return s_uploading && msSinceLastChunk(Uptime::ms()) < UPLOAD_STALLED_MS;
}

void uploadChunk(const uint8_t* data, size_t len) {
    s_lastChunkMs = Uptime::ms();
    if (s_failed || !len) return;
    if (s_mode == Mode::NONE) {
        if (data[0] == ESP_IMAGE_MAGIC) {
            fail(Texts::updPlainFirmware);
            return;
        } else if (!s_keyLen) {
            fail(NO_KEY_MESSAGE);
            return;
        } else if (data[0] == 'J') {
            s_mode = Mode::PACKAGE;
        } else {
            fail(Texts::updNotAnUpdate);
            return;
        }
    }
    feedPackage(data, len);
}

bool uploadEnd() {
    s_receiving = false;
    if (s_failed) return false;
    if (s_mode == Mode::PACKAGE) {
        if (s_ps == PState::SIG_MAGIC && s_fhave == 0) { fail(NOT_SIGNED_MESSAGE); return false; }   // ended at JCEND!: not signed
        if (s_ps != PState::DONE) { fail(Texts::updIncomplete); return false; }
        if (!s_sigOk) { fail(BAD_SIGNATURE_MESSAGE); return false; }
        if (!s_haveFirmware) { fail(Texts::updNoFirmware); return false; }
        if (!Update.end(true)) { fail(Texts::fill(Texts::updNotInstalled, String(Update.errorString()))); return false; }
        s_fwBegun = false;
        if (!writeManifest()) { fail(NO_ROOM_MESSAGE); return false; }
        s_message = Texts::fill(Texts::updInstalling, s_pkgVersion);
        DiagnosticLog::record("OTA", "package %s received: firmware + %u of its %u web files changed and waiting; rebooting into trial",
                              s_pkgVersion.c_str(), (unsigned)s_staged, (unsigned)s_webPaths.size());
        debugFocus(Logging::FOCUS_UPDATE_INSTALL, false, Texts::updAccepted);
        return true;
    }
    fail(Texts::updNothing);
    return false;
}

String uploadVersion() { return s_pkgVersion; }
bool uploadRefusedOlder() { return s_refusedOlder; }

String uploadMessage() {
    return s_failed ? Texts::fill(Texts::updFailed, s_message) : s_message;
}

const char EMERGENCY_UPDATE_PAGE[] = R"html(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Clock update</title></head>
<body style="font-family:sans-serif;max-width:420px;margin:30px auto;padding:0 16px">
<h2>Clock update (basic page)</h2>
<p>Choose the update file and press Install. The clock restarts by itself; this takes a few minutes.</p>
<form method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="firmware" accept=".jcupdate"><br><br><button type="submit">Install</button>
</form></body></html>)html";

} // namespace Updater
