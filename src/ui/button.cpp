#include "button.h"

void Button::begin(int pin) {
  pin_ = pin;
  if (pin_ < 0) return;
  pinMode(pin_, INPUT_PULLUP);
  raw_ = digitalRead(pin_) != 0;
  stable_ = raw_;
  lastChangeMs_ = millis();
}

void Button::update() {
  if (pin_ < 0) return;

  const bool sample = digitalRead(pin_) != 0;
  if (sample != raw_) {
    // Level changed: restart the debounce timer.
    raw_ = sample;
    lastChangeMs_ = millis();
  }

  if (raw_ != stable_ && millis() - lastChangeMs_ >= kDebounceMs) {
    stable_ = raw_;

    if (!stable_) {
      // Press edge.
      pressedAtMs_ = millis();
      held_ = true;
      longFired_ = false;
    } else {
      // Release edge: a short press only counts if the long press has not
      // already been reported for this hold, so one long hold never yields
      // both events.
      held_ = false;
      if (enabled_ && !longFired_) shortPending_ = true;
    }
  }

  if (held_ && enabled_ && !longFired_ &&
      millis() - pressedAtMs_ >= kLongPressMs) {
    longFired_ = true;
    longPending_ = true;
  }
}

bool Button::consumeShortPress() {
  const bool v = shortPending_;
  shortPending_ = false;
  return v;
}

bool Button::consumeLongPress() {
  const bool v = longPending_;
  longPending_ = false;
  return v;
}

uint32_t Button::heldMs() const {
  return held_ ? (millis() - pressedAtMs_) : 0;
}
