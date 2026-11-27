#pragma once
// =============================================================================
// ImageLibrary - the pictures events show instead of their icon
//
// One shared library on the clock (decided 2026-10-21): FS_IMAGE_DIR/<id>.bmp,
// id = the file's CRC-32 in 8 hex digits, so the same picture is stored once
// however many events use it. Events refer to it by id (PersonalEvent::image).
// The event form makes the BMP in the browser (photo, icon, line art) - the
// clock only checks and stores it. One still picture per file: animations
// (from video/GIF, 2026-10-21 briefly) were dropped for space and are
// refused on upload; the screen code still plays any multi-frame BMP
// (frame count in biXPelsPerMeter, ms per frame in biYPelsPerMeter).
// The page checks the CRC itself first, so a picture already here isn't sent.
//
// Not in /IMG: an update package owns that folder and deletes what it
// doesn't contain (Updater.cpp, OWNED_DIRS); /images is protected from
// packages instead. Backed up inside the Excel export (index.html).
// =============================================================================
#include <Arduino.h>
#include "Config.h"

namespace ImageLibrary {
    bool   isValidId(const char* id);                // 8 lowercase hex digits
    // CRC-32 (as zlib) of `len` bytes, continuing from `crc` (start with 0).
    uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len);
    String idFromCrc(uint32_t crc);
    // Whether `bytes` more fit while still leaving room for an update to
    // unpack its web files next to the current ones (measured, not guessed:
    // the size of everything a package would replace, plus a margin). `why`
    // gets a sentence for the user when not.
    bool   hasRoomFor(size_t bytes, String* why);
    // How many bytes of pictures still fit by that rule (Settings > Pictures).
    size_t roomLeft();
    // Deletes library images no event refers to; returns how many.
    int    removeUnused(const EventList& events);
}
