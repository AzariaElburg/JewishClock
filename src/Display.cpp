#include "Tunables.h"
#include <Arduino.h>
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "Display.h"
#include "Uptime.h"
#include "DisplayLock.h"
#include "Config.h"
#include "Font.h"
#include "OnScreenElement.h"
#include "StringTools.h"
#include "WiFiManager.h"
#include "NTPManager.h"
#include "DiagnosticLog.h"
#include "Texts.h"
#include <Arduino_GFX_Library.h>
#include <LittleFS.h>
#include <atomic>
#include "esp32-hal-spi.h"     // HSPI

// The SPI bus index the display is on. On the ESP32-S3, HSPI == 1 == its SPI3
// controller (GPSPI3); FSPI == 0 == SPI2. (Classic ESP32 numbers these
// differently - an old comment here called 1 "FSPI".)
static const uint8_t TFT_SPI_BUS = HSPI;

// Live display-link test (freeze) - see Display.h. Written by any task, read
// by update() on the main loop.
static std::atomic<bool>     s_frozen{false};
static std::atomic<uint32_t> s_lastBlitUs{0};
// Hitches: gaps between two finished frames longer than FRAME_HITCH_US /
// FRAME_BIG_HITCH_US, counted since boot (/api/status). s_prevFrameEndUs = 0:
// no frame before (startup, frozen, still) - the next gap isn't counted.
static std::atomic<uint32_t> s_hitches{0}, s_bigHitches{0};
// Calibration mode: test pattern (true) or live content (false) - see updateCalibration().
static std::atomic<bool> s_calibPattern{false};
// Each window frame starts this long after the panel's TE pulse - the saved
// "Tear lines" calibration (Config.h DEFAULT_CALIB_TEAR_US). See waitForTe().
static std::atomic<int32_t> s_tearUs{DEFAULT_CALIB_TEAR_US};
static int64_t  s_prevFrameEndUs = 0;

// === TUNABLES ===  (rule: see Config.h header)
static const uint32_t DISPLAY_RETRY_MS = 60000UL;
// A normal frame takes ~40-65 ms (drawing + SPI blit); a gap above this is a
// hitch the eye can see in scrolling text, above the second a clear jump.
static const uint32_t FRAME_HITCH_US     = 100000UL;
static const uint32_t FRAME_BIG_HITCH_US = 250000UL;
// Pictures taller than this are shrunk to it (and never more than the window).
// Tested by eye 2026-10-26 (85-row window, tear lines at 12 ms): up to 68
// rows they stay clear of the tear lines (69: bottom row torn, 70: top too) -
// but at 68 they're too small; the user chose the full window (TFT_HEIGHT =
// no limit): nicer pictures, their top/bottom edge may tear.
static const int      PICTURE_MAX_H_ROWS = TFT_HEIGHT;
// A write the clock does by itself waits for a still screen at most this long
// (goodMomentToWrite()) - a waiting log line is lost if the power goes off
// meanwhile.
static const uint32_t FLASH_WRITE_MAX_WAIT_MS = 60000UL;
TUNABLE_SHARED(FLASH_WRITE_MAX_WAIT_MS)   // tests wait this long for a write that waits for a still screen
// Single-scroll mode never stands still: there a write waits only this long,
// so a burst of them (a reconnect's log lines, settings) goes as one write.
static const uint32_t FLASH_WRITE_GATHER_MS = 2000UL;
// ... and the screen counts as still only after standing this long (a frame
// or two between moving items isn't a moment to write in - seen 2026-10-30).
static const uint32_t STILL_SETTLE_MS = 300UL;
// Text: the letters and digits (FONT_LETTER_H rows on the baseline) are
// centred in the window, then moved down this much - chosen by the user from
// previews 2026-11-04 (85-row window: 20 rows empty above them, 17 below).
static const int      TEXT_BELOW_MIDDLE_PX = 2;

// Top row of the text's character cell in a window (y, h) - see TEXT_BELOW_MIDDLE_PX.
static int textTopY(int winY, int winH) {
    return winY + (winH + FONT_LETTER_H) / 2 + TEXT_BELOW_MIDDLE_PX - FONT_BASELINE;
}

static void onTe();   // the panel's TE pulse - see keepTeOn(); IRAM_ATTR on the definition only (each use is a new section: two clashed)

/**
 * @brief Draws a standardized error icon on the canvas.
 * This indicates a problem loading or rendering an image.
 * @param canvas The canvas to draw on.
 * @param x The top-left x-coordinate.
 * @param y The top-left y-coordinate.
 * @param w The width of the icon.
 * @param h The height of the icon.
 * @param winX The x-coordinate of the clipping window.
 * @param winY The y-coordinate of the clipping window.
 * @param winW The width of the clipping window.
 * @param winH The height of the clipping window.
 */
static void drawErrorIcon(Arduino_GFX* canvas, int x, int y, int w, int h, int winX, int winY, int winW, int winH) {
    if (!canvas) return;

    // Clip icon to the display window
    int iconX = x, iconY = y, iconW = w, iconH = h;
    if (iconX < winX) { iconW -= (winX - iconX); iconX = winX; }
    if (iconY < winY) { iconH -= (winY - iconY); iconY = winY; }
    if (iconX + iconW > winX + winW) iconW = (winX + winW) - iconX;
    if (iconY + iconH > winY + winH) iconH = (winY + winH) - iconY;
    if (iconW <= 2 || iconH <= 2) return;

    canvas->fillRect(iconX, iconY, iconW, iconH, COLOR_DARKGREY);
    canvas->drawRect(iconX, iconY, iconW, iconH, COLOR_RED);
    canvas->drawLine(iconX + 1, iconY + 1, iconX + iconW - 2, iconY + iconH - 2, COLOR_RED);
    canvas->drawLine(iconX + 1, iconY + iconH - 2, iconX + iconW - 2, iconY + 1, COLOR_RED);
}

/**
 * @brief Reads and validates a BMP file's header, determining its format.
 * @param bmpFile An open File object for the BMP.
 * @param width Output parameter for the image width.
 * @param height Output parameter for the image height.
 * @param max_height The maximum allowed height for the image.
 * @param format Output parameter for the detected BMP format (565 or 555).
 * @return true if the header is valid, false otherwise.
 */
static bool readBmpHeader(File& bmpFile, int32_t& width, int32_t& height, int max_height, DisplayManager::BmpFormat& format) {
    format = DisplayManager::BMP_FMT_UNKNOWN;
    if (!bmpFile) return false;
    bmpFile.seek(0);

    if (bmpFile.read() != 'B' || bmpFile.read() != 'M') {
        CLOCK_LOG("Display", "ERROR: '%s' is not a valid BMP (bad magic number).", bmpFile.name());
        return false;
    }

    bmpFile.seek(18); // Seek to width/height
    if (bmpFile.read((uint8_t*)&width, 4) != 4) return false;
    if (bmpFile.read((uint8_t*)&height, 4) != 4) return false;
    height = abs(height);

    bmpFile.seek(28); // Seek to BPP
    uint16_t bpp = 0;
    if (bmpFile.read((uint8_t*)&bpp, 2) != 2 || bpp != 16) {
        CLOCK_LOGF("Display", Texts::bmpNot16Bit, bmpFile.name());
        return false;
    }

    uint32_t compression = 0;
    if (bmpFile.read((uint8_t*)&compression, 4) != 4) return false;

    if (compression == 0) { // BI_RGB
        // While the spec implies 555, 565 is a common de-facto standard for BI_RGB.
        // We will assume 565 here and let BI_BITFIELDS handle explicit 555.
        format = DisplayManager::BMP_FMT_565;
    } else if (compression == 3) { // BI_BITFIELDS
        uint32_t red_mask, green_mask, blue_mask;
        bmpFile.seek(54); // Standard offset for masks after BITMAPINFOHEADER
        if (bmpFile.read((uint8_t*)&red_mask, 4) != 4) return false;
        if (bmpFile.read((uint8_t*)&green_mask, 4) != 4) return false;
        if (bmpFile.read((uint8_t*)&blue_mask, 4) != 4) return false;

        if (red_mask == 0xF800 && green_mask == 0x07E0 && blue_mask == 0x001F) {
            format = DisplayManager::BMP_FMT_565; // Explicit RGB565
        } else if (red_mask == 0x7C00 && green_mask == 0x03E0 && blue_mask == 0x001F) {
            format = DisplayManager::BMP_FMT_555; // Explicit xRGB1555
        } else {
            CLOCK_LOGF("Display", Texts::bmpMasks, bmpFile.name(), (unsigned)red_mask, (unsigned)green_mask, (unsigned)blue_mask);
            return false;
        }
    } else {
        CLOCK_LOGF("Display", Texts::bmpCompression, bmpFile.name(), (int)compression);
        return false;
    }

    if (height <= 0 || height > max_height) {
        CLOCK_LOGF("Display", Texts::bmpHeight, bmpFile.name(), (int)height, (int)max_height);
        return false;
    }
    return true;
}

// =============================================================================
// Static members
// =============================================================================

Arduino_ESP32SPI* DisplayManager::_bus  = nullptr;

// Deletes the bus through its exact, concrete type - always what `new`
// created, never a subclass - so this is well-defined even though the GFX
// library gives the class no virtual destructor. GCC still warns on any
// delete of such a class; silenced here only, deliberately.
static void deleteBus(Arduino_ESP32SPI*& bus) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdelete-non-virtual-dtor"
    delete bus;
#pragma GCC diagnostic pop
    bus = nullptr;
}
Arduino_GFX*      DisplayManager::_gfx  = nullptr;
Arduino_GFX*      DisplayManager::_canvas = nullptr;
DisplayMode       DisplayManager::_mode = DisplayMode::Blank;
bool              DisplayManager::_isInitialized = false;
bool              DisplayManager::_displayReal = false;
int64_t           DisplayManager::_lastInitAttemptMs = 0;

int DisplayManager::_winX = DEFAULT_CALIB_X;
int DisplayManager::_winY = DEFAULT_CALIB_Y;
int DisplayManager::_winW = DEFAULT_CALIB_W;
int DisplayManager::_winH = DEFAULT_CALIB_H;

