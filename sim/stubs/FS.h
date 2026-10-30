#pragma once
// PC stand-in for the ESP32 file system API (simulator only - see sim/README.md).
// "/path" on the clock = <SIM_FS root>/path on the PC.

#include <Arduino.h>
#include <memory>
#include <vector>
#include <string>

#define FILE_READ   "r"
#define FILE_WRITE  "w"
#define FILE_APPEND "a"

namespace fs {

class File : public Stream {
public:
    File() {}
    File(FILE* fp, const std::string& clockPath, const std::string& pcPath);   // a file
    File(const std::string& clockPath, const std::string& pcPath);              // a directory
    ~File() override;
    File(const File&) = default;
    File& operator=(const File&) = default;

    operator bool() const { return (bool)fp_ || isDir_; }
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t n) override;
    using Print::write;
    int available() override;
    int read() override;
    int peek() override;
    size_t read(uint8_t* buf, size_t n);
    bool seek(uint32_t pos);
    size_t position() const;
    size_t size() const;
    void close();
    bool isDirectory() const { return isDir_; }
    const char* name() const;
    const char* path() const { return clockPath_.c_str(); }
    File openNextFile(const char* mode = FILE_READ);
    void flush() override;

private:
    std::shared_ptr<FILE> fp_;
    bool isDir_ = false;
    std::string clockPath_, pcPath_;
    std::shared_ptr<std::vector<std::string>> entries_;
    std::shared_ptr<size_t> nextEntry_;
};

class FS {
public:
    File open(const char* path, const char* mode = FILE_READ, bool create = false);
    File open(const String& path, const char* mode = FILE_READ, bool create = false) { return open(path.c_str(), mode, create); }
    bool exists(const char* path);
    bool exists(const String& path) { return exists(path.c_str()); }
    bool remove(const char* path);
    bool remove(const String& path) { return remove(path.c_str()); }
    bool rename(const char* from, const char* to);
    bool rename(const String& from, const String& to) { return rename(from.c_str(), to.c_str()); }
    bool mkdir(const char* path);
    bool mkdir(const String& path) { return mkdir(path.c_str()); }
    bool rmdir(const char* path);
    bool rmdir(const String& path) { return rmdir(path.c_str()); }
    size_t totalBytes() { return 4063232; }
    size_t usedBytes();
};

} // namespace fs

using fs::File;
