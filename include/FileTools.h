#pragma once
#include <Arduino.h>
#include <FS.h>

namespace FileTools {
    void skipUtf8ByteOrderMark(fs::File &f);
    void listDirRecursive(const char * path, const String& indent);
}