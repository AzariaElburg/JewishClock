#include "Config.h"
#include "FileTools.h"
#include <Arduino.h>
#include <LittleFS.h>
#include "StringTools.h"

namespace FileTools {

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