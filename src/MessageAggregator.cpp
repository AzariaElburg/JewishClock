#include "MessageAggregator.h"
#include "Uptime.h"
#include "DateRules.h"
#include "EventsLock.h"
#include "Display.h"
#include "NTPManager.h"
#include "Storage.h"
#include "OnlineUpdate.h"
#include "DiagnosticLog.h"
#include "Updater.h"
#include "JewishDateEngine.h"
#include "Calendar.h"
#include "lang.h"
#include "WiFiManager.h"
#include "StringTools.h"
#include "Font.h"
#include "OnScreenElement.h"
#include "Tunables.h"
#include "Texts.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <limits.h>
#include <vector>
#include <atomic>

// === TUNABLES ===  (rule: see Config.h header)
// A long status line (setup info, "Connecting to ...") starts over this long
// after it has been scrolled to its end - see loop().
static const uint32_t STATUS_LINE_REPEAT_AFTER_MS = 2000;
// "Internet unreachable" (offlineNoticeDue()): shown once no known network
// has been joined for this long - a short drop and its reconnect show nothing.
static const int64_t OFFLINE_NOTICE_AFTER_MS = 60LL * 1000;
TUNABLE_SHARED(OFFLINE_NOTICE_AFTER_MS)
// Online, but no time server answered for this long: the status slide says so (the
// clock's own crystal drifts about a second a day - the time shown is still right).
static const int64_t NTP_NOTICE_AFTER_MS = 3LL * 24 * 3600 * 1000;
TUNABLE_SHARED(NTP_NOTICE_AFTER_MS)
// ... then every Nth round (batch / scroll lap); every round during the
// startup minutes (BOOT_OVERLAY_MS, MessageAggregator.h).
static const uint32_t OFFLINE_NOTICE_EVERY_N_ROUNDS = 4;
TUNABLE_SHARED(OFFLINE_NOTICE_EVERY_N_ROUNDS)
TUNABLE_SHARED_AS(BOOT_OVERLAY_MS, MessageAggregator::BOOT_OVERLAY_MS)

static std::vector<OnScreenElement> g_elements;
static uint16_t            g_currentColor = COLOR_TEXT_NORMAL;

static const uint32_t MAX_MESSAGE_ID = 1000000;
static uint32_t g_messageId       = 1; // Start at 1, so 0 can be an "invalid" ID

// --- Alternating mode: fixed batches ---
// One full pass through contentOrder's due-and-available types is decided
// ONCE, up front, as an ordered list of type identities - then played back
// one slot at a time, each getting its full configured dwell, with the list
// itself never touched again until every slot in it has been shown. Only
// once the batch is exhausted is a new one built (which is also the only
// moment any type's "every Nth" frequency gate is (re-)evaluated).
//
// This is deliberately NOT re-evaluated on every tick: doing so is what
// caused two different bugs earlier - (1) an unrelated type's frequency gate
// flipping mid-display could silently swap out whatever was already on
// screen before its own dwell time was up, and (2) re-checking "is this type
// due" against a live, freely-ticking round counter let a low-frequency type
// occasionally stay "due" across two consecutive advances (the round timer
// and the dwell/advance timer are independent and can drift out of phase),
// making it appear far more often than its configured frequency. Deciding
// membership exactly once per batch removes both failure modes structurally,
// not just in the cases already observed.
// A slot is one slide: a content type, and which of its parts (a type can
// have several - one per holiday of the day, two rain/dew lines; see partsOf()).
struct BatchSlot { ContentType type; size_t part; };
static std::vector<BatchSlot> s_currentBatch;
static const AppConfig* s_loopCfg = nullptr;   // the settings loop() last ran with (upcomingRounds())
// A new order or "every Nth" (Calendar Content on the page): the batch on
// screen used to play on in the old order to its end - up to minutes on a
// Friday at a slow scroll, and the user's father took it for "the drag didn't
// take effect" (2026-11-09, measured on the simulator). Now the slide on
// screen finishes and the next one starts a new batch in the new order -
// without showing the slide just shown again first (s_avoidFirst).
static bool      s_orderChanged = false;
static BatchSlot s_avoidFirst   = {ContentType::COUNT, 0};
static bool      s_haveAvoid    = false;
static size_t    s_batchIndex   = 0;
static uint32_t  s_batchCounter = 0; // "round" counter for Alternating mode - one per completed batch, not tied to wall-clock time.

static int64_t  s_lastDwellTime = 0;   // Uptime::ms()
static RotationMode s_lastRotationMode = RotationMode::SingleScroll; // Initialize to a default

// --- Content round counter (per-item frequency: "every Nth round") ---
// SingleScroll has no batch concept (it shows everything due, concatenated
// into one ticker, every tick) - but "every Nth round" still means every Nth
// full SHOWING of that combined ticker, not every Nth wall-clock dwell
// period. A message's length (and so how long one lap actually takes to
// scroll through) has nothing to do with how important or frequent its
// content should be, so the round only advances once Display reports the
// current ticker has completed a full lap (DisplayManager::
// consumeSingleScrollLapCompleted()) - never on a timer. This also means a
// new round can never be requested while the previous one is still
// mid-transition, which used to let a content change overwrite Display's
// already-queued "next" message before it was ever shown - same class of
// bug the comment on s_currentBatch above describes for Alternating mode,
// just reached a different way here. Alternating mode uses s_batchCounter
// above instead - see the comment on s_currentBatch for why.
static uint32_t s_roundCounter    = 0;

// Demo state (see "Demo" further down).
static std::atomic<int> s_demoRequest{0};      // set by the web handler: 1 = start, 2 = stop
static bool     s_demoActive = false;
static uint32_t s_demoStartRound = 0;          // SingleScroll: ends after one full lap
static bool     s_demoOnScreen = false;         // SingleScroll: its ticker built for the screen (the lap counts from then)
static std::vector<std::vector<OnScreenElement>> s_demoSlides;
static std::vector<std::vector<OnScreenElement>> buildDemoSlides(const AppConfig& cfg, const DisplayStrings& strings);

static const EventList* s_events = nullptr;
static bool s_forceRefresh = false;
static bool s_startupMinutes = false;   // loop()'s showOverlay: the minutes after a start
static void _nextMessageId() {
    // Increment and wrap, ensuring the ID is always between 1 and MAX_MESSAGE_ID.
    g_messageId = (g_messageId % MAX_MESSAGE_ID) + 1;
}

// This is a local copy of the logic from DisplayManager to avoid circular dependencies.
// It's used to pre-calculate the width of text elements.
static int _measureString(const char* utf8text) {
    if (!utf8text) return 0;

    auto decodeUTF8 = [](const uint8_t** ptr) -> uint32_t {
        uint8_t b = **ptr; (*ptr)++;
        if (b < 0x80) return b;
        if ((b & 0xE0) == 0xC0) { uint32_t cp = (b & 0x1F) << 6; b = **ptr; (*ptr)++; return cp | (b & 0x3F); }
        if ((b & 0xF0) == 0xE0) { uint32_t cp = (b & 0x0F) << 12; b = **ptr; (*ptr)++; cp |= (b & 0x3F) << 6; b = **ptr; (*ptr)++; cp |= (b & 0x3F); return cp; }
        return 0xFFFD;
    };

    const uint8_t* p = (const uint8_t*)utf8text;
    int width = 0;
    while (*p) { width += fontGetAdvanceForCodepoint(decodeUTF8(&p)); }
    return width;
}

