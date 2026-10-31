// =============================================================================
// JewishClock.ino
// Main Arduino sketch — ESP32-S3 N16R8
//
// Hardware: VIEWE 480×128 NV3041A (SPI), BH1750 I2C, ESP32-S3
//
// Module init order:
//   1. Storage      — must be first (everything else reads config from it)
//   2. WebUI        — registers routes right after Storage, not last: WiFiManager's
//                     AP-mode captive portal (step 5) can start serving requests
//                     immediately, before Calendar/MessageAggregator exist yet. Route
//                     *registration* only needs cfg + events (both already loaded by
//                     step 1); the handlers themselves tolerate those modules not
//                     being ready yet (e.g. the "/" handler's own loading-page check).
//   3. Display      — early: shows boot splash; no args (uses Config.h defaults,
//                     then immediately overridden by loaded calibration)
//   4. BrightnessManager — inits AmbientLight sensor
//   5. WiFiManager  — STA connect or AP captive portal
//   6. NTPManager   — time sync (needed by Calendar and BrightnessManager)
//   7. Calendar     — loads LittleFS cache; fetches Hebcal when WiFi ready
//   8. MessageAggregator — wires Calendar + personal events into scroll queue
//
// Display API note (Display.h / Display.cpp):
//   begin()            — no args; driver + LEDC init using Config.h defaults
//   setWindow(x,y,w,h) — immediately override with user calibration from config
//   setBrightness(n)   — PWM level
//   setScrollSpeed(n)  — pixels per ~16 ms frame (added by us)
//   setMessage(s,col)  — sets scrolling text; resets scroll position
//   update()           — no args; advances scroll + redraws (call every loop)
// =============================================================================

#include <esp_task_wdt.h>
#include <esp_system.h>
#include "Config.h"
#include "Version.h"
#include "Storage.h"
#include "LogLevel.h"
#include "Display.h"
#include "WiFiManager.h"
#include "Location.h"
#include "NTPManager.h"
#include "Calendar.h"
#include "MessageAggregator.h"
#include "BrightnessManager.h"
#include "AmbientLight.h"
#include "DiagnosticLog.h"
#include "EventsLock.h"
#include "DisplayLock.h"
#include "NTPLock.h"
#include "WiFiLock.h"
#include "WebUI.h"
#include "Updater.h"
#include "ImageLibrary.h"
#include "OnScreenElement.h"
#include "lang.h"
#include "StringTools.h"
#include "Font.h"

// === TUNABLES ===  (rule: see Config.h header)

// Main-loop task watchdog (why it exists: see setup()). Sized generously
// above the worst legitimate blocking operation: HTTPClient's default
// connect+read timeout is 5000 ms each (Calendar.cpp's Hebcal/Zmanim fetches,
// GeolocationService's explicit 5000 ms), and TLS handshake overhead can
// stack on top - 20 s leaves comfortable margin without masking a genuine
// hang for long.
static const uint32_t WDT_TIMEOUT_SEC = 20;

// Boot overlay (AP name/address) shown for at least this long, so there's
// time to read it and connect to the clock's own AP - crucial on networks
// with client isolation, where devices can't see each other.
static const unsigned long BOOT_OVERLAY_MS = 3UL * 60UL * 1000UL;

// TIMED_STEP / loop total: log a step or loop pass slower than this.
// Display's own SPI blit is a ~54-65 ms baseline every pass, so anything
// below ~100 ms is that, not a finding.
static const uint32_t SLOW_STEP_US = 200000;

// Serial output buffer (see setup()). Drains at ~11.5 KB/s at 115200 baud;
// only a burst bigger than this makes logging block again.
static const size_t SERIAL_TX_BUFFER_BYTES = 4096;

// Periodic "[Trace] ... State" lines, even when nothing changed.
static const unsigned long TRACE_LOG_INTERVAL_MS = 5000;

// ─── Global state ─────────────────────────────────────────────────────────────

static AppConfig g_cfg;
static EventList g_events;

