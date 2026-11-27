#include "Config.h"
#include "FileTools.h"
#include <Arduino.h>
#include <LittleFS.h>
#include "StringTools.h"
#include "DiagnosticLog.h"

namespace FileTools {

// The checks both writers share: the temp file holds exactly `want` bytes, then it replaces the
// file (LittleFS's rename replaces the target in one step).
static bool finish(const char* path, const String& tmp, size_t want, size_t written) {
    size_t onFile = 0;
    File check = LittleFS.open(tmp, "r");
    if (check) { onFile = check.size(); check.close(); }
    if (!want || written != want || onFile != want) {
        DiagnosticLog::record("Storage", "Save of %s failed: %u of %u bytes written (flash full?) - previous file kept",
                              path, (unsigned)(written < onFile ? written : onFile), (unsigned)want);
        LittleFS.remove(tmp);
        return false;
    }
    if (!LittleFS.rename(tmp, path)) {
        DiagnosticLog::record("Storage", "Save failed: rename %s -> %s - previous file kept", tmp.c_str(), path);
        LittleFS.remove(tmp);
        return false;
    }
    return true;
}

// The JSON writer hands its output over a character at a time. Into the file in 512-byte pieces,
// and not another byte once the flash refuses one (2026-11-18, resilience flash_full): straight
// into the file, every character after a refused one was tried again - each try a search for a
// free block - and a refused events save took 15-20 s while holding EventsLock; the main loop
// waited on it and the task watchdog restarted the clock.
class PieceWriter : public Print {
public:
    explicit PieceWriter(File& f) : f_(f) {}
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* p, size_t n) override {
        if (failed_) return 0;
        size_t taken = 0;
        while (n) {
            size_t k = n < sizeof(buf_) - len_ ? n : sizeof(buf_) - len_;
            memcpy(buf_ + len_, p, k);
            len_ += k; p += k; n -= k; taken += k;
            if (len_ == sizeof(buf_) && !push()) return 0;
        }
        return taken;
    }
    bool push() {   // the piece into the file; false once the file has refused
        if (failed_ || !len_) return !failed_;
        size_t k = f_.write(buf_, len_);
        written_ += k;
        failed_ = k != len_;
        len_ = 0;
        return !failed_;
    }
    size_t written() const { return written_; }
private:
    File& f_;
    uint8_t buf_[512];
    size_t len_ = 0, written_ = 0;
    bool failed_ = false;
};

bool writeJsonAtomic(const char* path, const JsonDocument& doc) {
    String tmp = String(path) + ".tmp";
    size_t want = measureJson(doc);
    File f = LittleFS.open(tmp, "w", true);
    if (!f) {
        DiagnosticLog::record("Storage", "Save failed: cannot open %s for writing - %s left unchanged", tmp.c_str(), path);
        return false;
    }
    PieceWriter out(f);
    serializeJson(doc, out);
    out.push();
    f.close();
    return finish(path, tmp, want, out.written());   // what reached the file, not what the writer was handed
}

bool writeTextAtomic(const char* path, const String& text) {
    String tmp = String(path) + ".tmp";
    File f = LittleFS.open(tmp, "w", true);
    if (!f) {
        DiagnosticLog::record("Storage", "Save failed: cannot open %s for writing - %s left unchanged", tmp.c_str(), path);
        return false;
    }
    size_t written = f.write((const uint8_t*)text.c_str(), text.length());
    f.close();
    return finish(path, tmp, text.length(), written);
}

void skipUtf8ByteOrderMark(fs::File &f) {
    if (!f || f.isDirectory()) return;

    // Look at the first byte without advancing the internal file pointer
    if (f.peek() == 0xEF) {
        uint8_t bom[3];
        // Try to read the 3-byte sequence
        if (f.read(bom, 3) == 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) {
            // The sequence matches the UTF-8 Byte Order Mark; the pointer is now at byte 4.
        } else {
            // It was not a Byte Order Mark; reset the file pointer back to the very beginning.
            f.seek(0);
        }
    }
}

void listDirRecursive(const char * path, const String& indent) {
    File root = LittleFS.open(path);
    if(!root || !root.isDirectory()){
        CLOCK_LOG("FS_List", "%sFailed to open directory: %s", indent.c_str(), path);
        return;
    }

    File file = root.openNextFile();
    if (!file && strcmp(path, "/") == 0) { // Only show empty message for root
        CLOCK_LOG("FS_List", "  Filesystem is empty.");
    }

    while(file){
        // path(), not name(): name() is only the file's own name ("Torah.bmp"),
        // which listed folder contents as if they were in the root.
        String pathName = file.path();

        if(file.isDirectory()){
            CLOCK_LOG("FS_List", "%sDIR : %s", indent.c_str(), pathName.c_str());
            listDirRecursive(pathName.c_str(), indent + "  ");
        } else {
            CLOCK_LOG("FS_List", "%sFILE: %s (%u bytes)", indent.c_str(), pathName.c_str(), (unsigned)file.size());
        }
        file = root.openNextFile();   // the next entry of the FOLDER (file.openNextFile() stopped the listing early)
    }
}

} // namespace FileTools