#include <Arduino.h>
#include "GeolocationService.h"

namespace GeolocationService {

static uint32_t g_lastSyncDurationMs = 0;

// "offset" = the zone's UTC offset now (used only for a zone the time-zone
// table doesn't have - Location.cpp).
static const char* SYNC_URL = "http://ip-api.com/json/?fields=status,timezone,offset,city,country,countryCode,lat,lon";

uint32_t getLastSyncDurationMs() { return g_lastSyncDurationMs; }
void setLastSyncDurationMs(uint32_t ms) { g_lastSyncDurationMs = ms; }

const char* getSyncUrl() { return SYNC_URL; }

} // namespace GeolocationService
