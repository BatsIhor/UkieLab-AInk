#include "EPD.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ============================================================
// Constructor / Destructor
// ============================================================

EPD::EPD(int16_t cs, int16_t dc, int16_t rst, int16_t busy)
    : _cs(cs), _dc(dc), _rst(rst), _busy(busy),
      _black(nullptr),
#ifndef EPD_BW_ONLY
      _red(nullptr), _combinedBuf(nullptr),
#endif
      _power_is_on(false), _hibernating(false), _paging_active(false), _refresh_pending(false),
      _font(nullptr), _textColor(GxEPD_BLACK),
      _cursorX(0), _cursorY(0)
{
    allocateBuffers();
}

EPD::~EPD()
{
    freeBuffers();
}

void EPD::freeBuffers()
{
#ifdef EPD_BW_ONLY
    free(_black);
    _black = nullptr;
#else
    free(_combinedBuf);
    _combinedBuf = nullptr;
    _black = nullptr;
    _red   = nullptr;
#endif
}

bool EPD::allocateBuffers()
{
#ifdef EPD_BW_ONLY
    if (_black) return true;

    _black = (uint8_t*)malloc(EPD_BUF_SIZE);
    if (_black) {
        memset(_black, 0xFF, EPD_BUF_SIZE);
        return true;
    }

    Serial.printf("EPD: buffer alloc FAILED! Need %d bytes, largest free block: %d\n",
                  EPD_BUF_SIZE, ESP.getMaxAllocHeap());
    return false;
#else
    if (_combinedBuf) return true;

    _combinedBuf = (uint8_t*)malloc(2 * EPD_BUF_SIZE);
    if (_combinedBuf) {
        _black = _combinedBuf;
        _red   = _combinedBuf + EPD_BUF_SIZE;
        memset(_black, 0xFF, EPD_BUF_SIZE);
        memset(_red,   0xFF, EPD_BUF_SIZE);
        return true;
    }

    Serial.printf("EPD: buffer alloc FAILED! Need %d bytes, largest free block: %d\n",
                  2 * EPD_BUF_SIZE, ESP.getMaxAllocHeap());
    _black = nullptr;
    _red   = nullptr;
    return false;
#endif
}

// ============================================================
// 1. HAL -- SPI + hardware control
// ============================================================

void EPD::_writeCommand(uint8_t cmd)
{
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(_dc, LOW);
    digitalWrite(_cs, LOW);
    SPI.transfer(cmd);
    digitalWrite(_cs, HIGH);
    SPI.endTransaction();
}

void EPD::_writeData(uint8_t data)
{
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(_dc, HIGH);
    digitalWrite(_cs, LOW);
    SPI.transfer(data);
    digitalWrite(_cs, HIGH);
    SPI.endTransaction();
}

void EPD::_waitBusy(const char* msg, uint32_t timeout_ms)
{
    uint32_t start = millis();
    while (digitalRead(_busy) == LOW) {
        if (millis() - start > timeout_ms) {
            Serial.printf("EPD timeout: %s (%lums)\n", msg, timeout_ms);
            break;
        }
        delay(1);
        yield();  // Feed watchdog, let other FreeRTOS tasks run
    }
}

void EPD::_reset(uint16_t rst_dur_ms)
{
    if (_rst >= 0) {
        digitalWrite(_rst, LOW);
        delay(rst_dur_ms);
        digitalWrite(_rst, HIGH);
        delay(200);
        _waitBusy("reset-settle", 2000);
    }
}

// ============================================================
// 2. EPD init sequence (unified GD7965 + UC8179)
// ============================================================

