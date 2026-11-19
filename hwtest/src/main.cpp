/*
 * Final integration check: camera + SD concurrently, with BL toggling.
 *
 * This is the pass/fail gate for M0.5. It exercises what the real firmware
 * will do: capture a JPEG frame, write it to SD, and pulse the backlight,
 * all at the same time. If SD and the backlight shared a pin, writes would
 * fail as soon as the backlight toggled.
 */
#include <Arduino.h>
#include <ESP.h>
#include "driver/gpio.h"
#include "SD_MMC.h"
#include "esp_camera.h"

#define TFT_BL_PIN 42
#define TFT_CS_PIN 45
#define TFT_DC_PIN 47
#define SD_CLK 39
#define SD_CMD 38
#define SD_D0  40

// Camera pins (CAMERA_MODEL_ESP32S3_EYE)
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM   4
#define SIOC_GPIO_NUM   5
#define Y9_GPIO_NUM    16
#define Y8_GPIO_NUM    17
#define Y7_GPIO_NUM    18
#define Y6_GPIO_NUM    12
#define Y5_GPIO_NUM    10
#define Y4_GPIO_NUM     8
#define Y3_GPIO_NUM     9
#define Y2_GPIO_NUM    11
#define VSYNC_GPIO_NUM  6
#define HREF_GPIO_NUM   7
#define PCLK_GPIO_NUM  13

static void banner(const char *t) {
  Serial.println();
  Serial.println("========================================");
  Serial.printf("  %s\n", t);
  Serial.println("========================================");
}

static bool initCamera() {
  camera_config_t cfg = {};
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.pin_d0 = Y2_GPIO_NUM;  cfg.pin_d1 = Y3_GPIO_NUM;
  cfg.pin_d2 = Y4_GPIO_NUM;  cfg.pin_d3 = Y5_GPIO_NUM;
  cfg.pin_d4 = Y6_GPIO_NUM;  cfg.pin_d5 = Y7_GPIO_NUM;
  cfg.pin_d6 = Y8_GPIO_NUM;  cfg.pin_d7 = Y9_GPIO_NUM;
  cfg.pin_xclk = XCLK_GPIO_NUM;
  cfg.pin_pclk = PCLK_GPIO_NUM;
  cfg.pin_vsync = VSYNC_GPIO_NUM;
  cfg.pin_href = HREF_GPIO_NUM;
  cfg.pin_sscb_sda = SIOD_GPIO_NUM;
  cfg.pin_sscb_scl = SIOC_GPIO_NUM;
  cfg.pin_pwdn = PWDN_GPIO_NUM;
  cfg.pin_reset = RESET_GPIO_NUM;
  cfg.xclk_freq_hz = 20000000;
  cfg.frame_size = FRAMESIZE_VGA;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.grab_mode = CAMERA_GRAB_LATEST;
  cfg.fb_location = CAMERA_FB_IN_PSRAM;
  cfg.jpeg_quality = 12;
  cfg.fb_count = 2;
  return esp_camera_init(&cfg) == ESP_OK;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("################################################");
  Serial.println("#  FINAL: camera + SD + backlight concurrently");
  Serial.println("################################################");

  gpio_reset_pin((gpio_num_t)TFT_BL_PIN);
  gpio_set_direction((gpio_num_t)TFT_BL_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)TFT_BL_PIN, 1);  // backlight on

  if (!initCamera()) { Serial.println("camera init FAILED"); return; }
  Serial.println("camera: OK");

  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 3)) {
    Serial.println("SD mount FAILED"); return;
  }
  Serial.printf("SD: OK, %llu MB\n", SD_MMC.cardSize() / (1024 * 1024));

  File f = SD_MMC.open("/_final.avi", FILE_WRITE);
  if (!f) { Serial.println("file open FAILED"); return; }

  banner("10 SECONDS OF CAPTURE+WRITE (backlight toggling)");
  uint32_t t0 = millis();
  int frames = 0, bytes = 0;
  bool bl = true;

  while (millis() - t0 < 10000) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      size_t n = f.write(fb->buf, fb->len);
      if (n != fb->len) {
        Serial.printf("SHORT WRITE at frame %d\n", frames);
        esp_camera_fb_return(fb);
        break;
      }
      bytes += n;
      frames++;
      esp_camera_fb_return(fb);
    }

    // Toggle backlight constantly: if BL and SD shared GPIO38 this would
    // corrupt the SD command line and the write above would fail.
    bl = !bl;
    gpio_set_level((gpio_num_t)TFT_BL_PIN, bl);

    delay(90);
  }

  f.flush();
  f.close();
  gpio_set_level((gpio_num_t)TFT_BL_PIN, 1);

  uint32_t dt = millis() - t0;
  banner("RESULT");
  Serial.printf("Frames captured : %d in %u ms\n", frames, dt);
  Serial.printf("Bytes written   : %d (%.1f KB avg/frame)\n",
                bytes, frames ? (bytes / 1024.0) / frames : 0);
  Serial.printf("Measured fps    : %.1f\n", frames * 1000.0 / dt);
  Serial.printf("Write rate      : %.0f KB/s\n",
                dt ? (bytes / 1024.0) / (dt / 1000.0) : 0);

  File chk = SD_MMC.open("/_final.avi");
  if (chk) {
    Serial.printf("File on SD      : %u bytes (match=%s)\n",
                  (unsigned)chk.size(), chk.size() == (size_t)bytes ? "YES" : "NO");
    chk.close();
  }
  SD_MMC.remove("/_final.avi");
  SD_MMC.end();
  esp_camera_deinit();

  Serial.println();
  Serial.println("PASS criteria: no short write, file size matches,");
  Serial.println("frames ~ 100 over 10s, backlight still controllable.");
  Serial.println("========================================");
}

void loop() { delay(5000); }