// "Internet unreachable" this round? The user, 2026-11-06: the clock goes on
// with its saved calendar when the home Wi-Fi is gone (router down, renamed),
// but says so - every round in the startup minutes, then every 4th, until a
// network is back. A slot of its own in the slides (ContentType::COUNT).
static bool offlineNoticeDue(uint32_t roundCounter) {
    if (WiFiManager::offlineForMs() < OFFLINE_NOTICE_AFTER_MS) return false;
    return s_startupMinutes || roundCounter % OFFLINE_NOTICE_EVERY_N_ROUNDS == 0;
}
// The status slide - system messages, English in every display language (the user,
// 2026-11-14):
//  - "No internet!" (offlineNoticeDue());
//  - online, but no time server answered for NTP_NOTICE_AFTER_MS: "No NTP for 3 days",
//    the count going on as long as it lasts (the settings' name, NTP).
//    One wording each, chosen to fit the window without scrolling (the user, 2026-11-14:
//    no long/short versions; measured in the 475 px window: 292 px and 440 px - "for 100
//    days" and more is a little wider and scrolls);
//  - the clock's own Wi-Fi and its password - in the startup minutes and whenever
//    offline (no lockout: the clock has no buttons, so a person who doesn't know the
//    password and can't reach the clock over the home network unplugs it and reads it
//    off the screen). The name is the Wi-Fi up now: the clock's, or "...-Setup".
static bool ntpNoticeDue() {
    return WiFiManager::offlineForMs() < OFFLINE_NOTICE_AFTER_MS && NTPManager::isSynced()
        && NTPManager::msSinceSync() >= NTP_NOTICE_AFTER_MS;
}
// An update that went back by itself (Updater::failedUpdateOnScreen()): "Update to 2.3.0
// failed" - so the family knows to call (the user, 2026-11-14): at most 3 days, or until
// "OK, seen" in Settings. Asked once a round (a small file).
static String s_failedUpdate;
// "Error - see Settings" (the user, 2026-11-15) - whatever the clock found wrong by itself,
// Settings saying what: texts too long in its own files (Storage::textNotesOnScreen(): for 3 days),
// the update feed not there (OnlineUpdate::feedMissingOnScreen(): after 3 days, until it is back).
static bool s_textsTooLong = false;
static bool statusNoticeDue(uint32_t roundCounter) {
    s_failedUpdate = Updater::failedUpdateOnScreen();
    time_t local = NTPManager::getLocalEpoch();
    s_textsTooLong = Storage::textNotesOnScreen(local > 0 ? (long)(local / 86400) : 0) || OnlineUpdate::feedMissingOnScreen();
    return s_startupMinutes || offlineNoticeDue(roundCounter)
        || ((ntpNoticeDue() || NTPManager::hasRefusedServer() || s_failedUpdate.length() || s_textsTooLong) && roundCounter % OFFLINE_NOTICE_EVERY_N_ROUNDS == 0);
}
static String ntpNotice() {
    return Texts::fill(Texts::noNtp, String((long)(NTPManager::msSinceSync() / (24LL * 3600 * 1000))));
}
static std::vector<OnScreenElement> offlineNotice(const DisplayStrings& strings) {
    (void)strings;
    bool offline = WiFiManager::offlineForMs() >= OFFLINE_NOTICE_AFTER_MS;
    String t;
    auto add = [&t](const String& part) { t += (t.length() ? String(Texts::partsJoin) : String()) + part; };
    if (offline) add(Texts::noInternet);
    else if (NTPManager::hasRefusedServer()) add(Texts::ntpServerRefused);
    else if (ntpNoticeDue()) add(ntpNotice());
    if (s_failedUpdate.length()) add(Texts::fill(Texts::updateFailed, s_failedUpdate));
    // One wording for whatever Settings lists as wrong (the user, 2026-11-15: "something wrong, see
    // Settings", fitting the window): measured 467 px in the default 475 px window - alone on its slide
    // it doesn't scroll. Today: texts too long in the clock's own files.
    if (s_textsTooLong) add(Texts::errorSeeSettings);
    String name = WiFiManager::ownWifiName();
    if ((offline || s_startupMinutes) && name.length())
        add(Texts::fill(Texts::clockWifi, name, WiFiManager::apPassword()));
    if (!t.length()) return {};
    return { OnScreenElement(OnScreenElementType::TEXT, t, _measureString(t.c_str())) };
}

static long getTodayAbsolute() {
    NTPManager::DateTime dt;
    if (!NTPManager::getDateTime(dt)) return 0;
    return JewishDateEngine::gregorianToAbsolute(dt.year, dt.month, dt.day);
}

static bool nextOccurrence(const PersonalEvent& ev, bool hebrew, long todayAbs, long& outAbs, int& outAge);

// Days until an event's next date (0 = today), the age then (-1 = no year)
// and whether that date is its Hebrew one. An event with both dates counts
// from whichever comes first; on a tie, the Gregorian one. Only coming dates
// count: until 2026-11-01 the nearest date counted even if it had passed - a
// Gregorian birthday yesterday then hid a Hebrew one's advance warning.
// hebrewTodayAbs: "today" for the Hebrew date - the Hebrew day as the clock
// shows it (changes at the rollover, צאת הכוכבים), or the civil day
// (EventDayChange::Midnight, or Hebrew date unknown: 0). Gregorian dates
// always count from the civil day.
static int getEventOffsetDays(const PersonalEvent& event, long hebrewTodayAbs, int& age, bool& hebrewDate) {
    age = -1;
    hebrewDate = false;
    long todayAbs = getTodayAbsolute();
    if (todayAbs <= 0) return INT_MAX;
    long hebToday = hebrewTodayAbs > 0 ? hebrewTodayAbs : todayAbs;

    int best = INT_MAX;
    long abs; int a;
    if (event.hasGregorian && nextOccurrence(event, false, todayAbs, abs, a)) {
        best = (int)(abs - todayAbs);
        age = a;
    }
    // Yahrzeits and birthdays/other resolve the Cheshvan/Kislev-30 and
    // Adar-I/II questions differently - see nextOccurrence().
    if (event.hasJewish && nextOccurrence(event, true, hebToday, abs, a) && (int)(abs - hebToday) < best) {
        best = (int)(abs - hebToday);
        age = a;
        hebrewDate = true;
    }
    return best;
}

void MessageAggregator::begin(const AppConfig& cfg, const EventList& events) {
    s_events = &events;
    String startMsg = StringTools::formatProjectName(cfg.projectName);
    g_elements.emplace_back(OnScreenElementType::TEXT, startMsg, _measureString(startMsg.c_str()));
    _nextMessageId();
}

