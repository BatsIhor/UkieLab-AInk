#pragma once

#include <Arduino.h>
#include "EPD.h"

// Double-buffered framebuffer manager for the AI Canvas.
// Front buffer = what's currently displayed on the panel (owned by this class).
// Back buffer = EPD's own buffer (shared, not allocated here) — drawing target.
// This sharing saves 48KB of RAM and eliminates copy steps for display refresh.
class FramebufferManager {
public:
    FramebufferManager();
    ~FramebufferManager();

    // Initialize with external back buffer (EPD's _black buffer).
    // Only allocates the front buffer internally.
    bool init(uint8_t* externalBackBuffer);

    // Drawing on back buffer
    void setPixel(int16_t x, int16_t y, bool white);
    bool getPixel(int16_t x, int16_t y) const;
    void clear(bool white = true);

    // Direct buffer access for render engine
    uint8_t* getBackBuffer() { return _back; }
    const uint8_t* getBackBuffer() const { return _back; }
    const uint8_t* getFrontBuffer() const { return _front; }

    // Copy back buffer to front, return dirty rect
    struct DirtyRect {
        int16_t x, y, w, h;
        bool empty;
    };
    DirtyRect commit();

    // Copy back to front without computing dirty rect (after full refresh)
    void swapAfterFullRefresh();

    bool isValid() const { return _front && _back; }

    static constexpr uint32_t BUFFER_SIZE = EPD_BUF_SIZE;

private:
    uint8_t* _front;  // What's on display (heap-allocated, owned)
    uint8_t* _back;   // Drawing target (EPD's buffer, NOT owned)
};