DisplayManager::DisplayState       DisplayManager::_displayState = DisplayState::STATIC;
DisplayManager::AltScrollPhase     DisplayManager::_altScrollPhase = AltScrollPhase::DONE;
int64_t                            DisplayManager::_altItemStartMs = 0;
int64_t                            DisplayManager::_altCycleStartMs = 0;
int64_t                            DisplayManager::_altRevealDoneAtMs = 0;
bool                                DisplayManager::_altCheckpointArmed = false;
bool                                DisplayManager::_singleScrollLapCompleted = false;
std::vector<OnScreenElement> DisplayManager::_currentElements;
std::vector<OnScreenElement> DisplayManager::_nextElements;
int                          DisplayManager::_currentMessagePixelWidth = 0;
int                          DisplayManager::_nextMessagePixelWidth = 0;
bool                         DisplayManager::_isCurrentRtl = false;
bool                         DisplayManager::_isNextRtl = false;
bool                         DisplayManager::_hasNewMessage = false;
std::unordered_map<String, DisplayManager::CachedBmp, StringHash> DisplayManager::_bmpPixelCache;

uint16_t                     DisplayManager::_messageColor = COLOR_TEXT_NORMAL;
int                          DisplayManager::_scrollX = 0;
int                          DisplayManager::_scrollSpeed = SCROLL_SPEED_DEFAULT;
int64_t                      DisplayManager::_lastScrollMs = 0;

int      DisplayManager::_calibBorderPhase = 0;
int64_t  DisplayManager::_lastCalibMs = 0;

// =============================================================================
// Public API
// =============================================================================

void     DisplayManager::setFrozen(bool frozen)    { s_frozen = frozen; }
bool     DisplayManager::isFrozen()                { return s_frozen; }

// Still message (see Display.h): set on the main loop, drawn by update().
static bool   s_still = false;        // showing the still message: no other drawing
static bool   s_stillDrawn = false;   // drawn once already
static bool   s_stillRtl = false;
static String s_stillText;
void DisplayManager::showStill(const char* utf8text, bool isRtl) {
    s_stillText = utf8text; s_stillRtl = isRtl;
    s_still = true; s_stillDrawn = false;
}
void DisplayManager::endStill() { s_still = false; }   // every normal frame redraws the whole window

void DisplayManager::preloadImages() {
    for (const char* folder : { "/IMG", FS_IMAGE_DIR, FS_CUSTOM_DIR }) {
        File dir = LittleFS.open(folder);
        if (!dir || !dir.isDirectory()) continue;
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            String path = f.path();
            f.close();
            if (!path.endsWith(".bmp") && !path.endsWith(".BMP")) continue;
            if (!isImageUsable(path.c_str())) continue;
            // Shrunk for the window now too, not when first shown (a hitch each).
            DisplayLock::Guard guard;
            getShownBmp(path.c_str());
        }
    }
}

// Read without the lock (loadBmp()), stored with it - the screen never waits
// for the file. A copy loaded meanwhile by the screen itself wins (emplace
// keeps the first); the same file either way.
bool DisplayManager::isImageUsable(const char* path) {
    {
        DisplayLock::Guard guard;
        auto it = _bmpPixelCache.find(String(path));
        if (it != _bmpPixelCache.end()) return it->second.valid;
    }
    CachedBmp loaded = loadBmp(path, TFT_HEIGHT);
    DisplayLock::Guard guard;
    return _bmpPixelCache.emplace(String(path), loaded).first->second.valid;
}

bool DisplayManager::hasImage(const char* path) {
    DisplayLock::Guard guard;
    auto it = _bmpPixelCache.find(String(path));
    return it != _bmpPixelCache.end() && it->second.valid;
}

void DisplayManager::forgetImage(const char* path) {
    DisplayLock::Guard guard;
    _bmpPixelCache.erase(String(path));   // the pixels go when nothing uses them any more (CachedBmp::data)
}

bool DisplayManager::imageFile(const char* path, ImageFile& out) {
    DisplayLock::Guard guard;
    auto it = _bmpPixelCache.find(String(path));
    if (it == _bmpPixelCache.end() || !it->second.valid || !it->second.fileHeader) return false;
    const CachedBmp& b = it->second;
    out.pixels = b.data;
    out.header = b.fileHeader;
    out.width = b.width; out.height = b.height;
    out.stride = (((uint32_t)b.width * 2) + 3) & ~3u;
    out.size = b.fileHeader->size() + (size_t)out.stride * b.height;
    return true;
}

std::vector<DisplayManager::ImageEntry> DisplayManager::imagesIn(const char* folder) {
    DisplayLock::Guard guard;
    std::vector<ImageEntry> out;
    for (const auto& kv : _bmpPixelCache) {
        const CachedBmp& b = kv.second;
        if (!b.valid || !kv.first.startsWith(folder)) continue;
        out.push_back({ kv.first, b.width / b.frameCount, b.height, b.frameCount, b.frameMs, b.fileSize });
    }
    return out;
}

size_t DisplayManager::ImageFile::read(uint8_t* buf, size_t maxLen, size_t index) const {
    const size_t hdr = header->size(), rowBytes = (size_t)width * 2;
    size_t n = 0;
    while (n < maxLen && index < size) {
        size_t k;
        if (index < hdr) {
            k = std::min(maxLen - n, hdr - index);
            memcpy(buf + n, header->data() + index, k);
        } else {
            // The file's rows go bottom to top; memory's top to bottom. A
            // row's padding (odd widths) is zeros - checked when loaded.
            size_t off = index - hdr, fileRow = off / stride, inRow = off % stride;
            k = std::min(maxLen - n, (size_t)stride - inRow);
            if (inRow < rowBytes) {
                k = std::min(k, rowBytes - inRow);
                memcpy(buf + n, (const uint8_t*)(pixels.get() + (size_t)(height - 1 - fileRow) * width) + inRow, k);
            } else {
                memset(buf + n, 0, k);
            }
        }
        n += k; index += k;
    }
    return n;
}
uint32_t DisplayManager::getLastBlitUs()           { return s_lastBlitUs; }
uint32_t DisplayManager::getHitches(bool big)      { return big ? s_bigHitches : s_hitches; }

bool DisplayManager::begin() {
    CLOCK_LOG("Display", "-> begin()");
    int64_t now = Uptime::ms();
    if (_isInitialized) return true;
    if (_lastInitAttemptMs && now - _lastInitAttemptMs < DISPLAY_RETRY_MS) {
        return _displayReal;
    }
    _lastInitAttemptMs = now;

    // -------------------------------------------------------------------------
    // Backlight PWM Initialization (must be done BEFORE _gfx->begin())
    // -------------------------------------------------------------------------
    // The NV3041A driver may depend on the backlight pin being configured.
    // We set it up first to ensure the hardware is in a known state.

    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = (ledc_timer_bit_t)BACKLIGHT_PWM_BITS,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = BACKLIGHT_PWM_HZ,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .gpio_num       = TFT_BL,
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .timer_sel      = LEDC_TIMER_0,
        .duty           = 0, // Start with backlight off
        .hpoint         = 0,
        .flags          = {}
    };
    ledc_channel_config(&ledc_channel);

    if (_gfx) { delete _gfx; _gfx = nullptr; }
    if (_bus) deleteBus(_bus);
    if (_canvas) { delete _canvas; _canvas = nullptr; }

    _bus = new Arduino_ESP32SPI(
        TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI,
        GFX_NOT_DEFINED,
        TFT_SPI_BUS
    );

    _gfx = new Arduino_NV3041A(_bus, TFT_RST, 0, true);

    // Speed and its history: see TFT_SPI_HZ in Config.h.
    bool ok = _gfx->begin(TFT_SPI_HZ);
    if (!ok) {
        if (_gfx) { delete _gfx; _gfx = nullptr; }
        if (_bus) deleteBus(_bus);
        _isInitialized = false;
        _displayReal = false;
        CLOCK_LOG("Display", "tried display HW. FAILED so Display is SIMULATED.");
        return false;
    }

    // === HARDWARE TEST ===
    // Directly fill the screen with colors to verify basic hardware communication.
    _gfx->fillScreen(COLOR_RED);
    delay(500);
    _gfx->fillScreen(COLOR_GREEN);
    delay(500);
    _gfx->fillScreen(COLOR_BLUE);
    delay(500);
    // Black again: with window-only frames (sendFrame()) the rest of the
    // panel keeps whatever it got last - it stayed blue.
    _gfx->fillScreen(COLOR_BACKGROUND);

    // TE pulses (keepTeOn() switches them on, between frames). Pull-down: a
    // pad that isn't driving would otherwise pass noise off as pulses.
    pinMode(TFT_TE, INPUT_PULLDOWN);
    attachInterrupt(TFT_TE, onTe, RISING);

    // Create a canvas for double-buffering to prevent flicker
    // The canvas MUST be initialized before use to allocate its framebuffer (that was the source of
    // the StoreProhibited crash). Its ~120 KB framebuffer is what can fail (new itself aborts rather
    // than return null): a canvas without one would crash the first frame, at every start - so it
    // counts as a failed start, tried again in DISPLAY_RETRY_MS like any other (2026-11-18 review).
    _canvas = new Arduino_Canvas(TFT_WIDTH, TFT_HEIGHT, _gfx); // Use the correct canvas for Arduino_GFX
    if (!_canvas->begin() || !((Arduino_Canvas*)_canvas)->getFramebuffer()) {
        DiagnosticLog::record("Display", "no memory for the screen's framebuffer - display start failed, tried again later");
        delete _canvas; _canvas = nullptr;
        delete _gfx; _gfx = nullptr;
        if (_bus) deleteBus(_bus);
        _isInitialized = false;
        _displayReal = false;
        return false;
    }

    _isInitialized = true;
    _displayReal = true;

    setBrightness(0);

    clearFull();

    _mode = DisplayMode::Normal;

    return true;
}

