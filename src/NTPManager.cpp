// =============================================================================
// NTPManager.cpp
// JewishClock - Raw UDP NTP time synchronisation with step/slew discipline.
// =============================================================================

#include <Arduino.h>  
#include "NTPManager.h"
#include "NTPLock.h"
#include "DiagnosticLog.h"
#include "LogLevel.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <sys/time.h>
#include <time.h>
#include <atomic>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include "StringTools.h"
#include "DebugTime.h"

// === TUNABLES ===  (rule: see Config.h header)
// =============================================================================

static const long     STEP_THRESHOLD_SEC   = 60;    // Offset ≥ this → hard step
static const long     IGNORE_THRESHOLD_MS  = 300;   // Offset < this → ignore
static const long     MAX_SLEW_MS_PER_SEC  = 5;     // Max slew rate (ms per real second)

static const unsigned long SYNC_INTERVAL_MS  = 6UL * 3600UL * 1000UL;  // 6 hours
static const unsigned long RETRY_INTERVAL_MS = 5UL * 1000UL;            // 5 seconds (fast-sync at boot)
static const unsigned long REPLY_TIMEOUT_MS  = 2UL * 1000UL;            // Give up on a lost reply so we can retry

static const unsigned long DNS_REFRESH_MS        = 24UL * 3600UL * 1000UL; // Re-resolve the server name daily
static const unsigned long DNS_RETRY_SYNCED_MS   = 60UL * 1000UL;          // After a failed lookup, once synced
static const unsigned long DNS_GIVE_UP_MS        = 30UL * 1000UL;          // Treat a lookup with no answer as failed
static const uint32_t NO_REPLY_LOG_AFTER = 3;   // lost replies in a row before the persistent log hears of it
static DiagnosticLog::Streak s_replyStreak{NO_REPLY_LOG_AFTER};

// Protocol constants - fixed by NTP itself, not tunable.
static const int      NTP_PACKET_SIZE = 48;
static const uint16_t NTP_LOCAL_PORT  = 2390;
static const uint16_t NTP_PORT        = 123;

// NTP epoch offset: seconds from 1 Jan 1900 to 1 Jan 1970
static const unsigned long NTP_EPOCH_OFFSET = 2208988800UL;
// No NTP answer can be older than this (2026-01-01 UTC) - picks the NTP era,
// see recvPacket(). Valid until 2026 + 136 years = 2162.
static const int64_t NTP_ERA_PIVOT_UNIX = 1767225600LL;

// The reason for firmware 2.0 (Arduino-ESP32 3.x): 64-bit time. With 32-bit
// time the clock's dates go wrong on 19 January 2038. This stops any build
// that would silently bring 32-bit time back (e.g. an older platform).
static_assert(sizeof(time_t) == 8, "time_t must be 64-bit - the clock would show wrong dates from 19 Jan 2038");

// =============================================================================
// Module state
// =============================================================================

static WiFiUDP        g_udp;
static uint8_t        g_packet[NTP_PACKET_SIZE];

static String         g_server       = "pool.ntp.org";

// Time model:
//   currentUtcMs() ≈ (g_baseUnixTime * 1000 + g_baseFracMs)
//                  + (millis() - g_baseMillis)
//                  + slew_correction
//
//   g_baseUnixTime   = integer UTC seconds at last sync
//   g_baseFracMs     = sub-second millisecond component at last sync
//   g_baseMillis     = millis() value at last sync
//   g_pendingSlewMs  = total ms still to be injected/withdrawn via slew
//
static bool           g_haveSync      = false;
static time_t         g_baseUnixTime  = 0;
static long           g_baseFracMs    = 0;
static unsigned long  g_baseMillis    = 0;
std::atomic<int64_t>  g_debugTimeOffsetSec{0};   // see DebugTime.h - read lock-free by log timestamps
std::atomic<bool>     g_timeSyncedThisBoot{false}; // see DebugTime.h - set on first sync, never cleared
static long           g_pendingSlewMs = 0;

static unsigned long  g_lastAttemptMs = 0;   // millis() of last send
static unsigned long  g_lastGoodMs    = 0;   // millis() of last successful sync
static bool           g_waiting       = false; // packet sent, awaiting reply

// =============================================================================
// Internal helpers
// =============================================================================

