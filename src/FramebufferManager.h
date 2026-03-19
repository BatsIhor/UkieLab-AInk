#pragma once

#include <Arduino.h>
#include "EPD.h"

// Framebuffer manager for the AI Canvas.
//
// Double-buffered mode (default for 7.5"):
//   Front buffer = what's currently displayed (owned, heap-allocated).
//   Back buffer = EPD's own buffer (shared, not allocated here).
//   commit() compares front/back to compute a dirty rect for partial refresh.
//
// Single-buffered mode (fallback for 10.2" or low memory):
//   No front buffer allocated. commit() always returns full-screen dirty rect.
//   Screenshots read from the back buffer directly.
//   Saves EPD_BUF_SIZE bytes of RAM (76.8KB on 10.2").
class FramebufferManager {
public:
    FramebufferManager();
    ~FramebufferManager();

    // Initialize with external back buffer (EPD's _black buffer).
    // Allocates front buffer if memory permits; falls back to single-buffer mode.
    bool init(uint8_t* externalBackBuffer);

    // Drawing on back buffer
    void setPixel(int16_t x, int16_t y, bool white);
    bool getPixel(int16_t x, int16_t y) const;
    void clear(bool white = true);

    // Direct buffer access for render engine
    uint8_t* getBackBuffer() { return _back; }
    const uint8_t* getBackBuffer() const { return _back; }

    // Returns front buffer if double-buffered, otherwise back buffer (for screenshots)
    const uint8_t* getFrontBuffer() const { return _front ? _front : _back; }

    // Copy back buffer to front, return dirty rect
    struct DirtyRect {
        int16_t x, y, w, h;
        bool empty;
    };
    DirtyRect commit();

    // Copy back to front without computing dirty rect (after full refresh)
    void swapAfterFullRefresh();

    // Valid if back buffer is set (front buffer is optional)
    bool isValid() const { return _back != nullptr; }
    bool isDoubleBuffered() const { return _front != nullptr; }

    static constexpr uint32_t BUFFER_SIZE = EPD_BUF_SIZE;

private:
    uint8_t* _front;  // What's on display (heap-allocated, owned) — NULL in single-buffer mode
    uint8_t* _back;   // Drawing target (EPD's buffer, NOT owned)
};