void MessageAggregator::loop(const AppConfig& cfg, bool showOverlay) {
    s_loopCfg = &cfg;
    // Performance Gate: Only rebuild the message list once per second.
    static int64_t lastUpdate = 0;
    if (Uptime::ms() - lastUpdate < 1000) return;
    lastUpdate = Uptime::ms();
    s_startupMinutes = showOverlay;

    // CLOCK_LOG("Aggregator", "Loop Start. RotationMode: %d", (int)cfg.rotationMode);

    // --- Rotation Mode State Management ---
    if (cfg.rotationMode != s_lastRotationMode) {
        s_currentBatch.clear();
        s_batchIndex = 0;
        s_lastDwellTime = Uptime::ms(); // Reset timer on mode change
        s_lastRotationMode = cfg.rotationMode;
    }

    // --- Demo start/stop (requested from the web page, see requestDemo()) ---
    int req = s_demoRequest.exchange(0);
    if (req == 1) {
        s_demoSlides = buildDemoSlides(cfg, getStrings(cfg.language));
        s_demoActive = true;
        s_batchIndex = 0;
        s_lastDwellTime = Uptime::ms();
        DisplayManager::consumeSingleScrollLapCompleted();   // a lap of the previous ticker doesn't count
        s_demoStartRound = s_roundCounter;
        s_demoOnScreen = false;   // single scroll: its lap counts from when its ticker is built (buildElements)
        CLOCK_LOG("Aggregator", "Demo started: %u slides", (unsigned)s_demoSlides.size());
    } else if (req == 2 && s_demoActive) {
        s_demoActive = false;
        CLOCK_LOG("Aggregator", "Demo stopped");
    }
    if (!s_demoActive && !s_demoSlides.empty()) {   // ended (stopped or played through): back to the normal rotation
        s_demoSlides.clear();
        s_currentBatch.clear();
        s_batchIndex = 0;
        s_lastDwellTime = Uptime::ms();
    }

    // --- SingleScroll's round advance (lap-based - see comment above s_roundCounter) ---
    if (cfg.rotationMode == RotationMode::SingleScroll && DisplayManager::consumeSingleScrollLapCompleted()) {
        s_roundCounter++;
    }

    {   // Calendar Content's order / frequency changed? (see s_orderChanged)
        static uint8_t s_seenOrder[(int)ContentType::COUNT], s_seenFreq[(int)ContentType::COUNT];
        static bool s_seen = false;
        if (!s_seen || memcmp(s_seenOrder, cfg.contentOrder, sizeof(s_seenOrder)) != 0
                    || memcmp(s_seenFreq, cfg.contentFrequency, sizeof(s_seenFreq)) != 0) {
            if (s_seen) s_orderChanged = true;
            memcpy(s_seenOrder, cfg.contentOrder, sizeof(s_seenOrder));
            memcpy(s_seenFreq, cfg.contentFrequency, sizeof(s_seenFreq));
            s_seen = true;
        }
    }

    if (cfg.rotationMode == RotationMode::Alternating) {
        int64_t dwellMs = (int64_t)cfg.messageDwellSec * 1000;
        int64_t elapsed = Uptime::ms() - s_lastDwellTime;
        bool advance;
        if (DisplayManager::isAlternatingScrollActive()) {
            // Currently holding before, mid-round, or repeating another round of
            // the item's reveal scroll (Display decides whether another round
            // fits) - never switch away mid-scroll regardless of dwell time.
            advance = false;
        } else {
            uint32_t revealMs = DisplayManager::getAlternatingScrollRevealElapsedMs();
            if (revealMs > 0) {
                // Scrolling (one or more rounds) has finished - Display only
                // stops looping once too little dwell time was left for another
                // round, so just linger half the dwell time more from there.
                advance = (elapsed >= revealMs + dwellMs / 2);
            } else {
                // Item fit without scrolling - switch at the normal configured time.
                advance = (elapsed >= dwellMs);
            }
        }
        if (advance) {
            if (s_orderChanged && !s_demoActive) {   // a new batch in the new order, from this slide on
                s_haveAvoid = s_batchIndex < s_currentBatch.size();
                if (s_haveAvoid) s_avoidFirst = s_currentBatch[s_batchIndex];
                s_currentBatch.clear();
                s_orderChanged = false;
                CLOCK_LOG("Aggregator", "Calendar Content order/frequency changed - new batch from the next slide");
            }
            s_batchIndex++; // buildElements() starts a fresh batch once this runs off the end.
            s_lastDwellTime = Uptime::ms();
        }
    }

    // If the content has changed, generate a new ID to trigger a display update.
    // Snapshot the *whole* previous element list (not just the first entry) so a
    // change further down the rotation — e.g. only a later element's text
    // differs — isn't silently missed.
    std::vector<OnScreenElement> oldElements = g_elements;
    // Language is tracked separately because it can flip the RTL layout (icon
    // left/right of its label) without any element's text content changing at
    // all — e.g. while only an icon with no label is on screen.
    static char s_lastLanguage[4] = "";
    bool languageChanged = (strcmp(cfg.language, s_lastLanguage) != 0);
    StringTools::utf8Copy(s_lastLanguage, cfg.language);

    // 1. Clear and rebuild the list of on-screen elements.
    g_elements.clear();
    g_currentColor = COLOR_TEXT_NORMAL;

    // If the boot overlay is active, it takes precedence.
    if (showOverlay) {
        String overlayMsg = WiFiManager::getStatusMessage();
        // A status line (setup info, "Connecting to ...") is the only item:
        // in Alternating mode a long one scrolled to its end once and then
        // stood still there - nothing came next (seen 2026-10-24: the setup
        // screen showed only the end of the setup line, its password). Start it over once read.
        static int64_t s_statusLineEndMs = 0;   // when its scroll was first seen finished
        bool statusLine = !overlayMsg.isEmpty() || !NTPManager::isSynced();
        if (!statusLine || DisplayManager::getAlternatingScrollRevealElapsedMs() == 0) s_statusLineEndMs = 0;
        else if (!s_statusLineEndMs) s_statusLineEndMs = Uptime::ms() | 1;
        else if (Uptime::ms() - s_statusLineEndMs >= STATUS_LINE_REPEAT_AFTER_MS) { s_forceRefresh = true; s_statusLineEndMs = 0; }
        if (!overlayMsg.isEmpty()) {
            g_elements.emplace_back(OnScreenElementType::TEXT, overlayMsg, _measureString(overlayMsg.c_str()));
            g_currentColor = COLOR_GOLD; // Use a distinct color for setup info.
        } else if (NTPManager::isSynced()) {
            // If time is synced and we are NOT in AP mode, it's safe to show
            // the normal calendar messages, even if still in the boot window.
            buildElements(cfg);
        } else {
            // During the boot window, if we aren't showing calendar data yet,
            // provide a helpful status message about the connection attempt.
            String joining = WiFiManager::joiningSsid();
            bool anyNetwork = false;
            for (int i = 0; i < WIFI_NETWORKS; i++) anyNetwork |= wifiSsidAt(cfg, i)[0] != '\0';
            if (anyNetwork) {
                // While choosing among several known networks, no name yet.
                // Joined but the time servers don't answer: say that, not
                // "Connecting" (2026-11-10, drill ntp_blocked_at_start).
                String msg = WiFi.status() == WL_CONNECTED ? String(Texts::waitingForTime)
                           : joining.length() ? Texts::fill(Texts::connecting, joining) : String(Texts::lookingForWifi);
                g_elements.emplace_back(OnScreenElementType::TEXT, msg, _measureString(msg.c_str()));
            } else {
                String msg = Texts::noWifiConfigured;
                g_elements.emplace_back(OnScreenElementType::TEXT, msg, _measureString(msg.c_str()));
            }
            g_currentColor = COLOR_GOLD;
        }
    } else {
        // Normal operation: build the list of elements.
        buildElements(cfg);
    }

    // 2. Check if the generated elements are different from the last run.
    bool contentChanged = (g_elements.size() != oldElements.size());
    if (!contentChanged) {
        for (size_t i = 0; i < g_elements.size(); i++) {
            if (g_elements[i].content != oldElements[i].content) { contentChanged = true; break; }
        }
    }
    if (contentChanged || languageChanged || s_forceRefresh) {
        _nextMessageId();
        s_forceRefresh = false;
    }
}

void MessageAggregator::forceRefresh() {
    s_forceRefresh = true;
}

// Produces the on-screen elements for exactly one content type: its own
// "every Nth round" frequency gate (against whichever round counter the
// caller passes in - see the comments on s_roundCounter/s_batchCounter),
// plus whatever data-presence checks that type needs (e.g. no Havdalah text
// -> nothing to show), collapsed into one pure function of (type, current
// state). Empty return means "nothing to show for this type right now" -
// the single convention every type already followed individually; this just
// makes it queryable per-type instead of only as a side effect of rebuilding
// everything at once.
//
// This is the ONLY place that needs to change to add a new content type in
// the future: a new enum value in ContentType, a case here, and a slot in
// contentOrder/contentFrequency - neither SingleScroll's loop nor
// Alternating's batching below ever need to change, since they only ever
// ask "is type X due and does it have anything to show", by identity, for
// whichever type(s) they're interested in.
//
// `part`: a type can have several slides - so far only Holiday, one per
// holiday of the day (partsOf()). Every other type has just part 0.
// Hebcal names both days of a two-day Rosh Chodesh alike; the clock says
// which day: "א׳ דראש חודש חשוון" (the 30th) / "ב׳ דראש חודש חשוון" (the 1st
// after a 30-day month). A one-day Rosh Chodesh stays as it is.
static String withRoshChodeshDay(const char* name, long dayAbs) {
    static const char RC[] = "ראש חודש";
    if (strncmp(name, RC, sizeof(RC) - 1) != 0 || dayAbs <= 0) return name;
    int y, m, d, py, pm, pd;
    JewishDateEngine::absoluteToHebrew(dayAbs, y, m, d);
    JewishDateEngine::absoluteToHebrew(dayAbs - 1, py, pm, pd);
    if (d == 30) return String("א׳ ד") + name;
    if (d == 1 && pd == 30) return String("ב׳ ד") + name;
    return name;
}

// "חנוכה: נר א׳" on 25 Kislev ... "חנוכה: נר ח׳" on the 8th day, or "". By
// the Hebrew date, so it changes when the Hebrew day does - at צאת הכוכבים
// (the user's father, 2026-10-20: calendars write "נר ראשון" on 25 Kislev,
// and from צאת הכוכבים everything follows the day that starts then). No
// lighting time (decided 2026-10-20).
static String chanukahText(long dayAbs) {
    if (dayAbs <= 0) return "";
    int y, m, d;
    JewishDateEngine::absoluteToHebrew(dayAbs, y, m, d);
    long n = dayAbs - JewishDateEngine::hebrewToAbsolute(y, HebrewMonths::KISLEV, 25) + 1;
    if (n < 1 || n > 8) return "";
    char num[16];
    JewishDateEngine::numToGematria((int)n, num, sizeof(num));
    return String("חנוכה: נר ") + num;
}

