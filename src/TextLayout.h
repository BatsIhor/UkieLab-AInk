#pragma once

#include <Arduino.h>
#include "EPD.h"
#include "gfxfont.h"

// Text layout engine with word wrapping, alignment, and overflow handling.
class TextLayout {
public:
    enum HAlign { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };
    enum VAlign { VALIGN_TOP, VALIGN_MIDDLE, VALIGN_BOTTOM };
    enum Overflow { OVERFLOW_CLIP, OVERFLOW_TRUNCATE, OVERFLOW_SHRINK };

    struct LayoutParams {
        int16_t x = 0;
        int16_t y = 0;
        int16_t maxWidth = 0;    // 0 = no limit
        int16_t maxHeight = 0;   // 0 = no limit
        HAlign hAlign = ALIGN_LEFT;
        VAlign vAlign = VALIGN_TOP;
        Overflow overflow = OVERFLOW_CLIP;
        float lineHeight = 1.2f;
        const GFXfont* font = nullptr;
    };

    struct LayoutResult {
        int16_t width;
        int16_t height;
        int lineCount;
        bool truncated;
    };

    // Render text with layout params to EPD
    static LayoutResult render(EPD& epd, const String& text, const LayoutParams& params,
                               uint16_t color = GxEPD_BLACK);

    // Measure text without rendering
    static LayoutResult measure(EPD& epd, const String& text, const LayoutParams& params);

private:
    struct Line {
        int start;
        int len;
        int16_t width;
    };

    static void wordWrap(EPD& epd, const String& text, const GFXfont* font,
                         int16_t maxWidth, Line* lines, int maxLines, int& lineCount);
    static int16_t measureString(EPD& epd, const String& text, int start, int len,
                                 const GFXfont* font);
};