void DisplayManager::setBrightness(uint32_t duty) {
    if (!_isInitialized) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void DisplayManager::setTearUs(int us) { s_tearUs = us; }

void DisplayManager::setWindow(int x, int y, int w, int h) {
    DisplayLock::Guard guard;
    _winX = x; _winY = y; _winW = w; _winH = h;
    // This function is safe to call even if not initialized,
    // but we log it to avoid confusion during debugging.
}

void DisplayManager::clearWindow() {
    if (_canvas) {
        _canvas->fillRect(_winX, _winY, _winW, _winH, COLOR_BACKGROUND);
    }
}

// ── Frames timed by the panel's TE pulse (Config.h TFT_TE) ────────────────────
// The panel redraws itself every ~15.5 ms on its own clock; a frame sent
// regardless was caught half-sent - a tear line moving through the text.
// Since 2026-10-26: only the window is sent (sendFrame()), at 80 MHz (Config.h
// TFT_SPI_HZ) that's ~15 ms, and each frame starts s_tearUs after a
// TE pulse. The tear lines then stand still, in the empty bands above and
// below the letters - tuned by eye (user, 12 ms +-0.5 best). Full-height
// pictures can still show them at their top and bottom. Background and the
// attempts that failed: CLAUDE.md, "Scroll tearing".
static volatile int64_t  s_teLastUs = 0;
static volatile uint32_t s_tePeriodUs = 0;
static void IRAM_ATTR onTe() {
    int64_t now = esp_timer_get_time();
    if (s_teLastUs) {
        int64_t d = now - s_teLastUs;
        if (d > 8000 && d < 40000) s_tePeriodUs = (uint32_t)d;
    }
    s_teLastUs = now;
}
uint32_t DisplayManager::getTePeriodUs() { return esp_timer_get_time() - s_teLastUs < 1000000 ? s_tePeriodUs : 0; }

// The pad drives only after TE_CTRL (E7h, vendor register: FF=A5 opens,
// FF=00 closes) te_out_oe=0x10 plus 0x35 (TE on, one pulse per redraw).
// Sent right after the panel's init it didn't take; between frames it does
// - so whenever TE has been silent for a second, at most every 5 s.
void DisplayManager::keepTeOn() {
    static Timer s_tried;
    if (esp_timer_get_time() - s_teLastUs < 1000000) return;
    if (!s_tried.idleOrPassed(5000)) return;
    s_tried.start();
    _gfx->startWrite();
    _bus->writeCommand(0xFF); _bus->write(0xA5);
    _bus->writeCommand(0xE7); _bus->write(0x10);
    _bus->writeCommand(0xFF); _bus->write(0x00);
    _bus->writeCommand(0x35); _bus->write(0x00);
    _gfx->endWrite();
}

// Until the next moment s_tearUs after a TE pulse (at most one
// redraw). No TE (wire or panel): at once, as without it.
static uint32_t s_lastTeWaitUs = 0;   // how long the last frame waited (not sending time)
static void waitForTe() {
    int64_t last = s_teLastUs, now = esp_timer_get_time();
    uint32_t P = s_tePeriodUs;
    if (!P || now - last > 4 * (int64_t)P) return;
    int64_t phase = (now - last) % P, target = s_tearUs.load() % P;
    int64_t until = now + (phase <= target ? target - phase : P - phase + target);
    while (esp_timer_get_time() < until) {}
    s_lastTeWaitUs = (uint32_t)(until - now);
}

// The frame to the panel: only the X/Y/W/H window - the rest is behind the
// clock's front (85 of 128 rows, 454 of 480 columns). The whole panel while
// calibrating, where what lies where matters more than smoothness.
void DisplayManager::sendFrame(bool wholePanel) {
    s_lastTeWaitUs = 0;
    uint16_t* fb = ((Arduino_Canvas*)_canvas)->getFramebuffer();
    int x = std::max(0, _winX), y = std::max(0, _winY);
    int w = std::min<int>(_winX + _winW, TFT_WIDTH) - x, h = std::min<int>(_winY + _winH, TFT_HEIGHT) - y;
    if (wholePanel || w <= 0 || h <= 0) {
        _gfx->draw16bitRGBBitmap(0, 0, fb, TFT_WIDTH, TFT_HEIGHT);
        return;
    }
    waitForTe();
    _gfx->startWrite();
    ((Arduino_NV3041A*)_gfx)->writeAddrWindow(x, y, w, h);
    for (int row = 0; row < h; row++) _bus->writePixels(fb + (size_t)(y + row) * TFT_WIDTH + x, w);
    _gfx->endWrite();
}

void DisplayManager::clearFull() {
    if (!_canvas) return;
    _canvas->fillScreen(COLOR_BACKGROUND);
}

void DisplayManager::setMode(DisplayMode mode) {
    DisplayLock::Guard guard;
    _mode = mode;
    clearWindow();
    if (mode == DisplayMode::Calibration) {
        _calibBorderPhase = 0;
        _lastCalibMs = 0;
    }
}

void DisplayManager::setScrollSpeed(int pixelsPerFrame) {
    if (pixelsPerFrame < 1) pixelsPerFrame = 1;
    _scrollSpeed = pixelsPerFrame;
}

void DisplayManager::setMessage(const std::vector<OnScreenElement>& elements, uint16_t color, const AppConfig& cfg) {
    DisplayLock::Guard guard;
    if (_mode == DisplayMode::Blank || _mode == DisplayMode::Setup) _mode = DisplayMode::Normal;

    _nextElements = elements;
    _messageColor = color; // Color can be updated immediately
    _hasNewMessage = true;

    CLOCK_LOG("Display", "setMessage: Queued %d new elements for transition.", elements.size());
}

void DisplayManager::analyzeElements(std::vector<OnScreenElement>& elements, const AppConfig& cfg, int& outWidth, bool& outIsRtl) {
    outWidth = 0;
    // Scroll direction normally follows the configured UI/message language,
    // not a per-message guess from character content. A Hebrew-language
    // clock scrolls RTL even on a message that happens to be all digits
    // (e.g. a Havdalah time); an English/Dutch clock scrolls LTR even for a
    // message that happens to be all Hebrew (e.g. a holiday name).
    // One exception (decided 2026-10-18): text with Latin letters and no
    // Hebrew at all scrolls LTR on any clock - "Connecting to MyNetwork2..."
    // and the other status messages (always English, on purpose), or an
    // event name typed in English, used to come in from the left on a
    // Hebrew clock. Per-character bidi analysis (getRunType) is still used
    // inside drawString() to order glyphs correctly within a line - that's
    // a separate concern from which way the whole message scrolls.
    bool hasHebrew = false, hasLatin = false;
    for (const auto& p : elements) {
        if (p.type != OnScreenElementType::TEXT) continue;
        const uint8_t* q = (const uint8_t*)p.content.c_str();
        while (*q) {
            uint32_t cp = decodeUTF8(&q);
            if (cp >= 0x0590 && cp <= 0x05FF) hasHebrew = true;
            else if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0x00C0 && cp <= 0x024F)) hasLatin = true;
        }
    }
    outIsRtl = (hasLatin && !hasHebrew) ? false : (strcmp(cfg.language, "he") == 0);

    for (auto& p : elements) {
        if (p.type == OnScreenElementType::STATIC_IMAGE || p.type == OnScreenElementType::ANIMATED_SPRITE) {
            // getCachedBmp() is the single place that opens/parses a BMP - first
            // call for a path does the real LittleFS work (and logs any
            // failure), every call after that (including this one, for every
            // element on every message rebuild) is a RAM lookup. Previously
            // this function re-opened and re-parsed the header itself, every
            // single time any message containing an icon was built.
            const CachedBmp* bmp = getShownBmp(p.content.c_str());
            p.isValid = bmp->valid;
            if (!bmp->valid) {
                p.width = FONT_H; // Use height for a square error icon
            } else {
                p.type = (bmp->frameCount > 1) ? OnScreenElementType::ANIMATED_SPRITE : OnScreenElementType::STATIC_IMAGE;
                p.width = (int)((bmp->fitted(pictureMaxH()) ? bmp->fitWidth : bmp->width) / bmp->frameCount);
                p.frame_count = (int)bmp->frameCount;
                if (bmp->frameMs) p.frame_delay_ms = (int)bmp->frameMs;   // the file's own speed (animations from video)
            }
        }
    }

    const char* sep = (cfg.messageSeparator[0] != '\0') ? cfg.messageSeparator : " * ";
    int sepWidth = measureString(sep);

    for (size_t i = 0; i < elements.size(); ++i) {
        const auto& p = elements[i];
        outWidth += p.width;
        if (i < elements.size() - 1 && !elements[i + 1].glueToPrevious) {
            outWidth += sepWidth;
        }
    }

    if (elements.empty()) {
        String projName = StringTools::formatProjectName(cfg.projectName);
        outWidth = measureString(projName.c_str());
    }
}

void DisplayManager::setMessage(const char* utf8text, uint16_t color, const AppConfig& cfg, bool isRtl) {
    // Overload to handle simple, single-segment messages like the boot screen.
    std::vector<OnScreenElement> elements;
    elements.emplace_back(OnScreenElementType::TEXT, String(utf8text), measureString(utf8text));
    setMessage(elements, color, cfg); // This will now use the new queuing mechanism
}

void DisplayManager::resetScroll(const AppConfig& cfg) {
    if (_currentMessagePixelWidth <= _winW) {
        // Center the text if it fits within the window.
        _scrollX = (_winW - _currentMessagePixelWidth) / 2;
    } else {
        if (_isCurrentRtl) {
            // RTL text starts with its right edge aligned to the window's right edge,
            // and then scrolls right, filling the screen from the start.
            _scrollX = _winW - _currentMessagePixelWidth;
        } else {
            // LTR text starts with its left edge at the window's left edge,
            // and then scrolls left.
            _scrollX = 0;
        }
    }
    _lastScrollMs = 0;
}

bool DisplayManager::isStill() {
    if (s_still) return true;
    if (_mode != DisplayMode::Normal) return false;   // calibration's frame moves
    return contentStill();
}

