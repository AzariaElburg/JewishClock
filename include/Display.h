#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "Config.h"
#include <vector>
#include <memory>
#include "Font.h"
#include <unordered_map>
#include "OnScreenElement.h"
#include "FS.h"

// How many frames a BMP holds (side by side). The clock's own encoders put
// the count in biXPelsPerMeter (normally a DPI field, real values are in the
// thousands): 1 = a still picture, 2..64 dividing the width = that many
// frames - at any width, since replaced pictures may be narrow or wide
// (2026-10-22). Anything else (an old file): a strip whose width is a clean
// multiple of its height is square frames. The one rule for the display
// and for the upload checks (WebUI.cpp); Data/index.html's decodeBmp() matches.
inline int32_t bmpFrameCount(int32_t width, int32_t height, int32_t xPelsPerMeter) {
    if (xPelsPerMeter == 1) return 1;
    if (xPelsPerMeter >= 2 && xPelsPerMeter <= 64 && width % xPelsPerMeter == 0) return xPelsPerMeter;
    if (width > height && height > 0 && width % height == 0) return width / height;
    return 1;
}

// ─── Render modes ─────────────────────────────────────────────────────────────

// The default std::hash specialization for Arduino String objects can be problematic
// with some toolchains. A custom hash functor is a more robust solution.
struct StringHash {
    size_t operator()(const String& s) const {
        // A simple and effective hash function for strings.
        size_t h = 0;
        for(char c : s) {
            h = h * 31 + c;
        }
        return h;
    }
};

enum class DisplayMode {
    Normal,
    Calibration,
    Setup,
    Blank
};

// ─── Colors (RGB565) ──────────────────────────────────────────────────────────

#define COLOR_BLACK       0x0000
#define COLOR_WHITE       0xFFFF
#define COLOR_YELLOW      0xFFE0
#define COLOR_GOLD        0xFEA0
#define COLOR_BLUE        0x001F
#define COLOR_CYAN        0x07FF
#define COLOR_RED         0xF800
#define COLOR_GREEN       0x07E0
#define COLOR_DARKGREY    0x39E7

#define COLOR_TEXT_NORMAL  COLOR_WHITE
#define COLOR_TEXT_HOLIDAY COLOR_GOLD
#define COLOR_BACKGROUND   COLOR_BLACK
#define COLOR_TRANSPARENT  0xF81F // Magenta
#define COLOR_BORDER_CALIB COLOR_CYAN

#define MAX_MESSAGE_LEN 1024

// ─── Display Manager ──────────────────────────────────────────────────────────

class DisplayManager {
public:
    enum BmpFormat {
        BMP_FMT_UNKNOWN,
        BMP_FMT_565, // Standard RGB565
        BMP_FMT_555  // Standard xRGB1555
    };

    // A BMP decoded once into a flat top-down RGB565 buffer (PSRAM), so repeat
    // draws (every scroll frame) never touch LittleFS again. For an animated
    // sprite sheet this holds all frames side by side, same as on disk. Also
    // the single source of truth for a path's frame count/type - analyzeElements()
    // reads these same fields instead of re-opening and re-parsing the file
    // itself, so there is exactly one place that decides what a given BMP is.
    struct CachedBmp {
        uint16_t* pixels     = nullptr;   // = data.get(), what the draw loop reads
        // Owns `pixels`; shared with a download of the picture (ImageFile),
        // so forgetImage() can't free them under a download still sending.
        std::shared_ptr<uint16_t> data;
        // The file's bytes before its pixels - set only when the whole file
        // can be rebuilt from memory byte for byte (see ImageFile).
        std::shared_ptr<const std::vector<uint8_t>> fileHeader;
        int32_t   width      = 0; // Full on-disk width (all frames, if animated)
        int32_t   height     = 0;
        int32_t   frameCount = 1;
        int32_t   frameMs    = 0;     // ms per frame stored in the file (biYPelsPerMeter), 0 = none
        size_t    fileSize   = 0;
        bool      valid      = false; // false = permanently failed to load (missing/corrupt/OOM)
        // A picture taller than the window, shrunk to its height (getShownBmp()),
        // frames side by side like `pixels`. Made once per window height.
        std::shared_ptr<uint16_t> fit;
        int32_t   fitWidth   = 0;     // all frames
        int32_t   fitHeight  = 0;
        int       fitForH    = 0;     // the height it was made (or failed) for - pictureMaxH()
        bool fitted(int h) const { return fit && fitForH == h; }
    };

