#include "camera_service.h"

#include <esp_timer.h>

#include "board/board_config.h"

namespace {
// Exponentially weighted average, so one slow frame does not skew the figure.
constexpr float kIntervalWeight = 0.15f;

const char* sensorNameFromPid(int pid) {
  switch (pid) {
    case OV2640_PID: return "OV2640";
    case OV3660_PID: return "OV3660";
    case OV5640_PID: return "OV5640";
    default:         return "unknown";
  }
}
}  // namespace

bool CameraService::begin(const Config& cfg) {
  if (ready_) return true;

  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;

  c.pin_pwdn  = board::CAM_PIN_PWDN;
  c.pin_reset = board::CAM_PIN_RESET;
  c.pin_xclk  = board::CAM_PIN_XCLK;
  c.pin_sccb_sda = board::CAM_PIN_SIOD;
  c.pin_sccb_scl = board::CAM_PIN_SIOC;

  c.pin_d0 = board::CAM_PIN_D0;
  c.pin_d1 = board::CAM_PIN_D1;
  c.pin_d2 = board::CAM_PIN_D2;
  c.pin_d3 = board::CAM_PIN_D3;
  c.pin_d4 = board::CAM_PIN_D4;
  c.pin_d5 = board::CAM_PIN_D5;
  c.pin_d6 = board::CAM_PIN_D6;
  c.pin_d7 = board::CAM_PIN_D7;

  c.pin_vsync = board::CAM_PIN_VSYNC;
  c.pin_href  = board::CAM_PIN_HREF;
  c.pin_pclk  = board::CAM_PIN_PCLK;

  c.xclk_freq_hz = 20000000;

  // JPEG straight from the sensor: no CPU-side encoding needed, which is what
  // makes MJPEG streaming possible on this chip at all.
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size   = cfg.frameSize;
  c.jpeg_quality = cfg.jpegQuality;

  // LATEST rather than WHEN_EMPTY: when the network or SD is slower than the
  // sensor we want the newest picture, not a backlog of stale ones.
  c.grab_mode = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.fb_count    = constrain(cfg.fbCount, 1, 3);
  } else {
    // Should never happen on N16R8, but degrade gracefully instead of failing.
    c.fb_location = CAMERA_FB_IN_DRAM;
    c.fb_count    = 1;
    c.frame_size  = FRAMESIZE_SVGA;
  }

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("[camera] init failed: 0x%x\n", err);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (!s) {
    Serial.println("[camera] sensor handle unavailable");
    esp_camera_deinit();
    return false;
  }

  sensorName_ = sensorNameFromPid(s->id.PID);

  // This module reports itself flipped by default; correct it here so that
  // stream, recording and detection all share the same orientation.
  s->set_vflip(s, 1);
  s->set_brightness(s, 1);
  s->set_saturation(s, 0);

  width_  = 640;
  height_ = 480;
  ready_  = true;

  Serial.printf("[camera] ready: %s  %ux%u  q=%d  fb=%d\n",
                sensorName_, width_, height_, cfg.jpegQuality, c.fb_count);
  return true;
}

void CameraService::end() {
  if (!ready_) return;
  esp_camera_deinit();
  ready_ = false;
}

camera_fb_t* CameraService::acquire(uint32_t timeoutMs) {
  if (!ready_) return nullptr;

  const int64_t startUs = esp_timer_get_time();
  camera_fb_t* fb = nullptr;

  // esp_camera_fb_get() blocks; poll so a stuck sensor cannot wedge the caller.
  const uint32_t deadline = millis() + timeoutMs;
  do {
    fb = esp_camera_fb_get();
    if (fb) break;
    delay(2);
  } while ((int32_t)(millis() - deadline) < 0);

  if (!fb) return nullptr;

  const int64_t nowUs = esp_timer_get_time();
  if (lastCaptureUs_ != 0) {
    const uint32_t dtMs = (uint32_t)((nowUs - lastCaptureUs_) / 1000);
    avgIntervalMs_ = (avgIntervalMs_ == 0)
                         ? dtMs
                         : (uint32_t)(avgIntervalMs_ * (1.0f - kIntervalWeight) +
                                      dtMs * kIntervalWeight);
  }
  lastCaptureUs_ = nowUs;

  if (fb->format == PIXFORMAT_JPEG) {
    width_  = fb->width;
    height_ = fb->height;
  }
  (void)startUs;
  return fb;
}

void CameraService::release(camera_fb_t* fb) {
  if (fb) esp_camera_fb_return(fb);
}

void CameraService::applyImageSettings(int jpegQuality, bool vflip, bool hflip) {
  if (!ready_) return;

  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;

  // These are register writes on the sensor, not a reinit, so orientation and
  // quality can be changed live without dropping the stream.
  s->set_vflip(s, vflip ? 1 : 0);
  s->set_hmirror(s, hflip ? 1 : 0);
  s->set_quality(s, constrain(jpegQuality, 6, 63));

  Serial.printf("[camera] applied q=%d vflip=%d hflip=%d\n",
                jpegQuality, vflip, hflip);
}

float CameraService::measuredFps() const {
  if (avgIntervalMs_ == 0) return 0.0f;
  return 1000.0f / (float)avgIntervalMs_;
}