// Tracked so a sustained DNS outage (retried every 5s while unsynced)
// persists one WARN line when it starts and one when it recovers,
// instead of flooding the capped persistent log with an identical line
// every 5s and rotating out everything else - see CLAUDE.md. Serial
// (CLOCK_LOG below) still gets every attempt, unconditionally, for
// whoever's actively watching live.
static bool s_dnsWasOk = true;
static uint32_t s_dnsFailStreak = 0;

// ── Non-blocking server-name lookup ──────────────────────────────────────────
// A UDP request can only go to an IP address, so the server name has to be
// resolved first. WiFi.hostByName() did that by WAITING on the main loop -
// the task that also draws the display and serves the web page - so a DNS
// server that went silent froze the clock for seconds at every resync, mid-
// life included. Instead the question is handed to lwIP's own task
// (tcpip_callback -> dns_gethostbyname) and the answer arrives through
// dnsFound() on that task; loop() just checks the state each pass. The
// address is then reused for DNS_REFRESH_MS, so lookups are rare anyway.
//
// Possible way forward, deliberately NOT built (2026-10-16): if NTP ever
// becomes unusable while the web still works (NTP's UDP port 123 blocked by
// a router/ISP, public NTP servers retired or banned), fall back to:
//   1. NTP by fixed IP, no DNS needed - e.g. time.google.com 216.239.35.0,
//      time.cloudflare.com 162.159.200.1;
//   2. the UTC in the HTTP "Date:" header of responses the clock already
//      fetches (Hebcal, geolocation) - second resolution, ample for a clock
//      that shows minutes.
enum : uint8_t { DNS_IDLE, DNS_PENDING, DNS_DONE, DNS_FAILED };
static std::atomic<uint8_t>  s_dnsState{DNS_IDLE};
static std::atomic<uint32_t> s_dnsGen{0};     // bumped by begin(); stale answers are dropped
static ip_addr_t             s_dnsResult;     // written on the lwIP task before state = DONE
static char                  s_dnsHost[64];
static unsigned long         s_dnsStartedMs = 0;
static unsigned long         s_dnsRetryAtMs = 0;

static IPAddress     s_serverIp;
static bool          s_haveServerIp    = false;
static unsigned long s_serverIpAtMs    = 0;

static void dnsFound(const char*, const ip_addr_t* ip, void* arg) {   // lwIP task
    if ((uint32_t)(uintptr_t)arg != s_dnsGen) return;
    if (ip && IP_IS_V4(ip)) { s_dnsResult = *ip; s_dnsState = DNS_DONE; }
    else                    { s_dnsState = DNS_FAILED; }
}

static void dnsStartOnTcpip(void* arg) {                              // lwIP task
    ip_addr_t addr;
    // Ask for an IPv4 address explicitly (the UDP send below is IPv4-only);
    // the IP_IS_V4 checks on the answer stay as a second guard.
#if LWIP_IPV4 && LWIP_IPV6
    err_t e = dns_gethostbyname_addrtype(s_dnsHost, &addr, dnsFound, arg, LWIP_DNS_ADDRTYPE_IPV4);
#else
    err_t e = dns_gethostbyname(s_dnsHost, &addr, dnsFound, arg);
#endif
    if (e == ERR_OK) { s_dnsResult = addr; s_dnsState = IP_IS_V4(&addr) ? DNS_DONE : DNS_FAILED; }   // answered from lwIP's cache
    else if (e != ERR_INPROGRESS) { s_dnsState = DNS_FAILED; }        // ERR_INPROGRESS: dnsFound() follows
}

static void startDnsLookup(unsigned long now) {
    strlcpy(s_dnsHost, g_server.c_str(), sizeof(s_dnsHost));
    s_dnsStartedMs = now;
    s_dnsState = DNS_PENDING;
    if (tcpip_callback(dnsStartOnTcpip, (void*)(uintptr_t)(uint32_t)s_dnsGen) != ERR_OK) s_dnsState = DNS_FAILED;
}

