// Entry point: camera, motion detection, SD recording, network and web.
//
// Frame ownership rule: CameraService::acquire() is the only way to get a
// frame, and every acquired frame must be released. The web stream task and
// the main loop both acquire; the driver serialises them internally, and
// CAMERA_GRAB_LATEST keeps either side from being handed a stale picture.
//
// Settings live in NVS and are applied at boot, then refreshed at runtime
// whenever the web UI saves a change. Modules never read NVS themselves: main
// owns the translation from Settings to each module's Config.
#include <Arduino.h>
#include <ESP.h>

#include "board/board_config.h"
#include "camera/camera_service.h"
#include "config/config_service.h"
#include "config/wifi_store.h"
#include "detection/motion_detector.h"
#include "display/display_service.h"
#include "network/network_service.h"
#include "network/relay_client.h"
#include "storage/event_recorder.h"
#include "ui/ui_controller.h"
#include "web/web_server.h"
#include "secrets.h"

namespace {

CameraService gCamera;
NetworkService gNetwork;
MotionDetector gMotion;
EventRecorder gRecorder;
ConfigService gConfig;
WifiStore gWifi;
WebServer gWeb;
DisplayService gDisplay;
UiController gUi;
// Shared with the web layer (status JSON). Must stay out of the anonymous
// namespace so web_server.cpp can reference the type via the passed pointer.
RelayClient gRelay;

// Set when the web UI changes settings, so the main loop rebuilds the module
// configs once everything is quiet rather than mid-frame.
volatile bool gConfigDirty = false;

// Raised by the UI on a long press of the provisioning key. Handled in loop()
// so the AP comes up outside the button handler.
volatile bool gProvisionRequest = false;

// The recorder needs a frame for every capture while a clip is running, while
// the web stream also pulls frames. This pump only runs during recording so it
// does not compete with streaming when nothing is being captured.
void pumpRecordingFrames() {
  if (!gRecorder.isRecording()) return;

  camera_fb_t* fb = gCamera.acquire(1000);
  if (!fb) return;
  gRecorder.offerFrame(fb);
  gCamera.release(fb);
}

MotionDetector::Config motionConfigFromSettings() {
  const ConfigService::Settings& s = gConfig.get();
  MotionDetector::Config c;
  c.sensitivity      = s.sensitivity;
  c.minChangedBlocks = s.minChangedBlocks;
  c.confirmSamples   = s.confirmSamples;
  c.cooldownMs       = (uint32_t)s.cooldownSec * 1000UL;
  c.sampleIntervalMs = s.sampleIntervalMs;
  return c;
}

EventRecorder::Config recorderConfigFromSettings() {
  const ConfigService::Settings& s = gConfig.get();
  EventRecorder::Config c;
  c.clipDurationMs    = (uint32_t)s.clipDurationSec * 1000UL;
  c.nominalFps        = 10;
  c.extendOnMotion    = s.extendOnMotion;
  c.maxClipDurationMs = (uint32_t)s.maxClipSec * 1000UL;
  c.minFreeMb         = s.minFreeMb;
  return c;
}

RelayClient::Config relayConfigFromSettings() {
  const ConfigService::Settings& s = gConfig.get();
  RelayClient::Config c;
  c.host   = RELAY_HOST;
  c.port   = RELAY_PORT;
  c.token  = RELAY_TOKEN;
  c.fps    = s.relayFps;
  c.enabled = s.relayEnabled && RELAY_HOST[0] != 0;
  return c;
}

// Applies everything that can change without recreating the camera.
void applySettings() {
  gMotion.setConfig(motionConfigFromSettings());
  gRecorder.setConfig(recorderConfigFromSettings());
  gRecorder.setRecordingEnabled(gConfig.get().recordingEnabled);
  gRelay.setConfig(relayConfigFromSettings());

  // Live sensor tweaks: cheap enough to reapply on every save.
  if (gCamera.isReady()) {
    const ConfigService::Settings& s = gConfig.get();
    gCamera.applyImageSettings(s.jpegQuality, s.vflip, s.hflip);
  }
}

void printBootBanner() {
  Serial.println();
  Serial.println("################################################");
  Serial.println("#  CAM-AI  network camera                      #");
  Serial.println("#  ESP32-S3 N16R8 + OV2640 + ST7789 + microSD  #");
  Serial.println("################################################");
  Serial.printf("firmware      : 0.6.0 (M7 cloud relay)\n");
  Serial.printf("flash         : %u MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.printf("psram         : %s, %u MB free\n",
                psramFound() ? "yes" : "no",
                ESP.getFreePsram() / (1024 * 1024));
  Serial.printf("heap free     : %u KB\n", ESP.getFreeHeap() / 1024);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1200);  // let the CH343 bridge settle before the first log line