// The clock's own content (slides) stands: no scroll now, no animation - whatever
// is drawn over it (calibration's moving frame).
bool DisplayManager::contentStill() {
    bool standing = _displayState == DisplayState::STATIC ||
        (_displayState == DisplayState::ALT_SCROLL && _altScrollPhase != AltScrollPhase::SCROLLING);
    if (!standing) return false;
    for (const auto& p : _currentElements)
        if (p.type == OnScreenElementType::ANIMATED_SPRITE) return false;
    return true;
}

// isStill() as of the last frame, for other tasks (update() sets it).
static std::atomic<int64_t> s_stillSinceMs{0};    // Uptime::ms() it became still, 0 = moving
static std::atomic<bool> s_framesStarted{false};
static std::atomic<bool> s_scrollMode{false};      // single-scroll: never still
static std::atomic<bool> s_contentMoving{false};   // screenMoving(): the slides scroll / animate (update() sets it)

bool DisplayManager::goodMomentToWrite(int64_t waitingSinceMs) {
    if (!s_framesStarted) return true;
    int64_t now = Uptime::ms(), since = s_stillSinceMs;
    if (s_scrollMode) return now - waitingSinceMs >= FLASH_WRITE_GATHER_MS;
    return (since && now - since >= STILL_SETTLE_MS) || now - waitingSinceMs >= FLASH_WRITE_MAX_WAIT_MS;
}

bool DisplayManager::screenMoving() {
    return s_contentMoving;
}

bool DisplayManager::isAlternatingScrollActive() {
    return _displayState == DisplayState::ALT_SCROLL && _altScrollPhase != AltScrollPhase::DONE;
}

uint32_t DisplayManager::getAlternatingScrollRevealElapsedMs() {
    if (_displayState != DisplayState::ALT_SCROLL || _altScrollPhase != AltScrollPhase::DONE) return 0;
    return (uint32_t)(_altRevealDoneAtMs - _altItemStartMs);
}

bool DisplayManager::consumeSingleScrollLapCompleted() {
    bool v = _singleScrollLapCompleted;
    _singleScrollLapCompleted = false;
    return v;
}

bool DisplayManager::copyFrame(uint16_t* dst, std::vector<OnScreenElement>* elements, int* scrollX) {
    DisplayLock::Guard guard;   // update() renders and sends a frame under the same lock
    if (!_canvas || !dst) return false;
    memcpy(dst, ((Arduino_Canvas*)_canvas)->getFramebuffer(), (size_t)TFT_WIDTH * TFT_HEIGHT * sizeof(uint16_t));
    if (elements) *elements = _currentElements;
    if (scrollX) *scrollX = _scrollX;
    return true;
}

std::vector<OnScreenElement> DisplayManager::getCurrentMessageElements() {
    // Returns a copy, not the old const-reference: the only caller is
    // /api/preview (AsyncTCP task), while update() (main task) can
    // reassign _currentElements - including freeing its old backing
    // buffer - at any moment. A reference handed across tasks like that
    // was a use-after-free waiting to happen; copying it out under
    // DisplayLock is the whole fix, same pattern as the rest of this file.
    DisplayLock::Guard guard;
    return _currentElements;
}

// Alternating mode, a new item just put on screen: too wide for the window ->
// hold, then scroll (ALT_SCROLL, its timer starts now); fits -> stand still.
// One caller (update()), for every item - the first after a start included.
void DisplayManager::startAlternatingItem(int64_t now) {
    if (_currentMessagePixelWidth > _winW) {
        _displayState = DisplayState::ALT_SCROLL;
        _altScrollPhase = AltScrollPhase::HOLD;
        _altItemStartMs = now;
        _altRevealDoneAtMs = 0;
        CLOCK_LOG("Display", "  -> State -> ALT_SCROLL (long item)");
    } else {
        _displayState = DisplayState::STATIC;
        CLOCK_LOG("Display", "  -> State -> STATIC (short item)");
    }
}

