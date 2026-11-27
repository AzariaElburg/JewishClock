// =============================================================================
// NTPManager.cpp
// JewishClock - Raw UDP NTP time synchronisation with step/slew discipline.
// =============================================================================

#include <Arduino.h>  
#include "FileTools.h"
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
#include "Uptime.h"
#include "Config.h"
#include "Display.h"
#include <LittleFS.h>
#include <esp_system.h>   // esp_random(): the question's stamp
#include "Tunables.h"
#include "Texts.h"
#include "NetHealth.h"
#include <ArduinoJson.h>

// === TUNABLES ===  (rule: see Config.h header)
// =============================================================================

// Correcting the clock (2026-11-17, after RFC 4330/5905/8633 and chrony; the slew before ran
// the wrong way - a clock 1 s behind ended 2 s behind):
static const int64_t  STEP_THRESHOLD_MS    = 1000;  // off by this or more: a jump - once a second reply agrees
static const int64_t  IGNORE_THRESHOLD_MS  = 50;    // off by less: left as it is
static const int64_t  MAX_SLEW_MS_PER_SEC  = 5;     // in between: corrected gradually, this fast (1 s in 200 s)
static const int64_t  CONFIRM_AGREE_MS     = 500;   // two replies agree on a jump when within this of each other
static const int64_t  FIRST_SYNC_FAR_DAYS  = 400;   // a first time this far after the last seen one (or before it): confirmed too
static const int64_t  MAX_ROOT_DISTANCE_MS = 1000;  // a server this unsure of its own time (RFC 5905 MAXDIST): not used
// A round trip below zero can't come from the network (delay only adds); only from measuring.
// The send is stamped before it goes out (a slow send then only lengthens it), so what is left is
// rounding on our side - the uptime in whole ms, the times cut to whole ms: this much - plus the
// server's own clock steps, which it states in every reply ("precision", checkReply()): twice that.
// Beyond the sum the reply is broken or forged - refused (the user, 2026-11-19: a number worked
// out per server, not a guess; no ping needed - it doesn't depend on the network).
static const int64_t  OWN_ROUNDING_MS = 3;
static const int      COARSEST_SERVER_PREC = -4;   // a server's stated steps taken as at most 2^-4 s = 62.5 ms (a forged
                                                   // "precision" can't open the door wide)

static const int64_t SYNC_INTERVAL_MS  = 6LL * 3600 * 1000;    // 6 hours - at most; each wait drawn anew:
static const int64_t SYNC_JITTER_MS    = 30LL * 60 * 1000;     // ... this much shorter at random (5.5-6 h): never
                                                               // the same beat (the NTP Pool: "query the NTP servers at random
                                                               // times of the day"; the user, 2026-11-21)
static const int64_t RETRY_INTERVAL_MS = 5LL * 1000;           // unsynced, or confirming: the next ask, to the NEXT server
// Never sooner to the same server (2026-11-21, the user: from the source, not a guess): RFC 4330 s5 -
// at most once a minute; s10's 15 s is only the bare floor. Neither Cloudflare nor Google states its own.
static const int64_t SAME_SERVER_MIN_MS = 60LL * 1000;
static const int64_t REPLY_TIMEOUT_MS  = 2LL * 1000;           // Give up on a lost reply so we can retry
static const int64_t KOD_RATE_WAIT_MS  = 5LL * 60 * 1000;      // "RATE": that server not asked for this long (doubling while repeated)
static const int64_t FAILED_RETRY_MS   = 64LL * 1000;          // synced, an ask failed: again after this, doubling up to SYNC_INTERVAL_MS
// A first opinion on a jump counts this long; a reply after it starts a new pair (2026-11-21: a stale
// one, minutes old and from another situation, "disagreed" with a right reply and dropped it).
static const int64_t CONFIRM_MAX_AGE_MS = 4 * SAME_SERVER_MIN_MS;
// A server that fails (silent with the internet there, an unusable reply, no address) rests (not asked)
// SAME_SERVER_MIN_MS x 1, 2, 4... while it keeps failing, up to this - RFC 4330 s10: back off when
// there is no answer (2026-11-21: the tests' clocks, asking every 16 s for hours, got the home address
// rate-limited by a pool server - silent drops, "RATE" kisses).
static const int64_t SILENT_REST_MAX_MS = 256LL * 1000;
// A kiss-o'-death RATE: never longer than this (RFC 8633 s5.4: "should not exceed a poll interval
// value of 13 (two hours)") - also when the kiss's own poll field asks for more.
static const int64_t KOD_RATE_MAX_MS = 2LL * 3600 * 1000;
// "Not recommended" (troubleWith()): cleared after this many good answers on separate days; asked once a
// day meanwhile - TROUBLE_CHECK_MIN_MS + up to TROUBLE_CHECK_SPREAD_MS at random (the user, 2026-11-21).
static const int      TROUBLE_GOOD_DAYS       = 7;
static const int64_t  TROUBLE_CHECK_MIN_MS    = 20LL * 3600 * 1000;
static const int64_t  TROUBLE_CHECK_SPREAD_MS = 8LL * 3600 * 1000;

static const int64_t DNS_REFRESH_MS        = 24LL * 3600 * 1000;  // Re-resolve the server name daily
static const int64_t DNS_REQUERY_MIN_MS    = 3600LL * 1000;       // after a failure, at most this often (the NTP Pool's rule)
static const int64_t DNS_RETRY_SYNCED_MS   = 60LL * 1000;         // After a failed lookup, once synced
static const int64_t DNS_GIVE_UP_MS        = 30LL * 1000;         // Treat a lookup with no answer as failed
static const int64_t TIME_SEEN_STEP_SEC    = 30LL * 86400;        // the latest time seen is saved this often (era pivot)
static const uint32_t NO_REPLY_LOG_AFTER = 3;   // lost replies in a row before the persistent log hears of it
static DiagnosticLog::Streak s_replyStreak{NO_REPLY_LOG_AFTER};
// Asked by the tests (sim/resilience.py drill_ntp), never copied there (Tunables.h):
TUNABLE_SHARED(STEP_THRESHOLD_MS) TUNABLE_SHARED(IGNORE_THRESHOLD_MS) TUNABLE_SHARED(MAX_SLEW_MS_PER_SEC)
TUNABLE_SHARED(CONFIRM_AGREE_MS) TUNABLE_SHARED(FIRST_SYNC_FAR_DAYS) TUNABLE_SHARED(MAX_ROOT_DISTANCE_MS)
TUNABLE_SHARED(OWN_ROUNDING_MS) TUNABLE_SHARED(COARSEST_SERVER_PREC) TUNABLE_SHARED(SYNC_INTERVAL_MS)
TUNABLE_SHARED(RETRY_INTERVAL_MS) TUNABLE_SHARED(SAME_SERVER_MIN_MS) TUNABLE_SHARED(REPLY_TIMEOUT_MS)
TUNABLE_SHARED(KOD_RATE_WAIT_MS) TUNABLE_SHARED(FAILED_RETRY_MS)
TUNABLE_SHARED(SYNC_JITTER_MS) TUNABLE_SHARED(CONFIRM_MAX_AGE_MS) TUNABLE_SHARED(SILENT_REST_MAX_MS) TUNABLE_SHARED(KOD_RATE_MAX_MS)
TUNABLE_SHARED(TROUBLE_GOOD_DAYS) TUNABLE_SHARED(TROUBLE_CHECK_MIN_MS) TUNABLE_SHARED(TROUBLE_CHECK_SPREAD_MS) TUNABLE_SHARED(NO_REPLY_LOG_AFTER)

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

// The settings' servers (empty ones left out) and the one in use (the last asked). Each question
// goes to the best that may be asked (pickServer(): the recommended ones first, in the list's order)
// - since 2026-11-21; from 2026-11-13 in turn, before that only the first of the list.
static String         g_servers[NTP_SERVER_SLOTS];
static int            g_serverCount  = 0;
static int            g_cur          = 0;
static String         g_server       = NTP_SERVER_1;   // = g_servers[g_cur]

// Time model (2026-11-17):
//   currentUtcMs() = g_baseUtcMs + (Uptime::ms() - g_baseMs) + slewDone()
//   g_baseUtcMs  = UTC ms at g_baseMs (the last sync, jump or start of a slew)
//   g_baseMs     = Uptime::ms() then (64-bit: no sync for months stays right)
//   g_slewMs     = a correction being added gradually since g_baseMs, MAX_SLEW_MS_PER_SEC
//                  at most: the clock runs 0.5% fast or slow meanwhile - never backwards.
// Nothing here changes when the time is read: a sync rebases it (rebase()).
static bool           g_haveSync      = false;
static int64_t        g_baseUtcMs     = 0;
static int64_t        g_baseMs        = 0;
static int64_t        g_slewMs        = 0;
std::atomic<int64_t>  g_debugTimeOffsetSec{0};   // see DebugTime.h - read lock-free by log timestamps
std::atomic<bool>     g_timeSyncedThisBoot{false}; // see DebugTime.h - set on first sync, never cleared