  printBootBanner();

  // Settings first: everything downstream depends on them.
  gConfig.begin();

  const ConfigService::Settings& s = gConfig.get();

  CameraService::Config camCfg;
  camCfg.frameSize   = (framesize_t)s.frameSize;
  camCfg.jpegQuality = s.jpegQuality;
  // Three buffers for three consumers: the MJPEG stream (httpd task), the
  // motion detector (main loop) and the SD recorder pump. With only two, the
  // stream held both and the detector starved, freezing its readings whenever
  // a viewer was connected.
  camCfg.fbCount     = 3;

  if (!gCamera.begin(camCfg)) {
    Serial.println("[boot] camera failed; halting so the fault is obvious");
    return;
  }
  gCamera.applyImageSettings(s.jpegQuality, s.vflip, s.hflip);

  if (!gMotion.begin(&gCamera, motionConfigFromSettings())) {
    Serial.println("[boot] motion detector unavailable; streaming continues");
  }

  if (!gRecorder.begin(&gCamera, &gMotion, recorderConfigFromSettings())) {
    Serial.println("[boot] SD unavailable; recording disabled, streaming continues");
  }
  gRecorder.setRecordingEnabled(s.recordingEnabled);

  // Stored credentials win over secrets.h, so a device provisioned on site keeps
  // its network after a reflash. secrets.h remains the build-time default.
  gWifi.begin();
  const char* netSsid = gWifi.hasCredentials() ? gWifi.ssid() : WIFI_SSID;
  const char* netPass = gWifi.hasCredentials() ? gWifi.password() : WIFI_PASS;
  // Credentials live in NVS (static buffers) for the lifetime of the program,
  // which is what NetworkService expects when it keeps the pointers.
  gNetwork.begin(netSsid, netPass);

  // Display first so the web layer can expose its rotation control; it is
  // still the lowest-priority consumer and must not block anything if it fails.
  DisplayService::Config dispCfg;
  dispCfg.brightness = s.displayBrightness;
  dispCfg.fpsTarget = 8;
  // Auto-off is safe now that M6 supplies a wake source: UiController calls
  // updateActivity() on every button press, and setPage() re-lights the panel.
  // Before that, any autoOffSec > 0 blanked the screen with no way back.
  dispCfg.autoOffSec = 60;
  if (!gDisplay.begin(&gCamera, &gNetwork, &gRecorder, dispCfg)) {
    Serial.println("[boot] display unavailable; continuing without it");
  } else {
    // Orientation is a per-panel property measured on this module: portrait
    // index 0 shows the picture upright (letterboxed, matching the web
    // preview). Persisted so a reboot keeps it.
    gDisplay.setRotation(s.displayRotation);
  }

  // Buttons and on-panel menu. Started before the web server so the panel shows
  // a page immediately, and it tolerates a missing display (gDisplay.isReady()).
  gUi.begin(&gDisplay, &gConfig, &gNetwork, &gConfigDirty,
            const_cast<bool*>(&gProvisionRequest));

