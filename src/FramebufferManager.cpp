#include "FramebufferManager.h"
#include <string.h>
#include <stdlib.h>

FramebufferManager::FramebufferManager()
    : _front(nullptr), _back(nullptr)
{
}

FramebufferManager::~FramebufferManager()
{
    free(_front);
    // _back is NOT owned by us (it's EPD's buffer)
}

bool FramebufferManager::init(uint8_t* externalBackBuffer)
{
    if (!externalBackBuffer) {
        Serial.println("FramebufferManager: external back buffer is null!");
        return false;
    }

    _back = externalBackBuffer;

    if (!_front) {
        // Skip front buffer on large displays (10.2" = 76800 bytes) to preserve
        // heap for WiFi, AsyncTCP, HTTP server, and JSON parsing (~140KB needed).
        bool skipFrontBuffer = (BUFFER_SIZE > 48000);
        if (!skipFrontBuffer) {
            _front = (uint8_t*)malloc(BUFFER_SIZE);
        }
        if (_front) {
            memset(_front, 0xFF, BUFFER_SIZE); // white
            Serial.printf("FramebufferManager: double-buffered (%u bytes)\n", BUFFER_SIZE);
        } else {
            Serial.printf("FramebufferManager: single-buffer mode (%u bytes saved)\n", BUFFER_SIZE);
        }
    }

    return true;  // Valid even without front buffer
}

void FramebufferManager::setPixel(int16_t x, int16_t y, bool white)
{
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT || !_back) return;

    uint32_t idx  = (uint32_t)y * (EPD_WIDTH / 8) + (x >> 3);
    uint8_t  mask = 0x80 >> (x & 7);

    if (white) {
        _back[idx] |= mask;
    } else {
        _back[idx] &= ~mask;
    }
}

bool FramebufferManager::getPixel(int16_t x, int16_t y) const
{
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT || !_back) return true;

    uint32_t idx  = (uint32_t)y * (EPD_WIDTH / 8) + (x >> 3);
    uint8_t  mask = 0x80 >> (x & 7);

    return (_back[idx] & mask) != 0; // true = white
}

void FramebufferManager::clear(bool white)
{
    if (!_back) return;
    memset(_back, white ? 0xFF : 0x00, BUFFER_SIZE);
}

FramebufferManager::DirtyRect FramebufferManager::commit()
{
    DirtyRect rect = {0, 0, 0, 0, true};
    if (!_back) return rect;

    // Single-buffer mode: no front buffer to compare against.
    // Return full-screen dirty rect so caller always does a full refresh.
    if (!_front) {
        rect.x = 0;
        rect.y = 0;
        rect.w = EPD_WIDTH;
        rect.h = EPD_HEIGHT;
        rect.empty = false;
        return rect;
    }

    // Double-buffer mode: compare front/back to find dirty region
    int16_t bytesPerRow = EPD_WIDTH / 8;
    int16_t minRow = EPD_HEIGHT, maxRow = -1;
    int16_t minCol = bytesPerRow, maxCol = -1;

    for (int16_t row = 0; row < EPD_HEIGHT; row++) {
        uint32_t rowBase = (uint32_t)row * bytesPerRow;
        for (int16_t col = 0; col < bytesPerRow; col++) {
            if (_front[rowBase + col] != _back[rowBase + col]) {
                if (row < minRow) minRow = row;
                if (row > maxRow) maxRow = row;
                if (col < minCol) minCol = col;
                if (col > maxCol) maxCol = col;
            }
        }
    }

    if (maxRow < 0) {
        return rect;
    }

    // Copy back to front
    memcpy(_front, _back, BUFFER_SIZE);

    rect.x = minCol * 8;
    rect.y = minRow;
    rect.w = (maxCol - minCol + 1) * 8;
    rect.h = maxRow - minRow + 1;
    rect.empty = false;

    return rect;
}

void FramebufferManager::swapAfterFullRefresh()
{
    if (_front && _back) {
        memcpy(_front, _back, BUFFER_SIZE);
    }
    // Single-buffer mode: no-op
}

bool FramebufferManager::releaseFrontBuffer()
{
    if (!_front) return false;
    free(_front);
    _front = nullptr;
    Serial.printf("FramebufferManager: front buffer released (%u bytes freed)\n", BUFFER_SIZE);
    return true;
}

bool FramebufferManager::reacquireFrontBuffer()
{
    if (_front) return true;  // Already allocated
    _front = (uint8_t*)malloc(BUFFER_SIZE);
    if (_front) {
        if (_back) memcpy(_front, _back, BUFFER_SIZE);
        else memset(_front, 0xFF, BUFFER_SIZE);
        Serial.printf("FramebufferManager: front buffer reacquired (%u bytes)\n", BUFFER_SIZE);
        return true;
    }
    Serial.println("FramebufferManager: front buffer reacquire failed, staying single-buffered");
    return false;
}