static Timer          g_sinceAttempt;        // since the last send; not running = send now
static Timer          g_sinceGood;           // since the last successful sync
static bool           g_waiting       = false; // packet sent, awaiting reply
static bool           g_sendPending   = false; // an ask due, waiting for the server's address
// The question in flight: a random transmit timestamp the answer must carry back as its
// origin (RFC 4330 s5 - only an answer to THIS question counts: no late, duplicate or
// forged one), when it left, and to which address.
static uint8_t        g_sentStamp[8];
static int64_t        g_sentAtMs      = 0;
static IPAddress      g_sentTo;
// A jump waiting for a second reply to agree (the next server's, if there is one).
static bool           g_confirming    = false;
static int64_t        g_confirmDiffMs = 0;   // the first reply's time minus the clock's (or minus uptime, unsynced)
static int64_t        g_confirmAtMs   = 0;   // when (uptime) - older than CONFIRM_MAX_AGE_MS: no longer counts
static int            g_confirmFrom   = -1;  // whose opinion it was: the second one must be another server's
// Each server's rest (RFC 4330 s10, RFC 8633 s5.4): not asked until this uptime (0 = free) - after a
// kiss-o'-death (RATE: KOD_RATE_WAIT_MS doubling, or the kiss's own poll field, at most KOD_RATE_MAX_MS;
// DENY/RSTR: for good) or a failure (silence with the internet there, an unusable reply, no address:
// SAME_SERVER_MIN_MS doubling up to SILENT_REST_MAX_MS). A good answer ends its rests.
static int64_t        g_restUntil[NTP_SERVER_SLOTS]  = {};
static uint8_t        g_badInRow[NTP_SERVER_SLOTS]   = {};
static uint8_t        g_rateInRow[NTP_SERVER_SLOTS]  = {};
static int64_t        g_lastAskMs[NTP_SERVER_SLOTS]  = {};   // uptime of the last question to it (0 = none): SAME_SERVER_MIN_MS
// Something came back from the address asked during this question's wait (late, repeated, not ours):
// the server isn't silent - a wait with no usable answer then isn't held against it as silence.
static bool           g_heardFromServer = false;
// A question due now - after a failure (the next server at once), a forced sync, the internet back,
// a new list; sent as soon as a server may be asked (pickServer()).
static bool           g_askNow        = false;
// The question in flight is a not-recommended server's daily check (not the clock's regular one):
// its failure asks no other server.
static bool           g_isCheck       = false;
static bool           g_allRestingSaid = false;   // "every server resting" said once per wait
// A silent server, or a name without an address: is it the server or the internet? (the user,
// 2026-11-21: "when NTP is silent, immediately check the internet connection, and only upon that check
// decide"). Nothing asked meanwhile. Internet there -> the server's fault (troubleWith()); none -> nobody's,
// watched (NetHealth, once a minute) until it answers, then the same server asked "as if nothing happened".
static bool           g_netCheck      = false;
static int64_t        g_netCheckFromMs = 0;
static char           g_netCheckWhy[32] = "";
static bool           g_netDownSaid   = false;
// Synced, the last ask(s) failed: the next one sooner than 6 h - 64 s, doubling (RFC 4330 s10); 0 = none failed.
static int64_t        g_failedRetryMs = 0;
static int64_t        g_syncWaitMs    = SYNC_INTERVAL_MS;   // this round's wait to the next regular sync (drawn per sync)
static void askFailed() { g_failedRetryMs = g_failedRetryMs ? (g_failedRetryMs * 2 < SYNC_INTERVAL_MS ? g_failedRetryMs * 2 : SYNC_INTERVAL_MS) : FAILED_RETRY_MS; }
// The latest time this clock has seen (TIME_SEEN_PATH, below) - what its file says.
static std::atomic<int64_t> s_timeSeenSaved{0};

// "Not recommended" (the user, 2026-11-21): a server that failed while the internet was there - silent,
// an unusable reply, a kiss-o'-death, no address - goes to the bottom of the order: asked only when every
// recommended one can't be. Once a day (TROUBLE_CHECK_MIN_MS + up to TROUBLE_CHECK_SPREAD_MS, at random)
// it gets one question; TROUBLE_GOOD_DAYS good answers on separate days clear the mark, a failure starts
// the count again. Kept by name in NTP_TROUBLE_PATH (a restart doesn't forget), shown in Settings.
struct Trouble {
    bool    on          = false;
    char    why[48]     = "";
    int64_t sinceUnix   = 0;    // UTC s when marked; 0 = before the first sync (filled in then)
    uint8_t goodDays    = 0;    // good answers since, one a day at most
    int64_t lastGoodUnix = 0;   // UTC s of the last one counted
    int64_t lastGoodMs  = 0;    // ... its uptime (0 = not this run)
    int64_t nextCheckMs = 0;    // uptime of its next daily question (not kept: after a restart, a new draw)
};
static Trouble        g_trouble[NTP_SERVER_SLOTS];
static std::atomic<bool>    s_troubleDirty{false};
static std::atomic<int64_t> s_troubleWaitMs{0};   // Uptime::ms() a write began waiting for a still screen, 0 = none

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
static uint64_t s_dnsFailStreak = 0;

// ── The packet, both ways (one copy for syncing and the page's check) ───────
// The transmit timestamp: random, not our time (we may have none yet) - the server copies
// it back as the origin, and nobody can guess it (RFC 4330 s3/s5). Kept in `stamp`.
static void fillRequest(uint8_t* p, uint8_t* stamp) {
    memset(p, 0, NTP_PACKET_SIZE);
    p[0] = 0b00100011;   // LI=0, VN=4, Mode=3 (client)
    p[1] = 0;            // stratum
    p[2] = 6;            // polling interval
    p[3] = 0xEC;         // clock precision
    for (int i = 0; i < 8; i += 4) { uint32_t r = esp_random(); memcpy(stamp + i, &r, 4); }
    if (!stamp[0] && !stamp[1]) stamp[0] = 1;   // never all zero (= "no origin")
    memcpy(p + 40, stamp, 8);
}

// NTP's seconds field is 32-bit, counted from 1900: it wraps every 136 years
// (first on 7 Feb 2036) and carries no era. The answer is read as the first
// time at or after the pivot: the build's NTP_ERA_PIVOT_UNIX (2026), raised to
// a year before the latest time this clock has seen (timeSeen(), kept in
// TIME_SEEN_PATH) - so a clock that keeps running reads every answer right for
// ever, not just until 2162 (2026-11-14; the clock's dates go to 2239).
static int64_t s_eraPivotUnix = NTP_ERA_PIVOT_UNIX;

static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]; }
static uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

// An NTP timestamp (64-bit: seconds since 1900 . fraction) as Unix ms. Its era (2026-11-17,
// RFC 5905's way): the one nearest the latest time this clock has seen, when it has one - right
// while the true time is within 68 years of it, and a wrong saved time (two lying servers once)
// can't throw every later answer 136 years ahead (the pivot rule read a real 2026 answer as 2162
// after a saved 2036). Without a saved time: the first at or after the pivot, as before.
static int64_t timestampUnixMs(uint64_t ts) {
    int64_t unixSec = (int64_t)(ts >> 32) - (int64_t)NTP_EPOCH_OFFSET;
    const int64_t ERA = (int64_t)1 << 32;
    int64_t seen = s_timeSeenSaved;
    if (seen > 0) {
        int64_t k = (seen - unixSec + ERA / 2) / ERA;   // eras forward to the nearest (seen is after 2026: k >= 0)
        if (seen - unixSec + ERA / 2 < 0) k = 0;
        unixSec += k * ERA;
    } else {
        while (unixSec < s_eraPivotUnix) unixSec += ERA;
    }
    return unixSec * 1000 + (int64_t)(((ts & 0xFFFFFFFFull) * 1000ull) >> 32);
}

