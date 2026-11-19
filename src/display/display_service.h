// DisplayService -- ST7789 TFT preview and status display.
//
// The display is a GoldenMorning GMT020-02-8P: 240x320 portrait ST7789 on a
// 4-wire SPI bus. The camera is VGA (640x480 landscape). In the default
// portrait orientation the preview is letterboxed: 1/2 scale decode to
// 320x240, then 3/4 scale to 240x180 centred on the 240x320 glass with black
// bars, so the LCD shows the same upright picture as the web page. Landscape
// rotations (1 and 3) still push the full 320x240 frame unchanged.
//
// Data path (per PROJECT_PLAN.md 3.3):
//   JPEG frame (VGA) -> JPEGDEC 1/2 scale decode -> 320x240 RGB565 (PSRAM)
//                    -> [letterbox 240x180 on 240x320] -> SPI push -> ST7789
//
// Design constraints:
//   - 5-10 fps target; the display is a convenience, not a monitor.
//   - Frame buffers live in PSRAM (150 KB each); double buffering would cost
//     300 KB, so we use a single buffer and accept tearing.
//   - The task runs at low priority and yields aggressively; recording and
//     streaming must never be blocked by the display.
//   - If the display fails to init, everything else keeps running.
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "camera/camera_service.h"
#include "network/network_service.h"
#include "storage/event_recorder.h"

class DisplayService {
 public:
  // What the panel currently shows. The preview page still draws the camera
  // image (plus the status bar); the other pages replace the image with a
  // text screen, which is cheaper than decoding a frame nobody looks at.
  enum class Page : uint8_t {
    Preview = 0,   // camera image + status bar
    Events,        // last clip, SD space, clip count
    Settings,      // editable menu driven by UiController
    Network,       // IP, SSID, RSSI, uptime, firmware
    PageCount
  };

  struct Config {
    uint8_t brightness = 200;    // 0..255, PWM duty for the backlight
    uint16_t fpsTarget = 8;      // frames per second to push to the panel
    uint16_t autoOffSec = 60;    // seconds of inactivity before backlight off
  };

  // One editable row of the settings page. The value is owned by the caller
  // (UiController) and drawn by the display, so the menu never touches NVS.
  struct MenuRow {
    const char* label = "";
    char value[16] = {0};
    bool selected = false;
    bool editing = false;
  };

  bool begin(CameraService* camera, NetworkService* network,
             EventRecorder* recorder, const Config& cfg);
  void end();

  // Kept for API compatibility; the real work runs on the display task.
  void loop();

  // Called from the display task loop; blocks on the frame queue.
  static void taskEntry(void* arg);

  void setBrightness(uint8_t b) { cfg_.brightness = b; }
  void setAutoOffSeconds(uint16_t s) { cfg_.autoOffSec = s; }

  bool isReady() const { return ready_; }
  bool backlightOn() const { return backlightOn_; }

  // Marks user activity and re-lights the panel if auto-off turned it off.
  // Public because the buttons in M6 (and any future HTTP control) are the
  // actual wake source.
  void updateActivity();

  // Rotates the panel by changing the ST7789 MADCTL register at runtime, so the
  // orientation can be found from the web UI instead of re-flashing. 0 and 2
  // are portrait letterbox modes (0 is upright and matches the web preview, 2
  // is upside down); 1 and 3 are full-screen landscape.
  void setRotation(uint8_t r);
  uint8_t rotation() const { return rotation_; }

  // Page switching. Called from UiController on a button press; safe from any
  // task because it only stores a volatile value that the display task reads.
  void setPage(Page p);
  Page page() const { return (Page)(uint8_t)page_; }

  // Supplies the rows for the Settings page. The pointer must stay valid; the
  // display only reads it. count is clamped to kMaxMenuRows.
  void setMenuRows(const MenuRow* rows, int count);
  // Text shown at the bottom of any page (transient status, e.g. "SAVED").
  void setHint(const char* text);

 private:
  static constexpr int kMaxMenuRows = 6;

  void initSpi();
  void initSt7789();
  void drawPixel(int x, int y, uint16_t color);
  void fillRect(int x, int y, int w, int h, uint16_t color);
  void drawStatusBar();
  void decodeAndShow(camera_fb_t* fb);
  void pushFrame(const uint16_t* frame, int w, int h);
  void setBacklight(bool on);
  void taskLoop();
  void updateOutputGeometry();
  void composeLetterboxPortrait();

  // Text and page rendering. All coordinates are in output (rotated) space.
  void drawText(int x, int y, const char* s, uint16_t fg, uint16_t bg, int scale = 1);
  void drawFrameRect(int x, int y, int w, int h, uint16_t color);
  void renderTextPage(Page p);
  void renderPreviewChrome();

  CameraService* camera_ = nullptr;
  NetworkService* network_ = nullptr;
  EventRecorder* recorder_ = nullptr;
  Config cfg_{};

  bool ready_ = false;
  bool backlightOn_ = false;

  // Single PSRAM frame buffer: 320 * 240 * 2 bytes = 153,600 bytes.
  uint16_t* frameBuf_ = nullptr;
  // Second buffer holding the 240x320 letterboxed portrait image. Same pixel
  // count as the frame buffer, so the cost is one more 150 KB.
  uint16_t* rotBuf_ = nullptr;

  static constexpr int kDispW = 320;
  static constexpr int kDispH = 240;

  // Geometry currently pushed to the panel, which depends on rotation_.
  int outW_ = kDispW;
  int outH_ = kDispH;

  uint32_t lastFrameMs_ = 0;
  uint32_t lastActivityMs_ = 0;
  uint32_t frameCount_ = 0;
  uint8_t rotation_ = 0;
  uint32_t decodeFails_ = 0;

  // Page state. volatile: written from the main-loop UI task, read from the
  // display task. A torn read would at worst redraw the previous page.
  volatile uint8_t page_ = (uint8_t)Page::Preview;
  // Bumped on every page change so the display task knows to repaint even if
  // the page identity did not change (e.g. entering/leaving menu edit mode).
  volatile uint32_t redrawSeq_ = 0;

  const MenuRow* menuRows_ = nullptr;
  int menuCount_ = 0;
  char hint_[32] = {0};

  TaskHandle_t taskHandle_ = nullptr;
  static constexpr int kTaskStackWords = 8192;  // 32 KB stack for JPEGDEC + SPI
};