static std::vector<String> holidayTexts(const DayInfo& today) {
    std::vector<String> texts;
    // Fast start/end times have their own slide (ContentType::Fast), keyed to
    // civil days so they stay up past sunset, unlike these. A name that is
    // just a repeat of the date string is skipped.
    long todayAbs = today.hYear > 0 ? JewishDateEngine::hebrewToAbsolute(today.hYear, today.hMonth, today.hDay) : 0;
    for (const char* h : { today.holidayHebrew, today.holidayHebrew2 }) {
        if (h[0] && strcmp(h, today.hebrewDateStr) != 0) texts.push_back(withRoshChodeshDay(h, todayAbs));
    }
    String chanukah = chanukahText(todayAbs);
    if (chanukah.length()) texts.push_back(chanukah);
    return texts;
}

// ── Slide formatting shared by the real slides and the demo ──────────────────
// (buildContentType() below and buildDemoSlides() further down) - so the
// demo shows exactly what the real slides look like.

static OnScreenElement textElement(const String& s) {
    return OnScreenElement(OnScreenElementType::TEXT, s, _measureString(s.c_str()));
}

// One of the clock's own pictures by name ("Candles" = /IMG/Candles.bmp), or
// the family's replacement for it (FS_CUSTOM_DIR, Settings > Pictures) when
// there is one the screen can show.
static String builtinImage(const char* name) {
    String custom = customImagePath(name);
    if (DisplayManager::hasImage(custom.c_str())) return custom;
    return String("/IMG/") + name + ".bmp";
}

// Icon + text glued together, no label (Candles, Havdalah, Parasha, Mishna).
static std::vector<OnScreenElement> iconAndText(const String& imagePath, const String& text, int frameDelayMs = 0) {
    std::vector<OnScreenElement> out;
    OnScreenElement icon(OnScreenElementType::STATIC_IMAGE, imagePath, FONT_H);
    if (frameDelayMs) icon.frame_delay_ms = frameDelayMs;
    out.push_back(icon);
    OnScreenElement t = textElement(text);
    t.glueToPrevious = true;
    out.push_back(t);
    return out;
}

// The picture for an event's emoji - the font has no emoji - or "": then the
// icon stays text (a custom text icon, or an emoji with no picture).
// Pictures: utils/generate_event_icons.py, or the family's replacement
// (builtinImage()). "🕯️" and "🕯" (with/without the U+FE0F variation
// selector) are the same icon.
static String eventIconPath(const char* icon) {
    static const struct { const char* emoji; const char* name; } pictures[] = {
        { "🎂", "Cake" },        { "🕯", "MemorialCandle" }, { "💍", "Rings" },
        { "✡", "StarOfDavid" },  { "🕍", "Synagogue" },      { "🎉", "Party" },
        { "👶", "Baby" },        { "🎓", "GradCap" },        { "🏠", "House" },
        { "🗓", "Calendar" },
    };
    String s = icon;
    s.replace("\xEF\xB8\x8F", "");   // U+FE0F
    s.trim();
    for (const auto& p : pictures) {
        if (s == p.emoji) return builtinImage(p.name);
    }
    return String();
}

// " בעוד 3 ימים" - an event's reminder, or "" on the day itself.
static String formatEventReminder(int diffDays, const DisplayStrings& strings) {
    if (diffDays <= 0) return String();
    return String(" ") + strings.omerIn + " " + String(diffDays) + " " + (diffDays == 1 ? strings.daySingular : strings.dayPlural);
}

// "🎂 Name (33)", or with a reminder "... בעוד 3 ימים". age < 0 = no age.
// withIcon = false: without the icon (it is shown as a picture instead).
// withReminder = false: without the reminder (appendEvent() puts the
// Hebrew-date star between the two).
static String formatEventLabel(const PersonalEvent& ev, int age, int diffDays, const DisplayStrings& strings,
                               bool withIcon = true, bool withReminder = true) {
    char iconBuf[sizeof(ev.icon) + 2] = "";   // the icon (up to 31 bytes) + a space
    if (withIcon && ev.icon[0]) snprintf(iconBuf, sizeof(iconBuf), "%s ", ev.icon);
    String label = String(iconBuf) + ev.name;
    if (age >= 0) label += " (" + String(age) + ")";
    if (withReminder) label += formatEventReminder(diffDays, strings);
    return label;
}

// Adds one event to an events slide: its picture glued to its label, the
// way Candles/Havdalah are. The picture is the event's uploaded image if it
// has one the screen can show, otherwise its icon (decided 2026-10-21: the
// image replaces the icon; the icon is the fallback). Several events of a
// day are glued on after ", ".
// hebrewDate: its Hebrew date brought it to the screen - on an event with
// both dates, a small Star of David after the age says so (2026-11-01; the
// Gregorian date gets no mark; utils/generate_star.py).
static void appendEvent(std::vector<OnScreenElement>& out, const PersonalEvent& ev, int age, int diffDays,
                        const DisplayStrings& strings, bool hebrewDate = false) {
    if (!out.empty()) {   // every event ends with its text element
        bool glued = out.back().glueToPrevious;
        out.back() = textElement(out.back().content + ", ");
        out.back().glueToPrevious = glued;
    }
    const bool star = hebrewDate && ev.hasJewish && ev.hasGregorian;
    String picture = ev.image[0] ? imagePath(ev.image) : String();
    bool ownImage = picture.length() && DisplayManager::hasImage(picture.c_str());
    if (!ownImage && ev.iconImage[0]) {   // an icon the font can't draw, as the page drew it
        picture = imagePath(ev.iconImage);
        ownImage = DisplayManager::hasImage(picture.c_str());
    }
    if (!ownImage) {
        picture = eventIconPath(ev.icon[0] ? ev.icon : Storage::defaultIconForType(ev.type));   // (every event read gets one)
    }
    if (picture.length()) {
        OnScreenElement pic(OnScreenElementType::STATIC_IMAGE, picture, FONT_H);
        if (!ownImage) pic.frame_delay_ms = 90;   // the flames' cadence, as Candles.bmp
        pic.glueToPrevious = !out.empty();
        out.push_back(pic);
    }
    // No picture at all: the icon stays in the text (a typed text icon).
    OnScreenElement t = textElement(String(picture.length() ? " " : "") +
                                    formatEventLabel(ev, age, diffDays, strings, picture.length() == 0, !star) +
                                    (star ? " " : ""));
    t.glueToPrevious = !out.empty();
    out.push_back(t);
    if (star) {
        OnScreenElement mark(OnScreenElementType::STATIC_IMAGE, builtinImage("HebrewDate"), FONT_H);
        mark.glueToPrevious = true;
        out.push_back(mark);
        // the reminder after it - and the text element every event ends with
        OnScreenElement rest = textElement(formatEventReminder(diffDays, strings));
        rest.glueToPrevious = true;
        out.push_back(rest);
    }
}

// Examples (he): "צום: מחר 05:10-19:03" (day before), "צום: 05:10-19:03"
// (fast day), "צום: 19:29 עד מחר 19:57" / "צום: עד 19:57" (Tisha B'Av eve /
// day), "תענית בכורות: מחר 04:49" (no end time), "סעודה מפסקת עד 18:20".
// "מחר " / "מחרתיים " before a time that many civil days ahead, else nothing.
static String daysAheadWord(int inDays, const DisplayStrings& strings) {
    if (inDays == 1) return String(strings.tomorrow) + " ";
    if (inDays == 2) return String(strings.dayAfterTomorrow) + " ";
    return String();
}

// A time of day as the screen shows it: "17:26", or with the 24-hour clock
// off (Settings, cfg.use24Hour) "5:26" - no AM/PM, a candle-lighting or fast
// time says itself which half of the day it is in. The calendar keeps its
// times as "HH:MM" (compared as such); only the screen gets this form
// (2026-11-10: the setting was saved but used nowhere on the clock).
static String clockTime(const char* hhmm, const AppConfig& cfg) {
    int h, m, sec;
    if (cfg.use24Hour || !DateRules::parseTime(hhmm, h, m, sec)) return String(hhmm ? hhmm : "");
    char buf[8];
    snprintf(buf, sizeof(buf), "%d:%02d", h % 12 ? h % 12 : 12, m);
    return buf;
}