// A reply, checked (RFC 4330 s5, RFC 5905 s7-8, RFC 8633 s5.3-5.4): "" = good, and `utcMs` =
// the server's time at the moment the reply arrived (gotAtMs) - its send time plus half the
// travel time (round trip minus the server's own time on it). Otherwise why it isn't used;
// `kiss` = the kiss-o'-death code (stratum 0), "" for any other reason.
static String checkReply(const uint8_t* p, const uint8_t* stamp, int64_t sentAtMs, int64_t gotAtMs,
                         int64_t& utcMs, char kiss[5], int64_t* roundTripMs = nullptr) {
    kiss[0] = 0;
    int li = p[0] >> 6, vn = (p[0] >> 3) & 7, mode = p[0] & 7, stratum = p[1];
    if (mode != 4) return Texts::fill(Texts::ntpNotServerReply, String(mode));
    if (vn < 1 || vn > 4) return "unknown NTP version " + String(vn);
    if (memcmp(p + 24, stamp, 8) != 0) return Texts::ntpNotAnswer;
    if (stratum == 0) {   // a kiss-o'-death: its code in the reference id, the times not to be used
        for (int i = 0; i < 4; i++) kiss[i] = (p[12 + i] >= 32 && p[12 + i] < 127) ? (char)p[12 + i] : '?';
        kiss[4] = 0;
        return Texts::fill(Texts::ntpKiss, String(kiss));
    }
    if (stratum > 15) return Texts::fill(Texts::ntpClockUnset, String(stratum));
    if (li == 3) return Texts::ntpClockAlarm;
    uint64_t t2 = be64(p + 32), t3 = be64(p + 40);
    if (!t3 || !t2) return "it answered without a time";
    // Root distance: its delay to the reference / 2 + its dispersion, 16.16 seconds.
    int64_t rootDelayMs = ((int64_t)(int32_t)be32(p + 4) * 1000) >> 16;
    int64_t rootDispMs  = ((int64_t)be32(p + 8) * 1000) >> 16;
    if (rootDelayMs < 0 || rootDelayMs / 2 + rootDispMs > MAX_ROOT_DISTANCE_MS)
        return "the server is unsure of its own time (root distance " + String((long)(rootDelayMs / 2 + rootDispMs)) + " ms)";
    int64_t serverHeldMs = (int64_t)((((t3 - t2) & 0xFFFFFFFFFFFFull) * 1000ull) >> 32);   // t3 - t2, as ms
    int64_t delayMs = (gotAtMs - sentAtMs) - serverHeldMs;
    // Below zero only by measuring (OWN_ROUNDING_MS, above): ours, plus the server's own clock steps
    // on its two times - precision is log2 seconds, signed (-20 = 1 us, -6 = 16 ms); clamped so a
    // forged precision can't open the door wide (2026-11-19: -5 ms on a busy PC, the send stamped
    // after it, refused a good reply).
    int prec = (int8_t)p[3];
    if (prec > COARSEST_SERVER_PREC) prec = COARSEST_SERVER_PREC;
    int64_t serverStepMs = prec >= -10 ? (1000 >> -prec) : 0;   // finer than 1 ms: rounding only
    if (delayMs < -(OWN_ROUNDING_MS + 2 * serverStepMs)) return Texts::fill(Texts::ntpImpossibleTrip, String((long)delayMs));
    if (roundTripMs) *roundTripMs = delayMs;   // as measured (below zero: our rounding)
    if (delayMs < 0) delayMs = 0;
    utcMs = timestampUnixMs(t3) + delayMs / 2;
    return "";
}

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
// One lookup = one NameLookup (2026-11-14: was written out twice, for the
// server in use and for the page's "check this server"). Two of them, as an
// index in the callback's argument together with a generation: start() and
// cancel() bump it, so an answer still on its way for an older question is
// dropped.
enum : uint8_t { DNS_IDLE, DNS_PENDING, DNS_DONE, DNS_FAILED };
struct NameLookup {
    std::atomic<uint8_t>  state{DNS_IDLE};
    std::atomic<uint32_t> gen{0};
    ip_addr_t             result;                        // written on the lwIP task before state = DONE
    char                  host[NTP_SERVER_MAX_CHARS + 1];
    Timer                 since;                         // the question's age (give up)

    void start(const char* name);
    void cancel() { gen++; state = DNS_IDLE; }
    // DNS_DONE (ip filled) / DNS_FAILED (no answer within giveUpMs too) once,
    // then DNS_IDLE; DNS_PENDING while waiting.
    uint8_t poll(int64_t giveUpMs, IPAddress& ip);
};
static NameLookup s_lookups[2];
static NameLookup& s_dns  = s_lookups[0];   // the server in use
static NameLookup& s_tDns = s_lookups[1];   // the page's check

static void* lookupArg(int which, uint32_t gen) { return (void*)(uintptr_t)((gen << 1) | (uint32_t)which); }
static NameLookup* lookupFor(void* arg, bool& current) {
    uint32_t a = (uint32_t)(uintptr_t)arg;
    NameLookup* l = &s_lookups[a & 1];
    current = (a >> 1) == (l->gen.load() & 0x7FFFFFFFu);
    return l;
}
static void dnsFound(const char*, const ip_addr_t* ip, void* arg) {   // lwIP task
    bool current; NameLookup* l = lookupFor(arg, current);
    if (!current) return;
    if (ip && IP_IS_V4(ip)) { l->result = *ip; l->state = DNS_DONE; }
    else                    { l->state = DNS_FAILED; }
}
static void dnsStartOnTcpip(void* arg) {                              // lwIP task
    bool current; NameLookup* l = lookupFor(arg, current);
    if (!current) return;
    ip_addr_t addr;
    // Ask for an IPv4 address explicitly (the UDP send below is IPv4-only);
    // the IP_IS_V4 checks on the answer stay as a second guard.
#if LWIP_IPV4 && LWIP_IPV6
    err_t e = dns_gethostbyname_addrtype(l->host, &addr, dnsFound, arg, LWIP_DNS_ADDRTYPE_IPV4);
#else
    err_t e = dns_gethostbyname(l->host, &addr, dnsFound, arg);
#endif
    if (e == ERR_OK) { l->result = addr; l->state = IP_IS_V4(&addr) ? DNS_DONE : DNS_FAILED; }   // answered from lwIP's cache
    else if (e != ERR_INPROGRESS) { l->state = DNS_FAILED; }        // ERR_INPROGRESS: dnsFound() follows
}
void NameLookup::start(const char* name) {
    uint32_t g = (gen.fetch_add(1) + 1) & 0x7FFFFFFFu;
    StringTools::utf8Copy(host, name);
    since.start();
    state = DNS_PENDING;
    if (tcpip_callback(dnsStartOnTcpip, lookupArg(this == &s_lookups[1] ? 1 : 0, g)) != ERR_OK) state = DNS_FAILED;
}
uint8_t NameLookup::poll(int64_t giveUpMs, IPAddress& ip) {
    uint8_t st = state;
    if (st == DNS_PENDING && since.passed(giveUpMs)) { gen++; st = DNS_FAILED; }   // a late answer is dropped
    if (st == DNS_DONE) ip = IPAddress(ip4_addr_get_u32(ip_2_ip4(&result)));
    if (st == DNS_DONE || st == DNS_FAILED) state = DNS_IDLE;
    return st;
}

// Each server's address, kept by its slot: a server change doesn't look its name up again - the NTP
// Pool's rule (2026-11-21): "re-query DNS at most once an hour" after trouble, else daily.
static IPAddress s_slotIp[NTP_SERVER_SLOTS];
static bool      s_slotHaveIp[NTP_SERVER_SLOTS]  = {};
static Timer     s_slotIpAge[NTP_SERVER_SLOTS];       // since its address arrived
static bool      s_slotSuspect[NTP_SERVER_SLOTS] = {};   // failed since then: looked up again after DNS_REQUERY_MIN_MS
// The pause after a failed lookup (5 s, once synced 60 s). Not running = no
// pause: look up at once. (A "0 = now" in a 32-bit millis() value here stopped
// every time sync from day 24.8 to 49.7 of uptime - found 2026-11-14.)
static Timer     s_dnsPause;
static int64_t   s_dnsPauseMs = 0;
static String    s_dnsPauseName;   // the name whose lookup failed: the pause is for it - another name is looked up at once
                                   // (2026-11-22: a typo in a server name fixed in Settings waited out the pause)
static char      s_moveWhy[48] = "";   // why the next question goes to another server (said when it does)
static char      s_countryPool[3] = "";   // ISO 3166-1 alpha-2 lowercase ("il"); "" = use the global pool

static int64_t currentUtcMs();

// pool.ntp.org / N.pool.ntp.org -> cc.pool.ntp.org / N.cc.pool.ntp.org when s_countryPool is set.
// Returns name.c_str() unchanged for non-pool names or when no country is known.
// Valid until the next call (static buffer).
static bool isGlobalPool(const String& name) {
    if (name == "pool.ntp.org") return true;
    return name.length() == 14 && name[1] == '.' && name[0] >= '0' && name[0] <= '3'
           && strcmp(name.c_str() + 2, "pool.ntp.org") == 0;
}
static const char* poolEffectiveName(const String& name) {
    if (!s_countryPool[0] || !isGlobalPool(name)) return name.c_str();
    static char buf[32];   // "0.il.pool.ntp.org" = 18 chars
    if (name[1] == '.')
        snprintf(buf, sizeof(buf), "%c.%s.pool.ntp.org", name[0], s_countryPool);
    else
        snprintf(buf, sizeof(buf), "%s.pool.ntp.org", s_countryPool);
    return buf;
}

