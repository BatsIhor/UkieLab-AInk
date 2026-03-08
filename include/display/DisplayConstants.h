#ifndef DISPLAY_CONSTANTS_H
#define DISPLAY_CONSTANTS_H

#include <Arduino.h>

namespace ScreenLayout {
#ifdef epd102
constexpr int SCREEN_WIDTH = 960;
constexpr int SCREEN_HEIGHT = 640;
constexpr bool IS_TRICOLOR = false;
#elif defined(epd75)
constexpr int SCREEN_WIDTH = 800;
constexpr int SCREEN_HEIGHT = 480;
constexpr bool IS_TRICOLOR = false;
#elif defined(epd75_3c)
// 7.5" Tri-Color (Black/Red/White) - Waveshare 7.5" V2 BWR
constexpr int SCREEN_WIDTH = 800;
constexpr int SCREEN_HEIGHT = 480;
constexpr bool IS_TRICOLOR = true;
#else
#error "No display type defined. Please define epd102, epd75, or epd75_3c"
#endif
} // namespace ScreenLayout

// Color definitions for tri-color displays
namespace DisplayColors {
#ifdef epd75_3c
// Tri-color display color values
constexpr uint16_t BLACK = 0x0000;
constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t RED = 0xF800; // GxEPD_RED for tri-color displays
#else
// B/W displays only have black and white
constexpr uint16_t BLACK = 0x0000;
constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t RED = 0x0000; // Falls back to black on B/W displays
#endif
} // namespace DisplayColors

#endif // DISPLAY_CONSTANTS_H