void DisplayManager::update(const AppConfig& cfg) {
    // Guard, not manual take()/give(): this function returns early from
    // several different points below (uninitialized display, frame-rate
    // throttle, no canvas) - a scope guard means every one of those is
    // automatically covered, instead of relying on remembering to unlock
    // at each new return a future edit might add.
    DisplayLock::Guard guard;
    int64_t now = Uptime::ms();
    int64_t diagFrameStartUs = Uptime::us(); // Diagnostic: isolates render time vs. SPI transfer time per frame
    if (!_isInitialized && (now - _lastInitAttemptMs >= DISPLAY_RETRY_MS)) {
        begin();
    }

    if (!_isInitialized || !_gfx) return;
    if (!isStill()) s_stillSinceMs = 0;
    else if (!s_stillSinceMs) s_stillSinceMs = now | 1;
    s_contentMoving = !s_still && !s_calibPattern && (_mode == DisplayMode::Normal || _mode == DisplayMode::Calibration) && !contentStill();
    s_scrollMode = cfg.rotationMode == RotationMode::SingleScroll && _mode == DisplayMode::Normal;
    s_framesStarted = true;

    if (s_frozen) { s_prevFrameEndUs = 0; return; }   // live test: no drawing, no SPI traffic at all

    if (s_still) {          // update being written to flash: one still frame, then nothing (see Display.h)
        if (!s_stillDrawn && _canvas && _gfx) {
            _canvas->fillRect(_winX, _winY, _winW, _winH, COLOR_BACKGROUND);
            int w = measureString(s_stillText.c_str());
            drawString(_winX + (_winW - w) / 2, textTopY(_winY, _winH), s_stillText.c_str(), COLOR_GOLD, false, s_stillRtl);
            _gfx->draw16bitRGBBitmap(0, 0, ((Arduino_Canvas*)_canvas)->getFramebuffer(), TFT_WIDTH, TFT_HEIGHT);
            s_stillDrawn = true;
        }
        s_prevFrameEndUs = 0;
        return;
    }

    // Calibration: the live content (or the test pattern) with the window
    // frame drawn on top. Entering/leaving it, or switching content <->
    // pattern, clears the whole panel once (the pattern covers all of it).
    static DisplayMode s_lastMode = DisplayMode::Blank;
    static bool s_lastPattern = false;
    bool calib = _mode == DisplayMode::Calibration, pattern = calib && s_calibPattern;
    bool sendWhole = calib;
    if ((_mode == DisplayMode::Calibration) != (s_lastMode == DisplayMode::Calibration) || pattern != s_lastPattern) {
        clearFull();
        _lastScrollMs = 0;   // draw at once
        sendWhole = true;    // leaving calibration: also clears what it drew outside the window
    }
    s_lastMode = _mode;
    s_lastPattern = pattern;

    if (pattern) {
        updateCalibration();
    } else if (_mode != DisplayMode::Blank) {
        // --- State Machine Logic ---
        if (_hasNewMessage) {
            _hasNewMessage = false;
            CLOCK_LOG("Display", "update: Processing new message queue...");
            analyzeElements(_nextElements, cfg, _nextMessagePixelWidth, _isNextRtl);
            CLOCK_LOG("Display", "  -> New message analyzed. Width: %d, RTL: %s", _nextMessagePixelWidth, _isNextRtl ? "yes" : "no");

            if (cfg.rotationMode == RotationMode::Alternating) {
                // Alternating mode: every item the same way - the first after a start
                // too (2026-11-14: was two copies, "first message" and "hard cut").
                // A hard cut to the new item, no scroll/dissolve: one frame still shows
                // the old item (none, the first time); the very next frame already
                // shows the new one, nothing in between.
                _currentElements = _nextElements;
                _nextElements.clear();
                _currentMessagePixelWidth = _nextMessagePixelWidth;
                _isCurrentRtl = _isNextRtl;

                startAlternatingItem(now);
                resetScroll(cfg);
            } else if (_currentElements.empty()) {
                // SingleScroll's very first message: nothing to transition from.
                _currentElements = _nextElements;
                _nextElements.clear();
                _currentMessagePixelWidth = _nextMessagePixelWidth;
                _isCurrentRtl = _isNextRtl;
                // SingleScroll is one continuous ticker, by design (see
                // MessageAggregator::buildElements) - unlike Alternating,
                // it should never sit still just because this particular
                // round's concatenated content happens to be short.
                _displayState = DisplayState::LOOPING_SCROLL;
                CLOCK_LOG("Display", "  -> State -> LOOPING_SCROLL (SingleScroll)");
                resetScroll(cfg); // Set initial scroll position
            } else {
                // An old message exists. Start transitioning.
                _displayState = DisplayState::TRANSITIONING;
                // The scroll position _scrollX is NOT reset here. It continues from where it was.
                CLOCK_LOG("Display", "  -> Existing message found. State -> TRANSITIONING");
            }
        }

        if (now - _lastScrollMs < 16) return;
        _lastScrollMs = now;

        const char* sep = (cfg.messageSeparator[0] != '\0') ? cfg.messageSeparator : " * ";
        int sepWidth = measureString(sep);
        int totalCurrentWidthWithSep = _currentMessagePixelWidth + sepWidth;

        // --- Scroll Position Update & State Transitions ---
        switch (_displayState) {
            case DisplayState::STATIC:
                _scrollX = (_winW - _currentMessagePixelWidth) / 2;
                break;

            case DisplayState::LOOPING_SCROLL:
                if (_isCurrentRtl) {
                    _scrollX += _scrollSpeed;
                    if (_scrollX >= totalCurrentWidthWithSep) { _scrollX -= totalCurrentWidthWithSep; _singleScrollLapCompleted = true; }
                } else {
                    _scrollX -= _scrollSpeed;
                    if (_scrollX < -totalCurrentWidthWithSep) { _scrollX += totalCurrentWidthWithSep; _singleScrollLapCompleted = true; }
                }
                break;

            case DisplayState::ALT_SCROLL: {
                // Alternating mode's too-long item: hold at the start position
                // (a third of the configured dwell time) so the beginning/icon
                // is actually seen - once only, before the first pass. Then
                // scroll continuously: each pass reveals the tail end (the
                // same reveal-complete point as a one-shot scroll, no need to
                // scroll all the way off-screen). At that point, if there's
                // still worthwhile dwell budget left, keep going - stitch
                // another copy on seamlessly (same mechanism LOOPING_SCROLL
                // already uses for its continuous loop: draw two copies,
                // separated by cfg.messageSeparator, wrapping scrollX by one
                // period) - no reset, no extra hold, it just keeps flowing.
                // Once a pass is greenlit to start, it always runs to
                // completion; the budget check only ever gates whether to
                // *start* another pass, never cuts one off mid-scroll (the
                // outer MessageAggregator dwell-switch is held off the whole
                // time via isAlternatingScrollActive()). When budget finally
                // runs out, freeze right at that pass's reveal-complete point
                // and hold for half the dwell time before moving on.
                int64_t dwellMs = (int64_t)cfg.messageDwellSec * 1000;
                if (_altScrollPhase == AltScrollPhase::HOLD) {
                    if (now - _altItemStartMs >= dwellMs / 3) {
                        _altScrollPhase = AltScrollPhase::SCROLLING;
                        _altCycleStartMs = now; // start timing pass 1
                        _altCheckpointArmed = true;
                    }
                } else if (_altScrollPhase == AltScrollPhase::SCROLLING) {
                    int stopX = _isCurrentRtl ? 0 : (_winW - _currentMessagePixelWidth);

                    if (_isCurrentRtl) {
                        _scrollX += _scrollSpeed;
                    } else {
                        _scrollX -= _scrollSpeed;
                    }

                    bool reachedCheckpoint = _isCurrentRtl ? (_scrollX >= stopX) : (_scrollX <= stopX);
                    if (_altCheckpointArmed && reachedCheckpoint) {
                        _altCheckpointArmed = false;
                        int64_t passDurationMs = now - _altCycleStartMs;
                        int64_t elapsedTotal = now - _altItemStartMs;
                        int64_t remaining = (elapsedTotal < dwellMs) ? (dwellMs - elapsedTotal) : 0;
                        if (remaining >= passDurationMs / 2) {
                            // Enough budget left to be worth another pass - let it
                            // flow onward (scrollX keeps moving past this point);
                            // the wrap below will re-arm the next checkpoint.
                            _altCycleStartMs = now; // start timing the next pass
                        } else {
                            _scrollX = stopX; // freeze exactly at reveal-complete
                            _altScrollPhase = AltScrollPhase::DONE;
                            _altRevealDoneAtMs = now;
                        }
                    }

                    // Seamless wrap (only reached if still continuing) - same
                    // mechanism as LOOPING_SCROLL, and what re-arms the next
                    // pass's checkpoint detection.
                    if (_altScrollPhase == AltScrollPhase::SCROLLING) {
                        if (_isCurrentRtl) {
                            if (_scrollX >= totalCurrentWidthWithSep) {
                                _scrollX -= totalCurrentWidthWithSep;
                                _altCheckpointArmed = true;
                            }
                        } else {
                            if (_scrollX < -totalCurrentWidthWithSep) {
                                _scrollX += totalCurrentWidthWithSep;
                                _altCheckpointArmed = true;
                            }
                        }
                    }
                }
                // DONE: scrollX stays frozen at its final position.
                break;
            }

            case DisplayState::TRANSITIONING:
                // Keep scrolling the old message
                if (_isCurrentRtl) {
                    _scrollX += _scrollSpeed;
                } else {
                    _scrollX -= _scrollSpeed;
                }

                // Check if the transition is complete
                bool transitionDone = false;
                if (_isCurrentRtl) {
                    if (_scrollX > _winW) {
                        transitionDone = true;
                        CLOCK_LOG("Display", "Transition (RTL) complete. scrollX: %d > winW: %d", _scrollX, _winW);
                        // Rebase using the INCOMING message's own width, not the outgoing
                        // one's — it was drawn at (currentX - sepWidth - nextMessageWidth),
                        // so the new scrollX baseline must match that, not the old message's width.
                        _scrollX -= (sepWidth + _nextMessagePixelWidth);
                    }
                } else { // LTR
                    if (_scrollX < -_currentMessagePixelWidth) {
                        transitionDone = true;
                        CLOCK_LOG("Display", "Transition (LTR) complete. scrollX: %d < -msgWidth: %d", _scrollX, -_currentMessagePixelWidth);
                        _scrollX += totalCurrentWidthWithSep; // Adjust scroll for new message
                    }
                }

                if (transitionDone) {
                    CLOCK_LOG("Display", "Finalizing transition. New scrollX: %d", _scrollX);
                    _currentElements = _nextElements;
                    _currentMessagePixelWidth = _nextMessagePixelWidth;
                    _isCurrentRtl = _isNextRtl;
                    _nextElements.clear();

                    // TRANSITIONING is only ever entered from SingleScroll mode
                    // (Alternating hard-cuts instead - see above), which is one
                    // continuous ticker by design: always keep scrolling, even
                    // if this particular round's content happens to be short.
                    _displayState = DisplayState::LOOPING_SCROLL;
                    CLOCK_LOG("Display", "State -> LOOPING_SCROLL");
                    // Do not call resetScroll(), as _scrollX was already adjusted for seamlessness
                }
                break;
        }

        if (_canvas) {
            _canvas->fillRect(_winX, _winY, _winW, _winH, COLOR_BACKGROUND);
        } else {
            return;
        }

        // --- Drawing Logic ---
        int currentX = _winX + _scrollX;
        const int drawY = textTopY(_winY, _winH);

        auto drawOneElement = [&](int x, const OnScreenElement& p, bool isRtl) {
            if (p.type == OnScreenElementType::TEXT) {
                // The drawString function now needs to know the overall message direction
                // to handle mixed LTR/RTL runs correctly.
                drawString(x, drawY, p.content.c_str(), _messageColor, false, isRtl);
            } else if (p.isValid) { // Image or Sprite, and it's valid
                int frame_index = 0;
                int frame_width = p.width; // Use element width for both static and animated
                if (p.type == OnScreenElementType::ANIMATED_SPRITE && p.frame_count > 0 && p.frame_delay_ms > 0) {
                    frame_index = (int)((Uptime::ms() / p.frame_delay_ms) % p.frame_count);
                }
                drawBmp(p.content.c_str(), x, drawY, frame_index, frame_width);
            } else { // Image or Sprite, but it's invalid
                drawErrorIcon(_canvas, x, drawY, p.width, FONT_H, _winX, _winY, _winW, _winH);
            }
        };

        auto drawAllElements = [&](int startX, const std::vector<OnScreenElement>& elementsToDraw, bool isRtl) {
            int x = startX;
            size_t i = 0;
            while (i < elementsToDraw.size()) {
                // Gather a "cluster": an element and any that are glued to it
                // (e.g. an icon immediately followed by its label, with no separator).
                size_t j = i + 1;
                while (j < elementsToDraw.size() && elementsToDraw[j].glueToPrevious) j++;

                if (isRtl && j - i > 1) {
                    // In an RTL message, a glued icon+label group must mirror: the
                    // icon (drawn first/leftmost for LTR) belongs on the right of its
                    // label instead, matching Hebrew reading order. Draw the cluster
                    // in reverse so the icon ends up as the visually-rightmost part.
                    for (size_t k = j; k-- > i; ) {
                        drawOneElement(x, elementsToDraw[k], isRtl);
                        x += elementsToDraw[k].width;
                    }
                } else {
                    for (size_t k = i; k < j; ++k) {
                        drawOneElement(x, elementsToDraw[k], isRtl);
                        x += elementsToDraw[k].width;
                    }
                }

                i = j;
                if (i < elementsToDraw.size()) {
                    drawString(x, drawY, sep, _messageColor, false, isRtl);
                    x += sepWidth;
                }
            }
        };

        // --- Render based on state ---
        if (_displayState == DisplayState::STATIC) {
            drawAllElements(currentX, _currentElements, _isCurrentRtl);
        }
        else if (_displayState == DisplayState::LOOPING_SCROLL || _displayState == DisplayState::ALT_SCROLL) {
            // ALT_SCROLL uses the same seamless two-copy drawing as
            // LOOPING_SCROLL, since a pass may keep flowing into a stitched
            // repeat of the same message - the second copy simply never
            // becomes visible for a single-pass item (it sits off-screen
            // until scrollX actually reaches it), so this looks identical to
            // a plain one-shot reveal whenever no continuation happens.
            drawAllElements(currentX, _currentElements, _isCurrentRtl);
            if (_isCurrentRtl) {
                int sepX = currentX - sepWidth;
                drawString(sepX, drawY, sep, _messageColor, false, _isCurrentRtl);
                drawAllElements(currentX - totalCurrentWidthWithSep, _currentElements, _isCurrentRtl);
            } else {
                int sepX = currentX + _currentMessagePixelWidth;
                drawString(sepX, drawY, sep, _messageColor, false, _isCurrentRtl);
                drawAllElements(currentX + totalCurrentWidthWithSep, _currentElements, _isCurrentRtl);
            }
        }
        else if (_displayState == DisplayState::TRANSITIONING) {
            // Draw [current] * [next]
            drawAllElements(currentX, _currentElements, _isCurrentRtl);
            if (_isCurrentRtl) {
                int sepX = currentX - sepWidth;
                drawString(sepX, drawY, sep, _messageColor, false, _isCurrentRtl);
                // Position the incoming message by its OWN width so it always lands
                // right after the separator, regardless of whether it's longer or
                // shorter than the outgoing message (using the outgoing message's
                // width here, as before, made the incoming text overshoot into the
                // outgoing message's region whenever the incoming one was wider).
                drawAllElements(currentX - sepWidth - _nextMessagePixelWidth, _nextElements, _isNextRtl);
            } else {
                int sepX = currentX + _currentMessagePixelWidth;
                drawString(sepX, drawY, sep, _messageColor, false, _isCurrentRtl);
                drawAllElements(currentX + _currentMessagePixelWidth + sepWidth, _nextElements, _isNextRtl);
            }
        }
    }

    if (calib) {   // the window frame, moving, on top of whatever is shown
        drawCalibBorder();
        if (now - _lastCalibMs >= 80) { _lastCalibMs = now; if (++_calibBorderPhase >= 1000000000) _calibBorderPhase = 0; }   // never past int (years in calibration)
    }

    if (_gfx && _canvas) {
        keepTeOn();
        int64_t diagBlitStartUs = Uptime::us();
        sendFrame(sendWhole);
        uint32_t diagBlitUs = (uint32_t)(Uptime::us() - diagBlitStartUs - s_lastTeWaitUs);   // sending only, not the wait for TE
        s_lastBlitUs = diagBlitUs;
        int64_t frameEndUs = Uptime::us();
        if (s_prevFrameEndUs) {
            int64_t gapUs = frameEndUs - s_prevFrameEndUs;
            if (gapUs > FRAME_HITCH_US) s_hitches++;
            if (gapUs > FRAME_BIG_HITCH_US) s_bigHitches++;
        }
        s_prevFrameEndUs = frameEndUs;
        uint32_t diagTotalUs = (uint32_t)(Uptime::us() - diagFrameStartUs);
        // Diagnostic: throttled to ~2/sec so it doesn't flood the log while still showing a live trend.
        static int64_t diagLastLogMs = 0;
        if (Uptime::ms() - diagLastLogMs > 500) {
            diagLastLogMs = Uptime::ms();
            CLOCK_LOG("Display", "Frame timing: blit=%luus, total=%luus, render=%luus, scrollX=%d",
                      (unsigned long)diagBlitUs, (unsigned long)diagTotalUs,
                      (unsigned long)(diagTotalUs - diagBlitUs), _scrollX);
        }
    }
}