// The server asked next. An answer still on its way for the old name is dropped; the pause after
// a failed lookup stays (until 2026-11-14 a switch cleared it: with every name failing, lookups
// ran back to back).
static void useServer(int i) {
    if (i != g_cur || g_server != g_servers[i]) s_dns.cancel();
    g_cur = i;
    g_server = g_servers[i];
}

// From when server i may be asked: its rest over and SAME_SERVER_MIN_MS since its last question.
static int64_t freeAt(int i) {
    int64_t t = g_restUntil[i];
    if (g_lastAskMs[i] && g_lastAskMs[i] + SAME_SERVER_MIN_MS > t) t = g_lastAskMs[i] + SAME_SERVER_MIN_MS;
    return t;
}

// The best server that may be asked now: the recommended ones in the list's order (the user's
// preference); a not-recommended one only while every recommended one rests after a failure - a
// recommended one that was just asked (SAME_SERVER_MIN_MS) is waited for. A second opinion on a jump
// (`other` >= 0) must be another server's: any other that may be asked, recommended first - the same
// one only when the list has no other (2026-11-21: waiting for the first one's minute asked it twice).
// -1 = none may be now: `soonest` is the first that may be, at uptime `when` (INT64_MAX: none ever).
static int pickServer(int& soonest, int64_t& when, int other = -1) {
    const int64_t now = Uptime::ms();
    soonest = -1;
    when = INT64_MAX;
    if (g_serverCount < 2) other = -1;
    for (int pass = 0; pass < 2; pass++) {
        bool onlyWaiting = false;   // one of this group isn't resting - only asked within the minute
        for (int i = 0; i < g_serverCount; i++) {
            if (g_trouble[i].on != (pass == 1) || i == other) continue;
            int64_t t = freeAt(i);
            if (t <= now) return i;
            if (g_restUntil[i] <= now && other < 0) onlyWaiting = true;
            if (t < when) { when = t; soonest = i; }
        }
        if (onlyWaiting) return -1;
    }
    return -1;
}

static int64_t checkGapMs() { return TROUBLE_CHECK_MIN_MS + (int64_t)(esp_random() % (uint32_t)(TROUBLE_CHECK_SPREAD_MS + 1)); }

// This server not asked for `ms` (a kiss-o'-death, or a failure) - said live, and in the log.
static void rest(int i, int64_t ms, const char* why) {
    g_restUntil[i] = Uptime::ms() + ms;
    DiagnosticLog::record("NTP", Texts::ntpRest, g_servers[i].c_str(), (long long)(ms / 1000), why);
}

// Server i failed (the internet there): not recommended from now - the bottom of the order.
static void markTrouble(int i, const char* why) {
    Trouble& t = g_trouble[i];
    bool was = t.on;
    t.on = true;
    StringTools::utf8Copy(t.why, why);
    t.goodDays = 0;   // a failure starts the count again
    int64_t nowS = g_haveSync ? currentUtcMs() / 1000 : 0;
    if (!was) t.sinceUnix = nowS;
    t.lastGoodUnix = nowS;   // good days count from here
    t.lastGoodMs = Uptime::ms();
    t.nextCheckMs = Uptime::ms() + checkGapMs();
    s_troubleDirty = true;
    DiagnosticLog::record("NTP", was ? Texts::ntpStillTrouble : Texts::ntpMarked, g_servers[i].c_str(), why);
}

// Server i failed - silent with the internet there, an unusable reply, a kiss-o'-death, no address
// for its name: it rests (`restMs`; < 0: SAME_SERVER_MIN_MS doubling while it keeps failing, up to
// SILENT_REST_MAX_MS - RFC 4330 s10), is marked, and the next server is asked at once - unless this
// was its daily check (the clock has its time from the others).
static void troubleWith(int i, const char* why, int64_t restMs) {
    if (g_badInRow[i] < 16) g_badInRow[i]++;
    if (restMs < 0) {
        int64_t ms = SAME_SERVER_MIN_MS << (g_badInRow[i] - 1 < 10 ? g_badInRow[i] - 1 : 10);
        restMs = ms < SILENT_REST_MAX_MS ? ms : SILENT_REST_MAX_MS;
    }
    if (restMs > 0) rest(i, restMs, why);
    markTrouble(i, why);
    s_slotSuspect[i] = true;
    if (!g_isCheck) {
        g_askNow = true;
        StringTools::utf8Copy(s_moveWhy, why);
    }
    g_isCheck = false;
}

// A silence, or a name without an address: the internet checked before anyone is blamed.
static void startNetCheck(const char* why) {
    g_netCheck = true;
    g_netCheckFromMs = Uptime::ms();
    StringTools::utf8Copy(g_netCheckWhy, why);
    g_netDownSaid = false;
    NetHealth::checkNow();
    CLOCK_LOGF("NTP", Texts::ntpNetCheck, g_server.c_str(), why);
}

// Called every loop(). Returns true when a fresh address just arrived.
static bool serviceDns() {
    IPAddress ip;
    uint8_t st = s_dns.poll(DNS_GIVE_UP_MS, ip);
    if (st == DNS_DONE) {
        s_slotIp[g_cur] = ip;
        s_slotHaveIp[g_cur] = true;
        s_slotIpAge[g_cur].start();
        s_slotSuspect[g_cur] = false;
        s_dnsPause.stop();
        if (!s_dnsWasOk) {
            DiagnosticLog::record("NTP", "DNS lookup for %s works again after %u failed attempt(s)", g_server.c_str(), (unsigned)s_dnsFailStreak);
            s_dnsWasOk = true;
            s_dnsFailStreak = 0;
        }
        CLOCK_LOG("NTP", "Resolved %s -> %s", g_server.c_str(), ip.toString().c_str());
        return true;
    }
    if (st == DNS_FAILED) {
        s_dnsFailStreak++;
        if (s_dnsWasOk) {
            s_dnsWasOk = false;
            DiagnosticLog::record("NTP", "DNS lookup for %s failing", g_server.c_str());
        }
        CLOCK_LOG("NTP", "DNS failed for %s (streak=%llu)", g_server.c_str(), (unsigned long long)s_dnsFailStreak);
        s_dnsPauseMs = g_haveSync ? DNS_RETRY_SYNCED_MS : RETRY_INTERVAL_MS;
        s_dnsPauseName = poolEffectiveName(g_server);
        s_dnsPause.start();
        // An address kept from before still serves (a refresh failed); none: is it the name or the internet?
        if (!s_slotHaveIp[g_cur] && g_sendPending && !g_netCheck) {
            g_sendPending = false;
            startNetCheck(Texts::ntpWhyNoAddress);
        }
    }
    return false;
}

static void sendPacket() {
    if (WiFi.status() != WL_CONNECTED) return;
    g_heardFromServer = false;   // this question's own wait

    // Its address: none yet, a day old, or - after a failure - an hour old (asked again).
    const int c = g_cur;
    bool stale = !s_slotHaveIp[c] || s_slotIpAge[c].passed(DNS_REFRESH_MS) ||
                 (s_slotSuspect[c] && s_slotIpAge[c].passed(DNS_REQUERY_MIN_MS));
    if (stale && s_dns.state == DNS_IDLE && (s_dnsPause.idleOrPassed(s_dnsPauseMs) || !s_dnsPauseName.equalsIgnoreCase(poolEffectiveName(g_server))))
        s_dns.start(poolEffectiveName(g_server));
    g_sendPending = !s_slotHaveIp[c];
    if (!s_slotHaveIp[c]) return;   // its address first: loop() sends as soon as it lands
    IPAddress addr = s_slotIp[c];   // a refresh in flight doesn't hold up this request

    while (g_udp.parsePacket() > 0) {}   // anything still waiting answers an older question: dropped
    fillRequest(g_packet, g_sentStamp);
    g_sentAtMs = Uptime::ms();   // before the send - stamped after, a slow send shortened the round trip
    g_lastAskMs[c] = g_sentAtMs;

    g_udp.beginPacket(addr, NTP_PORT);
    g_udp.write(g_packet, NTP_PACKET_SIZE);
    g_udp.endPacket();

    g_sentTo   = addr;
    g_waiting  = true;
    CLOCK_LOGF("NTP", Texts::ntpSent, g_server.c_str());
}

