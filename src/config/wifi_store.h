// WifiStore -- WiFi credentials in their own NVS namespace.
//
// Why separate from ConfigService: credentials are secrets. Keeping them out of
// the settings JSON means the public /api/v1/config response can never leak the
// SSID/password by accident, and a "restore defaults" on the settings page
// cannot silently drop the device off the network.
//
// The values are stored in plaintext NVS. That is an accepted risk for a LAN
// device with no NVS encryption configured; it is documented in the plan's
// security section rather than pretended away.
#pragma once

#include <Arduino.h>

class WifiStore {
 public:
  // Buffers are static so callers (NetworkService) can hold the pointers.
  bool begin();
  bool hasCredentials() const { return ssid_[0] != 0; }

  const char* ssid() const { return ssid_; }
  const char* password() const { return pass_; }

  // Validates lengths (NVS/802.11 limits) and persists. Empty SSID clears.
  bool save(const char* ssid, const char* password);

 private:
  static constexpr size_t kMaxSsid = 32;
  static constexpr size_t kMaxPass = 64;

  char ssid_[kMaxSsid + 1] = {0};
  char pass_[kMaxPass + 1] = {0};
};
