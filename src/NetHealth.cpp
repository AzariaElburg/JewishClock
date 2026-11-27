#include "NetHealth.h"
#include "Config.h"
#include "Calendar.h"
#include "DiagnosticLog.h"
#include "Texts.h"
#include "Tunables.h"
#include "Uptime.h"
#include <HTTPClient.h>
#include <atomic>

// === TUNABLES ===  (rule: see Config.h header)
static const int64_t PROBE_MIN_GAP_MS  = 60LL * 1000;   // the internet check, at most this often
static const int64_t PROBE_TIMEOUT_MS  = 5LL * 1000;    // its answer within this, or no internet
TUNABLE_SHARED(PROBE_MIN_GAP_MS)

namespace {
std::atomic<int64_t> s_lastHeardMs{0};     // uptime of the last answer from any server (0: none yet)
std::atomic<int64_t> s_lastProbeMs{0};
std::atomic<bool>    s_probeWanted{false};
std::atomic<int64_t> s_lastProbeDoneMs{0};   // uptime the last check finished
std::atomic<bool>    s_down{false};        // the last check found nothing
DiagnosticLog::Streak s_probeStreak{1};
}

namespace NetHealth {

void heard() {
    s_lastHeardMs = Uptime::ms();
    s_down = false;
}

bool heardSince(int64_t uptimeMs) {
    int64_t t = s_lastHeardMs.load();
    return t > 0 && t >= uptimeMs;
}

bool checkedDownSince(int64_t uptimeMs) { return s_down.load() && s_lastProbeDoneMs.load() >= uptimeMs; }

void checkNow() {
    if (s_probeWanted.exchange(true)) return;
    Calendar::wakeTask();
}

bool down() { return s_down.load(); }

void probeWanted() {
    int64_t last = s_lastProbeMs.load();
    if (last && Uptime::ms() - last < PROBE_MIN_GAP_MS) return;
    if (s_probeWanted.exchange(true)) return;
    Calendar::wakeTask();
}

void probeInTask() {
    if (!s_probeWanted.exchange(false)) return;
    s_lastProbeMs = Uptime::ms();
    HTTPClient http;
    http.setConnectTimeout((int32_t)PROBE_TIMEOUT_MS);
    http.setTimeout((uint16_t)PROBE_TIMEOUT_MS);
    http.begin(CONNECTIVITY_CHECK_URL);
    int code = http.GET();
    http.end();
    s_lastProbeDoneMs = Uptime::ms();
    if (code > 0) {   // any answer: the internet is there - the silence is the time server's
        s_probeStreak.ok("Net", Texts::netStreak);
        heard();
    } else {
        s_probeStreak.fail("Net", Texts::netStreak, Texts::netNone);
        s_down = true;
    }
}

}  // namespace NetHealth