// Called every loop(). Returns true when a fresh address just arrived.
static bool serviceDns(unsigned long now) {
    uint8_t st = s_dnsState;
    if (st == DNS_PENDING && now - s_dnsStartedMs >= DNS_GIVE_UP_MS) st = DNS_FAILED;

    if (st == DNS_DONE) {
        s_serverIp = IPAddress(ip4_addr_get_u32(ip_2_ip4(&s_dnsResult)));
        s_haveServerIp = true;
        s_serverIpAtMs = now;
        s_dnsState = DNS_IDLE;
        if (!s_dnsWasOk) {
            DiagnosticLog::record("NTP", "DNS lookup for %s works again after %u failed attempt(s)", g_server.c_str(), (unsigned)s_dnsFailStreak);
            s_dnsWasOk = true;
            s_dnsFailStreak = 0;
        }
        CLOCK_LOG("NTP", "Resolved %s -> %s", g_server.c_str(), s_serverIp.toString().c_str());
        return true;
    }
    if (st == DNS_FAILED) {
        s_dnsState = DNS_IDLE;
        s_dnsFailStreak++;
        if (s_dnsWasOk) {
            s_dnsWasOk = false;
            DiagnosticLog::record("NTP", "DNS lookup for %s failing", g_server.c_str());
        }
        CLOCK_LOG("NTP", "DNS failed for %s (streak=%u)", g_server.c_str(), (unsigned)s_dnsFailStreak);
        s_dnsRetryAtMs = now + (g_haveSync ? DNS_RETRY_SYNCED_MS : RETRY_INTERVAL_MS);
    }
    return false;
}

static void sendPacket() {
    if (WiFi.status() != WL_CONNECTED) return;

    unsigned long now = millis();
    bool stale = !s_haveServerIp || now - s_serverIpAtMs >= DNS_REFRESH_MS;
    if (stale && s_dnsState == DNS_IDLE && (int32_t)(now - s_dnsRetryAtMs) >= 0) startDnsLookup(now);
    if (!s_haveServerIp) return;   // first lookup still running; loop() sends as soon as it lands
    IPAddress addr = s_serverIp;   // a refresh in flight doesn't hold up this request

    memset(g_packet, 0, NTP_PACKET_SIZE);
    // LI=0, VN=4, Mode=3 (client)
    g_packet[0]  = 0b00100011;
    g_packet[1]  = 0;    // stratum
    g_packet[2]  = 6;    // polling interval
    g_packet[3]  = 0xEC; // clock precision

    g_udp.beginPacket(addr, NTP_PORT);
    g_udp.write(g_packet, NTP_PACKET_SIZE);
    g_udp.endPacket();

    g_waiting = true;
    CLOCK_LOG("NTP", "Packet sent to %s", g_server.c_str());
}

// Parse received packet → UTC seconds (integer).
// Returns false if no packet available or packet malformed.
static bool recvPacket(time_t& outUtcSec, long& outFracMs) {
    int cb = g_udp.parsePacket();
    if (cb < NTP_PACKET_SIZE) return false;

    g_udp.read(g_packet, NTP_PACKET_SIZE);

    // Transmit Timestamp field: bytes 40–47
    unsigned long hi = ((unsigned long)g_packet[40] << 24) |
                       ((unsigned long)g_packet[41] << 16) |
                       ((unsigned long)g_packet[42] <<  8) |
                        (unsigned long)g_packet[43];

    unsigned long fracRaw = ((unsigned long)g_packet[44] << 24) |
                            ((unsigned long)g_packet[45] << 16) |
                            ((unsigned long)g_packet[46] <<  8) |
                             (unsigned long)g_packet[47];

    // NTP's seconds field is 32-bit, counted from 1900: it wraps on
    // 7 Feb 2036 and starts again at 0 ("era 1"). The old check here
    // (hi < 1970's value -> invalid) would have rejected every answer from
    // that day on. Instead, take the era that puts the time at or after
    // NTP_ERA_PIVOT_UNIX - right for 136 years after the pivot (to 2162).
    if (hi == 0) return false;   // "no time" (kiss-of-death / unsynchronized server)
    int64_t unixSec = (int64_t)hi - (int64_t)NTP_EPOCH_OFFSET;
    while (unixSec < NTP_ERA_PIVOT_UNIX) unixSec += (int64_t)1 << 32;

    outUtcSec = (time_t)unixSec;
    // NTP fractional: 2^32 units per second → convert to ms
    outFracMs = (long)(((uint64_t)fracRaw * 1000ULL) >> 32);
    return true;
}

