// 5x7 bitmap font, ASCII 0x20..0x7F, column-major per glyph.
//
// Each glyph is 5 bytes: one byte per column, bit 0 = top row, bit 6 = bottom.
// Stored as a flat table so it costs exactly 96 * 5 = 480 bytes of flash and no
// RAM. Only ASCII is provided: the panel has no CJK font and the UI text is
// English/numeric on purpose (see ui_controller.cpp).
#pragma once

#include <Arduino.h>

namespace font5x7 {

constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;

// Returns nullptr for characters outside the table.
const uint8_t* glyph(char c);

// Width in pixels of a string when drawn with 1 px spacing.
int textWidth(const char* s, int scale = 1);

}  // namespace font5x7