// Decodes a BMP from LittleFS exactly once into a flat top-down RGB565 buffer
// in PSRAM, and caches it (pixels, dimensions, AND frame count/type) by path -
// the one place that reads a BMP's header or pixel data, so every other piece
// of code (drawBmp() for rendering, analyzeElements() for layout) just looks
// the result up here instead of re-deriving it. Every call after the first
// for a given path is a pure RAM lookup, success or failure alike - failures
// (missing file, corrupt header, OOM) are cached too, with valid=false, so a
// permanently-missing icon doesn't re-hit LittleFS on every render frame or
// every new message. This never retries a failed path - acceptable because
// Data/ assets only ever change via uploadfs, which reboots the device and
// so clears this cache anyway; a file that's missing stays missing for the
// rest of that boot, same as before, just without the old re-check-every-call
// behavior that existed to notice it reappearing without a reboot. The one
// exception, event photos (uploaded/deleted at any time), goes through
// forgetImage().
// Always returns a valid pointer; check ->valid rather than for null.
// Caller holds DisplayLock (the main loop's drawing does).
const DisplayManager::CachedBmp* DisplayManager::getCachedBmp(const char* path) {
    String key(path);
    auto it = _bmpPixelCache.find(key);
    if (it != _bmpPixelCache.end()) return &it->second;
    // Up to the screen's height, not the window's: a picture taller than the
    // calibrated window is shrunk to fit it (getShownBmp()). Limited to the
    // window, every built-in picture became a red X once the window was
    // calibrated to 85 px (2026-10-25) - and stayed one, since a failed load
    // is remembered.
    return &_bmpPixelCache.emplace(key, loadBmp(path, TFT_HEIGHT)).first->second;
}

// How tall a picture may be: the window's height, or less (PICTURE_MAX_H_ROWS).
int DisplayManager::pictureMaxH() { return std::min<int>(_winH, PICTURE_MAX_H_ROWS); }

// What the screen shows of a picture: getCachedBmp(), plus - when it is taller
// than pictureMaxH() - a copy shrunk to that height (fit, same aspect; each
// frame of an animation on its own). Each pixel is the average of the picture
// pixels it covers; transparent where most of them are. Made on first use per
// height; the original stays for downloads (ImageFile).
// Caller holds DisplayLock.
const DisplayManager::CachedBmp* DisplayManager::getShownBmp(const char* path) {
    const CachedBmp* cached = getCachedBmp(path);
    const int maxH = pictureMaxH();
    if (!cached->valid || cached->height <= maxH || cached->fitForH == maxH || maxH <= 0) return cached;
    CachedBmp& b = _bmpPixelCache.find(String(path))->second;

    const int frames = b.frameCount > 0 ? b.frameCount : 1;
    const int srcW = b.width / frames, srcH = b.height, dstH = maxH;
    const int dstW = std::max(1, (srcW * dstH + srcH / 2) / srcH);
    const int rowW = dstW * frames;
    b.fitForH = maxH;   // a failure below isn't retried every frame: drawn cut instead
    b.fit.reset();
    uint16_t* px = (uint16_t*)heap_caps_malloc((size_t)rowW * dstH * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!px) {
        CLOCK_LOG("Display", "ERROR: Out of PSRAM shrinking '%s' to %d px high", path, dstH);
        return &b;
    }
    for (int dy = 0; dy < dstH; dy++) {
        const int y0 = dy * srcH / dstH, y1 = std::max(y0 + 1, (dy + 1) * srcH / dstH);
        for (int f = 0; f < frames; f++) {
            for (int dx = 0; dx < dstW; dx++) {
                const int x0 = dx * srcW / dstW, x1 = std::max(x0 + 1, (dx + 1) * srcW / dstW);
                uint32_t r = 0, g = 0, bl = 0, opaque = 0, total = 0;
                for (int y = y0; y < y1; y++) {
                    const uint16_t* row = b.pixels + (size_t)y * b.width + f * srcW;
                    for (int x = x0; x < x1; x++, total++) {
                        uint16_t c = row[x];
                        if (c == COLOR_TRANSPARENT) continue;
                        r += c >> 11; g += (c >> 5) & 0x3F; bl += c & 0x1F; opaque++;
                    }
                }
                uint16_t out = COLOR_TRANSPARENT;
                if (opaque * 2 >= total) {
                    out = (uint16_t)(((r / opaque) << 11) | ((g / opaque) << 5) | (bl / opaque));
                    if (out == COLOR_TRANSPARENT) out ^= 0x0020;   // an averaged colour must not read as "transparent"
                }
                px[(size_t)dy * rowW + f * dstW + dx] = out;
            }
        }
    }
    b.fit.reset(px, [](uint16_t* p) { heap_caps_free(p); });
    b.fitWidth = rowW;
    b.fitHeight = dstH;
    CLOCK_LOG("Display", "Shrunk '%s' from %d to %d px high for the window", path, srcH, dstH);
    return &b;
}

// The reading for getCachedBmp(): a file into a cache entry (valid=false if
// it can't be shown). Touches nothing shared, so it needs no lock - a web
// request loads a new picture with it while the screen keeps drawing
// (isImageUsable()); holding DisplayLock for the whole read froze the screen.
DisplayManager::CachedBmp DisplayManager::loadBmp(const char* path, int maxHeight) {
    auto cacheFailure = []() { return CachedBmp(); };   // valid=false by default

    File bmpFile = LittleFS.open(path, "r");
    if (!bmpFile) {
        CLOCK_LOG("Display", "ERROR: Failed to open image '%s'.", path);
        return cacheFailure();
    }

    BmpFormat format;
    int32_t width = 0, height = 0;
    if (!readBmpHeader(bmpFile, width, height, maxHeight, format)) {
        // Error already logged by readBmpHeader.
        bmpFile.close();
        return cacheFailure();
    }

    // Frame count in biXPelsPerMeter - see bmpFrameCount() (Display.h).
    int32_t xPelsPerMeter = 0, yPelsPerMeter = 0;
    bmpFile.seek(38);
    bmpFile.read((uint8_t*)&xPelsPerMeter, 4);
    // Likewise ms per frame in biYPelsPerMeter (event pictures made from a
    // video, ImageLibrary.h); the clock's own icons leave it 0.
    bmpFile.read((uint8_t*)&yPelsPerMeter, 4);
    int32_t frameCount = bmpFrameCount(width, height, xPelsPerMeter);

    uint32_t dataOffset = 0;
    bmpFile.seek(10);
    bmpFile.read((uint8_t*)&dataOffset, 4);

    // Per the BMP spec, each row is padded to a 4-byte boundary. This only
    // coincides with (width * 2) when width is even; for an odd-width 16bpp
    // image the on-disk stride is 2 bytes longer than the pixel data.
    uint32_t rowStrideBytes = (((uint32_t)width * 2) + 3) & ~3u;

    size_t pixelCount = (size_t)width * (size_t)height;
    uint16_t* pixels = (uint16_t*)heap_caps_malloc(pixelCount * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!pixels) {
        CLOCK_LOG("Display", "ERROR: Out of PSRAM caching BMP '%s' (%d x %d)", path, (int)width, (int)height);
        bmpFile.close();
        return cacheFailure();
    }
    std::shared_ptr<uint16_t> owner(pixels, [](uint16_t* p) { heap_caps_free(p); });   // frees on every return below too

    // Can memory give the file back byte for byte (ImageFile)? Only if it is
    // just header + rows - nothing after them, zeros as row padding - and
    // its colours are kept as they are (not converted from 555).
    const size_t fileSize = bmpFile.size();
    bool exact = format == BMP_FMT_565 && dataOffset >= 54 && dataOffset <= 1024 &&
                 fileSize == dataOffset + (size_t)rowStrideBytes * height;
    std::shared_ptr<std::vector<uint8_t>> header;
    if (exact) {
        header = std::make_shared<std::vector<uint8_t>>(dataOffset);
        bmpFile.seek(0);
        exact = bmpFile.read(header->data(), dataOffset) == dataOffset;
    }

    // BMPs are stored bottom-up on disk; flip to top-down once here so the
    // draw loop can index rows directly.
    const size_t padBytes = rowStrideBytes - (size_t)width * 2;
    for (int32_t row = 0; row < height; row++) {
        if (!bmpFile.seek(dataOffset + (uint32_t)(height - 1 - row) * rowStrideBytes)) {
            CLOCK_LOG("Display", "ERROR: Seek failed caching BMP '%s'", path);
            bmpFile.close();
            return cacheFailure();
        }
        uint16_t* dst = pixels + (size_t)row * width;
        size_t bytesToRead = (size_t)width * 2;
        if (bmpFile.read((uint8_t*)dst, bytesToRead) != bytesToRead) {
            CLOCK_LOG("Display", "ERROR: Read failed caching BMP '%s'", path);
            bmpFile.close();
            return cacheFailure();
        }
        if (exact && padBytes) {
            uint8_t pad[4] = { 0 };
            bmpFile.read(pad, padBytes);
            for (size_t i = 0; i < padBytes; i++) if (pad[i]) exact = false;
        }
        if (format == BMP_FMT_555) {
            for (int32_t i = 0; i < width; i++) {
                uint16_t c555 = dst[i];
                uint8_t r = (c555 >> 10) & 0x1F;
                uint8_t g = (c555 >> 5)  & 0x1F;
                uint8_t b = c555 & 0x1F;
                dst[i] = (r << 11) | ((g << 1) | (g >> 4)) << 5 | b;
            }
        }
    }
    bmpFile.close();

    CLOCK_LOG("Display", "Cached BMP '%s' in PSRAM: %d x %d, %d frame(s) (%u bytes)%s",
              path, (int)width, (int)height, (int)frameCount, (unsigned)(pixelCount * sizeof(uint16_t)),
              exact ? "" : " - downloads read the file (memory can't rebuild it exactly)");

    CachedBmp entry;
    entry.pixels = pixels;
    entry.data = owner;
    if (exact) entry.fileHeader = header;
    entry.fileSize = fileSize;
    entry.width = width;
    entry.height = height;
    entry.frameCount = frameCount;
    entry.frameMs = (frameCount > 1 && inRange(PICTURE_FRAME_MS_RANGE, yPelsPerMeter)) ? yPelsPerMeter : 0;
    entry.valid = true;
    return entry;
}

