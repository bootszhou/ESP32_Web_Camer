// RelayClient -- outbound WebSocket to the cloud relay (M7 public access).
//
// The device dials the relay server (Aliyun ECS) over TLS, authenticates with a
// pre-shared token, then idles on a heartbeat. Video is ONLY pushed when the
// relay says a viewer is watching ("look-to-stream"): without this, an idle
// camera would burn the cloud server's monthly traffic for nothing.
//
// Protocol (minimal JSON text frames + binary frames):
//   device -> relay  : {"type":"hello","token":"...","ver":"0.8.0"}
//                      binary: [0x00] + one JPEG frame (MJPEG over WS)
//                      binary: [0x01] + id(2, BE) + tunnel payload (M9)
//                      {"type":"pong"}
//                      {"type":"http_fin","id":N}   tunnel channel finished
//   relay  -> device : {"type":"hello_ok","mode":"push"}
//                      {"type":"start_stream","fps":8}
//                      {"type":"stop_stream"}
//                      {"type":"ping"}            (30 s cadence)
//                      {"type":"set_quality","q":N}   (optional, runtime)
//                      {"type":"http_open","id":N,"port":80,"method":"GET","path":"/api/v1/status"}
//                      {"type":"http_data","id":N}    (immediately followed by a
//                                                      [0x01] binary frame)
//                      {"type":"http_end","id":N}     request body complete
//                      {"type":"http_close","id":N}   abandon the channel
//
// The tunnel frames carry raw HTTP bytes so the LAN page and all of its
// handlers work unchanged behind the relay. See network/tunnel_client.h.
//
// Why a hand-rolled WS client instead of a library: the frame format is tiny
// (client->server frames are unmasked per RFC6455 when sent by the client they
// MUST be masked actually -- we mask with a zero key, which is protocol-legal),
// and pulling in ArduinoWebsockets + TLS doubles flash use for one endpoint.
#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "camera/camera_service.h"
#include "network/tunnel_client.h"

class RelayClient {
 public:
  struct Config {
    const char* host = nullptr;   // relay server host (domain or IP)
    uint16_t port = 443;          // TLS port; 9000 on a plain relay is NOT used
    const char* token = nullptr;  // pre-shared device token
    const char* deviceId = "cam-01";
    uint16_t fps = 8;             // default push fps when a viewer connects
    bool enabled = false;         // master switch, from settings
  };

  // Status snapshot for the /api/v1/status endpoint and the panel.
  struct Status {
    bool enabled = false;
    bool connected = false;
    bool viewerActive = false;     // relay says someone is watching
    uint32_t framesSent = 0;
    uint32_t bytesSent = 0;        // this session, JPEG bytes pushed
    uint32_t reconnects = 0;
    uint32_t lastErrorMs = 0;      // millis() of last failure
    char lastError[64] = {0};
  };

  bool begin(CameraService* camera, const Config& cfg);
  void end();

  // Call from the main loop. Non-blocking except for actual socket I/O,
  // which stays inside its own task so the main loop never stalls.
  void loop();

  // Exposed so the web UI can show tunnel activity next to the relay state.
  TunnelClient& tunnel() { return tunnel_; }
  const TunnelClient& tunnel() const { return tunnel_; }

  void setConfig(const Config& cfg);

  const Status& status() const { return status_; }

 private:
  static void taskEntry(void* arg);
  void taskLoop();
  // One connection attempt: dial, handshake, auth, then serve until error.
  void runSession();
  bool wsHandshake(WiFiClientSecure& tls, const char* host, uint16_t port);
  bool sendText(const char* json);
  void handleTextCommand(const char* text);
  void pumpTunnel();
  bool readServerFrame(char* buf, size_t cap, uint32_t timeoutMs);
  void setLastError(const char* what);

  CameraService* camera_ = nullptr;
  Config cfg_{};
  TunnelClient tunnel_;
  volatile bool running_ = false;
  bool taskStarted_ = false;
  Status status_{};

  // Written by the task, read by status(): volatile to avoid torn reads.
  volatile bool wsConnected_ = false;
  volatile bool viewerActive_ = false;
  uint32_t frameIntervalMs_ = 125;  // written by set_quality, read by the loop

  uint32_t nextAttemptMs_ = 0;
  uint32_t backoffMs_ = 2000;
  // Set by {"type":"http_data","id":N}: the next binary frame belongs to that
  // tunnel channel. 0 means "no channel expects a body frame right now".
  uint16_t pendingTunnelId_ = 0;
  static constexpr uint32_t kMaxBackoffMs = 60000;
  static constexpr uint32_t kPingTimeoutMs = 40000;  // relay pings every 30 s
};