    public:
    static bool begin();
    static void setBrightness(uint32_t duty);   // 0..BACKLIGHT_DUTY_MAX (Config.h)

    static void setWindow(int x, int y, int w, int h);
    static void setTearUs(int us);   // where the tear lines stand - Config.h DEFAULT_CALIB_TEAR_US

    static void setMessage(const char* utf8text, uint16_t color, const AppConfig& cfg, bool isRtl);
    static void setMessage(const std::vector<OnScreenElement>& elements, uint16_t color, const AppConfig& cfg);
    static std::vector<OnScreenElement> getCurrentMessageElements();
    // Copies the last finished frame (TFT_WIDTH x TFT_HEIGHT, RGB565) into dst,
    // under DisplayLock - never a half-drawn one - plus, from the same instant,
    // the message it shows and the scroll position. For /api/debug/screenshot.
    static bool copyFrame(uint16_t* dst, std::vector<OnScreenElement>* elements = nullptr, int* scrollX = nullptr);

    // Scroll speed in pixels per frame (called every ~16 ms).
    // Default: SCROLL_SPEED_DEFAULT from Config.h.
    static void setScrollSpeed(int pixelsPerFrame);
    static int  getScrollSpeed();

    static int getWindowWidth();
    static int measureString(const char* utf8text);

    static void update(const AppConfig& cfg);

    static void setMode(DisplayMode mode);
    static DisplayMode getMode();
    // Calibration mode shows either the live content (false) or a test
    // pattern of the whole panel with numbered edges (true) - both with the
    // window frame on top. Any task.
    static void setCalibrationPattern(bool on);
    static void showSetupScreen(const char* ssid, const char* ip);

    static void clearWindow();
    static void clearFull();

    static void resetScroll(const AppConfig& cfg);
    static bool scrollComplete();

    // Alternating-mode long-item scroll status, queried by MessageAggregator
    // to decide when to advance to the next item (see ALT_SCROLL below).
    static bool     isAlternatingScrollActive();

    // Nothing on the screen moves right now (a still message, the hold before
    // or after an alternating item's scroll, the "Updating..." frame) - a
    // flash write now pauses nothing anyone can see. Main loop only.
    static bool     isStill();
    // For a write to flash the clock does by itself (log, calendar cache,
    // learned settings): true when now is a good moment - the screen stands
    // still - or once it has waited a while (Display.cpp TUNABLES) since
    // waitingSinceMs; in single-scroll mode (never still) after a short
    // wait that gathers a burst into one write. Also true
    // before the screen draws at all. Any task.
    static bool     goodMomentToWrite(uint32_t waitingSinceMs);
    static uint32_t getAlternatingScrollRevealElapsedMs();

    // SingleScroll's round-advance signal: true once the currently-showing
    // combined ticker has scrolled through one full lap since this was last
    // consumed. Edge-triggered (reading it clears it) - MessageAggregator
    // uses this instead of a wall-clock timer, so "every Nth round" means
    // every Nth full showing of the message, and a new round can never be
    // requested while the previous one is still mid-transition (see
    // MessageAggregator::loop()'s SingleScroll round-advance comment).
    static bool consumeSingleScrollLapCompleted();

