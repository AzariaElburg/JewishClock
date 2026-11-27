// OnlineUpdate - see include/OnlineUpdate.h.
#include "FileTools.h"
#include "OnlineUpdate.h"
#include "Updater.h"
#include "UpdateKeyState.h"
#include "Version.h"
#include "VersionRule.h"
#include "Uptime.h"
#include "NTPManager.h"
#include "WiFiManager.h"
#include "DiagnosticLog.h"
#include "LogLevel.h"
#include "Calendar.h"
#include "Tunables.h"
#include "Texts.h"
#include "Restart.h"
#include "NetHealth.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <atomic>
#include <mutex>

TUNABLE_SHARED_AS(UPDATE_FEED_REPO, UPDATE_FEED_REPO)   // the manual links to it

namespace {

// === TUNABLES ===
// (The one version emailed for the phone test changes these in its own commit - checks every
// 5 minutes, installs at once - and the next version sets them back; no switch kept for it:
// the user, 2026-11-15. Its log: Settings > Advanced > Log level Debug for a day - the debug
// lines below.)
const int64_t FIRST_CHECK_AFTER_MS = 3LL * 60 * 1000;          // after a start, once the time is known
TUNABLE_SHARED(FIRST_CHECK_AFTER_MS)
const int64_t CHECK_EVERY_MS       = 24LL * 3600 * 1000;       // then once a day
const int64_t CHECK_RETRY_MS       = 60LL * 60 * 1000;         // a check that failed: again in an hour
const int     AUTO_INSTALL_HOUR    = 3;                         // automatic install: 03:00-03:59 local
TUNABLE_SHARED(AUTO_INSTALL_HOUR)
const int64_t PROGRESS_EVERY_BYTES = 256 * 1024;               // at log level Debug: a line per this much downloaded
const int64_t RESTART_AFTER_MS     = 3000;                      // installed: restart this long after (the log written)
const char*   USER_AGENT           = "JewishClock";             // GitHub's API needs one
// The feed not there (the user, 2026-11-15): in the log and Settings at once; after this many days
// on the clock's screen too ("Error - see Settings"), so the family can tell whoever maintains it
const long    FEED_MISSING_SCREEN_AFTER_DAYS = 3;
TUNABLE_SHARED(FEED_MISSING_SCREEN_AFTER_DAYS)

std::mutex          s_lock;     // guards the Strings below (main loop, web task, download task)
OnlineUpdate::Status s_status;
String              s_feed;     // the feed to ask (set by loop() from the settings)
String              s_url;      // the newer version's package
size_t              s_size = 0;
String              s_failedVersion, s_failedWhy;   // the last install that failed, and why (kept in the message)
std::atomic<bool>   s_checkWanted{false}, s_installWanted{false}, s_busy{false};
std::atomic<bool>   s_restartWanted{false};
Timer               s_sinceCheck, s_sinceInstallTry, s_restartTimer;
bool                s_checkFailed = false;
std::atomic<int64_t> s_feedHoldUntilMs{0};   // GitHub asked us to wait (429 / 403 rate limit): no check before
// The local day (since 1970) the feed was first found not there, 0 = it is there (or not asked yet).
// Kept in FEED_MISSING_PATH across restarts; read once by loop(), written only when it changes.
std::atomic<long>   s_missingSince{0};
std::atomic<bool>   s_missingSeen{false};   // Settings' "OK, seen": off the screen (until it is back and gone again)
bool                s_missingLoaded = false;

void writeMissingFile() {   // "<day>\n<seen 0/1>\n"
    FileTools::writeTextAtomic(FEED_MISSING_PATH, String(s_missingSince.load()) + "\n" + String(s_missingSeen ? 1 : 0) + "\n");
}

long localDay() {
    time_t t = NTPManager::getLocalEpoch();
    return t > 0 ? (long)(t / 86400) : 0;
}

void feedMissing(bool missing) {   // download task, after a check
    long since = s_missingSince.load();
    if (missing && !since) {
        long today = localDay();
        if (!today) return;
        s_missingSince = today;
        s_missingSeen = false;
        writeMissingFile();
    } else if (!missing && since) {
        s_missingSince = 0;
        s_missingSeen = false;
        LittleFS.remove(FEED_MISSING_PATH);
        DiagnosticLog::record("OTA", "the update feed is there again");
    }
}

void setState(const char* st) {
    std::lock_guard<std::mutex> g(s_lock);
    s_status.state = st;
}
void setState(const char* st, const String& msg) {
    std::lock_guard<std::mutex> g(s_lock);
    s_status.state = st;
    s_status.message = msg;
}

// An install that didn't happen: said now, and kept in the message while that version is offered
// (doCheck()) - every way it can fail goes through here (2026-11-20).
void installFailed(const String& version, const String& why) {
    setState("idle", why);
    std::lock_guard<std::mutex> g(s_lock);
    s_failedVersion = version;
    s_failedWhy = why;
}

String localNow() {
    time_t t = NTPManager::getLocalEpoch();
    if (t <= 0) return "";
    struct tm tm;
    gmtime_r(&t, &tm);   // getLocalEpoch(): local time counted as if UTC
    char b[20];
    snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    return b;
}

// The Updater fed straight from the download, a piece at a time - as a file from the page.
class UpdaterSink : public Stream {
public:
    size_t got = 0;
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* p, size_t n) override {
        Updater::uploadChunk(p, n);
        if ((got + n) / PROGRESS_EVERY_BYTES != got / PROGRESS_EVERY_BYTES)
            LOG_AT(LogLevel::DEBUG, "OTA", "downloaded %u bytes", (unsigned)(got + n));
        got += n;
        return n;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
};

// One GET of the feed's API (GitHub's JSON); body only for a 200.
int feedGet(const String& url, String& body) {
    WiFiClientSecure tls;
    tls.setInsecure();   // as Hebcal's: the package's signature is what proves it, not the connection
    HTTPClient http;
    http.setUserAgent(USER_AGENT);
    if (url.startsWith("https://")) http.begin(tls, url); else http.begin(url);
    http.addHeader("Accept", "application/vnd.github+json");
    static const char* WAIT_HEADERS[] = { "Retry-After", "X-RateLimit-Remaining", "X-RateLimit-Reset" };
    http.collectHeaders(WAIT_HEADERS, 3);
    int code = http.GET();
    if (code > 0) NetHealth::heard();   // it answered: the internet is there
    // Asked too often (GitHub: 60 an hour without an account - 429, or 403 with no requests left):
    // its own "come back" obeyed - Retry-After seconds, or the reset moment (2026-11-21, the user:
    // act on what a server answers, always).
    if (code == 429 || (code == 403 && http.header("X-RateLimit-Remaining") == "0")) {
        long waitS = http.header("Retry-After").toInt();
        long reset = http.header("X-RateLimit-Reset").toInt();
        time_t now = NTPManager::getTime();
        if (waitS <= 0 && reset > 0 && now > 0) waitS = reset - (long)now;
        if (waitS <= 0) waitS = (long)(CHECK_RETRY_MS / 1000);
        if (waitS > 24L * 3600) waitS = 24L * 3600;
        s_feedHoldUntilMs = Uptime::ms() + (int64_t)waitS * 1000;
        DiagnosticLog::record("OTA", Texts::svcWait, "api.github.com", waitS, code);
    }
    body = code == 200 ? http.getString() : String();
    http.end();
    return code;
}

void noUpdate(const String& msg, bool failed) {
    s_checkFailed = failed;   // failed: again in an hour; else with the daily check
    std::lock_guard<std::mutex> g(s_lock);
    if (s_status.message != msg) DiagnosticLog::record("OTA", "update check: %s", msg.c_str());
    s_status.lastCheck = localNow();
    s_status.state = "idle";
    s_status.available = "";
    s_url = "";
    s_status.message = msg;
}

void doCheck() {
    String feed;
    { std::lock_guard<std::mutex> g(s_lock); feed = s_feed; }
    LOG_AT(LogLevel::DEBUG, "OTA", "asking the update feed %s (running %s, key %s)", feed.c_str(), FIRMWARE_VERSION,
           UpdateKeyState::has() ? UpdateKeyState::id().c_str() : "none");
    setState("checking");
    String body;
    int code = feedGet(feed, body);
    // GitHub answers releases/latest with 404 both while a repository has no release yet (the real
    // feed, 2026-11-15) and when the repository isn't there - so the repository itself is asked
    // (the feed address without "/releases/..."): there -> nothing published yet; not there -> said
    // (the user, 2026-11-15: the clock asks, not only our tests).
    if (code == 404) {
        int cut = feed.indexOf("/releases/");
        String repoBody;
        int repo = cut > 0 ? feedGet(feed.substring(0, cut), repoBody) : 404;
        LOG_AT(LogLevel::DEBUG, "OTA", Texts::olFeed404, repo);
        if (repo == 200)      { noUpdate(Texts::olNoVersions, false); feedMissing(false); }
        else if (repo == 404) { noUpdate(Texts::fill(Texts::olFeedMissing, feed.substring(0, cut > 0 ? cut : feed.length())), true); feedMissing(true); }
        else                  noUpdate(Texts::fill(Texts::olCouldNotAsk, repo > 0 ? Texts::fill(Texts::olAnswerCode, String(repo)) : String(Texts::olNoConnection)), true);
        return;
    }
    if (code != 200) {
        s_checkFailed = true;
        setState("idle", Texts::fill(Texts::olCouldNotAsk, code > 0 ? Texts::fill(Texts::olAnswerCode, String(code)) : String(Texts::olNoConnection)));
        DiagnosticLog::record("OTA", "update check failed: %d", code);
        return;
    }
    LOG_AT(LogLevel::DEBUG, "OTA", Texts::olFeedAnswered, code, (unsigned)body.length());
    JsonDocument filter;
    filter["tag_name"] = true;
    filter["draft"] = true;
    filter["prerelease"] = true;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["browser_download_url"] = true;
    filter["assets"][0]["size"] = true;
    filter["assets"][0]["state"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
        s_checkFailed = true;
        setState("idle", Texts::olNotReadable);
        DiagnosticLog::record("OTA", Texts::olNotReadableLog, (unsigned)body.length());
        return;
    }
    String tag = doc["tag_name"] | "";
    if (tag.startsWith("v")) tag.remove(0, 1);
    // Only a package that is completely there (the user, 2026-11-15: "just new is not enough"):
    // a published release (not a draft, not a pre-release) whose package GitHub has finished
    // receiving ("uploaded" - while it still arrives it is "starter"/"open") and whose size it
    // knows. The release script also publishes only after the package is up and read back
    // (github_release.py), and the download must bring exactly that many bytes (doInstall()).
    // A half file that got through anyway would still fail the package's own end mark and
    // signature (Updater) - this keeps it from being tried at all.
    String url;
    size_t size = 0;
    const char* notReady = nullptr;
    if (doc["draft"] | false) notReady = Texts::olDraft;
    else if (doc["prerelease"] | false) notReady = "a pre-release";
    for (JsonObject a : doc["assets"].as<JsonArray>()) {
        String name = a["name"] | "";
        if (!name.endsWith(".jcupdate")) continue;
        String st = a["state"] | "uploaded";   // feeds other than GitHub's may leave it out
        size_t sz = a["size"] | 0;
        LOG_AT(LogLevel::DEBUG, "OTA", "feed: %s, package %s, %u bytes, %s", tag.c_str(), name.c_str(), (unsigned)sz, st.c_str());
        if (st != "uploaded") { if (!notReady) notReady = Texts::olBeingPutUp; continue; }
        if (sz == 0) { if (!notReady) notReady = Texts::olUnknownSize; continue; }
        url = a["browser_download_url"] | "";
        size = sz;
        break;
    }
    if (notReady) {
        url = "";
        DiagnosticLog::record("OTA", Texts::olNotTaken, tag.c_str(), notReady);
    }
    s_checkFailed = false;
    feedMissing(false);   // it answered
    std::lock_guard<std::mutex> g(s_lock);
    s_status.lastCheck = localNow();
    s_status.state = "idle";
    if (tag.length() && url.length() && OnlineUpdate::newer(tag, FIRMWARE_VERSION)) {
        if (s_status.available != tag) DiagnosticLog::record("OTA", "update %s available (running %s)", tag.c_str(), FIRMWARE_VERSION);
        s_status.available = tag;
        s_url = url;
        s_size = size;
        s_status.message = Texts::fill(Texts::olAvailable, tag);
        // the last try of this same version failed: still said - a check right after (the first
        // scheduled one, 3 min after a start) replaced the reason before anyone saw it (2026-11-20)
        if (s_failedVersion == tag && s_failedWhy.length()) s_status.message += Texts::fill(Texts::olLastTryFailed, s_failedWhy);
    } else {
        s_status.available = "";
        s_url = "";
        s_status.message = notReady && tag.length() && OnlineUpdate::newer(tag, FIRMWARE_VERSION)
                         ? Texts::fill(Texts::olNotReady, tag, String(notReady))
                         : tag.length() ? Texts::fill(Texts::olUpToDate, tag) : String(Texts::olNoVersionsShort);
    }
}

void doInstall() {
    String url, version;
    size_t size;
    { std::lock_guard<std::mutex> g(s_lock); url = s_url; version = s_status.available; size = s_size; }
    if (!url.length()) return;
    LOG_AT(LogLevel::DEBUG, "OTA", "download %s from %s, %u bytes expected", version.c_str(), url.c_str(), (unsigned)size);
    if (Updater::uploadInProgress()) { installFailed(version, Texts::olPageInstalling); return; }
    setState("downloading", Texts::fill(Texts::olDownloading, version));
    DiagnosticLog::record("OTA", Texts::olDownloadingLog, version.c_str());
    WiFiClientSecure tls;
    tls.setInsecure();
    HTTPClient http;
    http.setUserAgent(USER_AGENT);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);   // GitHub's download links redirect to its file store
    if (url.startsWith("https://")) http.begin(tls, url); else http.begin(url);
    http.addHeader("Accept", "application/octet-stream");
    int code = http.GET();
    if (code != 200) {
        http.end();
        installFailed(version, Texts::fill(Texts::olDownloadFailed, code > 0 ? Texts::fill(Texts::olAnswerCode, String(code)) : String(Texts::olNoConnection)));
        DiagnosticLog::record("OTA", "update download failed: %d", code);
        return;
    }
    LOG_AT(LogLevel::DEBUG, "OTA", Texts::olDownloadAnswered, code, http.getSize());
    Updater::uploadBegin("online " + version, size);
    UpdaterSink sink;
    int got = http.writeToStream(&sink);
    http.end();
    setState("installing");
    LOG_AT(LogLevel::DEBUG, "OTA", "download ended: %d (%u bytes of %u)", got, (unsigned)sink.got, (unsigned)size);
    // Exactly the size the feed named - a shorter or longer file is not the one that was put up
    if (got > 0 && sink.got != size) {
        Updater::uploadAbort(Texts::fill(Texts::olWrongSize, String((unsigned)sink.got), String((unsigned)size)));
    }
    bool ok = got > 0 && Updater::uploadEnd();
    if (!ok) {
        String why = got > 0 ? Updater::uploadMessage() : String(Texts::olBrokeOff);
        if (got <= 0) Updater::uploadAbort(Texts::olBrokeOff);
        installFailed(version, why);
        DiagnosticLog::record("OTA", "online update %s not installed: %s", version.c_str(), why.c_str());
        return;
    }
    setState("restarting", Texts::fill(Texts::olInstalled, version));
    s_restartWanted = true;
}

}  // namespace

