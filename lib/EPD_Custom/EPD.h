#pragma once

#include <Arduino.h>
#include <SPI.h>
#include "gfxfont.h"

// Color constants (RGB565 format, matching GxEPD2 values)
#define GxEPD_BLACK  0x0000
#define GxEPD_WHITE  0xFFFF
#define GxEPD_RED    0xF800

// Display geometry — override via build flags for different panels
#ifndef EPD_WIDTH
#define EPD_WIDTH    800
#endif
#ifndef EPD_HEIGHT
#define EPD_HEIGHT   480
#endif
#define EPD_BUF_SIZE ((EPD_WIDTH * EPD_HEIGHT) / 8)

// Refresh modes
enum EPDRefreshMode {
    EPD_REFRESH_FULL,     // Full display update with waveform
    EPD_REFRESH_PARTIAL,  // Partial window update
    EPD_REFRESH_FAST      // Fast update (reduced ghosting clearance)
};

// ============================================================
// EPD -- custom full-buffer driver for B/W e-paper displays
// Supported panels:
//   - GD7965 / UC8179  (7.5" 800x480)  — default
//   - SSD1677           (10.2" 960x640) — define EPD_PANEL_SSD1677
// Under EPD_BW_ONLY, allocates only the black plane buffer.
// ============================================================
class EPD {
public:
    EPD(int16_t cs, int16_t dc, int16_t rst, int16_t busy);
    ~EPD();

    // Initialise hardware and run EPD power-on sequence.
    void init(uint32_t baud, bool initial, uint16_t rst_dur, bool pulldown);

    // Fill frame buffer with a uniform color (BLACK / WHITE).
    void fillScreen(uint16_t color);

    // Equivalent to fillScreen(GxEPD_WHITE).
    void clearScreen();

    // No-op -- API compatibility with GxEPD2 setFullWindow().
    void setFullWindow();

    // Begin paging cycle (marks buffers as ready for drawing).
    void firstPage();

    // Flush buffers to the panel, end paging cycle, return false.
    bool nextPage();

    // Direct flush with refresh mode.
    void display(bool partial);
    void displayWithMode(EPDRefreshMode mode);

    // Partial refresh: update only a rectangular region.
    void partialRefresh(int16_t x, int16_t y, int16_t w, int16_t h);

    // Turn off panel voltages.
    void powerOff();

    // Block until any in-progress refresh completes.
    void waitReady();

    // -- Drawing primitives
    void drawPixel(int16_t x, int16_t y, uint16_t color);
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
    void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
    void drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);
    void fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);
    void fillTriangle(int16_t x0, int16_t y0,
                      int16_t x1, int16_t y1,
                      int16_t x2, int16_t y2, uint16_t color);

    // -- Text rendering
    void setFont(const GFXfont* font);
    void setTextColor(uint16_t color);
    void setCursor(int16_t x, int16_t y);
    void print(const String& text);
    void println(const String& text);
    void getTextBounds(const String& str, int16_t x, int16_t y,
                       int16_t* x1, int16_t* y1, uint16_t* w, uint16_t* h);
    const GFXfont* getFont() const { return _font; }

    // -- Buffer management
    void freeBuffers();
    bool allocateBuffers();
    bool buffersValid() const { return _black != nullptr; }

    // Direct buffer access for framebuffer manager
    uint8_t* getBlackBuffer() { return _black; }
    const uint8_t* getBlackBuffer() const { return _black; }

    // -- Adafruit-GFX compat stubs
    void setRotation(uint8_t r)  { (void)r; }
    void setTextWrap(bool wrap)  { (void)wrap; }
    void setTextSize(uint8_t s)  { (void)s; }
    void drawPaged(void (*cb)(const void*), const void* param) { cb(param); }

private:
    int16_t _cs, _dc, _rst, _busy;

    // Frame buffers (heap-allocated)
#ifdef EPD_BW_ONLY
    // B/W only: single 48KB buffer for black plane
    uint8_t* _black;
#else
    // Tri-color: two 48KB buffers (black + red) in single allocation
    uint8_t* _black;
    uint8_t* _red;
    uint8_t* _combinedBuf;
#endif

    bool _power_is_on;
    bool _hibernating;
    bool _paging_active;
    bool _refresh_pending;  // true after 0x12 refresh trigger, cleared after wait

    const GFXfont* _font;
    uint16_t _textColor;
    int16_t  _cursorX, _cursorY;

    // -- HAL
    void _writeCommand(uint8_t cmd);
    void _writeData(uint8_t data);
    void _waitBusy(const char* msg, uint32_t timeout_ms);
    void _reset(uint16_t rst_dur_ms);

    // -- Panel control
    void _initDisplay(uint16_t rst_dur);
    void _sendBuffersToDisplay();
    void _sendPartialToDisplay(int16_t x, int16_t y, int16_t w, int16_t h);

#ifdef EPD_PANEL_SSD1677
    // SSD1677-specific helpers (10.2" panel)
    void _ssd1677_setPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void _ssd1677_powerOn();
    void _ssd1677_powerOff();
    void _ssd1677_updateFull();
    void _ssd1677_updatePartial();
#endif

    // -- GFX helpers
    void _drawCircleHelper(int16_t x0, int16_t y0, int16_t r,
                           uint8_t cornermask, uint16_t color);
    void _fillCircleHelper(int16_t x0, int16_t y0, int16_t r,
                           uint8_t cornermask, int16_t delta, uint16_t color);

    // -- Text helpers
    void _drawChar(int16_t x, int16_t y, unsigned char c, uint16_t color);
};