static String formatFast(const FastNotice& f, const DisplayStrings& strings, const AppConfig& cfg) {
    String msg;
    if (f.kind == FastNotice::Kind::SeudaMafseket) return String(strings.seudaMafseketUntil) + " " + daysAheadWord(f.endInDays, strings) + clockTime(f.end, cfg);
    // Without an end time the fast's own name says which fast it is.
    msg = String(!f.end[0] && f.name[0] ? f.name : strings.fastLabel) + ": ";
    if (f.begin[0]) {
        msg += daysAheadWord(f.beginInDays, strings);
        msg += clockTime(f.begin, cfg);
    }
    if (f.end[0]) {
        if (f.begin[0] && f.endInDays == f.beginInDays) {
            msg += String("-") + clockTime(f.end, cfg);
        } else {
            if (f.begin[0]) msg += " ";
            msg += String(strings.until) + " ";
            msg += daysAheadWord(f.endInDays, strings);
            msg += clockTime(f.end, cfg);
        }
    }
    return msg;
}

// Omer day in the chosen wording. fullText = Hebcal's full sentence (Full mode).
static String formatOmer(int dayNum, const char* fullText, OmerMode mode, const DisplayStrings& strings) {
    String omer = fullText;
    if (mode == OmerMode::ShortText) { // Short: "16 לעומר"
        omer = String(dayNum) + " " + strings.omerOf;
    } else if (mode == OmerMode::Abbreviated) { // Abbreviated: "16 = 2 שבועות + 2 ימים לעומר"
        if (dayNum >= 7) {
            int w = dayNum / 7;
            int d = dayNum % 7;
            omer = String(dayNum) + " " + strings.whichAre + " " + String(w) + (w == 1 ? String(" ") + strings.weekSingular : String(" ") + strings.weekPlural);
            if (d > 0) {
                omer += String(" ") + strings.plus + " " + String(d) + (d == 1 ? String(" ") + strings.daySingular : String(" ") + strings.dayPlural);
            }
            omer += String(" ") + strings.omerOf;
        } else {
            omer = String(dayNum) + " " + strings.omerOf;
        }
    } else if (mode == OmerMode::Compact) {
        // Without scrolling (the user, 2026-11-06): the first of these that
        // fits the window - "23 = 3 שבועות + 2 ימים לעומר", without "לעומר",
        // without the spaces ("23=3 שבועות+2 ימים", the user's own form) -
        // else that last one, scrolling. Measured, not fixed: the window
        // changes with calibration.
        omer = String(dayNum) + " " + strings.omerOf;
        if (dayNum >= 7) {
            int w = dayNum / 7, d = dayNum % 7;
            String weeks = String(w) + " " + (w == 1 ? strings.weekSingular : strings.weekPlural);
            String days = d ? String(d) + " " + (d == 1 ? strings.daySingular : strings.dayPlural) : String();
            String spaced = String(dayNum) + " = " + weeks + (d ? String(" ") + strings.plus + " " + days : String());
            String tight = String(dayNum) + "=" + weeks + (d ? String(strings.plus) + days : String());
            const String tries[] = { spaced + " " + strings.omerOf, spaced, tight };
            for (const String& t : tries) {
                omer = t;
                if (_measureString(t.c_str()) <= DisplayManager::windowWidth()) break;
            }
        }
    }
    // Mode Full uses Hebcal's sentence as is (e.g. "היום ארבעה עשר יום...").
    return omer;
}

// "18 אוקטובר 2026" - the year follows the same "Show year" switch as the
// Hebrew date slide (cfg.showHebrewYear).
static String formatGregorianDate(int y, int m, int d, const AppConfig& cfg, const DisplayStrings& strings) {
    char gBuf[48];
    if (cfg.showHebrewYear) snprintf(gBuf, sizeof(gBuf), "%d %s %d", d, strings.gregorianMonths[m - 1], y);
    else                    snprintf(gBuf, sizeof(gBuf), "%d %s", d, strings.gregorianMonths[m - 1]);
    return gBuf;
}

// The rain/dew slide: one line per switch, or one for both when they say
// the same - "מוריד הטל, ותן ברכה" after Pesach. On Pesach's first day:
// "מוריד הטל ממוסף", then "ותן ברכה מהערב" (examples: Calendar.h).
static std::vector<String> prayerTexts(const AppConfig& cfg, const DisplayStrings& strings) {
    const long civilAbs = getTodayAbsolute();
    PrayerSwitchNotice p = Calendar::getPrayerSwitch(cfg.switchoverWindowDays, cfg.nusach, cfg.inIsrael, civilAbs);
    RainRequestNotice r = Calendar::getRainRequest(cfg.switchoverWindowDays, cfg.nusach, cfg.inIsrael, civilAbs);
    auto line = [](const char* phrase, const char* when) { return *when ? String(phrase) + " " + when : String(phrase); };
    const int pd = p.daysFromSwitch, rd = r.daysFromSwitch;
    const char* pWhen = pd > 0 ? "" : strings.prayerSwitchFrom[-pd];
    const char* rWhen = rd <= 0 ? strings.prayerSwitchEvening[-rd] : "";
    std::vector<String> texts;
    if (p.phrase && r.phrase && strcmp(pWhen, rWhen) == 0) {
        texts.push_back(line((String(p.phrase) + ", " + r.phrase).c_str(), pWhen));
    } else {
        if (p.phrase) texts.push_back(line(p.phrase, pWhen));
        if (r.phrase) texts.push_back(line(r.phrase, rWhen));
    }
    return texts;
}

static size_t partsOf(ContentType type, const DayInfo& today) {
    if (type == ContentType::PrayerSwitch) return 2;   // at most; an empty part shows nothing
    return type == ContentType::Holiday ? holidayTexts(today).size() : 1;
}