void DisplayManager::drawBmp(const char *path, int x, int y, int frame, int frameWidth) {
    if (!_canvas) return;

    const CachedBmp* bmp = getShownBmp(path);
    if (!bmp->valid) {
        drawErrorIcon(_canvas, x, y, frameWidth > 0 ? frameWidth : FONT_H, FONT_H, _winX, _winY, _winW, _winH);
        return;
    }

    // Shrunk to the window if it was taller (getShownBmp()); otherwise as
    // loaded - and if shrinking failed, cut at the window's edges below.
    const bool fitted = bmp->fitted(pictureMaxH());
    const uint16_t* pixels = fitted ? bmp->fit.get() : bmp->pixels;
    int32_t totalWidth = fitted ? bmp->fitWidth : bmp->width;
    int32_t height = fitted ? bmp->fitHeight : bmp->height;

    // One frame's width - the whole picture for a still one.
    int currentFrameWidth = totalWidth / (bmp->frameCount > 0 ? bmp->frameCount : 1);
    int frameOffset = frame * currentFrameWidth;

    if (frameOffset < 0 || (frameOffset + currentFrameWidth) > totalWidth) {
        CLOCK_LOG("Display", "ERROR: Frame %d is out of bounds for BMP '%s'", frame, path);
        drawErrorIcon(_canvas, x, y, currentFrameWidth, FONT_H, _winX, _winY, _winW, _winH);
        return;
    }

    // Center vertically on the image's own height rather than the caller's y
    // (which is computed for FONT_H-tall text). Icons aren't all FONT_H tall -
    // e.g. Havdala.bmp/Candles.bmp fill the full window height - so reusing
    // the text baseline clipped their bottom rows off the window.
    int yBase = _winY + (_winH - (int)height) / 2;

    for (int32_t row = 0; row < height; row++) {
        int drawY = yBase + row;
        if (drawY < _winY || drawY >= _winY + _winH) continue;

        const uint16_t* rowPixels = pixels + (size_t)row * totalWidth + frameOffset;

        for (int col = 0; col < currentFrameWidth; col++) {
            int drawX = x + col;
            if (drawX >= _winX && drawX < _winX + _winW && rowPixels[col] != COLOR_TRANSPARENT) {
                _canvas->drawPixel(drawX, drawY, rowPixels[col]);
            }
        }
    }
}

// =============================================================================
// UTF‑8 decode
// =============================================================================

uint32_t DisplayManager::decodeUTF8(const uint8_t** ptr) {
    uint8_t b = **ptr; (*ptr)++;
    if (b < 0x80) return b;
    if ((b & 0xE0) == 0xC0) {
        uint32_t cp = (b & 0x1F) << 6;
        b = **ptr; (*ptr)++;
        return cp | (b & 0x3F);
    }
    if ((b & 0xF0) == 0xE0) {
        uint32_t cp = (b & 0x0F) << 12;
        b = **ptr; (*ptr)++; cp |= (b & 0x3F) << 6;
        b = **ptr; (*ptr)++; cp |= (b & 0x3F);
        return cp;
    }
    return 0xFFFD;
}

// =============================================================================
// measureString
// =============================================================================

int DisplayManager::measureString(const char* utf8text) {
    if (!utf8text) return 0;
    const uint8_t* p = (const uint8_t*)utf8text;
    int width = 0;
    while (*p) { width += fontGetAdvanceForCodepoint(decodeUTF8(&p)); }
    return width;
}

// =============================================================================
// drawGlyph
// =============================================================================

// bg and fg mixed by a font pixel's coverage (0..15, 15 = all fg), in RGB565:
// the three colour fields spread apart in 32 bits so one multiply blends all.
static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t cover) {
    uint32_t a = ((uint32_t)cover * 32 + 7) / 15;   // 0..32
    uint32_t f = (fg | ((uint32_t)fg << 16)) & 0x07E0F81FUL;
    uint32_t b = (bg | ((uint32_t)bg << 16)) & 0x07E0F81FUL;
    uint32_t r = ((f * a + b * (32 - a)) >> 5) & 0x07E0F81FUL;
    return (uint16_t)(r | (r >> 16));
}

void DisplayManager::drawGlyph(int x, int y, uint32_t codepoint, uint16_t color, bool directToCanvas) {
    if (!_isInitialized) return;

    // Early exit if the glyph is completely off-screen. The actual draw is
    // shifted left by up to FONT_W (the glyph's left bearing - see below), so
    // this stays conservative on the right-hand bound rather than using x
    // directly, or a glyph entering from the right could be skipped a few
    // pixels too early.
    if (!directToCanvas && (x + FONT_W <= _winX || x - FONT_W >= _winX + _winW || y + FONT_H <= _winY || y >= _winY + _winH)) return;

    int idx = fontGetGlyphIdx(codepoint);

    if (idx < 0) {
        int rx = x, ry = y, rw = FONT_W - 2, rh = FONT_H - 2;
        if (rx < _winX) { rw -= (_winX - rx); rx = _winX; }
        if (ry < _winY) { rh -= (_winY - ry); ry = _winY; }
        if (rx + rw > _winX + _winW) rw = (_winX + _winW) - rx;
        if (ry + rh > _winY + _winH) rh = (_winY + _winH) - ry;
        if (rw > 0 && rh > 0 && _canvas) _canvas->drawRect(rx, ry, rw, rh, COLOR_DARKGREY);
        return;
    }

    const uint8_t* bitmap = fontGetGlyphPtr(idx);
    if (!bitmap) return;

    // The glyph is drawn ink-first: its left bearing already puts the ink's
    // leftmost column at x (see PROPORTIONAL METRICS in Font.h), and its
    // width may additionally be scaled down (fontSetWidthScale) by resampling
    // only `scaledWidth` output columns, nearest-neighbor, from the real
    // bitmap - so at 100% scale outCol N reads source column leftBearing+N
    // (i.e. unchanged), and at less than 100% some source columns are
    // skipped. Precompute the per-output-column source once, outside the
    // row loop, since it's the same for every row of this glyph.
    int scaledWidth = fontGetGlyphScaledInkWidth(idx);
    if (scaledWidth > FONT_W) scaledWidth = FONT_W; // Defensive; can't exceed the cell.
    uint8_t srcCols[FONT_W];
    for (int j = 0; j < scaledWidth; j++) srcCols[j] = fontGetGlyphSourceCol(idx, (uint8_t)j);

    // Each pixel holds how much of it the character covers (0-15, Font.h):
    // fully covered pixels get the text colour, edge pixels a blend of the
    // text colour and what is already on screen there - the smooth edges.
    // Written straight into the canvas's frame buffer (no rotation; one
    // uint16 per pixel), which the blend needs to read anyway.
    if (!_canvas) return;
    uint16_t* fb = ((Arduino_Canvas*)_canvas)->getFramebuffer();
    for (int row = 0; row < FONT_H; row++) {
        int dispY = y + row;
        if (directToCanvas) {
            if (dispY < 0) continue;
            if (dispY >= TFT_HEIGHT) break;
        } else {
            if (dispY < _winY) continue;
            if (dispY >= _winY + _winH) break;
        }
        for (int outCol = 0; outCol < scaledWidth; outCol++) {
            if (srcCols[outCol] == FONT_NO_COLUMN) continue;
            uint8_t cover = fontGetCoverage(bitmap, srcCols[outCol], row);
            if (!cover) continue;
            int dispX = x + outCol;
            if (directToCanvas) {
                if (dispX < 0) continue;
                if (dispX >= TFT_WIDTH) break;
            } else {
                if (dispX < _winX) continue;
                if (dispX >= _winX + _winW) break; // Exit inner loop for this row
            }
            uint16_t& px = fb[dispY * TFT_WIDTH + dispX];
            px = (cover == 15) ? color : blend565(px, color, cover);
        }
    }
}

// =============================================================================
// drawString
// =============================================================================

