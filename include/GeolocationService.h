#pragma once

#include "Config.h"

namespace GeolocationService {

    // The IP-API endpoint for a location lookup. Fetched by Calendar.cpp's
    // background task (Calendar::requestLookup()); the answer is read by
    // Location::applyLookup(). Nothing here makes the network call.
    const char* getSyncUrl();

    // Read-only diagnostic: duration of the most recently completed
    // lookup, in ms. Reported by Calendar.cpp (the only place that
    // now times one) via setLastSyncDurationMs() below.
    uint32_t getLastSyncDurationMs();
    void setLastSyncDurationMs(uint32_t ms);

} // namespace GeolocationService