// A good answer from server i: a not-recommended one counts a good day (one per TROUBLE_CHECK_MIN_MS at
// most); TROUBLE_GOOD_DAYS of them and it is recommended again.
static void goodAnswer(int i, int64_t now) {
    Trouble& t = g_trouble[i];
    if (!t.on || !g_haveSync) return;
    int64_t nowS = currentUtcMs() / 1000;
    if (!t.sinceUnix) t.sinceUnix = nowS;   // marked before the first sync
    int64_t gapS;
    if (t.lastGoodMs) gapS = (now - t.lastGoodMs) / 1000;   // this run: by uptime (a jump of the time doesn't count)
    else if (t.lastGoodUnix) gapS = nowS > t.lastGoodUnix ? nowS - t.lastGoodUnix : t.lastGoodUnix - nowS;   // from the file
    else { t.lastGoodUnix = nowS; t.lastGoodMs = now; s_troubleDirty = true; return; }   // marked unsynced: counts from here
    if (gapS < TROUBLE_CHECK_MIN_MS / 1000) return;
    t.lastGoodUnix = nowS;
    t.lastGoodMs = now;
    s_troubleDirty = true;
    if (++t.goodDays >= TROUBLE_GOOD_DAYS) {
        DiagnosticLog::record("NTP", Texts::ntpTroubleCleared, g_servers[i].c_str(), (unsigned)t.goodDays);
        t = Trouble();
        return;
    }
    DiagnosticLog::record("NTP", Texts::ntpGoodDay, g_servers[i].c_str(), (unsigned)t.goodDays, (unsigned)TROUBLE_GOOD_DAYS);
}

// The UTC ms the clock's model gives now (no debug offset); 0 before the first sync.
static int64_t slewDone(int64_t now) {
    if (!g_slewMs) return 0;
    int64_t can = (now - g_baseMs) * MAX_SLEW_MS_PER_SEC / 1000;
    int64_t want = g_slewMs < 0 ? -g_slewMs : g_slewMs;
    if (can > want) can = want;
    return g_slewMs < 0 ? -can : can;
}
static int64_t modelAt(int64_t now) { return g_baseUtcMs + (now - g_baseMs) + slewDone(now); }
static int64_t currentUtcMs() {
    if (!g_haveSync) return 0;
    return modelAt(Uptime::ms());
}
// The model restarted at `now` with time `utcMs`, plus `slewMs` to add gradually from here.
static void rebase(int64_t now, int64_t utcMs, int64_t slewMs) {
    g_baseUtcMs = utcMs;
    g_baseMs    = now;
    g_slewMs    = slewMs;
}

// The clock jumps to `utcMs` (at uptime `now`): the model, and the system clock (log
// timestamps, time() for the C library) with it.
static void stepTo(int64_t now, int64_t utcMs) {
    rebase(now, utcMs, 0);
    struct timeval tv = {(time_t)(utcMs / 1000), (suseconds_t)((utcMs % 1000) * 1000)};
    settimeofday(&tv, NULL);
    g_haveSync = true;
    g_timeSyncedThisBoot = true;   // after settimeofday: log timestamps read the system clock
}

// The last used reply's round trip (ms, as measured): the clock's time is right within half of
// it (RFC 5905) - and it includes how late the loop read the reply (2026-11-20: ~124 ms late on a
// loaded PC put the simulator 62 ms off; ~60 ms passes on the clock: up to ~30 ms). Logged with
// every sync, in /api/status "ntpRoundTripMs" - real round trips seen, not guessed.
static int64_t g_lastRoundTripMs = -1;

static void logStep(const char* what, int64_t utcMs, int64_t diffMs, bool wasSynced) {
    time_t s = (time_t)(utcMs / 1000);
    struct tm t;
    gmtime_r(&s, &t);
    char when[25], by[32] = "";
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &t);
    if (wasSynced) snprintf(by, sizeof(by), " (%+.3f s)", (double)diffMs / 1000.0);
    DiagnosticLog::record("NTP", Texts::ntpStep, what, when, g_server.c_str(), by, (long long)g_lastRoundTripMs);
}

// A first time before the saved "latest seen" one (it was wrong, or the flash came from a later
// clock): the saved one follows it - written at once by serviceTimeSeen() (30 days "ago").
static void seenFollows(int64_t utcMs) {
    int64_t sec = utcMs / 1000;
    if (s_timeSeenSaved > sec) s_timeSeenSaved = sec - TIME_SEEN_STEP_SEC;
}

// Replies that disagree on a jump: one line when it starts, one when two agree again.
static DiagnosticLog::Streak s_agreeStreak{1};

// A checked reply: the server's time `serverUtcMs` at uptime `now`.
//   unsynced: the first time - at once, unless far from the last time this clock has seen
//             (a server stuck in another year): then a second reply must agree;
//   off by < IGNORE_THRESHOLD_MS: left; < STEP_THRESHOLD_MS: corrected gradually;
//   more: a jump - only when a second reply (the next server's, if there is one) agrees.
static void applySync(int64_t now, int64_t serverUtcMs) {
    if (g_confirming && now - g_confirmAtMs > CONFIRM_MAX_AGE_MS) {
        CLOCK_LOG("NTP", "an older first opinion on a jump (%lld s ago) no longer counts", (long long)((now - g_confirmAtMs) / 1000));
        g_confirming = false;
    }
    int64_t model = g_haveSync ? modelAt(now) : now;   // unsynced: compared by uptime (both replies)
    int64_t diff = serverUtcMs - model;
    bool far;
    if (!g_haveSync) {
        int64_t seen = s_timeSeenSaved;
        far = seen > 0 && (serverUtcMs / 1000 < seen - 86400 || serverUtcMs / 1000 > seen + FIRST_SYNC_FAR_DAYS * 86400);
    } else {
        int64_t a = diff < 0 ? -diff : diff;
        if (!g_confirming && a < IGNORE_THRESHOLD_MS) {
            CLOCK_LOGF("NTP", Texts::ntpLeft, (long long)diff, (long long)g_lastRoundTripMs);
            g_sinceGood.startAt(now);
            return;
        }
        if (a < STEP_THRESHOLD_MS) {
            if (g_confirming) DiagnosticLog::record("NTP", Texts::ntpJumpNotConfirmed,
                                                    (double)g_confirmDiffMs / 1000.0, g_server.c_str(), (long long)diff);
            g_confirming = false;
            rebase(now, model, diff);
            CLOCK_LOGF("NTP", Texts::ntpGradual, (long long)diff, (long long)g_lastRoundTripMs);
            g_sinceGood.startAt(now);
            return;
        }
        far = true;
    }
    if (!far) {
        seenFollows(serverUtcMs);
        stepTo(now, serverUtcMs);
        g_confirming = false;
        logStep(Texts::ntpFirst, serverUtcMs, 0, false);
        g_sinceGood.startAt(now);
        return;
    }
    if (!g_confirming) {   // a second opinion first
        g_confirming = true;
        g_confirmDiffMs = diff;
        g_confirmFrom = g_cur;
        g_confirmAtMs = now;
        CLOCK_LOG("NTP", "%s says the clock is off by %+.3f s - asking again before a jump", g_server.c_str(), (double)diff / 1000.0);
        g_askNow = true;   // the next server that may be asked (never the same one sooner than SAME_SERVER_MIN_MS)
        StringTools::utf8Copy(s_moveWhy, Texts::ntpWhySecond);
        return;
    }
    int64_t gap = diff - g_confirmDiffMs;
    g_confirming = false;
    if (gap > CONFIRM_AGREE_MS || gap < -CONFIRM_AGREE_MS) {
        char d[96];
        snprintf(d, sizeof(d), "%+.3f s, then %+.3f s - no jump", (double)g_confirmDiffMs / 1000.0, (double)diff / 1000.0);
        s_agreeStreak.fail("NTP", Texts::ntpAgreeStreak, d);
        CLOCK_LOG("NTP", "%s", d);   // live every time (the log line: once while they keep disagreeing)
        askFailed();   // conflicting answers: ask again soon (64 s, doubling) - not after the 6-hour interval
        return;
    }
    s_agreeStreak.ok("NTP", "Two time replies agreeing on a jump");
    bool wasSynced = g_haveSync;
    if (!wasSynced) seenFollows(serverUtcMs);
    stepTo(now, serverUtcMs);
    logStep(wasSynced ? Texts::ntpClockSet : Texts::ntpFirstConfirmed, serverUtcMs, diff, wasSynced);
    g_sinceGood.startAt(now);
}

// A kiss-o'-death (RFC 4330 s8, RFC 8633 s5.4): this server asks not to be asked (now). Another
// server, if there is one; else wait - longer for "go away" than for "too often".
// RFC 5905 s7.4: "For kiss code RATE, the client MUST immediately reduce its polling interval to that
// server and continue to reduce it each time it receives a RATE kiss code. For kiss codes DENY and
// RSTR, the client MUST ... stop sending packets to that server". The kiss carries no "come back at";
// the reference server puts its own spacing in the packet's poll field (log2 s) and its client waits
// at least that - here too, never past KOD_RATE_MAX_MS (RFC 8633: not just any value, at most 2 h).
// Other codes: no protocol meaning - the reply isn't used, nothing more. (2026-11-21, the user.)
static void kissed(const char* code) {
    DiagnosticLog::record("NTP", Texts::ntpKissLog, g_server.c_str(), code);
    String whyText = Texts::fill(Texts::ntpKiss, code);   // the same words as the reply's check
    const char* why = whyText.c_str();
    if (strcmp(code, "RATE") == 0) {
        uint8_t n = g_rateInRow[g_cur] < 6 ? g_rateInRow[g_cur]++ : 6;   // longer each time it repeats
        int64_t ms = KOD_RATE_WAIT_MS << n;
        int8_t poll = (int8_t)g_packet[2];                                // the server's own spacing
        if (poll > 0) { int64_t asked = 1000LL << (poll < 13 ? poll : 13); if (asked > ms) ms = asked; }
        troubleWith(g_cur, why, ms < KOD_RATE_MAX_MS ? ms : KOD_RATE_MAX_MS);
    } else if (strcmp(code, "DENY") == 0 || strcmp(code, "RSTR") == 0) {
        g_restUntil[g_cur] = INT64_MAX;   // for good - until it leaves the list or the clock restarts
        DiagnosticLog::record("NTP", Texts::ntpDenied, g_server.c_str(), code);
        troubleWith(g_cur, why, 0);
    } else {
        troubleWith(g_cur, why, -1);   // no protocol meaning: an unusable reply like any other
    }
}