void EPD::_initDisplay(uint16_t rst_dur)
{
    if (_hibernating) _reset(rst_dur);

    // Power Setting
    _writeCommand(0x01);
    _writeData(0x07);
    _writeData(0x07);
    _writeData(0x3f);
    _writeData(0x3f);

#ifdef EPD_BW_ONLY
    // Panel Setting: KW (B/W) mode — matches working GD7965/GDEW075T7 driver
    _writeCommand(0x00);
    _writeData(0x1f);

    // Resolution Setting: 800 x 480
    _writeCommand(0x61);
    _writeData(EPD_WIDTH  >> 8);
    _writeData(EPD_WIDTH  & 0xFF);
    _writeData(EPD_HEIGHT >> 8);
    _writeData(EPD_HEIGHT & 0xFF);

    // Dual SPI disable
    _writeCommand(0x15);
    _writeData(0x00);

    // VCOM and Data Interval Setting (B/W: LUTKW, N2OCP copy new to old)
    _writeCommand(0x50);
    _writeData(0x29);
    _writeData(0x07);

    // TCON Setting
    _writeCommand(0x60);
    _writeData(0x22);
#else
    // Panel Setting: KWR (tri-color) mode
    _writeCommand(0x00);
    _writeData(0x0F);

    // Resolution Setting
    _writeCommand(0x61);
    _writeData(EPD_WIDTH  >> 8);
    _writeData(EPD_WIDTH  & 0xFF);
    _writeData(EPD_HEIGHT >> 8);
    _writeData(EPD_HEIGHT & 0xFF);

    _writeCommand(0x15);
    _writeData(0x00);

    _writeCommand(0x50);
    _writeData(0x11);
    _writeData(0x07);

    _writeCommand(0x60);
    _writeData(0x22);
#endif
}

void EPD::init(uint32_t baud, bool initial, uint16_t rst_dur, bool pulldown)
{
    (void)initial;
    (void)pulldown;
    (void)baud;

    // Configure GPIO
    pinMode(_cs,   OUTPUT); digitalWrite(_cs,   HIGH);
    pinMode(_dc,   OUTPUT); digitalWrite(_dc,   HIGH);
    if (_rst >= 0) {
        pinMode(_rst, OUTPUT); digitalWrite(_rst, HIGH);
    }
    if (_busy >= 0) {
        pinMode(_busy, INPUT);
    }

    SPI.begin();

    // Hardware reset
    _reset(rst_dur > 0 ? rst_dur : 10);

    // Init display registers
    _initDisplay(rst_dur > 0 ? rst_dur : 10);

    // Power On
    _writeCommand(0x04);
    _waitBusy("PowerOn", 2000);
    _power_is_on = true;
    _hibernating = false;
    Serial.printf("EPD: init complete (BUSY=%d)\n", digitalRead(_busy));
}

// ============================================================
// 3. Buffer management
// ============================================================

void EPD::fillScreen(uint16_t color)
{
    if (!_black) return;
    if (color == GxEPD_BLACK) {
        memset(_black, 0x00, EPD_BUF_SIZE);
    } else {
        // WHITE (default)
        memset(_black, 0xFF, EPD_BUF_SIZE);
    }
#ifndef EPD_BW_ONLY
    if (_red) {
        memset(_red, 0xFF, EPD_BUF_SIZE);
        if (color == GxEPD_RED) {
            memset(_black, 0xFF, EPD_BUF_SIZE);
            memset(_red,   0x00, EPD_BUF_SIZE);
        }
    }
#endif
}

void EPD::clearScreen()
{
    fillScreen(GxEPD_WHITE);
}

void EPD::setFullWindow()
{
    // No-op: API compatibility shim
}

// ============================================================
// 4. Buffer flush -> display
// ============================================================