static uint32_t g_lastMessageId = 0;
static int  g_lastSpeed    = -1;
static int  g_lastCharGap  = -1;
static int  g_lastWordGap  = -1;
static int  g_lastWidthScale = -1;

// Boot sequence tracking
static unsigned long g_bootStartMs = 0;

// =============================================================================
// Public Functions
// =============================================================================

// Allows other modules to trigger a display refresh on the next loop.
// This function is now obsolete as MessageAggregator is self-aware.

// =============================================================================
// setup()
// =============================================================================

void setup() {
    // Without a TX buffer, every Serial.printf WAITS until its text has left
    // the chip - ~87 us per character at 115200 baud, whether or not anyone
    // is listening. Calendar::update()'s two once-a-second log lines alone
    // stalled the main loop 11 ms every second: a visible hitch in the
    // scrolling text (found 2026-10-17). With a buffer, printf just copies
    // and returns; the UART drains it in the background.
    Serial.setTxBufferSize(SERIAL_TX_BUFFER_BYTES);   // must precede begin()
    Serial.begin(115200);
    // Wait up to 3 seconds for the Serial Monitor to actually connect
    // This ensures you don't miss the first messages.
    // while (!Serial && millis() < 3000); // Removed to prevent S3 boot stalls when headless
    // delay(200);
    Serial.print("\n\n");
    CLOCK_LOG("System", "=== JewishClock %s ===", FIRMWARE_VERSION);

    // Last-resort self-recovery: this build already ships ESP-IDF's default
    // Task Watchdog (CONFIG_ESP_TASK_WDT=1, 5 s), but that one only watches
    // CPU0's idle task - it does NOT watch the Arduino loop() task (which
    // runs on CPU1) or CPU1's idle task. A loop()-level hang (e.g. a
    // networking call that never returns) would NOT trip it and the device
    // would simply sit dead indefinitely, as observed during the 2026-09-30
    // networking investigation. Explicitly subscribing the loop() task closes
    // that gap: if loop() (and everything it calls synchronously - WiFi/AP/
    // mDNS/HTTP lifecycle, Calendar's HTTPS fetches, etc.) ever fails to
    // return to the top of loop() within WDT_TIMEOUT_SEC, the watchdog
    // panics and reboots instead of leaving the device unreachable forever.
    // ESP-IDF 5 (core 3.x): the watchdog is already running at this point
    // (sdkconfig ESP_TASK_WDT_INIT), so it is reconfigured, not initialized;
    // same settings as the 2.x init call - our timeout, panic + reboot, and
    // CPU0's idle task still watched, as the default does.
    const esp_task_wdt_config_t wdtConfig = {
        .timeout_ms     = WDT_TIMEOUT_SEC * 1000U,
        .idle_core_mask = (1U << 0),
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&wdtConfig);
    esp_task_wdt_add(NULL); // NULL = current task = Arduino's setup()/loop() task
    CLOCK_LOG("System", "Task watchdog armed: %u s timeout on main loop task", (unsigned)WDT_TIMEOUT_SEC);

    // 1. Storage: Must be first, as all other modules depend on the config it loads.
    // LittleFS already formats itself if it can't mount, so failing here
    // means the flash itself is in trouble. Not a halt: the wait below never
    // feeds the task watchdog armed above, so it reboots the clock after
    // WDT_TIMEOUT_SEC and the mount is tried again - every boot, until it works.
    if (!Storage::begin()) {
        CLOCK_LOG("Storage", "[FATAL] Init failed - watchdog will reboot in %u s and retry", (unsigned)WDT_TIMEOUT_SEC);
        while (true) delay(1000);
    }
    // The persistent log first, so the loads below can record their failures in it.
    DiagnosticLog::begin();
    DiagnosticLog::record("BOOT", "firmware=%s reset_reason=%d", FIRMWARE_VERSION, (int)esp_reset_reason());
    Storage::loadConfig(g_cfg);

    // As early as possible - before anything below has a chance to log -
    // so a persisted "debug" survives being set, flashed via uploadfs
    // (which restarts the device), and read back here on the very next
    // boot, rather than only taking effect once the Settings UI is reachable.
    Logging::setLevel(Logging::parseLevelName(g_cfg.logLevel, LogLevel::INFO));

    // The debug time offset is now fully managed via the Web UI.
    NTPManager::setDebugOffset(g_cfg.debugTimeOffset); // Apply offset from loaded config
    Storage::loadEvents(g_events);
    // Event pictures: the old per-event photos into the shared library (once),
    // and pictures no event uses any more (e.g. after an Excel import) away.
    if (ImageLibrary::migrateOldPhotos(g_events)) Storage::saveEvents(g_events);
    ImageLibrary::removeUnused(g_events);
    EventsLock::begin(); // Before WebUI::begin()/MessageAggregator::begin() below hand out any reference to g_events.
    CLOCK_LOG("System", "Config loaded. Project: %s, SSID: %s", g_cfg.projectName, g_cfg.wifiSSID[0] ? g_cfg.wifiSSID : "Not Set"); // Keep this line

    // 2. User Interface: registers routes now, right after Storage, rather than
    //    last - WiFiManager's AP-mode captive portal (step 4 below) can start
    //    serving requests immediately on its own, before Calendar/MessageAggregator
    //    exist yet (see WebUI::startServer()'s comment). Route *registration* only
    //    needs cfg + events, both already loaded above; the handlers themselves
    //    tolerate the rest of the system not being ready yet (e.g. the "/" route's
    //    own loading-page check).
    DisplayLock::begin(); // Before WebUI::begin() registers handlers that touch DisplayManager's shared state.
    NTPLock::begin();     // Same reason - web handlers read the time (NTPManager) directly.
    WebUI::begin(g_cfg, g_events);

    // 3. Display Hardware: Initializes the screen and backlight control pins.
    if (!DisplayManager::begin()) {
        // This is a non-fatal error. The device can still be managed via the Web UI.
        CLOCK_LOG("Display", "[ERROR] Init failed. Continuing without display.");
    }
    DisplayManager::setWindow(g_cfg.calibX, g_cfg.calibY, g_cfg.calibW, g_cfg.calibH);
    DisplayManager::setTearUs(g_cfg.calibTearUs);
    // All images into memory now (needs the window height), not on first
    // show - reading the candles animation from flash stalled the screen 1.1 s.
    DisplayManager::preloadImages();

    // 4. Brightness Manager: Depends on DisplayManager to control the backlight.
    //    It also initializes the AmbientLight sensor internally.
    if (!BrightnessManager::begin(g_cfg)) {
        // Non-fatal: manual brightness is unaffected; automatic brightness
        // runs on AmbientLight's simulated value (logged there).
        CLOCK_LOG("Brightness", "[WARN] Light sensor not found - automatic brightness uses a simulated reading.");
    }

    // Initial brightness now that the managers are ready.
    BrightnessManager::loop(g_cfg);
    DisplayManager::setScrollSpeed(g_cfg.scrollSpeed);
    fontSetSpacing((uint8_t)g_cfg.fontCharGapPx, (uint8_t)g_cfg.fontWordGapPx);
    fontSetWidthScale((uint8_t)g_cfg.fontWidthScalePct);
    g_lastCharGap = g_cfg.fontCharGapPx;
    g_lastWordGap = g_cfg.fontWordGapPx;
    g_lastWidthScale = g_cfg.fontWidthScalePct;
    CLOCK_LOG("main", "Display ready. Brightness %.1f%% (duty %u)", BrightnessManager::currentPct(), (unsigned)BrightnessManager::currentDuty());

    // Show boot splash while WiFi connects
    DisplayManager::setMessage(StringTools::formatProjectName(g_cfg.projectName).c_str(), COLOR_GOLD, g_cfg, (strcmp(g_cfg.language, "he") == 0));

    // 5. Networking & Time
    WiFiLock::begin(); // Before WiFiManager::begin() - its internal state machine can race against the AP-portal's /save handler as soon as the AP starts serving.
    WiFiManager::begin(g_cfg);
    NTPManager::begin(
        g_cfg.ntpServerCount > 0 ? g_cfg.ntpServers[0] : NTP_SERVER_1,
        g_cfg.tzPOSIX
    );

    // 6. Calendar & Events
    Calendar::begin(g_cfg);
    MessageAggregator::begin(g_cfg, g_events);

    // Newly installed firmware runs "on trial" and confirms itself only once
    // it has joined Wi-Fi and stayed up - otherwise the clock goes back to the
    // previous firmware by itself. Replaces confirming at the end of setup(),
    // which only caught crashes during setup, not e.g. an update that can't
    // get online (= can never receive the fix). See Updater.h.
    Updater::begin();

    // Record boot time for boot message display
    g_bootStartMs = millis();

    CLOCK_LOG("main", "Setup complete — entering loop");
}