  // Cloud relay (M7): dials out to the relay server. Dormant unless enabled in
  // settings AND a relay host is compiled in (secrets.h).
  gRelay.begin(&gCamera, relayConfigFromSettings());

  if (!gWeb.begin(&gCamera, &gNetwork, &gMotion, &gRecorder, &gConfig,
                  &gConfigDirty, &gDisplay, &gWifi, &gRelay)) {
    Serial.println("[boot] web server failed to start");
    return;
  }

  if (gNetwork.isConnected()) {
    Serial.printf("\n[ready] open http://%s/ in a browser\n\n",
                  gNetwork.localIp().toString().c_str());
  } else {
    Serial.println("\n[ready] no network yet; will serve once WiFi associates\n");
  }
}

void loop() {
  gNetwork.loop();
  gMotion.loop();

  // Keep NVS in step with whatever the network actually runs. When a new
  // credential fails and NetworkService rolls back, the stored copy must follow
  // -- otherwise the next boot would try the bad password again.
  {
    static String lastSsid;
    if (gNetwork.ssid() && lastSsid.length() > 0 &&
        lastSsid != gNetwork.ssid() && !gNetwork.credentialsPending()) {
      Serial.printf("[net] persisting active network \"%s\"\n", gNetwork.ssid());
      gWifi.save(gNetwork.ssid(), gNetwork.password());
    }
    if (gNetwork.ssid()) lastSsid = gNetwork.ssid();
  }

  // Buttons and menu. Non-blocking; also drives the backlight activity timer.
  gUi.loop();

  // Long press of the provisioning key: bring up the temporary AP. Done here
  // rather than in the button handler so the radio change happens in a known
  // place, once per request.
  if (gProvisionRequest) {
    gProvisionRequest = false;
    if (!gNetwork.provisioningActive()) {
      gNetwork.startProvisioning();
      Serial.printf("[boot] provisioning AP %s at http://%s/\n",
                    gNetwork.apSsid().c_str(),
                    WiFi.softAPIP().toString().c_str());
    }
  }

  // Detector fires, recorder captures.
  if (gMotion.consumeTrigger()) {
    gRecorder.onMotionTrigger();
  }

  // While motion is still present, keep the current clip open. Without this a
  // long episode ended after its base duration because the detector only fires
  // on a cooldown cadence.
  if (gMotion.isActive()) {
    gRecorder.noteActivity();
  }

  gRecorder.loop();

  // Auto-clean runs between clips, never during one: deleting files while the
  // FAT directory is being updated for an active write risks corruption.
  if (gConfig.get().autoCleanEnabled && !gRecorder.isRecording() &&
      gConfig.get().recordingEnabled) {
    static uint32_t lastCleanMs = 0;
    if (millis() - lastCleanMs > 60000) {
      lastCleanMs = millis();
      gRecorder.cleanOldest(gConfig.get().minFreeMb + 500);
    }
  }

  // Apply pending settings outside any frame-critical path.
  if (gConfigDirty) {
    gConfigDirty = false;
    applySettings();
    Serial.println("[boot] settings applied");
  }

  // Keep the panel's display config in step with the saved settings, so a
  // brightness/rotation change made on the web page shows up without a reboot.
  {
    const ConfigService::Settings& cur = gConfig.get();
    static uint8_t lastBrightness = 0xFF;
    static uint8_t lastRotation = 0xFF;
    if (cur.displayBrightness != lastBrightness) {
      lastBrightness = cur.displayBrightness;
      gDisplay.setBrightness(cur.displayBrightness);
    }
    if (cur.displayRotation != lastRotation) {
      lastRotation = cur.displayRotation;
      if (gDisplay.isReady() && gDisplay.rotation() != cur.displayRotation) {
        gDisplay.setRotation(cur.displayRotation);
      }
    }
  }

  pumpRecordingFrames();

  // RelayClient runs on its own task; nothing to pump here. The call below
  // keeps the API surface honest for future loop-driven work.
  (void)gRelay;

  delay(10);
}
