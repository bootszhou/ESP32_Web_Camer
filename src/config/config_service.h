// ConfigService -- persists user settings in NVS.
//
// Why NVS and not a JSON file on the SD card: the SD card can be removed, and
// settings must survive without it. NVS also gives wear levelling and atomic
// key updates, which matters because the web UI can write on every slider move.
//
// Every value is validated on load. A corrupted or out-of-range entry falls back
// to its default rather than propagating nonsense into the detector.
#pragma once

#include <Arduino.h>

class ConfigService {
 public:
  struct Settings {
    // --- motion detection ---
    uint8_t sensitivity = 65;       // 1..100, higher = more sensitive
    uint8_t minChangedBlocks = 3;   // 1..70
    uint8_t confirmSamples = 2;     // 1..5
    uint16_t cooldownSec = 30;      // 1..600
    uint16_t sampleIntervalMs = 250;  // 100..2000

    // --- recording ---
    uint16_t clipDurationSec = 10;  // 1..120
    bool extendOnMotion = false;
    uint16_t maxClipSec = 60;       // 10..300
    bool recordingEnabled = true;

    // --- camera ---
    uint8_t frameSize = 8;          // esp32-camera framesize enum, VGA=8
    uint8_t jpegQuality = 12;       // 6..30, lower = better quality
    bool vflip = true;
    bool hflip = false;

    // --- storage policy ---
    uint16_t minFreeMb = 200;       // refuse to record below this
    bool autoCleanEnabled = false;  // delete oldest when low

    // --- local display ---
    // Orientation index 0..3, measured on this panel: 0 is the upright
    // portrait letterbox that matches the web preview; 1/3 are landscape.
    uint8_t displayRotation = 0;
    uint8_t displayBrightness = 200;  // PWM duty 0..255
  };

  bool begin();
  bool save();

  const Settings& get() const { return s_; }

  // Validates and applies. Returns false (and leaves state untouched) if any
  // field is out of range. Does not persist: call save() separately.
  bool updateFromJson(const char* json, size_t len, String& error);

  // Restores defaults in RAM; call save() to persist.
  void resetToDefaults() { s_ = Settings{}; }

  // Serialises the current settings as JSON (no trailing newline).
  String toJson() const;

 private:
  void loadFromNvs();
  void validate();

  Settings s_{};
  bool nvsReady_ = false;
};