// =============================================================================
// loop()
// =============================================================================

// Helper macro for state-change-driven logging
#define LOG_IF_CHANGED(buffer, doLog, fmt, ...) \
    do { \
        char newMsg[128]; \
        snprintf(newMsg, sizeof(newMsg), fmt, ##__VA_ARGS__); \
        if (strcmp(buffer, newMsg) != 0 || doLog) { \
            CLOCK_LOG("main", "[Trace] %s", newMsg); \
            strncpy(buffer, newMsg, sizeof(buffer)); \
        } \
    } while (0)

void loop() {
    // Feed the task watchdog armed in setup() - must happen once per loop()
    // iteration regardless of which branch below runs, so it stays right at
    // the top, unconditionally, before anything that could itself stall.
    esp_task_wdt_reset();

    // This static variable is initialized only ONCE. On subsequent loops, it retains its value.
    static unsigned long lastLogMs = 0 - TRACE_LOG_INTERVAL_MS - 1; // Initialize to a value that ensures the first log runs immediately.
    unsigned long now = millis();
    // This check provides a periodic "heartbeat" log.
    bool doLog = (now - lastLogMs > TRACE_LOG_INTERVAL_MS);

    if (doLog) lastLogMs = now;

    // --- Permanent diagnostic (CLAUDE.md, "Never run a blocking call"): logs
    // any loop() step slow enough to stall the screen or the web server.
    // Removed once and the bug it catches came back - keep it. First added
    // in the 2026-09-30 investigation (commit 33bd2f7). Threshold: SLOW_STEP_US.
    uint32_t __loopStartUs = micros();
    #define TIMED_STEP(label, code) do { \
        uint32_t __stepStartUs = micros(); \
        code; \
        uint32_t __stepUs = micros() - __stepStartUs; \
        if (__stepUs > SLOW_STEP_US) DiagnosticLog::record("Timing", "SLOW STEP: %s took %u us", label, __stepUs); \
    } while (0)

    // 1. Non-blocking NTP step/slew poll
    TIMED_STEP("NTPManager::loop", NTPManager::loop());

    // 2. Wi-Fi connection state machine (reconnects, connect timeouts)
    TIMED_STEP("WiFiManager::loop", WiFiManager::loop());

    // 2b. The place follows the network joined (Location.h)
    TIMED_STEP("Location::loop", Location::loop(g_cfg));

    // 3. Boot sequence: show AP setup info for BOOT_OVERLAY_MS, or until time is known.
    bool hasTime = NTPManager::isSynced() || Calendar::getToday().hYear != 0;
    bool showOverlay = (now - g_bootStartMs < BOOT_OVERLAY_MS) || (!hasTime && WiFiManager::isApActive());

    static char lastLoopState[128] = "";
    LOG_IF_CHANGED(lastLoopState, doLog, "Loop State: showOverlay=%s, hasTime=%s, isApActive=%s", showOverlay ? "true" : "false", hasTime ? "true" : "false", WiFiManager::isApActive() ? "true" : "false");

    // 4. Calendar: check manifest + handle transition engine
    TIMED_STEP("Calendar::update", Calendar::update(g_cfg, g_events));

    // 4b. Font spacing/width: apply live changes (e.g. from the Web UI)
    // immediately, instead of waiting for the currently-shown text to
    // naturally change.
    if (g_cfg.fontCharGapPx != g_lastCharGap || g_cfg.fontWordGapPx != g_lastWordGap) {
        fontSetSpacing((uint8_t)g_cfg.fontCharGapPx, (uint8_t)g_cfg.fontWordGapPx);
        g_lastCharGap = g_cfg.fontCharGapPx;
        g_lastWordGap = g_cfg.fontWordGapPx;
        MessageAggregator::forceRefresh();
    }
    if (g_cfg.fontWidthScalePct != g_lastWidthScale) {
        fontSetWidthScale((uint8_t)g_cfg.fontWidthScalePct);
        g_lastWidthScale = g_cfg.fontWidthScalePct;
        MessageAggregator::forceRefresh();
    }

    // 5. Message dwell timer / rotation
    TIMED_STEP("MessageAggregator::loop", MessageAggregator::loop(g_cfg, showOverlay));

    // 6. Brightness: sensor / manual → PWM
    TIMED_STEP("BrightnessManager::loop", BrightnessManager::loop(g_cfg));

    // 7. Scroll speed (only when it changes)
    int speed = g_cfg.scrollSpeed;
    if (speed != g_lastSpeed) {
        DisplayManager::setScrollSpeed(speed);
        g_lastSpeed = speed;
    }

    // 8. Push message to display only when content changes
    // (setMessage() resets the scroll position, so avoid calling it every frame)
    const std::vector<OnScreenElement>& elements = MessageAggregator::getMessageElements();
    uint32_t msgId = MessageAggregator::getCurrentMessageId();

    static char lastAggState[128] = "";
    LOG_IF_CHANGED(lastAggState, doLog, "Aggregator State: elements=%u, msgId=%u, lastId=%u", (unsigned)elements.size(), (unsigned)msgId, (unsigned)g_lastMessageId);

    if (msgId != g_lastMessageId) {
        uint16_t color = MessageAggregator::getCurrentColor();
        CLOCK_LOG("main", "New message ID. Sending %d elements to DisplayManager:", elements.size());
        for(size_t i = 0; i < elements.size(); ++i) {
            const auto& p = elements[i];
            CLOCK_LOG("main", "  - Element %d: type=%d, content='%s'", i, (int)p.type, p.content.c_str());
        }
        DisplayManager::setMessage(elements, color, g_cfg);
        g_lastMessageId = msgId;
    }

    // 8b. While an update is written to flash, a still "Updating..." screen
    // instead of a moving one, which flickers then (see Display::showStill).
    // English in every language: the clock's own system messages are
    // English (decided 2026-10-18), only calendar content is Hebrew.
    static bool s_updateScreen = false;
    bool uploading = Updater::uploadInProgress();
    if (uploading != s_updateScreen) {
        s_updateScreen = uploading;
        if (uploading) DisplayManager::showStill("Updating...", false);
        else           DisplayManager::endStill();
    }

    // 9. Advance scroll, redraw (called every loop; internally rate-limited to ~16ms)
    TIMED_STEP("DisplayManager::update", DisplayManager::update(g_cfg));

    // 10. AsyncWebServer is ISR-driven — this is a no-op heartbeat
    TIMED_STEP("WebUI::loop", WebUI::loop());

    // 11. Firmware trial (confirm / roll back) and installing waiting web files
    TIMED_STEP("Updater::loop", Updater::loop());

    // 12. Waiting log lines to their file, in a batch (DiagnosticLog.h)
    TIMED_STEP("DiagnosticLog::loop", DiagnosticLog::loop());
    TIMED_STEP("Storage::loop", Storage::loop(g_cfg));

    // --- Permanent diagnostic: total loop() time, same SLOW_STEP_US threshold.
    uint32_t __loopTotalUs = micros() - __loopStartUs;
    if (__loopTotalUs > SLOW_STEP_US) DiagnosticLog::record("Timing", "SLOW LOOP: total %u us", __loopTotalUs);
    #undef TIMED_STEP
}