    // ── Live display-link test (Advanced settings) ──────────────────────────
    // Runtime only, never saved. Safe from any task.
    static void     setFrozen(bool frozen);       // true = no drawing, no SPI traffic at all
    static bool     isFrozen();
    static uint32_t getLastBlitUs();              // time to send the last frame (not the wait for TE)
    // Visible hitches since boot: gaps between frames over 100 ms (big: over
    // 250 ms) - FRAME_HITCH_US in Display.cpp. Any task.
    static uint32_t getHitches(bool big);
    static uint32_t getTePeriodUs();              // the panel's redraw period from its TE pulse, 0 = no TE

    // A still message while an update is written to flash: drawn once, then
    // no more drawing until endStill(). Writing flash pauses every frame
    // halfway, so a moving screen flickers for the whole ~30 s upload
    // (seen 2026-10-18). Main loop only (main.cpp); separate from setFrozen().
    static void     showStill(const char* utf8text, bool isRtl);
    static void     endStill();

    // Loads every image in /IMG into memory now, so the first time one is
    // shown doesn't stall the screen while it's read from flash (up to 1.1 s
    // for the candles animation). Called once at startup.
    static void     preloadImages();
    // Drops an image from that cache - an event photo that was replaced or
    // deleted; the next draw reads the file again.
    static void     forgetImage(const char* path);
    // True if the file is a BMP the screen can draw (loads it into that cache).
    static bool     isImageUsable(const char* path);
    // The same answer from memory only, without touching flash: true if the
    // picture is in that cache and drawable. Every picture is in it -
    // preloaded at startup, uploads load theirs, deletes forget theirs.
    // For the main loop: a flash check waits out any other task's flash
    // write (measured ~230 ms, "SLOW STEP: MessageAggregator::loop").
    static bool     hasImage(const char* path);

    // A picture's file, rebuilt from that cache: identical bytes, without
    // reading flash - a flash read pauses the screen (0.35-0.5 s seen while
    // the page loaded pictures, 2026-10-22). For the web server's downloads;
    // keeps the pixels alive by itself, needs no lock while sending.
    struct ImageFile {
        std::shared_ptr<uint16_t> pixels;
        std::shared_ptr<const std::vector<uint8_t>> header;
        int32_t  width = 0, height = 0;
        uint32_t stride = 0;   // bytes per row in the file
        size_t   size = 0;     // the file's size
        // The file's bytes from `index` on, at most maxLen of them; the count.
        size_t read(uint8_t* buf, size_t maxLen, size_t index) const;
    };
    // False if the picture isn't in memory (or its file isn't one memory can
    // rebuild exactly) - then send the file. Never loads anything.
    static bool     imageFile(const char* path, ImageFile& out);
    // The pictures in memory whose path starts with `folder` ("/IMG/"), as
    // the files describe them - for lists, without opening any file (which
    // took 0.9 s for Settings > Pictures' list and froze the screen).
    struct ImageEntry { String path; int32_t frameWidth, height, frames, frameMs; size_t fileSize; };
    static std::vector<ImageEntry> imagesIn(const char* folder);

private:
    enum class BidiRunType { LTR, RTL, NEUTRAL };
    static BidiRunType getRunType(uint32_t codepoint);
    // Unicode's Bidi_Mirrored punctuation (parentheses, brackets, braces) must
    // have its GLYPH swapped, not just its position, when drawn within a
    // right-to-left run - otherwise a "(" that logically opens a parenthetical
    // ends up visually facing the wrong way once RTL reversal draws it last.
    // Scoped to the punctuation this project's font/content actually uses,
    // not the full Unicode mirroring table.
    static uint32_t mirrorForRtl(uint32_t codepoint) {
        switch (codepoint) {
            case '(': return ')';
            case ')': return '(';
            case '[': return ']';
            case ']': return '[';
            case '{': return '}';
            case '}': return '{';
            case '<': return '>';
            case '>': return '<';
            default:  return codepoint;
        }
    }
    static bool isHebrew(uint32_t codepoint) {
        // The Hebrew Unicode block (U+0590 to U+05FF) contains all characters with a
        // strong Right-to-Left (RTL) directional property. This includes letters,
        // vowel points (nikkud), and Hebrew-specific punctuation like Geresh and Gershayim.
        return (codepoint >= 0x0590 && codepoint <= 0x05FF);
    }

