// CameraService -- the ONE place that talks to the camera.
//
// Everything else (web stream, motion detection, local display, recording)
// must obtain frames through this service. Calling esp_camera_fb_get() from
// multiple modules would make the frame buffers fight each other and produce
// tearing, dropped frames and inconsistent detection.
#pragma once

#include <Arduino.h>
#include "esp_camera.h"

class CameraService {
 public:
  struct Config {
    framesize_t frameSize = FRAMESIZE_VGA;   // 640x480
    int jpegQuality = 12;                     // lower = better quality, bigger
    int fbCount = 2;                          // double buffering in PSRAM
  };

  bool begin(const Config& cfg);
  void end();
  bool isReady() const { return ready_; }

  // Borrow a frame. Caller MUST call release() with the same pointer.
  // Returns nullptr if the camera is not ready or the capture failed.
  camera_fb_t* acquire(uint32_t timeoutMs = 1000);

  void release(camera_fb_t* fb);

  // Applies sensor tweaks that can be changed without reinitialising the
  // camera. Safe to call any time; a no-op if the sensor is not ready.
  void applyImageSettings(int jpegQuality, bool vflip, bool hflip);

  // Effective sensor settings, useful for logging and the status endpoint.
  uint16_t width() const { return width_; }
  uint16_t height() const { return height_; }

  const char* sensorName() const { return sensorName_; }

  // Average interval between successful captures, in milliseconds.
  // Updated on every acquire(); usable as a rough fps estimate.
  uint32_t averageFrameIntervalMs() const { return avgIntervalMs_; }
  float measuredFps() const;

 private:
  bool ready_ = false;
  uint16_t width_ = 0;
  uint16_t height_ = 0;
  const char* sensorName_ = "none";
  uint32_t avgIntervalMs_ = 0;
  uint32_t lastCaptureUs_ = 0;
};
