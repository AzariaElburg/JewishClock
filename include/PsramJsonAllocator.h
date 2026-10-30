#pragma once

#include <ArduinoJson.h>
#include "esp_heap_caps.h"

// An ArduinoJson::Allocator that draws from PSRAM (heap_caps_malloc with
// MALLOC_CAP_SPIRAM) instead of the default allocator's regular SRAM heap -
// same motivation as PsramVector.h, applied to JsonDocument instead of
// std::vector. Deliberately only used for the one JsonDocument whose size
// genuinely scales with externally-fetched data (Calendar.cpp's Hebcal
// manifest parse - see that call site), not the many small, fixed-shape
// JsonDocuments used for ordinary request/response bodies elsewhere, which
// stay on the default allocator on purpose (PSRAM access is slower than
// SRAM - not worth paying for something that small).
//
// Returns nullptr on failure, same contract as malloc - ArduinoJson already
// treats that as a normal DeserializationError::NoMemory, which calling
// code already checks for, so this never needs to throw (unlike
// PsramAllocator in PsramVector.h, which uses std::allocator's throwing
// contract because std::vector requires it).
class PsramJsonAllocator : public ArduinoJson::Allocator {
public:
    void* allocate(size_t size) override {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }
    void deallocate(void* ptr) override {
        heap_caps_free(ptr);
    }
    void* reallocate(void* ptr, size_t new_size) override {
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
    }

    static PsramJsonAllocator* instance() {
        static PsramJsonAllocator allocator;
        return &allocator;
    }
};
