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
// GPIO38 previously carried the TFT backlight on the extension board; the panel
// has been removed from this build (docs/progress.md M8), so the pin is a plain
// SD data line again.
// ---------------------------------------------------------------------------
constexpr int SD_PIN_CLK = 39;
constexpr int SD_PIN_CMD = 38;
constexpr int SD_PIN_D0  = 40;

// ---------------------------------------------------------------------------
// Buttons
// GPIO0 is also the BOOT strapping pin: it must NOT be held low at power-on
// or the chip enters download mode. Only read it during runtime.
// GPIO46 is a strapping pin too, measured LOW at power-on (safe, ROM logs on).
// ---------------------------------------------------------------------------
constexpr int BTN_PROVISION = 0;   // "interrupt/network" button on extension board
// The "reserved" button is still wired, but nothing uses it now that the local
// panel and its menu are gone (docs/progress.md M8). Kept as a hardware fact.
constexpr int BTN_SPARE     = 46;

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
