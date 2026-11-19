#include "ui_controller.h"

#include <Arduino.h>

#include "board/board_config.h"

namespace {
// Ignore all button input for this long after power-on. Both usable pins are
// strapping pins sampled by the ROM, and the first moments after reset are also
// when the power rails settle; a spurious edge here would look like a press.
constexpr uint32_t kBootIgnoreMs = 2000;

// Hint line text, kept short: the panel is 240 px wide in portrait and the
// font is 6 px per character including spacing (about 38 characters).
constexpr const char* kHintBrowse = "GPIO46: PAGE  GPIO46 HOLD: MENU";
constexpr const char* kHintMenu = "TAP: NEXT  HOLD: EDIT  GPIO0: EXIT";

}  // namespace

void UiController::begin(DisplayService* display, ConfigService* config,
                         NetworkService* network, volatile bool* configDirty,
                         bool* provisionRequest) {
  display_ = display;
  config_ = config;
  network_ = network;
  configDirty_ = configDirty;
  provisionRequest_ = provisionRequest;

  spareBtn_.begin(board::BTN_SPARE);
  provBtn_.begin(board::BTN_PROVISION);

  draft_ = config_ ? config_->get() : ConfigService::Settings{};

  bootMs_ = millis();
  bootIgnored_ = false;

  // Keep the provisioning key from firing during boot: it is GPIO0 and holding
  // it at reset would select download mode instead of running the firmware.
  spareBtn_.setEnabled(false);
  provBtn_.setEnabled(false);

  refreshRows();
  if (display_) {
    display_->setMenuRows(rows_, kRowCount);
    display_->setHint(kHintBrowse);
    display_->setPage(DisplayService::Page::Preview);
  }

  Serial.printf("[ui] buttons: spare=GPIO%d, provision=GPIO%d\n",
                board::BTN_SPARE, board::BTN_PROVISION);
}

void UiController::loop() {
  const uint32_t now = millis();

  spareBtn_.update();
  provBtn_.update();

  if (!bootIgnored_ && now - bootMs_ >= kBootIgnoreMs) {
    bootIgnored_ = true;
    spareBtn_.setEnabled(true);
    provBtn_.setEnabled(true);
    // Drop anything latched during the ignore window.
    spareBtn_.consumeShortPress();
    spareBtn_.consumeLongPress();
    provBtn_.consumeShortPress();
    provBtn_.consumeLongPress();
    Serial.println("[ui] buttons enabled");
  }

  if (!bootIgnored_) return;

  handleSpareButton();
  handleProvisionButton();

  // Any press counts as activity for the backlight timeout.
  if (spareBtn_.isHeld() || provBtn_.isHeld()) {
    if (display_) display_->updateActivity();
  }
}

void UiController::handleSpareButton() {
  if (spareBtn_.consumeLongPress()) {
    if (!menuMode_) {
      enterMenuMode();
    } else if (!editing_) {
      editing_ = true;
      showHint("EDITING - TAP TO CHANGE");
      refreshRows();
    }
    return;
  }

  if (!spareBtn_.consumeShortPress()) return;

  if (!menuMode_) {
    cyclePage();
    return;
  }

  if (editing_) {
    adjustSelected(+1);
  } else {
    moveSelection(+1);
  }
}

void UiController::handleProvisionButton() {
  // Short press inside the menu is the escape hatch; long press is the
  // provisioning trigger from PROJECT_PLAN.md 2.4 (runtime only, never at boot).
  if (provBtn_.consumeLongPress()) {
    if (display_) {
      display_->setHint("PROVISIONING...");
      display_->setPage(DisplayService::Page::Network);
    }
    Serial.println("[ui] provision key: long press");
    if (provisionRequest_) *provisionRequest_ = true;
    return;
  }

  if (!provBtn_.consumeShortPress()) return;

  if (menuMode_) {
    exitMenuMode();
  } else if (display_) {
    // Outside the menu the key just reports the provisioning hint.
    showHint("HOLD GPIO0 = PROVISION");
  }
}

void UiController::cyclePage() {
  if (!display_) return;
  const uint8_t next =
      ((uint8_t)display_->page() + 1) % (uint8_t)DisplayService::Page::PageCount;
  display_->setPage((DisplayService::Page)next);
  Serial.printf("[ui] page %u\n", (unsigned)next);
}

void UiController::enterMenuMode() {
  menuMode_ = true;
  editing_ = false;
  selected_ = 0;
  draft_ = config_ ? config_->get() : ConfigService::Settings{};
  refreshRows();
  if (display_) {
    display_->setHint(kHintMenu);
    display_->setPage(DisplayService::Page::Settings);
  }
  Serial.println("[ui] menu opened");
}

void UiController::exitMenuMode() {
  if (editing_) {
    // Leaving edit mode without confirm keeps the draft, so a long-press exit
    // does not silently discard the change the user just made.
    editing_ = false;
    refreshRows();
    return;
  }
  menuMode_ = false;
  // Restore whatever the settings page showed, without persisting the draft.
  if (display_) {
    display_->setMenuRows(rows_, kRowCount);
    display_->setHint(kHintBrowse);
  }
  Serial.println("[ui] menu closed");
}