int DisplayManager::drawString(int startX, int y, const char* utf8text, uint16_t color, bool directToCanvas, bool isOverallRtl) {
    // CLOCK_LOG("Display", "drawString: Entry. String length: %d", strlen(utf8text));
    if (!utf8text || !*utf8text || !_canvas) return 0;

    static uint32_t codepoints[MAX_MESSAGE_LEN];
    static BidiRunType runTypes[MAX_MESSAGE_LEN];
    int count = 0;
    const uint8_t* p = (const uint8_t*)utf8text;
    while (*p && count < (MAX_MESSAGE_LEN - 1)) {
        uint32_t cp = decodeUTF8(&p);
        runTypes[count] = getRunType(cp);
        codepoints[count++] = cp;
    }
    // ':' and '/' belong to a number only BETWEEN digits ("18:43", "5/10"),
    // as in Unicode's bidi rules; anywhere else they are plain punctuation.
    // Always counting them as part of a number put the colon of "צום: 05:18"
    // with the time: the screen showed ": 05:18-16:55צום" (found 2026-10-18).
    auto isDigit = [](uint32_t c) { return c >= '0' && c <= '9'; };
    for (int k = 0; k < count; k++) {
        if ((codepoints[k] == ':' || codepoints[k] == '/') &&
            !(k > 0 && isDigit(codepoints[k - 1]) && k + 1 < count && isDigit(codepoints[k + 1]))) {
            runTypes[k] = BidiRunType::NEUTRAL;
        }
    }

    // Bidi Algorithm:
    // 1. Decompose the string into a sequence of directional runs.
    struct BidiRun {
        int start;
        int end;
        BidiRunType type;
    };
    std::vector<BidiRun> runs;
    if (count > 0) {
        int i = 0;
        // 1. Decompose into atomic runs of identical character types.
        while (i < count) {
            BidiRun run;
            run.start = i;
            run.type = runTypes[i];
            while (i < count && runTypes[i] == run.type) {
                i++;
            }
            run.end = i;
            runs.push_back(run);
        }

        // 2. Resolve direction of neutral runs based on their neighbors.
        // This is a simplified version of the Unicode Bidi algorithm.
        // A run of digits only (a number, not a Latin word).
        auto digitsOnly = [&](const BidiRun& r) {
            for (int k = r.start; k < r.end; k++) if (!isDigit(codepoints[k]) && codepoints[k] != ':' && codepoints[k] != '/') return false;
            return true;
        };
        for (size_t j = 0; j < runs.size(); ++j) {
            if (runs[j].type == BidiRunType::NEUTRAL) {
                BidiRunType before = (j > 0) ? runs[j - 1].type : BidiRunType::NEUTRAL;
                BidiRunType after = (j < runs.size() - 1) ? runs[j + 1].type : BidiRunType::NEUTRAL;

                // Between two numbers in a Hebrew line, only one of + - , . joins
                // them into one number ("05:18-16:55", Unicode bidi W4); anything
                // else - "23 = 3" - goes the line's way, as a browser shows it
                // (23 first, at the right). Before 2026-11-06 "23=3 שבועות"
                // came out with the 3 at the right (Omer's Compact wording).
                if (isOverallRtl && before == BidiRunType::LTR && after == BidiRunType::LTR &&
                    digitsOnly(runs[j - 1]) && digitsOnly(runs[j + 1]) &&
                    !(runs[j].end - runs[j].start == 1 && (codepoints[runs[j].start] == '+' || codepoints[runs[j].start] == '-' ||
                                                           codepoints[runs[j].start] == ',' || codepoints[runs[j].start] == '.'))) {
                    runs[j].type = BidiRunType::RTL;
                } else if (before == after && before != BidiRunType::NEUTRAL) {
                    // If a neutral is between two strong types of the same direction, it takes on that direction.
                    runs[j].type = before;
                } else {
                    // Otherwise, it takes on the paragraph's base direction.
                    runs[j].type = isOverallRtl ? BidiRunType::RTL : BidiRunType::LTR;
                }
            }
        }

        // 3. Merge adjacent runs that now have the same type.
        if (runs.size() > 1) {
            std::vector<BidiRun> merged_runs;
            merged_runs.push_back(runs[0]);
            for (size_t j = 1; j < runs.size(); ++j) {
                if (runs[j].type == merged_runs.back().type) {
                    merged_runs.back().end = runs[j].end;
                } else {
                    merged_runs.push_back(runs[j]);
                }
            }
            runs = merged_runs;
        }
    }

    // 2. Draw the runs in the correct visual order.
    int currentX = startX;
    // One run: an RTL run's characters right to left (mirrored brackets etc.),
    // an LTR or neutral run's in logical order.
    auto drawRun = [&](const auto& run) {
        if (run.type == BidiRunType::RTL) {
            for (int k = run.end - 1; k >= run.start; k--) {
                uint32_t cp = mirrorForRtl(codepoints[k]);
                drawGlyph(currentX, y, cp, color, directToCanvas);
                currentX += fontGetAdvanceForCodepoint(cp);
            }
        } else {
            for (int k = run.start; k < run.end; k++) {
                drawGlyph(currentX, y, codepoints[k], color, directToCanvas);
                currentX += fontGetAdvanceForCodepoint(codepoints[k]);
            }
        }
    };
    // An RTL message: its runs from the last to the first; an LTR one in order.
    if (isOverallRtl) for (int j = (int)runs.size() - 1; j >= 0; j--) drawRun(runs[j]);
    else              for (const auto& run : runs) drawRun(run);
    return currentX - startX;
}

DisplayManager::BidiRunType DisplayManager::getRunType(uint32_t codepoint) {
    if (isHebrew(codepoint)) return BidiRunType::RTL;
    // Latin letters, numbers, and common time/date punctuation are LTR.
    if ((codepoint >= 'a' && codepoint <= 'z') || 
        (codepoint >= 'A' && codepoint <= 'Z') ||
        (codepoint >= '0' && codepoint <= '9') ||
        (codepoint == ':') || (codepoint == '/')) {
        return BidiRunType::LTR;
    }
    // Everything else (spaces, separators like '•') is neutral.
    return BidiRunType::NEUTRAL;
}

// =============================================================================
// Calibration mode
// =============================================================================
// Content: the clock's normal slides, with the window frame on top (update()).
// Pattern: the whole panel with numbered edges - ticks every 5 px, longer
// every 10, numbers every 40 (top/bottom) and 20 (left/right) - so you can
// read through the case's slit which numbers are visible, and type them as
// the window's X / Y / W / H; the frame shows where the clock puts it now.
// The numbers use the tiny 5x7 font below (the clock's own font is far too
// big for a ruler); only pixels and rectangles, so the PC simulator draws the
// same.

// 5x7 glyphs, one byte per row, bit 4 = leftmost column: 0-9, then X Y W H.
static const uint8_t TINY_GLYPHS[][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
};
static const int TINY_W = 6;   // glyph + 1 px gap

// Width of `s` in the tiny font (digits, X Y W H, spaces).
static int tinyWidth(const char* s) { return (int)strlen(s) * TINY_W - 1; }

static void tinyText(Arduino_GFX* g, int x, int y, const char* s, uint16_t color) {
    for (; *s; s++, x += TINY_W) {
        const char* order = "0123456789XYWH";
        const char* at = strchr(order, *s);
        if (*s == ' ' || !at) continue;
        const uint8_t* rows = TINY_GLYPHS[at - order];
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (rows[r] & (0x10 >> c)) g->drawPixel(x + c, y + r, color);
    }
}

void DisplayManager::setCalibrationPattern(bool on) { s_calibPattern = on; }

void DisplayManager::updateCalibration() {
    int64_t now = Uptime::ms();
    if (now - _lastScrollMs < 200) return;   // a still picture: redraw 5x a second (frame on top moves)
    _lastScrollMs = now;
    if (!_canvas) return;

    const int W = TFT_WIDTH, H = TFT_HEIGHT;
    _canvas->fillScreen(COLOR_BACKGROUND);
    char s[40];
    for (int x = 0; x < W; x += 5) {   // top and bottom edges
        int len = (x % 10 == 0) ? 6 : 3;
        _canvas->fillRect(x, 0, 1, len, COLOR_WHITE);
        _canvas->fillRect(x, H - len, 1, len, COLOR_WHITE);
        if (x % 40 == 0 && x > 0) {
            snprintf(s, sizeof(s), "%d", x);
            int lx = std::max(0, std::min(W - tinyWidth(s), x - tinyWidth(s) / 2));
            tinyText(_canvas, lx, 8, s, COLOR_GOLD);
            tinyText(_canvas, lx, H - 15, s, COLOR_GOLD);
        }
    }
    for (int y = 0; y < H; y += 5) {   // left and right edges
        int len = (y % 10 == 0) ? 6 : 3;
        _canvas->fillRect(0, y, len, 1, COLOR_WHITE);
        _canvas->fillRect(W - len, y, len, 1, COLOR_WHITE);
        if (y % 20 == 0 && y >= 20 && y <= H - 20) {
            snprintf(s, sizeof(s), "%d", y);
            tinyText(_canvas, 9, y - 3, s, COLOR_GOLD);
            tinyText(_canvas, W - 9 - tinyWidth(s), y - 3, s, COLOR_GOLD);
        }
    }
    // Where the window is now, in the middle.
    snprintf(s, sizeof(s), "X %d  Y %d  W %d  H %d", _winX, _winY, _winW, _winH);
    tinyText(_canvas, (W - tinyWidth(s)) / 2, H / 2 - 3, s, COLOR_CYAN);
}
void DisplayManager::drawCalibBorder() {
    if (!_isInitialized) return;
    const uint16_t onColor  = COLOR_CYAN;
    const uint16_t offColor = COLOR_BACKGROUND;
    const int dashLen = 6;

    int x0 = _winX,             y0 = _winY;
    int x1 = _winX + _winW - 1, y1 = _winY + _winH - 1;
    int phase = _calibBorderPhase % (dashLen * 2);

    auto borderPixel = [&](int px, int py, int pos) {
        bool on = (((pos + phase) % (dashLen * 2)) < dashLen);
        if (px < 0 || py < 0 || px >= TFT_WIDTH || py >= TFT_HEIGHT) return;
        _canvas->drawPixel(px, py, on ? onColor : offColor);
    };

    int pos = 0;
    for (int x = x0; x <= x1; x++) borderPixel(x, y0, pos++);
    for (int y = y0+1; y <= y1; y++) borderPixel(x1, y, pos++);
    for (int x = x1-1; x >= x0; x--) borderPixel(x, y1, pos++);
    for (int y = y1-1; y >  y0; y--) borderPixel(x0, y, pos++);
}