// Shared bodies for the public functions below that call each other
// in-process (getDateTime()/getLocalEpoch() -> getTime(), begin() ->
// setTimezone()) - the lock is taken once, by whichever public function
// is the actual entry point, so the inner call must reach these directly
// instead of going back through the public (lock-taking) wrapper, or it
// would try to take the same non-recursive mutex twice and deadlock.

// The pass's moment (beginPass()/endPass(), NTPManager.h): 0 = none, the live time.
static std::atomic<int64_t> s_passUtcMs{0};

static time_t getTimeInternal() {
    if (!g_haveSync) return 0;
    int64_t pass = s_passUtcMs.load();
    int64_t utcMs = pass ? pass : currentUtcMs();
    return (time_t)(utcMs / 1000LL) + g_debugTimeOffsetSec.load();
}

static void setTimezoneInternal(const char* tzPOSIX) {
    if (!tzPOSIX || !*tzPOSIX) return;
    setenv("TZ", tzPOSIX, 1);
    tzset();
    CLOCK_LOG("NTP", "Timezone updated (POSIX): %s", tzPOSIX);
}

// ── Checking one server for the page (2026-11-13) ────────────────────────────
// The settings page asks before it saves a server the user typed: does it have
// an address, and does it answer? Done here, on the main loop, without waiting:
// its own name lookup (s_tDns, as above) and its own UDP port, so the clock's
// own syncing is untouched. A few packets, as UDP may lose one.
static const uint16_t TEST_LOCAL_PORT       = 2391;
static const int64_t  TEST_DNS_GIVE_UP_MS   = 8LL * 1000;   // no address by then: failed
static const int64_t  TEST_REPLY_WAIT_MS    = 2LL * 1000;   // per packet
static const int      TEST_PACKETS          = 3;

enum : uint8_t { T_NONE, T_DNS, T_SENT, T_OK, T_FAIL };
static uint8_t   s_tState = T_NONE;
static String    s_tServer, s_tDetail;
static IPAddress s_tIp;
static Timer     s_tSentAge;    // since the last packet
static int       s_tSent = 0;
static WiFiUDP   s_tUdp;
static bool      s_tUdpOpen = false;
static uint8_t   s_tStamp[8];   // the last question's stamp (an answer to an earlier one is waited past)
static int64_t   s_tSentAtMs = 0;

static void testEnd(uint8_t state, const String& detail) {
    s_tState = state; s_tDetail = detail;
    s_tDns.cancel();
    if (s_tUdpOpen) { s_tUdp.stop(); s_tUdpOpen = false; }
    CLOCK_LOG("NTP", "Check of %s: %s %s", s_tServer.c_str(), state == T_OK ? "answers" : "failed", detail.c_str());
}
static void testSend() {
    fillRequest(g_packet, s_tStamp);
    s_tSentAtMs = Uptime::ms();
    s_tUdp.beginPacket(s_tIp, NTP_PORT);
    s_tUdp.write(g_packet, NTP_PACKET_SIZE);
    s_tUdp.endPacket();
    s_tSent++; s_tSentAge.start(); s_tState = T_SENT;
}
static void serviceTest() {   // from loop(), under NTPLock
    if (s_tState == T_DNS) {
        uint8_t st = s_tDns.poll(TEST_DNS_GIVE_UP_MS, s_tIp);
        if (st == DNS_FAILED) { testEnd(T_FAIL, Texts::ntpTestNoAddress); return; }
        if (st != DNS_DONE) return;
        s_tUdpOpen = s_tUdp.begin(TEST_LOCAL_PORT);
        if (!s_tUdpOpen) { testEnd(T_FAIL, Texts::ntpTestNoPort); return; }
        testSend();
    } else if (s_tState == T_SENT) {
        int cb = s_tUdp.parsePacket();
        if (cb >= NTP_PACKET_SIZE && s_tUdp.remoteIP() == s_tIp) {   // the same checks as the clock's own syncing
            s_tUdp.read(g_packet, NTP_PACKET_SIZE);
            int64_t utcMs = 0;
            char kiss[5];
            String why = checkReply(g_packet, s_tStamp, s_tSentAtMs, Uptime::ms(), utcMs, kiss);
            if (why.startsWith("not an answer")) return;   // an earlier question's: wait on
            if (why.length()) { testEnd(T_FAIL, Texts::fill(Texts::ntpTestBadAnswer, why)); return; }
            String d = "ok";
            if (g_haveSync) d = String((long)(utcMs - currentUtcMs())) + " ms from the clock's time";
            testEnd(T_OK, d);
        } else if (s_tSentAge.passed(TEST_REPLY_WAIT_MS)) {
            if (s_tSent < TEST_PACKETS) testSend();
            else testEnd(T_FAIL, Texts::fill(Texts::ntpTestNoAnswer, s_tIp.toString()));
        }
    }
}

// The latest time seen, kept for the era pivot (answerTime()). Its own file,
// not config.json: never in a backup or an import, so no other clock's (or a
// hand-edited) value moves this one's pivot. Read once at begin(); written by
// serviceTimeSeen() from the main loop, outside NTPLock, on a still screen.
static std::atomic<int64_t> s_timeSeenWaitMs{0};  // Uptime::ms() a write began waiting, 0 = none
static const int64_t TIME_SEEN_MAX_UNIX = daysFromCivil(DEBUG_TIME_YEAR_RANGE.hi + 1, 1, 1) * 86400;

static void loadTimeSeen() {   // from begin(), under NTPLock
    s_eraPivotUnix = NTP_ERA_PIVOT_UNIX;
    File f = LittleFS.open(TIME_SEEN_PATH, "r");
    if (!f) return;
    String t = f.readStringUntil('\n');
    f.close();
    char* end = nullptr;
    long long v = strtoll(t.c_str(), &end, 10);
    // Only a time this clock can have seen (never assume a file holds only what we wrote).
    if (end == t.c_str() || v < NTP_ERA_PIVOT_UNIX || v >= TIME_SEEN_MAX_UNIX) {
        CLOCK_LOGF("NTP", Texts::timeSeenIgnored, TIME_SEEN_PATH, t.c_str());
        return;
    }
    s_timeSeenSaved = v;
    int64_t pivot = v - 365LL * 86400;
    if (pivot > s_eraPivotUnix) s_eraPivotUnix = pivot;
}

// =============================================================================
// Public API
// =============================================================================

namespace NTPManager {

// The not-recommended marks (NTP_TROUBLE_PATH), by name: only the current list's are taken. Its own
// file, not config.json: what this clock found, never in a backup. Never assume it holds only what
// we wrote - each value checked.
static void loadTrouble() {   // from begin(), under NTPLock
    for (auto& t : g_trouble) t = Trouble();
    File f = LittleFS.open(NTP_TROUBLE_PATH, "r");
    if (!f) return;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, f);
    f.close();
    if (e || !doc["servers"].is<JsonArray>()) { CLOCK_LOGF("NTP", Texts::ntpTroubleFileBad, NTP_TROUBLE_PATH); return; }
    for (JsonObject o : doc["servers"].as<JsonArray>()) {
        const char* name = o["name"] | "";
        for (int i = 0; i < g_serverCount; i++) {
            if (!g_servers[i].equalsIgnoreCase(name)) continue;
            Trouble& t = g_trouble[i];
            t.on = true;
            StringTools::utf8Copy(t.why, o["why"] | "");
            int64_t since = o["since"] | (int64_t)0, last = o["lastGood"] | (int64_t)0;
            t.sinceUnix = since > 0 ? since : 0;
            t.lastGoodUnix = last > 0 ? last : 0;
            int good = o["good"] | 0;
            t.goodDays = (uint8_t)(good < 0 ? 0 : good >= TROUBLE_GOOD_DAYS ? TROUBLE_GOOD_DAYS - 1 : good);
            t.nextCheckMs = Uptime::ms() + checkGapMs();
        }
    }
}

