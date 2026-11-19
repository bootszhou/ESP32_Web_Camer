// Debounced push button with short-press and long-press events.
//
// Design constraints taken from PROJECT_PLAN.md 7.2:
//   - Non-blocking state machine. No `while` waiting for release: the main
//     loop also pumps frames for the recorder.
//   - Software debounce (kDebounceMs), not delay().
//   - Both GPIO0 and GPIO46 are strapping pins, so the first seconds after
//     power-on are ignored entirely (the ROM samples them before setup()).
//   - Active low with INPUT_PULLUP; GPIO46 measured LOW at power-on because the
//     extension board pulls it down, which is harmless for a pull-up input.
#pragma once

#include <Arduino.h>

class Button {
 public:
  void begin(int pin);

  // Call from the main loop as often as possible; cheap and non-blocking.
  void update();

  // One-shot readers: the flag is consumed by reading it.
  bool consumeShortPress();
  bool consumeLongPress();

  // Held down for at least the long-press threshold right now.
  bool isHeld() const { return held_; }
  uint32_t heldMs() const;

  void setEnabled(bool enabled) { enabled_ = enabled; }

 private:
  static constexpr uint32_t kDebounceMs = 25;
  static constexpr uint32_t kLongPressMs = 1000;

  int pin_ = -1;
  bool enabled_ = true;

  bool raw_ = true;          // last raw sample (true = released, active low)
  bool stable_ = true;       // debounced state
  uint32_t lastChangeMs_ = 0;

  uint32_t pressedAtMs_ = 0;
  bool held_ = false;
  bool shortPending_ = false;
  bool longPending_ = false;
  bool longFired_ = false;   // long press already reported for this hold
};
