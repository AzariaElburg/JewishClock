#define FONT_DEFINE_DATA   // the one place the font tables are defined - see Font.h
#include "Font.h"
#include "Config.h"

// See the "PROPORTIONAL METRICS" comment in Font.h for why this data and its
// computation live here, in exactly one .cpp file, instead of as `inline`
// functions filling `static` arrays in the header.
//
// Config.h is only included here, in this one .cpp file, purely for its two
// FONT_..._DEFAULT constants below - it never gets pulled into Font.h, so
// nothing that includes Font.h (which is most files, via Display.h) is
// affected. main.cpp pushes the actual configured values in via
// fontSetSpacing() at startup and whenever they change; these are just this
// module's own fallback until that first call.
// === TUNABLES ===
// Extra room on each side of the colon, on top of the normal gap: with
// only the letters' gap it looked squeezed between the digits of a time
// (2026-10-19). The left part is empty columns before its ink
// (fontGetGlyphSourceCol() returns FONT_NO_COLUMN for them).
static const uint8_t COLON_SIDE_PAD_PX = 2;

static uint8_t g_charGapPx = FONT_CHAR_GAP_DEFAULT; // Extra px after each glyph's ink, for legibility.
static uint8_t g_wordGapPx = FONT_WORD_GAP_DEFAULT; // Advance for glyphs with no ink at all (space).
static uint8_t g_widthScalePct = FONT_WIDTH_SCALE_DEFAULT; // 100 = glyphs drawn at their natural width.

static uint8_t g_glyphAdvance[FONT_GLYPH_COUNT];
static uint8_t g_glyphLeftBearing[FONT_GLYPH_COUNT];
// Ink width BEFORE (raw) and AFTER (scaled) the width-scale percentage. Raw is
// needed by drawGlyph() to map each drawn (scaled) column back to the source
// bitmap column it should sample - see fontGetGlyphSourceCol().
static uint8_t g_glyphRawInkWidth[FONT_GLYPH_COUNT];
static uint8_t g_glyphScaledInkWidth[FONT_GLYPH_COUNT];
static uint8_t g_glyphLeftPad[FONT_GLYPH_COUNT];   // empty columns before the ink (the colon's)
static bool    g_glyphMetricsReady = false;

void fontComputeMetrics() {
    const int colonIdx = fontGetGlyphIdx(':');
    for (int i = 0; i < FONT_GLYPH_COUNT; i++) {
        const uint8_t* bitmap = &FONT_DATA[(uint32_t)i * FONT_BYTES_PER_GLYPH];
        int minX = -1, maxX = -1;
        for (int row = 0; row < FONT_H; row++) {
            for (int col = 0; col < FONT_W; col++) {
                if (!fontGetCoverage(bitmap, col, row)) continue;
                if (minX < 0 || col < minX) minX = col;
                if (col > maxX) maxX = col;
            }
        }
        g_glyphLeftPad[i] = 0;
        if (minX < 0) {
            // No lit pixels at all (space, or any undefined glyph slot).
            g_glyphLeftBearing[i] = 0;
            g_glyphRawInkWidth[i] = 0;
            g_glyphScaledInkWidth[i] = 0;
            g_glyphAdvance[i] = g_wordGapPx;
        } else {
            int pad = (i == colonIdx) ? COLON_SIDE_PAD_PX : 0;
            g_glyphLeftPad[i] = (uint8_t)pad;
            int rawWidth = maxX - minX + 1 + pad;
            // Same percentage for every glyph, applied to ITS OWN ink width -
            // a wide glyph loses proportionally more pixels than a narrow one,
            // but the ratio between any two glyphs' widths is preserved.
            int scaledWidth = (rawWidth * g_widthScalePct + 50) / 100; // round to nearest
            if (scaledWidth < 1) scaledWidth = 1; // never fully vanish
            g_glyphLeftBearing[i] = (uint8_t)minX;
            g_glyphRawInkWidth[i] = (uint8_t)rawWidth;
            g_glyphScaledInkWidth[i] = (uint8_t)scaledWidth;
            g_glyphAdvance[i] = (uint8_t)(scaledWidth + g_charGapPx + pad);
        }
    }
    g_glyphMetricsReady = true;
}

void fontSetSpacing(uint8_t charGapPx, uint8_t wordGapPx) {
    g_charGapPx = charGapPx;
    g_wordGapPx = wordGapPx;
    fontComputeMetrics(); // Recompute the whole table now, from the real bitmaps.
}

void fontGetSpacing(uint8_t& charGapPx, uint8_t& wordGapPx) {
    charGapPx = g_charGapPx;
    wordGapPx = g_wordGapPx;
}

void fontSetWidthScale(uint8_t percent) {
    g_widthScalePct = percent;
    fontComputeMetrics();
}

uint8_t fontGetWidthScale() {
    return g_widthScalePct;
}

uint8_t fontGetGlyphAdvance(uint8_t index) {
    if (!g_glyphMetricsReady) fontComputeMetrics();
    if (index >= FONT_GLYPH_COUNT) return FONT_W;
    return g_glyphAdvance[index];
}

uint8_t fontGetGlyphLeftBearing(uint8_t index) {
    if (!g_glyphMetricsReady) fontComputeMetrics();
    if (index >= FONT_GLYPH_COUNT) return 0;
    return g_glyphLeftBearing[index];
}

uint8_t fontGetGlyphScaledInkWidth(uint8_t index) {
    if (!g_glyphMetricsReady) fontComputeMetrics();
    if (index >= FONT_GLYPH_COUNT) return 0;
    return g_glyphScaledInkWidth[index];
}

// For drawn (scaled) column `outCol` (0-based, within [0, scaledInkWidth)),
// returns which column of the REAL bitmap to sample - nearest-neighbor along
// the glyph's own ink range. At 100% scale this returns leftBearing+outCol
// exactly, i.e. the unscaled 1:1 mapping. FONT_NO_COLUMN = one of the empty
// columns before the ink (left pad): draw nothing there.
uint8_t fontGetGlyphSourceCol(uint8_t index, uint8_t outCol) {
    if (!g_glyphMetricsReady) fontComputeMetrics();
    if (index >= FONT_GLYPH_COUNT) return 0;
    uint8_t minX = g_glyphLeftBearing[index];
    uint8_t rawWidth = g_glyphRawInkWidth[index];
    uint8_t scaledWidth = g_glyphScaledInkWidth[index];
    if (scaledWidth == 0) return minX;
    int srcOffset = ((int)outCol * (int)rawWidth) / (int)scaledWidth;
    if (srcOffset >= rawWidth) srcOffset = rawWidth - 1;
    if (srcOffset < g_glyphLeftPad[index]) return FONT_NO_COLUMN;
    return (uint8_t)(minX + srcOffset - g_glyphLeftPad[index]);
}

uint8_t fontGetAdvanceForCodepoint(uint32_t cp) {
    uint8_t idx = fontGetGlyphIdx((uint16_t)cp);
    if (idx >= FONT_GLYPH_COUNT) return FONT_W;
    return fontGetGlyphAdvance(idx);
}