// Current model time in ms since Unix epoch (UTC).
// Accounts for millis() elapsed + any slew correction.
static int64_t currentUtcMs() {
    if (!g_haveSync) return 0;

    unsigned long nowMs  = millis();
    unsigned long elapsed = nowMs - g_baseMillis;

    // Apply slew: limit correction to MAX_SLEW_MS_PER_SEC per real second elapsed
    long slewApplied = 0;
    if (g_pendingSlewMs != 0) {
        long maxAdjust = (long)((int64_t)elapsed * MAX_SLEW_MS_PER_SEC / 1000);
        if (maxAdjust < 0) maxAdjust = 0;
        long absPending = g_pendingSlewMs < 0 ? -g_pendingSlewMs : g_pendingSlewMs;
        if (maxAdjust > absPending) maxAdjust = absPending;
        slewApplied = (g_pendingSlewMs > 0) ? maxAdjust : -maxAdjust;

        g_pendingSlewMs -= slewApplied;
        // Move base forward so we don't re-apply the same slew next call
        g_baseMillis    += slewApplied;
        g_pendingSlewMs -= 0;  // (already adjusted above)
    }

    return (int64_t)g_baseUnixTime * 1000LL
           + (int64_t)g_baseFracMs
           + (int64_t)(elapsed + slewApplied);
}

// Apply a new UTC measurement from NTP.
static void applySync(time_t newUtcSec, long newFracMs) {
    unsigned long nowMs = millis();

    if (!g_haveSync) {
        // First sync: just step
        g_baseUnixTime  = newUtcSec;
        g_baseFracMs    = newFracMs;
        g_baseMillis    = nowMs;
        g_pendingSlewMs = 0;
        g_haveSync      = true;
        g_lastGoodMs    = nowMs;
        
        // Set system clock so standard C time functions (and StringTools) work
        struct timeval tv = {newUtcSec, (suseconds_t)(newFracMs * 1000)};
        settimeofday(&tv, NULL);
        g_timeSyncedThisBoot = true;   // after settimeofday: log timestamps read the system clock

        // Format human readable UTC time
        struct tm timeinfo;
        gmtime_r(&newUtcSec, &timeinfo);
        char timeStr[25];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
        
        CLOCK_LOG("NTP", "First sync: %lld (%s UTC) (step)", (long long)newUtcSec, timeStr);
        return;
    }

    // Compare NTP measurement against our model
    int64_t modelMs  = currentUtcMs();
    int64_t actualMs = (int64_t)newUtcSec * 1000LL + newFracMs;
    int64_t diffMs   = actualMs - modelMs;

    long diffSec = (long)(diffMs / 1000LL);

    if (diffSec >= STEP_THRESHOLD_SEC || diffSec <= -STEP_THRESHOLD_SEC) {
        // Large offset → hard step
        g_baseUnixTime  = newUtcSec;
        g_baseFracMs    = newFracMs;
        g_baseMillis    = nowMs;
        g_pendingSlewMs = 0;
        CLOCK_LOG("NTP", "Sync: step %.1f s", (double)diffMs / 1000.0);
    } else if (diffMs <= -IGNORE_THRESHOLD_MS || diffMs >= IGNORE_THRESHOLD_MS) {
        // Moderate offset → schedule slew
        g_pendingSlewMs += (long)diffMs;
        CLOCK_LOG("NTP", "Sync: slew %+ld ms (total pending: %+ld ms)",
                      (long)diffMs, g_pendingSlewMs);
    } else {
        // Tiny offset → ignore
        CLOCK_LOG("NTP", "Sync: within ±%ld ms — ignored", IGNORE_THRESHOLD_MS);
    }

    g_lastGoodMs = nowMs;
    g_haveSync   = true;
    g_timeSyncedThisBoot = true;
}

// Shared bodies for the public functions below that call each other
// in-process (getDateTime()/getLocalEpoch() -> getTime(), begin() ->
// setTimezone()) - the lock is taken once, by whichever public function
// is the actual entry point, so the inner call must reach these directly
// instead of going back through the public (lock-taking) wrapper, or it
// would try to take the same non-recursive mutex twice and deadlock.

static time_t getTimeInternal() {
    if (!g_haveSync) return 0;
    int64_t utcMs = currentUtcMs();
    return (time_t)(utcMs / 1000LL) + g_debugTimeOffsetSec.load();
}

static void setTimezoneInternal(const char* tzPOSIX) {
    if (!tzPOSIX || !*tzPOSIX) return;
    setenv("TZ", tzPOSIX, 1);
    tzset();
    CLOCK_LOG("NTP", "Timezone updated (POSIX): %s", tzPOSIX);
}

// =============================================================================
// Public API
// =============================================================================