void EPD::_sendBuffersToDisplay()
{
    if (!_black) { Serial.println("EPD: no buffer!"); return; }

    Serial.printf("EPD: sendBuffers (BUSY=%d, power=%d, refresh_pending=%d)\n",
                  digitalRead(_busy), _power_is_on, _refresh_pending);

    // Only wait for idle if a previous refresh is in progress
    if (_refresh_pending) {
        _waitBusy("idle", 25000);
        _refresh_pending = false;
    }

    if (_power_is_on) {
        Serial.println("EPD: power off before refresh");
        _writeCommand(0x02);
        _waitBusy("PowerOff-pre", 5000);
        _power_is_on = false;
    }

    Serial.println("EPD: reset + init");
    _reset(10);
    _initDisplay(10);
    _writeCommand(0x04);
    delay(10);
    _waitBusy("PowerOn", 1000);
    _power_is_on = true;

    // Count non-white bytes for debug
    uint32_t nonWhite = 0;
    for (uint32_t i = 0; i < EPD_BUF_SIZE; i++) {
        if (_black[i] != 0xFF) nonWhite++;
    }
    Serial.printf("EPD: buffer has %u non-white bytes of %u\n", nonWhite, EPD_BUF_SIZE);

#ifdef EPD_BW_ONLY
    // B/W panel (GD7965/GDEW075T7): image data goes to cmd 0x13 (current buffer)
    // Previous buffer 0x10 gets all-white for clean refresh

    // Send previous buffer (0x10) = all white
    _writeCommand(0x10);
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(_dc, HIGH);
    digitalWrite(_cs, LOW);
    for (uint32_t i = 0; i < EPD_BUF_SIZE; i++) {
        SPI.transfer(0xFF);
        if ((i & 0xFFF) == 0) yield();  // Every 4KB, let other tasks run
    }
    digitalWrite(_cs, HIGH);
    SPI.endTransaction();
    yield();

    Serial.println("EPD: sending image data to cmd 0x13");

    // Send current buffer (0x13) = our image data
    _writeCommand(0x13);
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(_dc, HIGH);
    digitalWrite(_cs, LOW);
    for (uint32_t i = 0; i < EPD_BUF_SIZE; i++) {
        SPI.transfer(_black[i]);
        if ((i & 0xFFF) == 0) yield();
    }
    digitalWrite(_cs, HIGH);
    SPI.endTransaction();
    yield();
#else
    // Tri-color panel: black plane to 0x10, red plane (inverted) to 0x13
    _writeCommand(0x10);
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(_dc, HIGH);
    digitalWrite(_cs, LOW);
    for (uint32_t i = 0; i < EPD_BUF_SIZE; i++) {
        SPI.transfer(_black[i]);
    }
    digitalWrite(_cs, HIGH);
    SPI.endTransaction();

    _writeCommand(0x13);
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(_dc, HIGH);
    digitalWrite(_cs, LOW);
    for (uint32_t i = 0; i < EPD_BUF_SIZE; i++) {
        SPI.transfer(~_red[i]);
    }
    digitalWrite(_cs, HIGH);
    SPI.endTransaction();
#endif

    // Trigger display refresh (non-blocking — returns immediately)
    // The panel needs ~4.2s to complete the B/W refresh.
    // The NEXT call to _sendBuffersToDisplay will wait for it at the top.
    Serial.println("EPD: triggering refresh (0x12)");
    _writeCommand(0x12);
    _refresh_pending = true;
    delay(200);  // Give panel time to assert BUSY before anyone checks
    yield();
    Serial.printf("EPD: refresh triggered (BUSY=%d)\n", digitalRead(_busy));
}