void UiController::moveSelection(int delta) {
  if (delta == 0) return;
  const int n = (int)kRowCount;
  selected_ = (uint8_t)(((int)selected_ + delta + n) % n);
  refreshRows();
}

void UiController::adjustSelected(int delta) {
  // Clamp helper. Written for C++11: the toolchain here does not accept generic
  // lambdas (no -std=c++14), so the type is explicit.
  auto step = [&](uint16_t& field, int lo, int hi, int inc) {
    int v = (int)field + delta * inc;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    field = (uint16_t)v;
  };
  auto step8 = [&](uint8_t& field, int lo, int hi, int inc) {
    int v = (int)field + delta * inc;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    field = (uint8_t)v;
  };

  switch ((Row)selected_) {
    case kRowSensitivity: step8(draft_.sensitivity, 1, 100, 5); break;
    case kRowMinBlocks:   step8(draft_.minChangedBlocks, 1, 20, 1); break;
    case kRowClipSeconds: step(draft_.clipDurationSec, 1, 120, 5); break;
    case kRowCooldown:    step(draft_.cooldownSec, 1, 600, 5); break;
    case kRowJpegQuality: step8(draft_.jpegQuality, 6, 30, 2); break;
    case kRowRecording:   draft_.recordingEnabled = !draft_.recordingEnabled; break;
    default: break;
  }

  refreshRows();

  // Persist immediately on change: the menu has no separate "save" gesture and
  // a settings page that silently forgets a change after a reboot is worse than
  // the small extra NVS write. The write happens once per key press, not per
  // frame, so wear is negligible.
  commitSettings();
}

void UiController::commitSettings() {
  if (!config_) return;

  ConfigService::Settings before = config_->get();

  // Reuse the web path: serialise the draft, run it through updateFromJson,
  // then save. That keeps one validation implementation for both UIs.
  String error;
  const String json = [&]() {
    String s = "{";
    s += "\"sensitivity\":" + String(draft_.sensitivity) + ",";
    s += "\"min_changed\":" + String(draft_.minChangedBlocks) + ",";
    s += "\"clip_duration_s\":" + String(draft_.clipDurationSec) + ",";
    s += "\"cooldown_s\":" + String(draft_.cooldownSec) + ",";
    s += "\"jpeg_quality\":" + String(draft_.jpegQuality) + ",";
    s += "\"recording_enabled\":";
    s += draft_.recordingEnabled ? "true" : "false";
    s += "}";
    return s;
  }();

  if (!config_->updateFromJson(json.c_str(), json.length(), error)) {
    Serial.printf("[ui] rejected: %s\n", error.c_str());
    draft_ = before;  // roll the UI back to what is actually in effect
    refreshRows();
    if (display_) display_->setHint("REJECTED");
    return;
  }

  config_->save();
  if (configDirty_) *configDirty_ = true;
  if (display_) display_->setHint(kHintMenu);

  Serial.printf("[ui] applied: sens=%u min=%u clip=%us cool=%us q=%u rec=%d\n",
                config_->get().sensitivity, config_->get().minChangedBlocks,
                config_->get().clipDurationSec, config_->get().cooldownSec,
                config_->get().jpegQuality,
                config_->get().recordingEnabled ? 1 : 0);
}

void UiController::refreshRows() {
  const ConfigService::Settings& s = draft_;
  for (int i = 0; i < kRowCount; i++) {
    rows_[i].selected = ((uint8_t)i == selected_) && menuMode_;
    rows_[i].editing = rows_[i].selected && editing_;
  }

  rows_[kRowSensitivity].label = "SENSITIVITY";
  snprintf(rows_[kRowSensitivity].value, sizeof(rows_[0].value), "%u",
           s.sensitivity);

  rows_[kRowMinBlocks].label = "MIN BLOCKS";
  snprintf(rows_[kRowMinBlocks].value, sizeof(rows_[0].value), "%u",
           s.minChangedBlocks);

  rows_[kRowClipSeconds].label = "CLIP SEC";
  snprintf(rows_[kRowClipSeconds].value, sizeof(rows_[0].value), "%u",
           s.clipDurationSec);

  rows_[kRowCooldown].label = "COOLDOWN S";
  snprintf(rows_[kRowCooldown].value, sizeof(rows_[0].value), "%u",
           s.cooldownSec);

  rows_[kRowJpegQuality].label = "JPEG Q";
  snprintf(rows_[kRowJpegQuality].value, sizeof(rows_[0].value), "%u",
           s.jpegQuality);

  rows_[kRowRecording].label = "RECORDING";
  snprintf(rows_[kRowRecording].value, sizeof(rows_[0].value), "%s",
           s.recordingEnabled ? "ON" : "OFF");

  if (display_) display_->setMenuRows(rows_, kRowCount);
}

void UiController::showHint(const char* text) {
  if (display_) display_->setHint(text);
}
