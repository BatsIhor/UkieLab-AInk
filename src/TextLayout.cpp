#include "TextLayout.h"
#include "FontRegistry.h"

static const int MAX_LINES = 64;

int16_t TextLayout::measureString(EPD& epd, const String& text, int start, int len,
                                   const GFXfont* font)
{
    if (!font || len <= 0) return 0;

    String sub = text.substring(start, start + len);
    int16_t x1, y1;
    uint16_t w, h;
    epd.setFont(font);
    epd.getTextBounds(sub, 0, 0, &x1, &y1, &w, &h);
    return (int16_t)w;
}

void TextLayout::wordWrap(EPD& epd, const String& text, const GFXfont* font,
                          int16_t maxWidth, Line* lines, int maxLines, int& lineCount)
{
    lineCount = 0;
    if (text.length() == 0 || !font) return;

    int textLen = text.length();
    int lineStart = 0;

    while (lineStart < textLen && lineCount < maxLines) {
        // Find end of current line (look for \n)
        int nlPos = text.indexOf('\n', lineStart);
        int lineEnd = (nlPos >= 0) ? nlPos : textLen;

        if (maxWidth <= 0) {
            // No wrapping
            lines[lineCount].start = lineStart;
            lines[lineCount].len = lineEnd - lineStart;
            lines[lineCount].width = measureString(epd, text, lineStart, lineEnd - lineStart, font);
            lineCount++;
            lineStart = lineEnd + 1;
            continue;
        }

        // Try to fit as much as possible within maxWidth
        int bestBreak = lineStart;
        int lastSpace = -1;

        for (int i = lineStart; i <= lineEnd; i++) {
            if (i < lineEnd && text[i] == ' ') {
                lastSpace = i;
            }

            int16_t w = measureString(epd, text, lineStart, i - lineStart, font);
            if (w > maxWidth && i > lineStart) {
                // Exceeded width
                if (lastSpace > lineStart) {
                    bestBreak = lastSpace;
                } else {
                    bestBreak = i - 1;  // character wrap
                    if (bestBreak <= lineStart) bestBreak = lineStart + 1;
                }
                break;
            }
            bestBreak = i;
        }

        if (bestBreak == lineEnd) {
            // Whole segment fits
            lines[lineCount].start = lineStart;
            lines[lineCount].len = lineEnd - lineStart;
            lines[lineCount].width = measureString(epd, text, lineStart, lineEnd - lineStart, font);
            lineCount++;
            lineStart = lineEnd + 1;
        } else {
            // Wrap at bestBreak
            lines[lineCount].start = lineStart;
            lines[lineCount].len = bestBreak - lineStart;
            lines[lineCount].width = measureString(epd, text, lineStart, bestBreak - lineStart, font);
            lineCount++;

            // Skip space at break point
            lineStart = bestBreak;
            if (lineStart < textLen && text[lineStart] == ' ') lineStart++;
        }
    }
}

TextLayout::LayoutResult TextLayout::measure(EPD& epd, const String& text, const LayoutParams& params)
{
    LayoutResult result = {0, 0, 0, false};
    const GFXfont* font = params.font;
    if (!font) font = FontRegistry::findClosest("sans", 12, false);

    Line lines[MAX_LINES];
    int lineCount = 0;
    wordWrap(epd, text, font, params.maxWidth, lines, MAX_LINES, lineCount);

    int lineH = (int)(font->yAdvance * params.lineHeight);
    result.lineCount = lineCount;
    result.height = lineCount > 0 ? lineCount * lineH : 0;

    for (int i = 0; i < lineCount; i++) {
        if (lines[i].width > result.width) result.width = lines[i].width;
    }

    if (params.maxHeight > 0 && result.height > params.maxHeight) {
        result.truncated = true;
    }

    return result;
}

TextLayout::LayoutResult TextLayout::render(EPD& epd, const String& text,
                                             const LayoutParams& params, uint16_t color)
{
    LayoutResult result = {0, 0, 0, false};
    const GFXfont* font = params.font;
    if (!font) font = FontRegistry::findClosest("sans", 12, false);

    // For OVERFLOW_SHRINK, try progressively smaller fonts
    const GFXfont* renderFont = font;
    if (params.overflow == OVERFLOW_SHRINK && params.maxWidth > 0) {
        LayoutResult meas = measure(epd, text, params);
        if (meas.height > params.maxHeight && params.maxHeight > 0) {
            // Try smaller fonts
            const int sizes[] = {24, 18, 12, 9};
            const char* family = "sans";
            for (int s = 0; s < 4; s++) {
                const GFXfont* smaller = FontRegistry::findClosest(family, sizes[s], false);
                if (!smaller) continue;

                LayoutParams testParams = params;
                testParams.font = smaller;
                LayoutResult testMeas = measure(epd, text, testParams);
                if (testMeas.height <= params.maxHeight || sizes[s] == 9) {
                    renderFont = smaller;
                    break;
                }
            }
        }
    }

    Line lines[MAX_LINES];
    int lineCount = 0;
    wordWrap(epd, text, renderFont, params.maxWidth, lines, MAX_LINES, lineCount);

    int lineH = (int)(renderFont->yAdvance * params.lineHeight);
    int totalH = lineCount * lineH;

    // Compute Y start based on vertical alignment
    int16_t startY = params.y;
    if (params.vAlign == VALIGN_MIDDLE && params.maxHeight > 0) {
        startY = params.y + (params.maxHeight - totalH) / 2;
    } else if (params.vAlign == VALIGN_BOTTOM && params.maxHeight > 0) {
        startY = params.y + params.maxHeight - totalH;
    }

    // Add baseline offset (GFXfont y = baseline)
    startY += renderFont->yAdvance;

    epd.setFont(renderFont);
    epd.setTextColor(color);

    int maxRenderedLines = lineCount;
    if (params.maxHeight > 0) {
        int maxFit = params.maxHeight / lineH;
        if (maxFit < maxRenderedLines) {
            maxRenderedLines = maxFit;
            result.truncated = true;
        }
    }

    result.height = maxRenderedLines * lineH;
    result.lineCount = maxRenderedLines;

    for (int i = 0; i < maxRenderedLines; i++) {
        String lineText = text.substring(lines[i].start, lines[i].start + lines[i].len);

        // Handle truncation with ellipsis on last line
        if (result.truncated && i == maxRenderedLines - 1 && params.overflow == OVERFLOW_TRUNCATE) {
            lineText += "...";
        }

        int16_t lineW = lines[i].width;
        if (lineW > result.width) result.width = lineW;

        int16_t drawX = params.x;
        if (params.hAlign == ALIGN_CENTER) {
            if (params.maxWidth > 0) {
                // Center within the maxWidth box starting at x
                drawX = params.x + (params.maxWidth - lineW) / 2;
            } else {
                // No maxWidth: x is the center point, offset left by half width
                drawX = params.x - lineW / 2;
            }
        } else if (params.hAlign == ALIGN_RIGHT) {
            if (params.maxWidth > 0) {
                drawX = params.x + params.maxWidth - lineW;
            } else {
                // No maxWidth: x is the right edge
                drawX = params.x - lineW;
            }
        }

        epd.setCursor(drawX, startY + i * lineH);
        epd.print(lineText);
    }

    return result;
}
