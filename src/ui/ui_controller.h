// UiController -- buttons, on-panel menu and provisioning entry.
//
// Rules from PROJECT_PLAN.md 7.2 that this file is built around:
//   - Two buttons only. GPIO46 (spare) is the everyday key; GPIO0
//     (interrupt/provision) is the system key.
//   - Both are strapping pins, so all input is ignored for the first 2 s after
//     boot (the ROM samples them before setup() runs).
//   - Non-blocking: update() is called from the main loop and returns at once.
//     A while() waiting for release here would stall the recorder pump.
//   - Nothing is written to NVS until the user explicitly confirms, so a stray
//     press cannot wear the flash or silently change behaviour.
//
// Settings are edited on the panel but owned by ConfigService: this class only
// mutates a working copy and, on confirm, hands it back through the same
// validation path the web UI uses.
#pragma once

#include <Arduino.h>

#include "config/config_service.h"
#include "display/display_service.h"
#include "network/network_service.h"
#include "ui/button.h"

class UiController {
 public:
  void begin(DisplayService* display, ConfigService* config,
             NetworkService* network, volatile bool* configDirty,
             bool* provisionRequest);

  // Call from the main loop on every iteration.
  void loop();

 private:
  // Menu rows, in display order. Keep the count in sync with kRowCount.
  enum Row : uint8_t {
    kRowSensitivity = 0,
    kRowMinBlocks,
    kRowClipSeconds,
    kRowCooldown,
    kRowJpegQuality,
    kRowRecording,
    kRowCount
  };

  void handleSpareButton();
  void handleProvisionButton();
  void enterMenuMode();
  void exitMenuMode();
  void cyclePage();
  void moveSelection(int delta);
  void adjustSelected(int delta);
  void commitSettings();
  void refreshRows();
  void showHint(const char* text);

  DisplayService* display_ = nullptr;
  ConfigService* config_ = nullptr;
  NetworkService* network_ = nullptr;
  volatile bool* configDirty_ = nullptr;
  bool* provisionRequest_ = nullptr;

  Button spareBtn_;
  Button provBtn_;

  bool bootIgnored_ = false;
  uint32_t bootMs_ = 0;

  // Two-level UI: browsing pages, or editing the settings menu.
  bool menuMode_ = false;
  bool editing_ = false;
  uint8_t selected_ = 0;
  // Auto-repeat clock for the edit-mode hold-to-decrement gesture.
  uint32_t editRepeatMs_ = 0;

  // Working copy. Written to NVS only on confirm.
  ConfigService::Settings draft_{};

  DisplayService::MenuRow rows_[kRowCount];
};