void EPD::_sendPartialToDisplay(int16_t x, int16_t y, int16_t w, int16_t h)
{
    if (!_black) return;

    // Align x to byte boundaries
    int16_t x1 = x & ~7;          // round down to multiple of 8
    int16_t x2 = ((x + w + 7) & ~7) - 1;  // round up to multiple of 8, minus 1
    int16_t wBytes = (x2 - x1 + 1) / 8;

    // Clamp
    if (x1 < 0) x1 = 0;
    if (y < 0) y = 0;
    if (x2 >= EPD_WIDTH) x2 = EPD_WIDTH - 1;
    if (y + h > EPD_HEIGHT) h = EPD_HEIGHT - y;
    if (h <= 0 || wBytes <= 0) return;

    if (_refresh_pending) {
        _waitBusy("idle-partial", 25000);
        _refresh_pending = false;
    }

    if (_power_is_on) {
        _writeCommand(0x02);
        _waitBusy("PowerOff-pre", 5000);
        _power_is_on = false;
    }

    _reset(10);
    _initDisplay(10);
    _writeCommand(0x04);
    delay(10);
    _waitBusy("PowerOn", 1000);
    _power_is_on = true;

    // Set partial window (0x90)
    _writeCommand(0x91); // Enter partial mode
    _writeCommand(0x90); // Set partial window
    _writeData(x1 >> 8);
    _writeData(x1 & 0xFF);
    _writeData(x2 >> 8);
    _writeData(x2 & 0xFF);
    _writeData(y >> 8);
    _writeData(y & 0xFF);
    _writeData((y + h - 1) >> 8);
    _writeData((y + h - 1) & 0xFF);
    _writeData(0x01); // PT_SCAN: gates scan inside and outside partial

    // Send black plane for partial region
    _writeCommand(0x10);
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(_dc, HIGH);
    digitalWrite(_cs, LOW);
    for (int16_t row = y; row < y + h; row++) {
        uint32_t rowOffset = (uint32_t)row * (EPD_WIDTH / 8) + (x1 / 8);
        for (int16_t col = 0; col < wBytes; col++) {
            SPI.transfer(_black[rowOffset + col]);
        }
    }
    digitalWrite(_cs, HIGH);
    SPI.endTransaction();

    // Send red plane (all white for B/W)
    _writeCommand(0x13);
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(_dc, HIGH);
    digitalWrite(_cs, LOW);
#ifdef EPD_BW_ONLY
    for (int16_t row = y; row < y + h; row++) {
        for (int16_t col = 0; col < wBytes; col++) {
            SPI.transfer(0x00); // inverted: no red
        }
    }
#else
    for (int16_t row = y; row < y + h; row++) {
        uint32_t rowOffset = (uint32_t)row * (EPD_WIDTH / 8) + (x1 / 8);
        for (int16_t col = 0; col < wBytes; col++) {
            SPI.transfer(~_red[rowOffset + col]);
        }
    }
#endif
    digitalWrite(_cs, HIGH);
    SPI.endTransaction();

    // Trigger partial refresh
    _writeCommand(0x12);
    delay(200);

    // Exit partial mode
    _writeCommand(0x92);
}

// ============================================================
// 5. Paging API
// ============================================================

void EPD::firstPage()
{
    _paging_active = true;
}

bool EPD::nextPage()
{
    _sendBuffersToDisplay();
    _paging_active = false;
    return false;
}

void EPD::display(bool partial)
{
    if (partial) {
        // Full-screen partial (still sends all data but in partial mode)
        _sendPartialToDisplay(0, 0, EPD_WIDTH, EPD_HEIGHT);
    } else {
        _sendBuffersToDisplay();
    }
}

void EPD::displayWithMode(EPDRefreshMode mode)
{
    switch (mode) {
        case EPD_REFRESH_PARTIAL:
        case EPD_REFRESH_FAST:
            _sendPartialToDisplay(0, 0, EPD_WIDTH, EPD_HEIGHT);
            break;
        case EPD_REFRESH_FULL:
        default:
            _sendBuffersToDisplay();
            break;
    }
}

void EPD::partialRefresh(int16_t x, int16_t y, int16_t w, int16_t h)
{
    _sendPartialToDisplay(x, y, w, h);
}

void EPD::waitReady()
{
    _waitBusy("waitReady", 25000);
}

void EPD::powerOff()
{
    if (_refresh_pending) {
        _waitBusy("idle-before-poweroff", 25000);
        _refresh_pending = false;
    }
    _writeCommand(0x02);
    delay(10);
    _waitBusy("PowerOff", 1000);
    _power_is_on = false;
}

// ============================================================
// 6. GFX -- Drawing primitives
// ============================================================

void EPD::drawPixel(int16_t x, int16_t y, uint16_t color)
{
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) return;
    if (!_black) return;

    uint32_t idx  = (uint32_t)y * (EPD_WIDTH / 8) + (x >> 3);
    uint8_t  mask = 0x80 >> (x & 7);

    if (color == GxEPD_BLACK) {
        _black[idx] &= ~mask; // clear bit -> black ink
    } else {
        // WHITE
        _black[idx] |=  mask; // set bit -> no ink
    }

#ifndef EPD_BW_ONLY
    if (_red) {
        if (color == GxEPD_RED) {
            _black[idx] |=  mask;
            _red[idx]   &= ~mask;
        } else {
            _red[idx]   |=  mask;
        }
    }
#endif
}