    static uint32_t decodeUTF8(const uint8_t** ptr);
    static void     drawGlyph(int x, int y, uint32_t codepoint, uint16_t color, bool directToCanvas = false);
    static int      drawString(int startX, int y, const char* utf8text, uint16_t color, bool directToCanvas = false, bool isOverallRtl = false);
    static void     drawBmp(const char *path, int x, int y, int frame = 0, int frameWidth = 0);
    static const CachedBmp* getCachedBmp(const char* path);
    static const CachedBmp* getShownBmp(const char* path);
    static CachedBmp        loadBmp(const char* path, int maxHeight);

    static void sendFrame(bool wholePanel);
    static int  pictureMaxH();
    static void keepTeOn();
    static void drawCalibBorder();
    static void updateCalibration();
    static void drawSetupUI();

    static Arduino_ESP32SPI* _bus;   // concrete type: Arduino_DataBus has no virtual destructor, so deleting through it is undefined
    static Arduino_GFX*      _gfx;
    static Arduino_GFX*      _canvas;
    static DisplayMode       _mode;
    static bool              _isInitialized;
    static bool              _displayReal;
    static uint32_t          _lastInitAttemptMs;

    static int _winX, _winY, _winW, _winH;

    // Private helper to calculate message properties without affecting state
    static void analyzeElements(std::vector<OnScreenElement>& elements, const AppConfig& cfg, int& outWidth, bool& outIsRtl);
    static std::unordered_map<String, CachedBmp, StringHash> _bmpPixelCache;

    enum class DisplayState {
        STATIC,             // Short message, centered and not moving.
        LOOPING_SCROLL,     // Long message, scrolling in a continuous loop (SingleScroll mode).
        TRANSITIONING,      // Scrolling from an old message to a new one (SingleScroll mode).
        ALT_SCROLL          // Alternating mode, item too long to fit: hold, then scroll (possibly
                             // several continuous stitched passes), then hold again.
    };
    enum class AltScrollPhase {
        HOLD,               // Brief pause at the start position before scrolling begins.
        SCROLLING,          // Continuously scrolling - may flow through several stitched passes.
        DONE                // Fully finished; scrollX frozen until the item changes.
    };
    // State Machine
    static DisplayState _displayState;
    static AltScrollPhase _altScrollPhase;
    static uint32_t        _altItemStartMs;   // millis() when the current alternating item was hard-cut in.
    static uint32_t        _altCycleStartMs;  // millis() when the current pass's scrolling began (for measuring its duration).
    static uint32_t        _altRevealDoneAtMs; // millis() when the final pass finished revealing (0 = not yet).
    static bool             _altCheckpointArmed; // True while waiting to detect the next reveal-complete
                                                  // crossing (one per pass); disarmed right after handling
                                                  // one, re-armed at the next seamless-wrap boundary.
    static bool             _singleScrollLapCompleted; // Set on every LOOPING_SCROLL wrap; see consumeSingleScrollLapCompleted().
    static std::vector<OnScreenElement> _currentElements;
    static std::vector<OnScreenElement> _nextElements;
    static int         _currentMessagePixelWidth;
    static int         _nextMessagePixelWidth;
    static bool        _isCurrentRtl;
    static bool        _isNextRtl;
    static bool        _hasNewMessage;
    static uint16_t _messageColor;
    static int      _scrollX;
    static int      _scrollSpeed;
    static uint32_t _lastScrollMs;

    static int      _calibBorderPhase;
    static uint32_t _lastCalibMs;
};