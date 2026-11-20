// WebServer -- HTTP endpoints for the browser UI, MJPEG stream and status API.
//
// Built on esp_http_server rather than WebServer: it runs in its own task and
// gives explicit control over chunked responses, which MJPEG needs. It also
// avoids the 1 KB header limits of the Arduino WebServer class.
#pragma once

#include <Arduino.h>
#include <esp_http_server.h>

#include "camera/camera_service.h"
#include "config/config_service.h"
#include "config/wifi_store.h"
#include "detection/motion_detector.h"
#include "display/display_service.h"
#include "network/network_service.h"
#include "storage/event_recorder.h"

class WebServer {
 public:
  // settingsDirty is owned by main: the web layer only raises the flag, it
  // never mutates module configs itself.
  bool begin(CameraService* camera, NetworkService* network,
             MotionDetector* motion = nullptr, EventRecorder* recorder = nullptr,
             ConfigService* config = nullptr,
             volatile bool* settingsDirty = nullptr,
             DisplayService* display = nullptr,
             WifiStore* wifi = nullptr,
             class RelayClient* relay = nullptr);
  void end();

  uint32_t streamClients() const { return streamClients_; }

 private:
  static esp_err_t handleIndex(httpd_req_t* req);
  static esp_err_t handleStatus(httpd_req_t* req);
  static esp_err_t handleSnapshot(httpd_req_t* req);
  static esp_err_t handleStream(httpd_req_t* req);
  static esp_err_t handlePlayback(httpd_req_t* req);
  static esp_err_t handlePlaybackStatus(httpd_req_t* req);
  static esp_err_t handlePlaybackCancel(httpd_req_t* req);
  static esp_err_t handleMotionPreview(httpd_req_t* req);
  static esp_err_t handleEvents(httpd_req_t* req);
  static esp_err_t handleEventFile(httpd_req_t* req);
  static esp_err_t handleEventDelete(httpd_req_t* req);
  static esp_err_t handleGetConfig(httpd_req_t* req);
  static esp_err_t handleSetConfig(httpd_req_t* req);
  static esp_err_t handleCleanup(httpd_req_t* req);
  static esp_err_t handleStopRecording(httpd_req_t* req);
  static esp_err_t handleRotate(httpd_req_t* req);
  static esp_err_t handleWifiGet(httpd_req_t* req);
  static esp_err_t handleWifiSet(httpd_req_t* req);

  static WebServer* instance_;

  httpd_handle_t server_ = nullptr;
  // Dedicated second httpd instance serving ONLY /stream, on kStreamPort.
  //
  // This esp_http_server build runs every URI of one server in a single task,
  // and handleStream() never returns while a viewer is connected. delay()
  // inside the stream loop does NOT help: the task is still inside the
  // handler, so the server loop never accepts the next request -- every other
  // endpoint (status, config, stop) queued behind the stream and the UI
  // froze. A second httpd instance gets its own task, so the API stays
  // responsive no matter how long the stream runs.
  httpd_handle_t streamServer_ = nullptr;
  CameraService* camera_ = nullptr;
  NetworkService* network_ = nullptr;
  MotionDetector* motion_ = nullptr;
  EventRecorder* recorder_ = nullptr;
  ConfigService* config_ = nullptr;
  volatile bool* settingsDirty_ = nullptr;
  DisplayService* display_ = nullptr;
  // Credentials live outside ConfigService so they never appear in the public
  // settings JSON. May be null if NVS failed; the endpoints then report 503.
  WifiStore* wifi_ = nullptr;
  // Cloud relay client (M7). May be null; the status JSON then omits relay
  // state rather than failing.
  class RelayClient* relay_ = nullptr;
  static uint32_t streamClients_;
  static uint32_t playbackClients_;
};