namespace NTPManager {

void begin(const char* server, const char* tzPOSIX) {
    NTPLock::Guard guard;
    g_server      = server;
    if (tzPOSIX) setTimezoneInternal(tzPOSIX);
    g_haveSync    = false;
    g_baseMillis  = millis();
    g_pendingSlewMs = 0; // Reset slew on begin
    g_lastAttemptMs = 0;
    g_lastGoodMs    = 0;
    g_waiting       = false;

    // A new server name: forget the old address, and drop any lookup still
    // in flight for the previous name (its answer is ignored by generation).
    s_dnsGen++;
    s_dnsState     = DNS_IDLE;
    s_haveServerIp = false;
    s_dnsRetryAtMs = 0;

    g_udp.begin(NTP_LOCAL_PORT);
    CLOCK_LOG("NTP", "Ready — server: %s, tz: %s", server, tzPOSIX ? tzPOSIX : "UTC");
}

void setTimezone(const char* tzPOSIX) {
    NTPLock::Guard guard;
    setTimezoneInternal(tzPOSIX);
}

void setDebugOffset(int64_t offset) {
    NTPLock::Guard guard;
    g_debugTimeOffsetSec = offset;
}

void loop() {
    NTPLock::Guard guard;
    unsigned long now = millis();

    // Check for incoming reply first (non-blocking)
    if (g_waiting) {
        time_t utcSec; long fracMs;
        if (recvPacket(utcSec, fracMs)) {
            applySync(utcSec, fracMs);
            g_waiting = false;
            s_replyStreak.ok("NTP", "Time server reply");
        } else if (now - g_lastAttemptMs >= REPLY_TIMEOUT_MS) {
            s_replyStreak.fail("NTP", "Time server reply", "no answer");
            // UDP is unreliable by design - a single lost reply must not stall
            // sync forever. Give up on this attempt so the interval check below
            // can send a fresh request instead of waiting for a packet that may
            // never arrive.
            CLOCK_LOG("NTP", "No reply within %lums - abandoning attempt, will retry", REPLY_TIMEOUT_MS);
            g_waiting = false;
            // pool.ntp.org hands out rotating servers - one that stopped
            // answering gets replaced by re-resolving the name next attempt.
            s_serverIpAtMs = now - DNS_REFRESH_MS;
        }
    }

    // A first address arriving while unsynced: send right away instead of
    // waiting out the rest of the retry interval.
    if (serviceDns(now) && !g_haveSync && !g_waiting) g_lastAttemptMs = 0;

    // Decide whether to send a new request
    unsigned long interval = g_haveSync ? SYNC_INTERVAL_MS : RETRY_INTERVAL_MS;
    if (now - g_lastAttemptMs >= interval) {
        g_lastAttemptMs = now;
        if (!g_waiting) sendPacket();   // Don't double-send
    }
}

time_t getTime() {
    NTPLock::Guard guard;
    return getTimeInternal();
}

bool getDateTime(DateTime& out) {
    NTPLock::Guard guard;
    time_t t = getTimeInternal();
    if (t == 0) return false;

    struct tm info;
    localtime_r(&t, &info);

    // Centralized "Expert" Arithmetic:
    // tm_year: years since 1900. tm_mon: 0-11.
    out.year   = info.tm_year + 1900;
    out.month  = info.tm_mon + 1;
    out.day    = info.tm_mday;
    out.hour   = info.tm_hour;
    out.minute = info.tm_min;
    out.second = info.tm_sec;
    out.isDST  = (info.tm_isdst > 0);
    return true;
}

// Days since 1970-01-01 for a Gregorian date (Howard Hinnant's
// days_from_civil - exact for any year, 64-bit safe).
static int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

// The local wall-clock time at this instant, as seconds "since 1970" (local
// date and time read as if they were UTC). Until 2026-10-24 this was
// t - mktime(gmtime(t)): that asks whether summer time applies at the LOCAL
// time that reads like UTC now - a different instant whenever the two fall on
// opposite sides of a switch, so for hours around each switch the result was
// an hour off (found by the abroad test: San Jose switched an hour early).
time_t getLocalEpoch() {
    NTPLock::Guard guard;
    time_t t = getTimeInternal();
    if (t == 0) return 0;
    struct tm lt;
    localtime_r(&t, &lt);
    return (time_t)(daysFromCivil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) * 86400 +
                    lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
}

void forceSync() {
    NTPLock::Guard guard;
    g_lastAttemptMs = 0;   // Forces send on next loop()
    g_waiting       = false;
}

bool isSynced() {
    NTPLock::Guard guard;
    return g_haveSync;
}

unsigned long millisSinceSync() {
    NTPLock::Guard guard;
    if (!g_haveSync) return 0;
    return millis() - g_lastGoodMs;
}

} // namespace NTPManager