static std::vector<OnScreenElement> buildContentType(ContentType type, size_t part, const AppConfig& cfg,
                                                       const DayInfo& today, const DisplayStrings& strings,
                                                       uint32_t roundCounter) {
    uint8_t freq = cfg.contentFrequency[(int)type];
    if (freq == 0 || (roundCounter % freq) != 0) return {};
    if (part > 0 && type != ContentType::Holiday && type != ContentType::PrayerSwitch) return {};

    std::vector<OnScreenElement> out;
    switch (type) {
    case ContentType::Events: {
    // Personal Events
    if (s_events && !s_events->empty()) {
        const DayInfo& hd = Calendar::getToday();
        long hebrewTodayAbs = (cfg.eventDayChange == EventDayChange::WithHebrewDay && hd.hYear > 0)
            ? JewishDateEngine::hebrewToAbsolute(hd.hYear, hd.hMonth, hd.hDay) : 0;
        EventsLock::take();
        for (const auto& ev : *s_events) {
            int age = -1;
            bool hebrewDate = false;
            int diffDays = getEventOffsetDays(ev, hebrewTodayAbs, age, hebrewDate);

            if (diffDays < 0 || diffDays > ev.warningDays) continue;
            appendEvent(out, ev, age, diffDays, strings, hebrewDate);
        }
        EventsLock::give();
    }
    break; }
    case ContentType::Candle: {
    // When it shows: cfg.candleShow (Calendar::getCandle()). By default the
    // plain civil date (not the sunset-rollover `today`), so tonight's candle
    // time keeps showing until civil midnight - see Calendar::getCivilToday().
    TimedNotice t = Calendar::getCandle(cfg);
    if (t.time[0]) {
        // Icon + time glued together, no text label ("מחר 16:22" when shown
        // the evening before and the user wants it). 90 ms = the animation's
        // baked-in frame cadence.
        String text = (cfg.candleShow.tomorrow ? daysAheadWord(t.inDays, strings) : String()) + clockTime(t.time, cfg);
        out = iconAndText(builtinImage("Candles"), text, 90);
    }
    break; }
    case ContentType::Havdalah: {
    // Same as Candle above (cfg.havdalahShow): by default only from civil
    // midnight of the day it occurs on, not from the Hebrew day's start.
    TimedNotice t = Calendar::getHavdalah(cfg);
    if (t.time[0]) {
        // Icon immediately followed by the time, no label, no separator.
        String text = (cfg.havdalahShow.tomorrow ? daysAheadWord(t.inDays, strings) : String()) + clockTime(t.time, cfg);
        out = iconAndText(builtinImage("Havdala"), text);
    }
    break; }
    case ContentType::Holiday: {
    // One slide per holiday of the day (see holidayTexts()); `part` picks it.
    std::vector<String> texts = holidayTexts(today);
    if (part < texts.size()) out.emplace_back(OnScreenElementType::TEXT, texts[part], _measureString(texts[part].c_str()));
    break; }
    case ContentType::Mevarchim: {
    // Shabbat Mevarchim is tagged by Hebcal only onto the Shabbat it's
    // actually announced on, so looking at just today's own row would only
    // ever show it that one day. getUpcomingMevarchim() instead looks ahead
    // to the next Shabbat that has one, so it shows all week leading up to
    // it - and still correctly shows only on an actual Mevarchim Shabbat,
    // since most weeks aren't one and the lookup comes back empty. Other
    // windows (cfg.mevarchimShow, by default like the parasha's): Calendar.
    String msg = Calendar::getMevarchim(cfg);
    if (msg.length() > 0) {
        out.emplace_back(OnScreenElementType::TEXT, msg, _measureString(msg.c_str()));
    }
    break; }
    case ContentType::Parasha: {
    // Show the weekly portion, replacing the word "פרשת" with a custom Torah
    // scroll icon for a more graphical presentation. Same look-ahead as
    // Mevarchim above (see getUpcomingParasha()) - shows the upcoming
    // Shabbat's portion all week, not just once "today" rolls into it - or
    // only around Shabbat (cfg.parashaShow, Calendar::getParasha()).
    String parasha = Calendar::getParasha(cfg);
    if (parasha.length() > 0) {
        out = iconAndText(builtinImage("Torah"), String(" ") + parasha);
    }
    break; }
    case ContentType::Omer: {
    if (cfg.omerMode != OmerMode::Off && today.omerHebrew[0]) {
        if (today.omerDay > 0) out.push_back(textElement(formatOmer(today.omerDay, today.omerHebrew, cfg.omerMode, strings)));
    }
    break; }
    case ContentType::DafYomi: {
    if (today.dafYomi[0]) {
        String msg = String(strings.dafYomi) + ": " + today.dafYomi;
        out.emplace_back(OnScreenElementType::TEXT, msg, _measureString(msg.c_str()));
    }
    break; }
    case ContentType::MishnaYomit: {
    if (today.mishnaYomit[0]) {
        // Icon + reference glued together, no text label (same pattern as Parasha/Havdalah).
        out = iconAndText(builtinImage("Mishnah"), String(" ") + today.mishnaYomit);
    }
    break; }
    case ContentType::HebrewDate: {
    String hDateStr = Calendar::formatUserHebrewDate(today.hebrewDateStr);
    out.emplace_back(OnScreenElementType::TEXT, hDateStr, _measureString(hDateStr.c_str()));
    break; }
    case ContentType::Fast: {
    FastNotice f = Calendar::getFastNotice(cfg);   // when: cfg.fastShow
    if (f.kind == FastNotice::Kind::None) break;
    out.push_back(textElement(formatFast(f, strings, cfg)));   // examples: see formatFast()
    break; }
    case ContentType::PrayerSwitch: {
    // "משיב הרוח ממחרתיים", "... ממחר", "... ממוסף", then "משיב הרוח";
    // "ותן טל ומטר ממחר בערב", "... מהערב", then "ותן טל ומטר" - see prayerTexts().
    std::vector<String> texts = prayerTexts(cfg, strings);
    if (part < texts.size()) out.emplace_back(OnScreenElementType::TEXT, texts[part], _measureString(texts[part].c_str()));
    break; }
    case ContentType::GregorianDate: {
    // Read straight from NTP rather than DayInfo: DayInfo's gregorianDate is
    // only filled when the Hebcal manifest has today's entry, so it would
    // vanish offline. NTP's local calendar date is the Gregorian date itself
    // (it rolls at midnight, unlike the Hebrew date which rolls at sunset).
    NTPManager::DateTime dt;
    if (!NTPManager::getDateTime(dt)) break;
    out.push_back(textElement(formatGregorianDate(dt.year, dt.month, dt.day, cfg, strings)));
    break; }
    default: break;
    } // switch
    return out;
}

// ── Demo (Advanced settings) ─────────────────────────────────────────────────
// One pass through samples of everything the clock can show, built with the
// same formatting as the real slides, then back to normal by itself. Played
// in the configured mode: Alternating = one slide per dwell, SingleScroll =
// one ticker of all of them. Not saved - a restart ends it too.

// Next occurrence (from today, inclusive) of an event on its Gregorian or
// Hebrew date: the absolute day and the age then (-1 = no year given).
// A date the mapping refused (DateRules / JewishDateEngine say it isn't one): never
// expected - every way in checks dates first - so something is wrong: said in the log,
// once per event and calendar, and the event skipped instead of shown on a made-up day.
static void mappingRefused(const PersonalEvent& ev, bool hebrew) {
    static std::vector<String> said;
    String key = String(ev.id) + (hebrew ? "h" : "g");
    for (const String& k : said) if (k == key) return;
    if (said.size() < 64) said.push_back(key);
    DiagnosticLog::record("Events", "\"%s\": its %s date %d/%d/%d is no date - not shown (please open the event and check it)",
                          ev.name, hebrew ? "Hebrew" : "Gregorian", hebrew ? ev.jDay : ev.gDay, hebrew ? ev.jMonth : ev.gMonth,
                          hebrew ? ev.jYear : ev.gYear);
}

static bool nextOccurrence(const PersonalEvent& ev, bool hebrew, long todayAbs, long& outAbs, int& outAge) {
    int gy, gm, gd, hy, hm, hd;
    JewishDateEngine::absoluteToGregorian(todayAbs, gy, gm, gd);
    JewishDateEngine::absoluteToHebrew(todayAbs, hy, hm, hd);
    for (int k = 0; k < 2; k++) {
        long abs; int year;
        if (hebrew) {
            if (!DateRules::validHebrew(0, ev.jMonth, ev.jDay)) return false;
            HebrewDate original = { ev.jYear, ev.jMonth, ev.jDay };   // 30 Cheshvan, Adar II ... in other years: the Hebcal rules
            HebrewDate r = strcmp(ev.type, "yahrzeit") == 0 ? JewishDateEngine::resolveYahrzeit(hy + k, original)
                                                           : JewishDateEngine::resolveBirthdayOrAnniversary(hy + k, original);
            if (r.month == 0) { mappingRefused(ev, true); return false; }
            abs = JewishDateEngine::hebrewToAbsolute(r.year, r.month, r.day);
            year = hy + k;
            outAge = ev.jYear > 0 ? year - ev.jYear : -1;
        } else {
            if (!DateRules::validGregorian(0, ev.gMonth, ev.gDay)) return false;
            abs = DateRules::resolveGregorian(gy + k, ev.gMonth, ev.gDay);   // 29 Feb in other years: 1 Mar
            if (abs == DateRules::RESOLVE_NONE) { mappingRefused(ev, false); return false; }
            year = gy + k;
            outAge = ev.gYear > 0 ? year - ev.gYear : -1;
        }
        if (abs >= todayAbs) { outAbs = abs; return true; }
    }
    return false;
}

static void addDateSlides(std::vector<std::vector<OnScreenElement>>& slides, long abs, const AppConfig& cfg, const DisplayStrings& strings) {
    int y, m, d;
    JewishDateEngine::absoluteToGregorian(abs, y, m, d);
    slides.push_back({ textElement(formatGregorianDate(y, m, d, cfg, strings)) });
    GregorianDate g = { y, m, d, false };
    HebrewDate h = JewishDateEngine::calculateHebrew(g);
    slides.push_back({ textElement(Calendar::formatUserHebrewDate(JewishDateEngine::formatHebrewDate(h, cfg.showHebrewYear).c_str())) });
}