// The per-server state follows its name when the list changes (the order may change): a resting
// or not-recommended server stays so - saving the list again doesn't make a server polite. A name
// that left the list takes its state along.
static void setServersInternal(const char* const* servers, int count) {
    String was = g_serverCount ? g_server : String();
    String wasFirst = g_serverCount ? g_servers[0] : String();
    struct Slot {
        String name; int64_t restUntil; uint8_t bad, rate; int64_t lastAsk; Trouble tr;
        IPAddress ip; bool haveIp; Timer age; bool suspect;
    };
    static Slot old[NTP_SERVER_SLOTS];
    const int oldCount = g_serverCount;
    for (int i = 0; i < oldCount; i++)
        old[i] = { g_servers[i], g_restUntil[i], g_badInRow[i], g_rateInRow[i], g_lastAskMs[i], g_trouble[i],
                   s_slotIp[i], s_slotHaveIp[i], s_slotIpAge[i], s_slotSuspect[i] };
    g_serverCount = 0;
    for (int i = 0; i < count && g_serverCount < NTP_SERVER_SLOTS; i++)
        if (servers[i] && servers[i][0]) g_servers[g_serverCount++] = servers[i];
    if (!g_serverCount) g_servers[g_serverCount++] = NTP_SERVER_1;   // Storage::clampConfig() keeps one; just in case
    bool kept[NTP_SERVER_SLOTS] = {};
    for (int j = 0; j < NTP_SERVER_SLOTS; j++) {
        int from = -1;
        for (int i = 0; j < g_serverCount && i < oldCount && from < 0; i++)
            if (old[i].name.equalsIgnoreCase(g_servers[j])) from = i;
        if (from >= 0) {
            const Slot& s = old[from];
            kept[from] = true;
            g_restUntil[j] = s.restUntil; g_badInRow[j] = s.bad; g_rateInRow[j] = s.rate; g_lastAskMs[j] = s.lastAsk;
            g_trouble[j] = s.tr; s_slotIp[j] = s.ip; s_slotHaveIp[j] = s.haveIp; s_slotIpAge[j] = s.age; s_slotSuspect[j] = s.suspect;
        } else {
            g_restUntil[j] = 0; g_badInRow[j] = g_rateInRow[j] = 0; g_lastAskMs[j] = 0;
            g_trouble[j] = Trouble(); s_slotHaveIp[j] = false; s_slotIpAge[j].stop(); s_slotSuspect[j] = false;
        }
    }
    for (int i = 0; i < oldCount; i++) if (!kept[i] && old[i].tr.on) s_troubleDirty = true;   // its mark leaves the file
    if (g_servers[0].equalsIgnoreCase(wasFirst))
        for (int i = 0; i < g_serverCount; i++)
            if (g_servers[i].equalsIgnoreCase(was)) { useServer(i); return; }   // the one in use stays
    useServer(0);
}

void begin(const char* const* servers, int count, const char* tzPOSIX) {
    NTPLock::Guard guard;
    g_serverCount = 0;
    setServersInternal(servers, count);
    if (tzPOSIX) setTimezoneInternal(tzPOSIX);
    g_haveSync    = false;
    rebase(Uptime::ms(), 0, 0);
    g_sinceAttempt.stop();
    g_sinceGood.stop();
    g_waiting       = false;
    g_sendPending   = false;
    g_confirming    = false;
    g_failedRetryMs = 0;
    g_askNow = g_isCheck = g_netCheck = false;
    // (setServersInternal() above started every slot clean: the list was empty)
    s_dns.cancel();
    s_dnsPause.stop();
    loadTimeSeen();
    loadTrouble();

    g_udp.begin(NTP_LOCAL_PORT);
    CLOCK_LOG("NTP", "Ready — server: %s (%d in the list), tz: %s", g_server.c_str(), g_serverCount, tzPOSIX ? tzPOSIX : "UTC");
}

void setServers(const char* const* servers, int count) {
    NTPLock::Guard guard;
    String was = g_server;
    setServersInternal(servers, count);
    if (g_server != was) {   // removed, or another one first now: the best of the new list at once
        g_waiting = false;
        g_askNow = true;
        CLOCK_LOG("NTP", "Servers changed - now using %s", g_server.c_str());
    } else if (g_sendPending) {
        g_askNow = true;   // its lookup may have been dropped with the change
    }
}

void testStart(const char* server) {
    NTPLock::Guard guard;
    if (s_tUdpOpen) { s_tUdp.stop(); s_tUdpOpen = false; }
    s_tServer = server;
    s_tDetail = "";
    s_tSent = 0;
    s_tDns.cancel();
    if (WiFi.status() != WL_CONNECTED) { s_tState = T_FAIL; s_tDetail = "the clock is not connected to the internet"; return; }
    s_tState = T_DNS;
    s_tDns.start(server);
}

void testResult(int& state, String& server, String& detail) {
    NTPLock::Guard guard;
    state = s_tState == T_NONE ? 0 : s_tState == T_OK ? 2 : s_tState == T_FAIL ? 3 : 1;
    server = s_tServer;
    detail = s_tDetail;
}

String currentServer() {
    NTPLock::Guard guard;
    return g_server;
}

void setTimezone(const char* tzPOSIX) {
    NTPLock::Guard guard;
    setTimezoneInternal(tzPOSIX);
}

void setCountryPool(const char* cc) {
    char lc[3] = "";
    if (cc && cc[0] && cc[1]) { lc[0] = StringTools::asciiToLower(cc[0]); lc[1] = StringTools::asciiToLower(cc[1]); }
    NTPLock::Guard guard;
    if (memcmp(lc, s_countryPool, 3) == 0) return;
    if (lc[0])
        DiagnosticLog::record("NTP", Texts::ntpCountryPool, lc);
    else
        DiagnosticLog::record("NTP", Texts::ntpCountryPoolOff);
    memcpy(s_countryPool, lc, 3);
    for (int i = 0; i < g_serverCount; i++)
        if (isGlobalPool(g_servers[i])) s_slotHaveIp[i] = false;   // re-resolve with the new name
}

void setDebugOffset(int64_t offset) {
    NTPLock::Guard guard;
    g_debugTimeOffsetSec = offset;
}

