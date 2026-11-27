#include "network_service.h"

#include <time.h>

namespace {
// Character buffer for the AP SSID so apSsid() can return a stable pointer.
char gApSsid[32] = {0};

// Anything before 2024-01-01 means SNTP has not landed yet. Compared as an
// epoch threshold so the logic does not care about the actual timezone.
constexpr time_t kPlausibleEpoch = 1704067200;

// Two public pools; sntp keeps trying until one answers.
const char* const kNtpServer1 = "ntp.aliyun.com";
const char* const kNtpServer2 = "pool.ntp.org";
}  // namespace

bool NetworkService::begin(const char* ssid, const char* password) {
  ssid_ = ssid;
  pass_ = password;

  if (!ssid_ || !*ssid_) {
    Serial.println("[net] no SSID configured");
    return false;
  }

  WiFi.mode(WIFI_STA);
  // Modem sleep makes the camera stream stutter badly; keep the radio awake.
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);  // we drive reconnection ourselves for backoff

  startConnect();

  // Wait briefly for the first association so the user sees an IP at boot,
  // but never hang forever: LAN-less operation must still proceed.
  const uint32_t deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && (int32_t)(millis() - deadline) < 0) {
    delay(200);
    Serial.print('.');
  }
  Serial.println();

  if (isConnected()) {
    Serial.printf("[net] connected to %s, IP %s, RSSI %d dBm\n",
                  ssid_, WiFi.localIP().toString().c_str(), WiFi.RSSI());
    startTimeSync();
  } else {
    Serial.println("[net] not connected yet, will keep retrying in background");
  }
  return isConnected();
}

void NetworkService::startConnect() {
  Serial.printf("[net] connecting to \"%s\" ...\n", ssid_);
  WiFi.begin(ssid_, pass_);
  started_ = true;
  nextAttemptMs_ = millis() + 15000;  // give this attempt 15s to succeed
}

void NetworkService::startTimeSync() {
  // configTime is idempotent and cheap, but re-arming it on every loop pass
  // would restart the SNTP request flood. Only call it on a fresh attempt.
  configTime(0, 0, kNtpServer1, kNtpServer2);
  ntpStartedMs_ = millis();
  timeSynced_ = false;
}

void NetworkService::pollTimeSync() {
  const time_t now = time(nullptr);
  if (now > kPlausibleEpoch) {
    if (!timeSynced_) {
      timeSynced_ = true;
      struct tm tmv;
      localtime_r(&now, &tmv);
      char stamp[32];
      strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
      Serial.printf("[net] clock synced: %s (UTC)\n", stamp);
    }
    return;
  }

  // Not synced yet: retry occasionally in case the first servers were
  // unreachable (captive portal, DNS not up yet, etc.).
  if (millis() - ntpStartedMs_ > kNtpRetryMs) {
    Serial.println("[net] clock still unset; retrying SNTP");
    startTimeSync();
  }
}