namespace OnlineUpdate {

bool newer(const String& candidate, const String& running) { return VersionRule::newer(candidate, running); }

void checkNow()   { s_checkWanted = true; Calendar::wakeTask(); }

long feedMissingSince() { return s_missingSince.load(); }

bool feedMissingOnScreen() {
    long since = s_missingSince.load(), today = localDay();
    return since && !s_missingSeen && today && today - since >= FEED_MISSING_SCREEN_AFTER_DAYS;
}

bool feedMissingSeen() { return s_missingSeen; }

void feedMissingSeenNow() {   // web task: Settings' "OK, seen"
    if (!s_missingSince.load() || s_missingSeen) return;
    s_missingSeen = true;
    writeMissingFile();
    DiagnosticLog::record("OTA", "update feed not there: 'OK, seen' in Settings - off the clock's screen");
}

void useFeed(const char* feed) {
    std::lock_guard<std::mutex> g(s_lock);
    s_feed = feed && feed[0] ? String(feed) : String(UPDATE_FEED_URL);
}
void installNow() { s_installWanted = true; Calendar::wakeTask(); }

void runInTask() {
    if (s_busy.exchange(true)) return;
    // Each at DEBUG while it runs (Updater::debugFocus - the A -> B -> C phone test only)
    if (s_checkWanted.exchange(false)) {
        // quietly: its own debug lines say where it starts and ends (a line pair per check filled the log)
        Updater::debugFocus(Logging::FOCUS_UPDATE_CHECK, true, nullptr);
        doCheck();
        Updater::debugFocus(Logging::FOCUS_UPDATE_CHECK, false, nullptr);
    }
    if (s_installWanted.exchange(false)) {
        Updater::debugFocus(Logging::FOCUS_UPDATE_DOWNLOAD, true, Texts::focusDownloading);
        doInstall();
        Updater::debugFocus(Logging::FOCUS_UPDATE_DOWNLOAD, false, Texts::focusDownloadOver);
    }
    s_busy = false;
}

void loop(const AppConfig& cfg) {
    static Timer every;
    if (every.running() && !every.passed(1000)) return;
    every.start();
    if (!s_missingLoaded) {   // once: the day the feed was first found not there, from before a restart
        s_missingLoaded = true;
        File f = LittleFS.open(FEED_MISSING_PATH, "r");
        if (f) {
            long d = f.readStringUntil('\n').toInt();
            bool seen = f.readStringUntil('\n').toInt() == 1;
            f.close();
            if (d > 0) { s_missingSince = d; s_missingSeen = seen; }
        }
    }
    {
        std::lock_guard<std::mutex> g(s_lock);
        s_feed = cfg.updateFeed[0] ? String(cfg.updateFeed) : String(UPDATE_FEED_URL);
    }
    if (s_restartWanted) {
        if (!s_restartTimer.running()) s_restartTimer.start();
        if (s_restartTimer.passed(RESTART_AFTER_MS)) Restart::now();
        return;
    }
    if (!NTPManager::isSynced() || WiFiManager::offlineForMs() > 0) return;
    if (Uptime::ms() < s_feedHoldUntilMs.load()) return;   // the feed asked us to wait (feedGet())
    // The check: a while after a start, then daily; a failed one again in an hour.
    int64_t due = !s_sinceCheck.running() ? FIRST_CHECK_AFTER_MS : s_checkFailed ? CHECK_RETRY_MS : CHECK_EVERY_MS;
    static Timer sinceStart;
    if (!sinceStart.running()) sinceStart.start();
    if ((!s_sinceCheck.running() && sinceStart.passed(due)) || (s_sinceCheck.running() && s_sinceCheck.passed(due))) {
        s_sinceCheck.start();
        checkNow();
    }
    // Automatic: at night, a newer version known, at most one try a night - never without a key
    // (it would go in unchecked: only by the user's own Install, after the warning).
    if (!cfg.autoUpdate || !UpdateKeyState::has()) return;
    bool have;
    { std::lock_guard<std::mutex> g(s_lock); have = s_status.available.length() > 0; }
    time_t t = NTPManager::getLocalEpoch();
    int hour = t > 0 ? (int)((t % 86400) / 3600) : -1;
    if (have && hour == AUTO_INSTALL_HOUR && s_sinceInstallTry.idleOrPassed(20LL * 3600 * 1000)) {
        s_sinceInstallTry.start();
        DiagnosticLog::record("OTA", Texts::olNightInstall);
        installNow();
    }
}

Status status() {
    std::lock_guard<std::mutex> g(s_lock);
    return s_status;
}

}  // namespace OnlineUpdate