static std::vector<std::vector<OnScreenElement>> buildDemoSlides(const AppConfig& cfg, const DisplayStrings& strings) {
    std::vector<std::vector<OnScreenElement>> slides;
    auto text = [&](const String& s) { slides.push_back({ textElement(s) }); };
    long todayAbs = getTodayAbsolute();

    text("Demo");

    // Today, both calendars.
    if (todayAbs > 0) addDateSlides(slides, todayAbs, cfg, strings);

    // Shabbat. Times: this week's real ones when known, else typical.
    const DayInfo& civil = Calendar::getCivilToday();
    slides.push_back(iconAndText(builtinImage("Candles"), clockTime(civil.candleLightStr[0] ? civil.candleLightStr : "17:39", cfg), 90));
    slides.push_back(iconAndText(builtinImage("Havdala"), clockTime(civil.havdalahStr[0] ? civil.havdalahStr : "18:36", cfg)));
    slides.push_back(iconAndText(builtinImage("Torah"), " לך־לך"));
    text("מברכים חודש כסלו");

    // Holidays - Hebcal's own wording, as the clock shows it - including
    // Chol HaMoed days.
    for (const char* h : { "ראש השנה תשפ״ח", "יום כיפור", "סוכות א׳", "סוכות ג׳ (חוה״מ)", "שמיני עצרת",
                           "חנוכה: נר ג׳", "פורים", "פסח א׳", "פסח ד׳ (חוה״מ)", "יום העצמאות", "שבועות", "תשעה באב" }) text(h);
    // Special Shabbatot.
    for (const char* h : { "שבת שובה", "שבת שקלים", "שבת זכור", "שבת הגדול", "שבת חזון", "שבת נחמו" }) text(h);
    // A day with two holidays: each gets its own slide, one after the other.
    text("תענית אסתר");
    text("ערב פורים");
    // Rosh Chodesh, each with its date: both days of a two-day one (30
    // Tishrei / 1 Cheshvan - always two days), and a one-day one (1 Nisan -
    // Adar always has 29 days). The next ones from today.
    if (todayAbs > 0) {
        int ty, tm, td, hy, hm, hd;
        JewishDateEngine::absoluteToGregorian(todayAbs, ty, tm, td);
        JewishDateEngine::absoluteToHebrew(todayAbs, hy, hm, hd);
        auto nextHebrew = [&](int month, int day) {
            long a = JewishDateEngine::hebrewToAbsolute(hy, month, day);
            return a >= todayAbs ? a : JewishDateEngine::hebrewToAbsolute(hy + 1, month, day);
        };
        long cheshvan1 = nextHebrew(2, 1);
        long nisan1 = nextHebrew(7, 1);
        addDateSlides(slides, cheshvan1 - 1, cfg, strings); text(withRoshChodeshDay("ראש חודש חשוון", cheshvan1 - 1));   // 30 Tishrei
        addDateSlides(slides, cheshvan1, cfg, strings);     text(withRoshChodeshDay("ראש חודש חשוון", cheshvan1));       // 1 Cheshvan
        addDateSlides(slides, nisan1, cfg, strings);        text(withRoshChodeshDay("ראש חודש ניסן", nisan1));          // 1 Nisan, one day
    }

    // Fasts, with times as the clock words them.
    FastNotice f;
    f.kind = FastNotice::Kind::Fast;
    StringTools::utf8Copy(f.begin, "05:18"); StringTools::utf8Copy(f.end, "16:55");
    f.beginInDays = f.endInDays = 1;  text(formatFast(f, strings, cfg));                   // the day before
    f.beginInDays = f.endInDays = 0;  text(formatFast(f, strings, cfg));                   // the fast day
    StringTools::utf8Copy(f.begin, "19:29"); StringTools::utf8Copy(f.end, "19:56");
    f.endInDays = 1; text(formatFast(f, strings, cfg));                                   // Tisha B'Av, evening to evening
    FastNotice s; s.kind = FastNotice::Kind::SeudaMafseket; StringTools::utf8Copy(s.end, "17:54");
    text(formatFast(s, strings, cfg));                                                     // before Yom Kippur

    // Omer, in each wording the settings offer (day 23 = 3 weeks and 2 days,
    // so the weeks-and-days wording shows both parts; Full = Hebcal's text).
    const char* omerFull = "היום שלושה ועשרים יום, שהם שלושה שבועות ושני ימים לעומר";
    text(formatOmer(23, omerFull, OmerMode::ShortText, strings));
    text(formatOmer(23, omerFull, OmerMode::Abbreviated, strings));
    text(formatOmer(23, omerFull, OmerMode::Compact, strings));
    text(formatOmer(23, omerFull, OmerMode::FullText, strings));

    // Rain / dew prayer.
    text(String("משיב הרוח ") + strings.prayerSwitchFrom[1]);
    text(String(cfg.nusach == Nusach::Sefard ? "ברך עלינו " : "ותן טל ומטר ") + strings.prayerSwitchEvening[0]);
    const bool noDew = cfg.nusach == Nusach::Ashkenaz && !cfg.inIsrael;   // see Calendar::getPrayerSwitch()
    if (noDew) text(String("בלי משיב הרוח ") + strings.prayerSwitchFrom[0]);
    if (!noDew) text(String("מוריד הטל ") + strings.prayerSwitchFrom[0]);
    text(String(cfg.nusach == Nusach::Sefard ? "ברכנו " : "ותן ברכה ") + strings.prayerSwitchEvening[0]);

    // Daily learning.
    text(String(strings.dafYomi) + ": נדה מ׳");
    slides.push_back(iconAndText(builtinImage("Mishnah"), " כלים כ״ד:ט״ו-ט״ז"));

    // Personal events from the real list, each on its own next date (the
    // date slides, then the event): a Hebrew-dated birthday, a Gregorian-
    // dated one, a yahrzeit, a reminder days before, an event with both
    // dates - shown on each of them - and one with a photo.
    if (s_events && todayAbs > 0) {
        EventsLock::take();
        const PersonalEvent* hebBday = nullptr; const PersonalEvent* gregBday = nullptr;
        const PersonalEvent* yahrzeit = nullptr; const PersonalEvent* reminder = nullptr; const PersonalEvent* both = nullptr;
        for (const auto& ev : *s_events) {
            bool yz = strcmp(ev.type, "yahrzeit") == 0;
            if (ev.hasJewish && ev.hasGregorian) { if (!both) both = &ev; }
            else if (yz) { if (!yahrzeit) yahrzeit = &ev; }
            else if (ev.hasJewish) { if (!hebBday) hebBday = &ev; }
            else if (ev.hasGregorian) { if (!gregBday) gregBday = &ev; }
        }
        // The reminder sample: an event with reminder days, preferably not
        // one already shown above (in a typical list only yahrzeits have them).
        for (const auto& ev : *s_events) {
            if (ev.warningDays <= 0) continue;
            if (!reminder) reminder = &ev;
            if (&ev != yahrzeit && &ev != hebBday && &ev != gregBday && &ev != both) { reminder = &ev; break; }
        }
        auto showOn = [&](const PersonalEvent* ev, bool hebrew, int daysBefore) {
            long abs; int age;
            if (!ev || (hebrew ? !ev->hasJewish : !ev->hasGregorian)) return;
            if (!nextOccurrence(*ev, hebrew, todayAbs, abs, age)) return;
            addDateSlides(slides, abs - daysBefore, cfg, strings);
            std::vector<OnScreenElement> slide;
            appendEvent(slide, *ev, age, daysBefore, strings, hebrew);
            slides.push_back(slide);
        };
        showOn(hebBday, true, 0);
        showOn(gregBday, false, 0);
        showOn(yahrzeit, true, 0);
        if (reminder) showOn(reminder, reminder->hasJewish, reminder->warningDays);
        showOn(both, false, 0);   // on its Gregorian date ...
        showOn(both, true, 0);    // ... and on its Hebrew date
        for (const auto& ev : *s_events) {   // an event with a photo, if one has
            if (ev.image[0] && DisplayManager::hasImage(imagePath(ev.image).c_str())) { showOn(&ev, ev.hasJewish, 0); break; }
        }
        EventsLock::give();
    }
    // Two events on one day, without a year (no age) - and English names,
    // scrolling left-to-right.
    PersonalEvent example{};
    std::vector<OnScreenElement> twoEvents;
    StringTools::utf8Copy(example.icon, "🎂");
    StringTools::utf8Copy(example.name, "Example");
    appendEvent(twoEvents, example, -1, 0, strings);
    StringTools::utf8Copy(example.icon, "💍");
    StringTools::utf8Copy(example.name, "Another");
    appendEvent(twoEvents, example, -1, 0, strings);
    slides.push_back(twoEvents);
    // Every event icon the event form offers, as pictures.
    std::vector<OnScreenElement> gallery;
    for (const char* icon : { "🎂", "🕯️", "💍", "✡️", "🕍", "🎉", "👶", "🎓", "🏠", "🗓️" }) {
        OnScreenElement pic(OnScreenElementType::STATIC_IMAGE, eventIconPath(icon), FONT_H);
        pic.frame_delay_ms = 90;
        pic.glueToPrevious = !gallery.empty();
        gallery.push_back(pic);
    }
    slides.push_back(gallery);

    text("End of demo");
    return slides;
}

void MessageAggregator::requestDemo(bool on) { s_demoRequest = on ? 1 : 2; }
bool MessageAggregator::isDemoActive() { return s_demoActive; }