void EPD::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    int16_t dx =  abs(x1 - x0);
    int16_t dy = -abs(y1 - y0);
    int16_t sx = x0 < x1 ? 1 : -1;
    int16_t sy = y0 < y1 ? 1 : -1;
    int16_t err = dx + dy;

    while (true) {
        drawPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void EPD::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    drawLine(x, y, x + w - 1, y, color);
    drawLine(x, y + h - 1, x + w - 1, y + h - 1, color);
    drawLine(x, y, x, y + h - 1, color);
    drawLine(x + w - 1, y, x + w - 1, y + h - 1, color);
}

void EPD::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    if (w <= 0 || h <= 0 || !_black) return;

    int16_t x2 = x + w - 1;
    int16_t y2 = y + h - 1;
    if (x >= EPD_WIDTH || y >= EPD_HEIGHT || x2 < 0 || y2 < 0) return;
    if (x  < 0)          x  = 0;
    if (y  < 0)          y  = 0;
    if (x2 >= EPD_WIDTH)  x2 = EPD_WIDTH  - 1;
    if (y2 >= EPD_HEIGHT) y2 = EPD_HEIGHT - 1;

    // Optimized byte-aligned fill for black/white
    uint8_t fillByte = (color == GxEPD_BLACK) ? 0x00 : 0xFF;
    int16_t bytesPerRow = EPD_WIDTH / 8;
    int16_t startByte = x / 8;
    int16_t endByte = x2 / 8;

    for (int16_t row = y; row <= y2; row++) {
        uint32_t rowBase = (uint32_t)row * bytesPerRow;

        // Handle partial first byte
        if (x & 7) {
            uint8_t startMask = 0xFF >> (x & 7);
            if (startByte == endByte) {
                uint8_t endMask = 0xFF << (7 - (x2 & 7));
                uint8_t mask = startMask & endMask;
                if (color == GxEPD_BLACK) {
                    _black[rowBase + startByte] &= ~mask;
                } else {
                    _black[rowBase + startByte] |= mask;
                }
                continue;
            }
            if (color == GxEPD_BLACK) {
                _black[rowBase + startByte] &= ~startMask;
            } else {
                _black[rowBase + startByte] |= startMask;
            }
            startByte++;
        }

        // Handle partial last byte
        int16_t actualEndByte = endByte;
        if ((x2 & 7) != 7) {
            uint8_t endMask = 0xFF << (7 - (x2 & 7));
            if (color == GxEPD_BLACK) {
                _black[rowBase + endByte] &= ~endMask;
            } else {
                _black[rowBase + endByte] |= endMask;
            }
            actualEndByte--;
        }

        // Fill full bytes in between
        if (actualEndByte >= startByte) {
            memset(&_black[rowBase + startByte], fillByte, actualEndByte - startByte + 1);
        }

        // Reset for next row
        startByte = x / 8;
        endByte = x2 / 8;
    }

#ifndef EPD_BW_ONLY
    if (_red) {
        // Set red plane to no-red for the filled area
        for (int16_t row = y; row <= y2; row++) {
            for (int16_t col = x; col <= x2; col++) {
                uint32_t idx = (uint32_t)row * bytesPerRow + (col >> 3);
                uint8_t mask = 0x80 >> (col & 7);
                if (color == GxEPD_RED) {
                    _red[idx] &= ~mask;
                    _black[idx] |= mask;
                } else {
                    _red[idx] |= mask;
                }
            }
        }
    }
#endif
}

void EPD::_drawCircleHelper(int16_t x0, int16_t y0, int16_t r,
                             uint8_t cornermask, uint16_t color)
{
    int16_t f     = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t x = 0;
    int16_t y = r;

    while (x < y) {
        if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
        x++; ddF_x += 2; f += ddF_x;
        if (cornermask & 0x4) { drawPixel(x0 + x, y0 + y, color); drawPixel(x0 + y, y0 + x, color); }
        if (cornermask & 0x2) { drawPixel(x0 + x, y0 - y, color); drawPixel(x0 + y, y0 - x, color); }
        if (cornermask & 0x8) { drawPixel(x0 - y, y0 + x, color); drawPixel(x0 - x, y0 + y, color); }
        if (cornermask & 0x1) { drawPixel(x0 - y, y0 - x, color); drawPixel(x0 - x, y0 - y, color); }
    }
}

