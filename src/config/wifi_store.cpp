#include "wifi_store.h"

#include <Preferences.h>

namespace {
constexpr const char* kNamespace = "camwifi";
Preferences gPrefs;
}  // namespace

bool WifiStore::begin() {
  if (!gPrefs.begin(kNamespace, false)) {
    Serial.println("[wifi] NVS unavailable");
    return false;
  }

  const String s = gPrefs.getString("ssid", "");
  const String p = gPrefs.getString("pass", "");
  strncpy(ssid_, s.c_str(), kMaxSsid);
  ssid_[kMaxSsid] = 0;
  strncpy(pass_, p.c_str(), kMaxPass);
  pass_[kMaxPass] = 0;

  if (ssid_[0]) {
    Serial.printf("[wifi] stored credentials for \"%s\"\n", ssid_);
  }
  return true;
}

bool WifiStore::save(const char* ssid, const char* password) {
  if (!ssid || !*ssid) {
    // Explicit clear (used by a future "forget network" action).
    gPrefs.remove("ssid");
    gPrefs.remove("pass");
    ssid_[0] = 0;
    pass_[0] = 0;
    return true;
  }

  const size_t sl = strlen(ssid);
  const size_t pl = password ? strlen(password) : 0;
  if (sl > kMaxSsid || pl > kMaxPass) {
    Serial.println("[wifi] credentials rejected: too long");
    return false;
  }
  // WPA2 personal requires 8..63 characters when a password is given.
  if (pl > 0 && pl < 8) {
    Serial.println("[wifi] password too short (min 8)");
    return false;
  }

  strncpy(ssid_, ssid, kMaxSsid);
  ssid_[kMaxSsid] = 0;
  if (password) {
    strncpy(pass_, password, kMaxPass);
    pass_[kMaxPass] = 0;
  } else {
    pass_[0] = 0;
  }

  gPrefs.putString("ssid", ssid_);
  gPrefs.putString("pass", pass_);
  Serial.printf("[wifi] saved credentials for \"%s\"\n", ssid_);
  return true;
}
