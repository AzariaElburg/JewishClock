#include "ImageLibrary.h"
#include "DiagnosticLog.h"
#include "Display.h"
#include "Storage.h"
#include <LittleFS.h>
#include <vector>

// === TUNABLES ===
// Kept free on top of what a typical update needs to unpack and what the log
// may still grow (see roomLeft()): a changed icon or two, and the clock's own
// files growing a little (calendar cache, settings).
static const size_t IMAGE_SPACE_MARGIN_BYTES = 160 * 1024;

namespace {

// What an ordinary update needs room for: it stages only the files that
// changed (Updater.cpp), normally the pages - the web files directly in "/"
// (index.html, update.html, ...); the icons in /IMG rarely change. An
// update that changes much more than that says so when it runs out of room.
size_t typicalUpdateBytes() {
    size_t sum = 0;
    File d = LittleFS.open("/");
    if (!d || !d.isDirectory()) return 0;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        if (!f.isDirectory() && !isClockDataPath(f.path())) sum += f.size();
        f.close();
    }
    d.close();
    return sum;
}

}  // namespace

namespace ImageLibrary {

bool isValidId(const char* id) {
    if (!id || strlen(id) != 8) return false;
    for (const char* p = id; *p; p++) if (!isxdigit((unsigned char)*p) || isupper((unsigned char)*p)) return false;
    return true;
}

uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

String idFromCrc(uint32_t crc) {
    char buf[9];
    snprintf(buf, sizeof(buf), "%08lx", (unsigned long)crc);
    return buf;
}

size_t roomLeft() {
    size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
    size_t free = total > used ? total - used : 0;
    // The log counted at its full size (DiagnosticLog::MAX_FILE_BYTES): a
    // margin that once covered it stopped doing so when the files grew
    // from 32 to 128 KB (2026-11-07) - pictures could have taken room an
    // update needs.
    size_t reserve = typicalUpdateBytes() + IMAGE_SPACE_MARGIN_BYTES + DiagnosticLog::growthLeft();
    return free > reserve ? free - reserve : 0;
}

bool hasRoomFor(size_t bytes, String* why) {
    size_t left = roomLeft();
    if (left >= bytes) return true;
    if (why) {
        *why = "Not enough room on the clock for this picture (" + String((bytes + 1023) / 1024) + " KB; " +
               String(left / 1024) +
               " KB left, keeping room for updates). A shorter animation, or removing pictures no longer needed, makes room.";
    }
    return false;
}

bool migrateOldPhotos(EventList& events) {
    bool changed = false;
    for (auto& ev : events) {
        String old = String(FS_OLD_PHOTO_DIR "/") + ev.id + ".bmp";
        if (!ev.id[0] || !LittleFS.exists(old)) continue;
        File f = LittleFS.open(old, "r");
        uint32_t crc = 0;
        uint8_t buf[512];
        size_t n;
        while ((n = f.read(buf, sizeof(buf))) > 0) crc = crc32(crc, buf, n);
        f.close();
        String id = idFromCrc(crc);
        LittleFS.mkdir(FS_IMAGE_DIR);
        if (LittleFS.exists(imagePath(id.c_str()))) LittleFS.remove(old);   // same picture already there
        else LittleFS.rename(old, imagePath(id.c_str()));
        strlcpy(ev.image, id.c_str(), sizeof(ev.image));
        changed = true;
        DiagnosticLog::record("Images", "moved the photo of event %s into the picture library as %s", ev.id, id.c_str());
    }
    LittleFS.rmdir(FS_OLD_PHOTO_DIR);   // only if empty
    return changed;
}

int removeUnused(const EventList& events) {
    // Deleted events keep their pictures while in the recycle bin (restorable).
    EventList deleted;
    Storage::loadEvents(deleted, FS_DELETED_EVENTS_PATH);
    File dir = LittleFS.open(FS_IMAGE_DIR);
    if (!dir || !dir.isDirectory()) return 0;
    std::vector<String> unused;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        String name = f.name();   // "<id>.bmp", or a leftover temp file
        f.close();
        String id = name.endsWith(".bmp") ? name.substring(0, name.length() - 4) : "";
        bool used = false;
        for (const auto& ev : events) if (id.length() && id == ev.image) { used = true; break; }
        for (const auto& ev : deleted) if (!used && id.length() && id == ev.image) { used = true; break; }
        if (!used) unused.push_back(String(FS_IMAGE_DIR "/") + name);
    }
    dir.close();
    for (const String& p : unused) {
        LittleFS.remove(p);
        DisplayManager::forgetImage(p.c_str());
    }
    if (!unused.empty()) DiagnosticLog::record("Images", "removed %d picture(s) no event uses", (int)unused.size());
    return (int)unused.size();
}

}  // namespace ImageLibrary
