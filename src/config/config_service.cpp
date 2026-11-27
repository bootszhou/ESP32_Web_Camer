#include "config_service.h"

#include <ArduinoJson.h>
#include <Preferences.h>

namespace {
constexpr const char* kNamespace = "camai";
constexpr uint32_t kVersion = 1;

// Keys are kept short: NVS limits key length to 15 characters.
Preferences gPrefs;

template <typename T>
void putOr(const char* key, T value) {
  gPrefs.putUInt(key, (uint32_t)value);
}

uint32_t getU32(const char* key, uint32_t fallback) {
  return gPrefs.getUInt(key, fallback);
}
}  // namespace

bool ConfigService::begin() {
  nvsReady_ = gPrefs.begin(kNamespace, false);
  if (!nvsReady_) {
    Serial.println("[config] NVS unavailable, using defaults");
    s_ = Settings{};
    return false;
  }

  if (!gPrefs.isKey("ver")) {
    // First boot: persist the defaults so the device always has a known state.
    Serial.println("[config] no settings found, writing defaults");
    s_ = Settings{};
    save();
    return true;
  }

  loadFromNvs();
  validate();
  Serial.printf("[config] loaded: sens=%u minBlk=%u cooldown=%us clip=%us q=%u\n",
                s_.sensitivity, s_.minChangedBlocks, s_.cooldownSec,
                s_.clipDurationSec, s_.jpegQuality);
  return true;
}

void ConfigService::loadFromNvs() {
  s_.sensitivity       = getU32("sens", s_.sensitivity);
  s_.minChangedBlocks  = getU32("minblk", s_.minChangedBlocks);
  s_.confirmSamples    = getU32("confirm", s_.confirmSamples);
  s_.cooldownSec       = getU32("cool", s_.cooldownSec);
  s_.sampleIntervalMs  = getU32("sampint", s_.sampleIntervalMs);

  s_.clipDurationSec   = getU32("clip", s_.clipDurationSec);
  s_.extendOnMotion    = getU32("extend", s_.extendOnMotion ? 1 : 0) != 0;
  s_.maxClipSec        = getU32("maxclip", s_.maxClipSec);
  s_.recordingEnabled  = getU32("recen", s_.recordingEnabled ? 1 : 0) != 0;

  s_.frameSize         = getU32("fsize", s_.frameSize);
  s_.jpegQuality       = getU32("jpegq", s_.jpegQuality);
  s_.vflip             = getU32("vflip", s_.vflip ? 1 : 0) != 0;
  s_.hflip             = getU32("hflip", s_.hflip ? 1 : 0) != 0;

  s_.minFreeMb         = getU32("minfree", s_.minFreeMb);
  s_.autoCleanEnabled  = getU32("autocln", s_.autoCleanEnabled ? 1 : 0) != 0;

  s_.relayEnabled      = getU32("relayen", s_.relayEnabled ? 1 : 0) != 0;
  s_.relayFps          = getU32("relayfps", s_.relayFps);
}

void ConfigService::validate() {
  // Clamp rather than reject: a value written by a future firmware revision (or
  // one bit-flipped in storage) should degrade to something harmless.
  const Settings d{};
  s_.sensitivity      = constrain(s_.sensitivity, 1, 100);
  s_.minChangedBlocks = constrain(s_.minChangedBlocks, 1, 20);
  s_.confirmSamples   = constrain(s_.confirmSamples, 1, 5);
  s_.cooldownSec      = constrain(s_.cooldownSec, 1, 600);
  s_.sampleIntervalMs = constrain(s_.sampleIntervalMs, 100, 2000);

  s_.clipDurationSec  = constrain(s_.clipDurationSec, 1, 120);
  s_.maxClipSec       = constrain(s_.maxClipSec, 10, 300);
  if (s_.maxClipSec < s_.clipDurationSec) {
    s_.maxClipSec = s_.clipDurationSec;
  }

  // 0..21 are the framesize enum values used by esp32-camera; VGA (8) is the
  // default. Anything outside that range means corrupt storage.
  s_.frameSize   = (s_.frameSize <= 21) ? s_.frameSize : d.frameSize;
  s_.jpegQuality = constrain(s_.jpegQuality, 6, 30);

  s_.minFreeMb   = constrain(s_.minFreeMb, 10, 30000);

  s_.relayFps = constrain(s_.relayFps, 1, 15);
}