void NetworkService::loop() {
  if (provisioning_) {
    if (millis() - provStartMs_ > provTimeoutMs_) {
      Serial.println("[net] provisioning window expired");
      stopProvisioning();
    }
  }

  // Apply a queued credential switch here, in the loop, not in the HTTP
  // handler. By now the response has been flushed, so dropping the station
  // cannot truncate the "saved" reply the user is waiting for.
  if (pendingSsid_[0]) {
    Serial.printf("[net] switching to \"%s\"\n", pendingSsid_);

    // Remember the working credentials so a bad new one can be rolled back.
    if (!tryingNew_ && ssid_) {
      strncpy(prevSsid_, ssid_, sizeof(prevSsid_) - 1);
      strncpy(prevPass_, pass_ ? pass_ : "", sizeof(prevPass_) - 1);
      prevSsid_[sizeof(prevSsid_) - 1] = 0;
      prevPass_[sizeof(prevPass_) - 1] = 0;
    }

    ssid_ = pendingSsid_;
    pass_ = pendingPass_;

    WiFi.disconnect(false, false);
    delay(50);
    backoffMs_ = 2000;
    startConnect();

    tryingNew_ = true;
    newCredStartMs_ = millis();

    pendingSsid_[0] = 0;
    pendingPass_[0] = 0;
    return;
  }

  // New credentials succeeded: forget the rollback copy.
  if (tryingNew_ && isConnected()) {
    Serial.printf("[net] new credentials for \"%s\" confirmed\n", ssid_);
    tryingNew_ = false;
    prevSsid_[0] = 0;
    prevPass_[0] = 0;
  }

  // New credentials never worked: put the old ones back. The NVS copy is left
  // untouched here on purpose -- main reads the rollback through ssid()/pass()
  // and decides whether to re-save, so the two stay consistent.
  if (tryingNew_ && !isConnected() &&
      millis() - newCredStartMs_ > kNewCredTimeoutMs) {
    Serial.printf("[net] new network failed; rolling back to \"%s\"\n", prevSsid_);
    if (prevSsid_[0]) {
      strncpy(pendingSsid_, prevSsid_, sizeof(pendingSsid_) - 1);
      strncpy(pendingPass_, prevPass_, sizeof(pendingPass_) - 1);
      pendingSsid_[sizeof(pendingSsid_) - 1] = 0;
      pendingPass_[sizeof(pendingPass_) - 1] = 0;
    }
    tryingNew_ = false;
    prevSsid_[0] = 0;
    prevPass_[0] = 0;
  }

  if (!ssid_) return;

  if (isConnected()) {
    // Reset backoff once we get through, so the next outage retries promptly.
    backoffMs_ = 2000;
    offlineSinceMs_ = 0;
    // Arm the clock on first association after boot (or after a retry reset).
    if (ntpStartedMs_ == 0) startTimeSync();
    pollTimeSync();
    return;
  }

  // If the station has never associated (wrong password, router gone) for a
  // sustained period, open the provisioning AP on its own. Without this a bad
  // credential stored in NVS bricks the LAN path with no way to reach the UI:
  // the user cannot press a button they may not know about, and the web page is
  // unreachable precisely when it is needed to fix the password.
  if (!provisioning_) {
    if (offlineSinceMs_ == 0) offlineSinceMs_ = millis();
    if (millis() - offlineSinceMs_ > kOfflineApAfterMs) {
      Serial.println("[net] offline too long; opening provisioning AP for recovery");
      startProvisioning();
    }
  }

  if ((int32_t)(millis() - nextAttemptMs_) < 0) return;

  Serial.printf("[net] reconnecting (backoff %lu ms)\n", (unsigned long)backoffMs_);
  WiFi.disconnect(false, false);
  delay(50);
  startConnect();

  backoffMs_ = min(backoffMs_ * 2, kMaxBackoffMs);
  nextAttemptMs_ = millis() + 15000;
}

const char* NetworkService::statusText() const {
  if (provisioning_) return "provisioning-ap";
  switch (WiFi.status()) {
    case WL_CONNECTED:        return "connected";
    case WL_NO_SSID_AVAIL:    return "ssid-not-found";
    case WL_CONNECT_FAILED:   return "connect-failed";
    case WL_CONNECTION_LOST:  return "connection-lost";
    case WL_DISCONNECTED:     return "disconnected";
    case WL_IDLE_STATUS:      return "idle";
    default:                  return "unknown";
  }
}

String NetworkService::apSsid() const {
  // MAC-derived suffix: stable across reboots, unique per device.
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(gApSsid, sizeof(gApSsid), "CAM-AI-%04X",
           (unsigned)(mac & 0xFFFF));
  return String(gApSsid);
}

void NetworkService::startProvisioning() {
  if (provisioning_) return;

  // AP+STA is required: plain WIFI_STA rejects softAP(), which is why the
  // rescue path silently failed before. The station keeps its config and
  // reconnects in the background while the AP serves the setup page.
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  const String ap = apSsid();
  const bool ok = WiFi.softAP(ap.c_str());
  if (!ok) {
    Serial.println("[net] softAP start failed");
    WiFi.mode(WIFI_STA);  // revert so the station keeps trying
    return;
  }

  provisioning_ = true;
  provStartMs_ = millis();

  Serial.printf("[net] provisioning AP \"%s\" open, browse http://%s/\n",
                ap.c_str(), WiFi.softAPIP().toString().c_str());
}

void NetworkService::stopProvisioning() {
  if (!provisioning_) return;
  WiFi.softAPdisconnect(true);
  provisioning_ = false;
  // Back to station-only mode; the station stays configured and reconnects on
  // the normal loop cadence.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  Serial.println("[net] provisioning AP stopped");
}

void NetworkService::setCredentials(const char* ssid, const char* password) {
  // Only queue the switch. loop() performs it on the next iteration so the
  // HTTP response that triggered this call can still be delivered.
  strncpy(pendingSsid_, ssid ? ssid : "", sizeof(pendingSsid_) - 1);
  pendingSsid_[sizeof(pendingSsid_) - 1] = 0;
  strncpy(pendingPass_, password ? password : "", sizeof(pendingPass_) - 1);
  pendingPass_[sizeof(pendingPass_) - 1] = 0;

  Serial.printf("[net] credentials for \"%s\" queued\n", pendingSsid_);
}