void loop() {
    NTPLock::Guard guard;
    const int64_t now0 = Uptime::ms();

    // A silence (or a name without an address) waits for the internet check - nothing asked meanwhile.
    if (g_netCheck) {
        if (NetHealth::heardSince(g_netCheckFromMs)) {
            g_netCheck = false;
            if (g_netDownSaid) {   // it was the internet: nobody to blame - the same server, now, as if nothing happened
                DiagnosticLog::record("NTP", Texts::ntpInternetBack);
                g_lastAskMs[g_cur] = 0;   // its question never reached it
                s_dnsPause.stop();        // a failed lookup's pause was for the outage too
                g_failedRetryMs = 0;
                g_askNow = true;
            } else {
                troubleWith(g_cur, g_netCheckWhy, -1);   // the internet is there: the server's fault
            }
        } else if (NetHealth::checkedDownSince(g_netCheckFromMs)) {
            if (!g_netDownSaid) {
                g_netDownSaid = true;
                DiagnosticLog::record("NTP", Texts::ntpNotBlamed, g_server.c_str(), g_netCheckWhy);
            }
            NetHealth::probeWanted();   // watched - at most once a minute - until something answers
        }
    }

    // Replies (non-blocking): only one from the address asked, answering THIS question
    // (checkReply()); anything else is dropped and the wait goes on.
    while (g_waiting) {
        int cb = g_udp.parsePacket();
        if (cb <= 0) break;
        int64_t gotAt = Uptime::ms();
        if (cb < NTP_PACKET_SIZE || g_udp.remoteIP() != g_sentTo) {
            CLOCK_LOGF("NTP", Texts::ntpDroppedPacket, g_udp.remoteIP().toString().c_str(), cb);
            continue;
        }
        g_udp.read(g_packet, NTP_PACKET_SIZE);
        int64_t utcMs = 0, roundTrip = -1;
        char kiss[5];
        String why = checkReply(g_packet, g_sentStamp, g_sentAtMs, gotAt, utcMs, kiss, &roundTrip);
        if (why.startsWith("not an answer")) {   // a late or repeated one: the real answer may still come
            CLOCK_LOG("NTP", "Dropped a reply from %s: %s", g_server.c_str(), why.c_str());
            g_heardFromServer = true;   // it answers - just not this question (not silence)
            NetHealth::heard();
            continue;
        }
        g_waiting = false;
        NetHealth::heard();   // whatever it said, the internet is there
        if (kiss[0]) { kissed(kiss); break; }
        if (why.length()) {   // the server answered, but its time can't be used: the next one
            s_replyStreak.fail("NTP", Texts::ntpReplyStreak, why.c_str());
            troubleWith(g_cur, why.c_str(), -1);
            break;
        }
        s_replyStreak.ok("NTP", Texts::ntpReplyStreak);
        g_failedRetryMs = 0;
        if (g_rateInRow[g_cur] || g_badInRow[g_cur]) {   // back after a kiss or a rest: said at once
            char was[48];
            if (g_rateInRow[g_cur]) snprintf(was, sizeof(was), "kiss-o'-death RATE x%u", (unsigned)g_rateInRow[g_cur]);
            else snprintf(was, sizeof(was), "%u failed question(s)", (unsigned)g_badInRow[g_cur]);
            DiagnosticLog::record("NTP", Texts::ntpBackAfterRest, g_server.c_str(), was);
        }
        g_badInRow[g_cur] = g_rateInRow[g_cur] = 0;   // it answers: no more backing off from it
        g_restUntil[g_cur] = 0;
        if (!g_isCheck) {
            g_syncWaitMs = SYNC_INTERVAL_MS - (int64_t)(esp_random() % (uint32_t)(SYNC_JITTER_MS + 1));   // the next: 5.5-6 h
            CLOCK_LOGF("NTP", Texts::ntpNextRegular, (long long)(g_syncWaitMs / 1000));
        }
        g_isCheck = false;
        g_lastRoundTripMs = roundTrip;
        applySync(gotAt, utcMs);
        goodAnswer(g_cur, gotAt);
        break;
    }
    if (g_waiting && Uptime::ms() - g_sentAtMs >= REPLY_TIMEOUT_MS) {
        s_replyStreak.fail("NTP", Texts::ntpReplyStreak, Texts::ntpWhySilent);
        // UDP is unreliable by design - a single lost reply must not stall
        // sync forever. Give up on this attempt so the interval check below
        // can send a fresh request instead of waiting for a packet that may
        // never arrive.
        CLOCK_LOG("NTP", "No reply within %lldms - abandoning attempt, will retry", (long long)REPLY_TIMEOUT_MS);
        g_waiting = false;
        if (g_heardFromServer) troubleWith(g_cur, Texts::ntpWhyNotOurs, -1);   // not silent - unusable
        else startNetCheck(Texts::ntpWhySilent);   // the server, or the internet? decided on the check
    }
    serviceTest();

    // An address arriving for an ask that was due: send it now - not after the next interval
    // (until 2026-11-17 only while unsynced: synced, a new or next server's first ask waited
    // a whole interval - 6 h after a server change).
    if (serviceDns() && g_sendPending && !g_waiting) g_askNow = true;
    if (g_waiting || g_netCheck) return;

    // A question now: one is due (after a failure, a forced sync, ...), or the interval is over -
    // 5.5-6 h once synced (after a failed ask 64 s, doubling), unsynced or confirming a jump
    // every RETRY_INTERVAL_MS. Always to the best server that may be asked (pickServer()):
    // none sooner than SAME_SERVER_MIN_MS after its last question, none resting.
    int64_t interval = g_failedRetryMs ? g_failedRetryMs : g_syncWaitMs;
    if (!g_haveSync || g_confirming) interval = RETRY_INTERVAL_MS;
    if (g_askNow || g_sinceAttempt.idleOrPassed(interval)) {
        int soonest;
        int64_t when;
        int pick = pickServer(soonest, when, g_confirming ? g_confirmFrom : -1);
        if (pick < 0 && g_confirming && when == INT64_MAX) pick = pickServer(soonest, when);   // no other ever: the same one
        if (pick < 0) {   // every server resting, or asked too recently: wait for the first free one
            g_askNow = true;
            if (!g_allRestingSaid) {
                g_allRestingSaid = true;
                if (when == INT64_MAX) DiagnosticLog::record("NTP", Texts::ntpAllRefused);
                else CLOCK_LOGF("NTP", Texts::ntpAllResting, (long long)((when - now0) / 1000), g_servers[soonest].c_str());
            }
            return;
        }
        if (pick != g_cur) {
            CLOCK_LOGF("NTP", Texts::ntpNextServer, g_server.c_str(), s_moveWhy[0] ? s_moveWhy : Texts::ntpWhyBestFree, g_servers[pick].c_str());
            useServer(pick);
        }
        s_moveWhy[0] = 0;
        g_askNow = false;
        g_allRestingSaid = false;
        g_isCheck = false;
        g_sinceAttempt.start();
        sendPacket();
        return;
    }

    // A not-recommended server's daily question (synced, nothing else going on): its good days counted.
    if (!g_haveSync || g_confirming) return;
    for (int i = 0; i < g_serverCount; i++) {
        if (!g_trouble[i].on || now0 < g_trouble[i].nextCheckMs || freeAt(i) > now0) continue;
        g_trouble[i].nextCheckMs = now0 + checkGapMs();
        if (i != g_cur) useServer(i);
        CLOCK_LOGF("NTP", Texts::ntpDailyCheck, g_server.c_str(), (unsigned)g_trouble[i].goodDays, (unsigned)TROUBLE_GOOD_DAYS);
        g_isCheck = true;
        sendPacket();
        if (!g_waiting) g_isCheck = false;   // no address yet: asked at the next one
        return;
    }
}

int64_t utcMs() {
    NTPLock::Guard guard;
    return currentUtcMs();
}

int64_t lastRoundTripMs() {
    NTPLock::Guard guard;
    return g_lastRoundTripMs;
}

time_t getTime() {
    NTPLock::Guard guard;
    return getTimeInternal();
}

void beginPass() {
    NTPLock::Guard guard;
    s_passUtcMs = currentUtcMs();   // unsynced: 0 - the live time (none) until a sync
}

void endPass() { s_passUtcMs = 0; }

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
    if (!g_waiting) g_askNow = true;   // the next server that may be asked - never one asked within SAME_SERVER_MIN_MS
}

bool isSynced() {
    NTPLock::Guard guard;
    return g_haveSync;
}

int64_t msSinceSync() {
    NTPLock::Guard guard;
    if (!g_haveSync) return 0;
    return g_sinceGood.elapsedMs();
}

void serviceTimeSeen() {
    int64_t now;
    {
        NTPLock::Guard guard;
        if (!g_haveSync) return;
        now = currentUtcMs() / 1000;   // real UTC, no debug offset
    }
    if (now - s_timeSeenSaved < TIME_SEEN_STEP_SEC) { s_timeSeenWaitMs = 0; return; }
    if (!s_timeSeenWaitMs) s_timeSeenWaitMs = Uptime::ms();
    if (!DisplayManager::goodMomentToWrite(s_timeSeenWaitMs)) return;
    char line[24];
    snprintf(line, sizeof(line), "%lld\n", (long long)now);
    FileTools::writeTextAtomic(TIME_SEEN_PATH, line);   // failed: logged, tried again in 30 days
    s_timeSeenSaved = now;
    s_timeSeenWaitMs = 0;
    CLOCK_LOG("NTP", "Latest time seen saved: %lld", (long long)now);
}

void serviceTroubleFile() {
    if (!s_troubleDirty) { s_troubleWaitMs = 0; return; }
    if (!s_troubleWaitMs) s_troubleWaitMs = Uptime::ms();
    if (!DisplayManager::goodMomentToWrite(s_troubleWaitMs)) return;
    JsonDocument doc;
    JsonArray a = doc["servers"].to<JsonArray>();
    {
        NTPLock::Guard guard;
        s_troubleDirty = false;
        for (int i = 0; i < g_serverCount; i++) {
            const Trouble& t = g_trouble[i];
            if (!t.on) continue;
            JsonObject o = a.add<JsonObject>();
            o["name"] = g_servers[i];
            o["why"] = t.why;
            o["since"] = t.sinceUnix;
            o["good"] = t.goodDays;
            o["lastGood"] = t.lastGoodUnix;
        }
    }
    if (a.size()) FileTools::writeJsonAtomic(NTP_TROUBLE_PATH, doc);   // failed: logged, written with the next change
    else if (LittleFS.exists(NTP_TROUBLE_PATH)) LittleFS.remove(NTP_TROUBLE_PATH);
    s_troubleWaitMs = 0;
}

int troubleList(TroubleInfo* out, int max) {
    NTPLock::Guard guard;
    int n = 0;
    for (int i = 0; i < g_serverCount && n < max; i++) {
        if (!g_trouble[i].on) continue;
        out[n].server   = g_servers[i];
        out[n].why      = g_trouble[i].why;
        out[n].sinceUnix = g_trouble[i].sinceUnix;
        out[n].refused  = (g_restUntil[i] == INT64_MAX);
        out[n].goodDays = g_trouble[i].goodDays;
        out[n].needDays = out[n].refused ? 0 : TROUBLE_GOOD_DAYS;
        n++;
    }
    return n;
}

bool hasRefusedServer() {
    NTPLock::Guard guard;
    for (int i = 0; i < g_serverCount; i++)
        if (g_restUntil[i] == INT64_MAX) return true;
    return false;
}


} // namespace NTPManager