void EPD::drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color)
{
    int16_t f     = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t x = 0;
    int16_t y = r;

    drawPixel(x0, y0 + r, color);
    drawPixel(x0, y0 - r, color);
    drawPixel(x0 + r, y0, color);
    drawPixel(x0 - r, y0, color);

    while (x < y) {
        if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
        x++; ddF_x += 2; f += ddF_x;
        drawPixel(x0 + x, y0 + y, color);
        drawPixel(x0 - x, y0 + y, color);
        drawPixel(x0 + x, y0 - y, color);
        drawPixel(x0 - x, y0 - y, color);
        drawPixel(x0 + y, y0 + x, color);
        drawPixel(x0 - y, y0 + x, color);
        drawPixel(x0 + y, y0 - x, color);
        drawPixel(x0 - y, y0 - x, color);
    }
}

void EPD::_fillCircleHelper(int16_t x0, int16_t y0, int16_t r,
                             uint8_t cornermask, int16_t delta, uint16_t color)
{
    int16_t f     = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t x = 0;
    int16_t y = r;

    while (x < y) {
        if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
        x++; ddF_x += 2; f += ddF_x;
        if (cornermask & 0x1) {
            drawLine(x0 + x, y0 - y, x0 + x, y0 - y + 2 * y + 1 + delta, color);
            drawLine(x0 + y, y0 - x, x0 + y, y0 - x + 2 * x + 1 + delta, color);
        }
        if (cornermask & 0x2) {
            drawLine(x0 - x, y0 - y, x0 - x, y0 - y + 2 * y + 1 + delta, color);
            drawLine(x0 - y, y0 - x, x0 - y, y0 - x + 2 * x + 1 + delta, color);
        }
    }
}

void EPD::fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color)
{
    drawLine(x0, y0 - r, x0, y0 + r, color);
    _fillCircleHelper(x0, y0, r, 3, 0, color);
}

void EPD::drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                        int16_t r, uint16_t color)
{
    drawLine(x + r, y, x + w - 1 - r, y, color);
    drawLine(x + r, y + h - 1, x + w - 1 - r, y + h - 1, color);
    drawLine(x, y + r, x, y + h - 1 - r, color);
    drawLine(x + w - 1, y + r, x + w - 1, y + h - 1 - r, color);
    _drawCircleHelper(x + r, y + r, r, 0x1, color);
    _drawCircleHelper(x + w - 1 - r, y + r, r, 0x2, color);
    _drawCircleHelper(x + w - 1 - r, y + h - 1 - r, r, 0x4, color);
    _drawCircleHelper(x + r, y + h - 1 - r, r, 0x8, color);
}

void EPD::fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                        int16_t r, uint16_t color)
{
    fillRect(x + r, y, w - 2 * r, h, color);
    _fillCircleHelper(x + w - 1 - r, y + r, r, 1, h - 2 * r - 1, color);
    _fillCircleHelper(x + r, y + r, r, 2, h - 2 * r - 1, color);
}

void EPD::fillTriangle(int16_t x0, int16_t y0,
                       int16_t x1, int16_t y1,
                       int16_t x2, int16_t y2, uint16_t color)
{
    if (y0 > y1) { int16_t t; t=y0; y0=y1; y1=t; t=x0; x0=x1; x1=t; }
    if (y1 > y2) { int16_t t; t=y1; y1=y2; y2=t; t=x1; x1=x2; x2=t; }
    if (y0 > y1) { int16_t t; t=y0; y0=y1; y1=t; t=x0; x0=x1; x1=t; }

    if (y0 == y2) {
        int16_t minx = min(x0, min(x1, x2));
        int16_t maxx = max(x0, max(x1, x2));
        drawLine(minx, y0, maxx, y0, color);
        return;
    }

    int32_t dx01 = x1 - x0, dy01 = y1 - y0;
    int32_t dx02 = x2 - x0, dy02 = y2 - y0;
    int32_t dx12 = x2 - x1, dy12 = y2 - y1;
    int32_t sa = 0, sb = 0;
    int16_t last = (y1 == y2) ? y1 : y1 - 1;

    for (int16_t y = y0; y <= last; y++) {
        int16_t ax = x0 + sa / dy01;
        int16_t bx = x0 + sb / dy02;
        sa += dx01; sb += dx02;
        if (ax > bx) { int16_t t = ax; ax = bx; bx = t; }
        drawLine(ax, y, bx, y, color);
    }

    sa = (int32_t)dx12 * (y1 - y1);
    sb = (int32_t)dx02 * (y1 - y0);
    for (int16_t y = y1; y <= y2; y++) {
        int16_t ax = x1 + (dy12 ? sa / dy12 : 0);
        int16_t bx = x0 + (dy02 ? sb / dy02 : 0);
        sa += dx12; sb += dx02;
        if (ax > bx) { int16_t t = ax; ax = bx; bx = t; }
        drawLine(ax, y, bx, y, color);
    }
}