bool ConfigService::save() {
  if (!nvsReady_) return false;

  putOr("sens", s_.sensitivity);
  putOr("minblk", s_.minChangedBlocks);
  putOr("confirm", s_.confirmSamples);
  putOr("cool", s_.cooldownSec);
  putOr("sampint", s_.sampleIntervalMs);

  putOr("clip", s_.clipDurationSec);
  putOr("extend", s_.extendOnMotion ? 1 : 0);
  putOr("maxclip", s_.maxClipSec);
  putOr("recen", s_.recordingEnabled ? 1 : 0);

  putOr("fsize", s_.frameSize);
  putOr("jpegq", s_.jpegQuality);
  putOr("vflip", s_.vflip ? 1 : 0);
  putOr("hflip", s_.hflip ? 1 : 0);

  putOr("minfree", s_.minFreeMb);
  putOr("autocln", s_.autoCleanEnabled ? 1 : 0);

  putOr("relayen", s_.relayEnabled ? 1 : 0);
  putOr("relayfps", s_.relayFps);

  gPrefs.putUInt("ver", kVersion);

  Serial.println("[config] saved to NVS");
  return true;
}

String ConfigService::toJson() const {
  JsonDocument doc;
  doc["sensitivity"]      = s_.sensitivity;
  doc["min_changed"]      = s_.minChangedBlocks;
  doc["confirm_samples"]  = s_.confirmSamples;
  doc["cooldown_s"]       = s_.cooldownSec;
  doc["sample_interval_ms"] = s_.sampleIntervalMs;

  doc["clip_duration_s"]  = s_.clipDurationSec;
  doc["extend_on_motion"] = s_.extendOnMotion;
  doc["max_clip_s"]       = s_.maxClipSec;
  doc["recording_enabled"] = s_.recordingEnabled;

  doc["frame_size"]       = s_.frameSize;
  doc["jpeg_quality"]     = s_.jpegQuality;
  doc["vflip"]            = s_.vflip;
  doc["hflip"]            = s_.hflip;

  doc["min_free_mb"]      = s_.minFreeMb;
  doc["auto_clean"]       = s_.autoCleanEnabled;

  doc["relay_enabled"] = s_.relayEnabled;
  doc["relay_fps"]     = s_.relayFps;

  String out;
  serializeJson(doc, out);
  return out;
}

bool ConfigService::updateFromJson(const char* json, size_t len, String& error) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json, len);
  if (err) {
    error = String("invalid json: ") + err.c_str();
    return false;
  }

  // Apply onto a copy so a partially invalid payload changes nothing.
  Settings next = s_;

  auto asU8 = [&](const char* k, uint8_t& out, int lo, int hi) -> bool {
    if (!doc[k].is<int>()) return true;  // absent key is fine
    const int v = doc[k].as<int>();
    if (v < lo || v > hi) {
      error = String(k) + " out of range (" + lo + ".." + hi + ")";
      return false;
    }
    out = (uint8_t)v;
    return true;
  };
  auto asU16 = [&](const char* k, uint16_t& out, int lo, int hi) -> bool {
    if (!doc[k].is<int>()) return true;
    const int v = doc[k].as<int>();
    if (v < lo || v > hi) {
      error = String(k) + " out of range (" + lo + ".." + hi + ")";
      return false;
    }
    out = (uint16_t)v;
    return true;
  };
  auto asBool = [&](const char* k, bool& out) -> bool {
    if (doc[k].is<bool>()) { out = doc[k].as<bool>(); return true; }
    if (doc[k].is<int>())  { out = doc[k].as<int>() != 0; return true; }
    return true;
  };

  if (!asU8("sensitivity", next.sensitivity, 1, 100)) return false;
  // Cap at 20 of the 70 blocks (~29%). Values near 35 (50%) made the detector
  // effectively deaf: real movement changes only 10-25 blocks, so nothing ever
  // triggered, and the UI gave no hint that the setting was the cause.
  if (!asU8("min_changed", next.minChangedBlocks, 1, 20)) return false;
  if (!asU8("confirm_samples", next.confirmSamples, 1, 5)) return false;
  if (!asU16("cooldown_s", next.cooldownSec, 1, 600)) return false;
  if (!asU16("sample_interval_ms", next.sampleIntervalMs, 100, 2000)) return false;

  if (!asU16("clip_duration_s", next.clipDurationSec, 1, 120)) return false;
  if (!asU16("max_clip_s", next.maxClipSec, 10, 300)) return false;
  if (!asU8("frame_size", next.frameSize, 5, 21)) return false;
  if (!asU8("jpeg_quality", next.jpegQuality, 6, 30)) return false;
  if (!asU16("min_free_mb", next.minFreeMb, 10, 30000)) return false;

  if (!asBool("extend_on_motion", next.extendOnMotion)) return false;
  if (!asBool("recording_enabled", next.recordingEnabled)) return false;
  if (!asBool("vflip", next.vflip)) return false;
  if (!asBool("hflip", next.hflip)) return false;
  if (!asBool("auto_clean", next.autoCleanEnabled)) return false;

  if (!asBool("relay_enabled", next.relayEnabled)) return false;
  if (!asU8("relay_fps", next.relayFps, 1, 15)) return false;

  if (next.maxClipSec < next.clipDurationSec) {
    error = "max_clip_s must be >= clip_duration_s";
    return false;
  }

  s_ = next;
  return true;
}