// Decides the next Alternating-mode batch: one pass through contentOrder,
// each type's frequency checked against s_batchCounter exactly once (this
// IS that once), keeping only the types that are due AND currently have
// something to show. s_batchCounter only advances here - once per batch,
// not on any wall-clock timer - so "every Nth round" means "every Nth
// batch", immune to drift between unrelated timers.
// The slides of round `counter`, in the configured order: each content type's parts that are
// due and have something to show, the status slide first when due. The one place a round is
// decided - the Alternating batch, the Single-scroll ticker and upcomingRounds() all use it.
static std::vector<BatchSlot> roundSlots(const AppConfig& cfg, const DayInfo& today, const DisplayStrings& strings, uint32_t counter) {
    std::vector<BatchSlot> slots;
    for (int i = 0; i < (int)ContentType::COUNT; i++) {
        ContentType type = (ContentType)cfg.contentOrder[i];
        for (size_t part = 0; part < partsOf(type, today); part++) {
            if (!buildContentType(type, part, cfg, today, strings, counter).empty()) slots.push_back({type, part});
        }
    }
    if (statusNoticeDue(counter)) slots.insert(slots.begin(), BatchSlot{ContentType::COUNT, 0});
    return slots;
}

static std::vector<OnScreenElement> slotElements(const BatchSlot& slot, const AppConfig& cfg, const DayInfo& today,
                                                 const DisplayStrings& strings, uint32_t counter) {
    return slot.type == ContentType::COUNT ? offlineNotice(strings) : buildContentType(slot.type, slot.part, cfg, today, strings, counter);
}

static void buildNewBatch(const AppConfig& cfg, const DayInfo& today, const DisplayStrings& strings) {
    s_batchCounter++;
    s_currentBatch = roundSlots(cfg, today, strings, s_batchCounter);
    s_batchIndex = 0;
    // Built because the order changed: don't open with the slide just shown.
    if (s_haveAvoid && s_currentBatch.size() > 1 &&
        s_currentBatch[0].type == s_avoidFirst.type && s_currentBatch[0].part == s_avoidFirst.part) s_batchIndex = 1;
    s_haveAvoid = false;
    CLOCK_LOG("Aggregator", "New batch #%u: %u items | freeHeap=%u maxAllocHeap=%u",
              (unsigned)s_batchCounter, (unsigned)s_currentBatch.size(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

void MessageAggregator::buildElements(const AppConfig& cfg) {
    const DayInfo& today = Calendar::getToday();
    const DisplayStrings& strings = getStrings(cfg.language);

    // Demo: its slides instead of the normal ones, in the same mode.
    if (s_demoActive) {
        if (cfg.rotationMode == RotationMode::SingleScroll) {
            // The demo's ticker starts over now, and its one lap counts from here - not from the
            // request: the screen took new content only when the running lap ended, the moment
            // that counted as the demo's lap, so it ended unseen; a request during the start-up
            // status line, before anything is built, the same (2026-11-18, sim/test_screen_edges.py).
            if (!s_demoOnScreen) {
                s_demoOnScreen = true;
                s_demoStartRound = s_roundCounter;
                s_forceRefresh = true;
            }
            if ((int32_t)(s_roundCounter - s_demoStartRound) >= 1) {
                s_demoActive = false;   // one full lap of the demo ticker shown
                CLOCK_LOG("Aggregator", "Demo finished");
            } else {
                g_elements.clear();
                for (const auto& slide : s_demoSlides) g_elements.insert(g_elements.end(), slide.begin(), slide.end());
                return;
            }
        } else {
            if (s_batchIndex < s_demoSlides.size()) {
                g_elements = s_demoSlides[s_batchIndex];
                return;
            }
            s_demoActive = false;       // last slide's dwell is over
            CLOCK_LOG("Aggregator", "Demo finished");
        }
        // Finished: fall through to the normal slides right away (loop()
        // resets the rotation on its next pass).
        s_currentBatch.clear();
        s_batchIndex = 0;
    }

    if (cfg.rotationMode == RotationMode::SingleScroll) {
        // Everything due this round, concatenated in the user's configured order.
        g_elements.clear();
        for (const BatchSlot& slot : roundSlots(cfg, today, strings, s_roundCounter)) {
            std::vector<OnScreenElement> items = slotElements(slot, cfg, today, strings, s_roundCounter);
            g_elements.insert(g_elements.end(), items.begin(), items.end());
        }
    } else { // Alternating
        // Play the current fixed batch, slot by slot. A slot's own content is
        // still re-rendered fresh here (so e.g. a live value updates while
        // on screen) - only the batch's MEMBERSHIP and ORDER are frozen once
        // decided. If a slot's data happens to have vanished since the batch
        // was built (rare - e.g. Havdalah's civil-day window closing
        // mid-batch), skip forward to the next slot rather than showing
        // nothing or stale content; this never changes what the batch
        // contains, just skips an already-decided slot that turned out
        // empty. Bounded so a pathological all-empty config can't spin forever.
        std::vector<OnScreenElement> current;
        int safety = 2 * ((int)ContentType::COUNT + 3) + 2;   // + 3: the extra holiday slides
        while (safety-- > 0) {
            if (s_currentBatch.empty() || s_batchIndex >= s_currentBatch.size()) {
                buildNewBatch(cfg, today, strings);
                if (s_currentBatch.empty()) break; // nothing due/available at all right now
            }
            current = slotElements(s_currentBatch[s_batchIndex], cfg, today, strings, s_batchCounter);
            if (!current.empty()) break;
            s_batchIndex++;
        }
        g_elements = (!s_currentBatch.empty() && s_batchIndex < s_currentBatch.size())
            ? current : std::vector<OnScreenElement>{};
        // The status slide: red when it says something is wrong (no internet, no time
        // server - the user, 2026-11-14), gold when it only gives the clock's Wi-Fi.
        // (Single-scroll: one ticker, one colour - not marked.)
        if (!g_elements.empty() && s_currentBatch[s_batchIndex].type == ContentType::COUNT)
            g_currentColor = (WiFiManager::offlineForMs() >= OFFLINE_NOTICE_AFTER_MS || ntpNoticeDue() || s_failedUpdate.length() || s_textsTooLong)
                             ? COLOR_RED : COLOR_GOLD;
    }

    // Fallback: If after all logic, g_elements is still empty, add the project name.
    if (g_elements.empty()) {
        String startMsg = StringTools::formatProjectName(cfg.projectName);
        g_elements.emplace_back(OnScreenElementType::TEXT, startMsg, _measureString(startMsg.c_str()));
    }
}

const std::vector<OnScreenElement>& MessageAggregator::getMessageElements() {
    return g_elements;
}

uint32_t MessageAggregator::getCurrentMessageId() {
    return g_messageId;
}

uint16_t MessageAggregator::getCurrentColor() {
    return g_currentColor;
}

// The next n rounds as this clock would show them now (2026-11-15): the same roundSlots() /
// slotElements() as the screen, from the clock's own settings, events and day - nothing
// changed by asking. Single-scroll: one slide per round (the ticker). For the tests: what a
// change on the page does to the clock, without watching rounds go by in real time.
std::vector<std::vector<std::vector<OnScreenElement>>> MessageAggregator::upcomingRounds(int n) {
    if (!s_loopCfg) return {};
    const AppConfig& cfg = *s_loopCfg;
    const DayInfo& today = Calendar::getToday();
    const DisplayStrings& strings = getStrings(cfg.language);
    bool single = cfg.rotationMode == RotationMode::SingleScroll;
    std::vector<std::vector<std::vector<OnScreenElement>>> rounds;
    for (int k = 1; k <= n; k++) {
        uint32_t counter = (single ? s_roundCounter : s_batchCounter) + k;
        std::vector<std::vector<OnScreenElement>> slides;
        for (const BatchSlot& slot : roundSlots(cfg, today, strings, counter)) {
            std::vector<OnScreenElement> els = slotElements(slot, cfg, today, strings, counter);
            if (els.empty()) continue;
            if (single && !slides.empty()) slides[0].insert(slides[0].end(), els.begin(), els.end());
            else slides.push_back(els);
        }
        if (slides.empty()) {   // as buildElements(): nothing due - the clock's name
            String name = StringTools::formatProjectName(cfg.projectName);
            slides.push_back({ OnScreenElement(OnScreenElementType::TEXT, name, _measureString(name.c_str())) });
        }
        rounds.push_back(slides);
    }
    return rounds;
}

uint32_t MessageAggregator::roundsDone() {
    return s_batchCounter + s_roundCounter;   // each counts in its own mode only
}
