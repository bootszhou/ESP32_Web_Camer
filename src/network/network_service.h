// NetworkService -- WiFi station connection with backoff reconnection.
//
// Design notes:
//   - Never block startup forever waiting for WiFi. The device must keep
//     detecting and recording to SD even when the network is down.
//   - Reconnect with increasing backoff so a missing router does not burn
//     power or spam the access point.
//   - Runtime provisioning brings up a temporary SoftAP while the station stays
//     configured: the device keeps recording, and the web UI is reachable at the
//     AP address even with no router in range.
#pragma once

#include <Arduino.h>
#include <WiFi.h>

class NetworkService {
 public:
  bool begin(const char* ssid, const char* password);

  // Call from the main loop; drives reconnection. Returns immediately.
  void loop();

  bool isConnected() const { return WiFi.status() == WL_CONNECTED; }
  IPAddress localIp() const { return WiFi.localIP(); }
  int32_t rssi() const { return isConnected() ? WiFi.RSSI() : 0; }

  // True once SNTP has produced a plausible wall clock. TLS certificate
  // validation needs this: an ESP32 boots at 1970, so a server cert whose
  // notBefore is later fails verification with MBEDTLS_ERR_X509_CERT_VERIFY_FAILED
  // (-9984) until the clock is set.
  bool timeSynced() const { return timeSynced_; }

  const char* statusText() const;

  // ---- runtime provisioning ----------------------------------------------
  // Brings up a SoftAP alongside the station so credentials can be entered from
  // a phone with no router available. Returns the AP SSID via apSsid().
  void startProvisioning();
  void stopProvisioning();
  bool provisioningActive() const { return provisioning_; }

  // Derived from the MAC so it is stable per device and not guessable by
  // pattern (still open by default; the web UI has no auth yet, see plan 5.3.5).
  String apSsid() const;

  // True while the AP is up. Auto-stops after kProvisionTimeoutMs so a forgotten
  // provisioning session does not broadcast an open AP indefinitely.
  void setProvisionTimeout(uint32_t ms) { provTimeoutMs_ = ms; }

  // Queues new station credentials. The switch happens on the NEXT loop()
  // iteration, never inside the caller's HTTP handler: disconnecting before the
  // response is sent would kill the very socket that reports success.
  void setCredentials(const char* ssid, const char* password);

  // True if a credential switch is pending (queued but not yet applied).
  bool credentialsPending() const { return pendingSsid_[0] != 0; }

  const char* ssid() const { return ssid_; }
  const char* password() const { return pass_; }

 private:
  void startConnect();
  void startTimeSync();
  void pollTimeSync();

  const char* ssid_ = nullptr;
  const char* pass_ = nullptr;
  bool started_ = false;

  uint32_t nextAttemptMs_ = 0;
  uint32_t backoffMs_ = 2000;
  static constexpr uint32_t kMaxBackoffMs = 60000;

  bool provisioning_ = false;
  uint32_t provStartMs_ = 0;
  uint32_t provTimeoutMs_ = 10UL * 60UL * 1000UL;  // 10 minutes

  // Pending credential switch. Copied into owned buffers so the caller's memory
  // does not have to outlive the request.
  char pendingSsid_[33] = {0};
  char pendingPass_[65] = {0};

  // Rollback state: while trying new credentials, keep the previous working
  // ones. If the new network never associates within kNewCredTimeoutMs, fall
  // back so a mistyped password cannot strand the device off the LAN.
  char prevSsid_[33] = {0};
  char prevPass_[65] = {0};
  bool tryingNew_ = false;
  uint32_t newCredStartMs_ = 0;
  static constexpr uint32_t kNewCredTimeoutMs = 30000;

  // How long the station may stay disconnected before the provisioning AP is
  // opened automatically as a rescue path (60 s).
  uint32_t offlineSinceMs_ = 0;
  static constexpr uint32_t kOfflineApAfterMs = 60000;

  // Wall clock (SNTP). Recording timestamps and TLS cert validation both need a
  // real date, so it is started as soon as the station associates. Retried on
  // every reconnect: a router that is slow to reach the internet must not leave
  // the device permanently stuck at 1970.
  bool timeSynced_ = false;
  uint32_t ntpStartedMs_ = 0;
  static constexpr uint32_t kNtpRetryMs = 30000;
};