// ============================================================
// 7. Text rendering (GFXfont format)
// ============================================================

void EPD::setFont(const GFXfont* font)
{
    _font = font;
}

void EPD::setTextColor(uint16_t color)
{
    _textColor = color;
}

void EPD::setCursor(int16_t x, int16_t y)
{
    _cursorX = x;
    _cursorY = y;
}

void EPD::_drawChar(int16_t x, int16_t y, unsigned char c, uint16_t color)
{
    if (!_font) return;
    if (c < _font->first || c > _font->last) return;

    const GFXglyph* glyph = &_font->glyph[c - _font->first];
    const uint8_t*  bitmap = _font->bitmap;

    uint16_t bitmapOffset = glyph->bitmapOffset;
    uint8_t  w = glyph->width;
    uint8_t  h = glyph->height;
    int8_t   xo = glyph->xOffset;
    int8_t   yo = glyph->yOffset;

    uint8_t  bits = 0, bit = 0;

    for (uint8_t row = 0; row < h; row++) {
        for (uint8_t col = 0; col < w; col++) {
            if (bit == 0) {
                bits = pgm_read_byte(&bitmap[bitmapOffset++]);
                bit  = 0x80;
            }
            if (bits & bit) {
                drawPixel(x + xo + col, y + yo + row, color);
            }
            bit >>= 1;
        }
    }
}

void EPD::println(const String& text)
{
    print(text);
    if (_font) _cursorY += _font->yAdvance;
    _cursorX = 0;
}

void EPD::print(const String& text)
{
    if (!_font) return;
    for (size_t i = 0; i < text.length(); i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= _font->first && c <= _font->last) {
            _drawChar(_cursorX, _cursorY, c, _textColor);
            _cursorX += _font->glyph[c - _font->first].xAdvance;
        }
    }
}

void EPD::getTextBounds(const String& str, int16_t x, int16_t y,
                        int16_t* x1, int16_t* y1, uint16_t* w, uint16_t* h)
{
    *x1 = x;
    *y1 = y;
    *w  = 0;
    *h  = 0;

    if (!_font || str.length() == 0) return;

    int16_t minX = 32767, minY = 32767, maxX = -32768, maxY = -32768;
    int16_t curX = x;

    for (size_t i = 0; i < str.length(); i++) {
        unsigned char c = (unsigned char)str[i];
        if (c < _font->first || c > _font->last) continue;

        const GFXglyph* g = &_font->glyph[c - _font->first];
        int16_t gx1 = curX + g->xOffset;
        int16_t gy1 = y    + g->yOffset;
        int16_t gx2 = gx1  + g->width  - 1;
        int16_t gy2 = gy1  + g->height - 1;

        if (gx1 < minX) minX = gx1;
        if (gy1 < minY) minY = gy1;
        if (gx2 > maxX) maxX = gx2;
        if (gy2 > maxY) maxY = gy2;

        curX += g->xAdvance;
    }

    if (maxX >= minX) {
        *x1 = minX;
        *y1 = minY;
        *w  = (uint16_t)(maxX - minX + 1);
        *h  = (uint16_t)(maxY - minY + 1);
    }
}
