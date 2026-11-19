// Single source of truth for every pin used by this project.
//
// These values are MEASURED on the actual hardware, not copied from a
// datasheet. See docs/hardware.md for the evidence behind each one.
//
// Anything in this file must be changed in exactly one place only.
#pragma once

#include <Arduino.h>

namespace board {

// ---------------------------------------------------------------------------
// Camera -- OV2640 on the parallel DVP bus.
// Verified working: init OK, 640x480 JPEG, ~12.5 KB/frame.
// Source: CAMERA_MODEL_ESP32S3_EYE in Doc/, confirmed on hardware.
// ---------------------------------------------------------------------------
constexpr int CAM_PIN_PWDN  = -1;
constexpr int CAM_PIN_RESET = -1;
constexpr int CAM_PIN_XCLK  = 15;
constexpr int CAM_PIN_SIOD  = 4;   // SCCB data
constexpr int CAM_PIN_SIOC  = 5;   // SCCB clock
constexpr int CAM_PIN_D7    = 16;  // Y9
constexpr int CAM_PIN_D6    = 17;  // Y8
constexpr int CAM_PIN_D5    = 18;  // Y7
constexpr int CAM_PIN_D4    = 12;  // Y6
constexpr int CAM_PIN_D3    = 10;  // Y5
constexpr int CAM_PIN_D2    = 8;   // Y4
constexpr int CAM_PIN_D1    = 9;   // Y3
constexpr int CAM_PIN_D0    = 11;  // Y2
constexpr int CAM_PIN_VSYNC = 6;
constexpr int CAM_PIN_HREF  = 7;
constexpr int CAM_PIN_PCLK  = 13;

// ---------------------------------------------------------------------------
// microSD -- 1-bit SDMMC.
// IMPORTANT: GPIO38 is shared with the TFT backlight on the extension board.
// The backlight has been rewired to GPIO42 (see TFT_PIN_BL below), which is
// what makes this pin available for SD again.
// ---------------------------------------------------------------------------
constexpr int SD_PIN_CLK = 39;
constexpr int SD_PIN_CMD = 38;
constexpr int SD_PIN_D0  = 40;

// ---------------------------------------------------------------------------
// TFT (ST7789, 240x320, 4-wire SPI, no touch).
// BL was moved from its original GPIO38 to GPIO42 to free the SD CMD line.
// DC/RST/CS/SCL/SDA are as measured and must not overlap the camera or SD.
// ---------------------------------------------------------------------------
// Rewired 2026-11-13: original pins 19/20 are USB D-/D+ and crash the SoC.
// Second attempt used GPIO9/GPIO14 but GPIO9 is camera D1. Final wiring:
//   SCL -> GPIO41, SDA -> GPIO2 (both verified idle in docs/hardware.md).
constexpr int TFT_PIN_SCLK = 41;
constexpr int TFT_PIN_MOSI = 2;
constexpr int TFT_PIN_RST  = 21;
constexpr int TFT_PIN_CS   = 45;
constexpr int TFT_PIN_DC   = 47;
constexpr int TFT_PIN_BL   = 42;  // rewired from 38

// ---------------------------------------------------------------------------
// Buttons
// GPIO0 is also the BOOT strapping pin: it must NOT be held low at power-on
// or the chip enters download mode. Only read it during runtime.
// GPIO46 is a strapping pin too, measured LOW at power-on (safe, ROM logs on).
// ---------------------------------------------------------------------------
constexpr int BTN_PROVISION = 0;   // "interrupt/network" button on extension board
constexpr int BTN_SPARE     = 46;  // "reserved" button on extension board

// ---------------------------------------------------------------------------
// Onboard WS2812 status LED
// ---------------------------------------------------------------------------
constexpr int LED_WS2812 = 48;

// ---------------------------------------------------------------------------
// Pins that must never be driven:
//   26..32  SPI flash -- touching these triggers a watchdog reset
//   35..37  octal PSRAM
//   43,44   UART to the CH343 bridge
// ---------------------------------------------------------------------------
constexpr bool isForbiddenPin(int gpio) {
  return (gpio >= 26 && gpio <= 37) || gpio == 43 || gpio == 44;
}

}  // namespace board
