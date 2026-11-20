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
#include "storage/avi_reader.h"
#include "storage/event_recorder.h"
#include <FS.h>

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

  // Recent clip list for the Events page. Called by the display task itself
  // (scanClips) -- the recorder only owns the SD card, the display just reads
  // names from its cached copy.
  struct ClipEntry {
    char name[24];   // "161500_001" style short label
    char dir[12];    // date directory "20261120"
    uint32_t kb;     // file size in KB
  };
  // Scan newest-first up to kMaxClipList clips from /events. Called on the
  // display task when the Events page repaints (SD scan is slow, so only on
  // page change, never per frame). Returns the number found.
  int scanClips();

  // ---- on-panel clip playback -------------------------------------------
  // Selection index into clipList_ for the Events page highlight bar.
  // Written from the UI task, read on the display task; a torn read at worst
  // highlights the neighbouring row for one repaint.
  volatile int clipSelected_ = 0;
  int clipSelected() const { return clipSelected_; }
  void setClipSelected(int i);
  // Clip count from the last scanClips() pass, for UI-side wrap logic.
  int clipCountPublic() const { return clipCount_; }

  // Requests playback of clipList_[index]. Actual start happens on the
  // display task (it owns the SPI bus and the JPEG decoder). index is
  // validated against clipCount_ there; if the list has been rescanned since,
  // the request may be dropped or clamped.
  void requestPlayback(int index);
  // Stops playback (if any) and repaints the Events list. Safe from any task.
  void stopPlayback();
  // Pause/resume during playback. Ignored when idle.
  void togglePause() { if (playState_ == (uint8_t)PlayState::Playing) playPaused_ = !playPaused_; }
  bool isPlaying() const { return playState_ == (uint8_t)PlayState::Playing; }

 private:
  // Playback state machine on the display task: idle -> loading -> playing.
  // Playing replaces the preview/repaint loop; every frame is decoded with
  // the same JPEGDEC path as the live preview, then a progress bar is drawn
  // on top. Exit conditions: end of file, requestPlaybackStop_, or an error.
  enum class PlayState : uint8_t { Idle = 0, Playing };
  void startPendingPlayback();
  void playTaskLoop();
  void endPlayback(const char* reason);
  // Returns true if a frame was decoded and pushed; false on decode failure
  // (playTaskLoop counts consecutive failures and aborts the clip after a
  // threshold, so a corrupt file cannot leave the panel frozen for minutes).
  bool renderPlaybackFrame(const uint8_t* jpeg, size_t len, int progress);
  void renderPlaybackOverlay(int progress, bool paused);

  volatile uint8_t playState_ = (uint8_t)PlayState::Idle;
  volatile bool playStopReq_ = false;
  volatile int playRequest_ = -1;   // clip index to start, -1 = none
  char playPath_[80] = {0};         // /events/<dir>/EVT_<name>.avi
  AviReader* avi_ = nullptr;
  uint8_t* playJpegBuf_ = nullptr;  // one-frame JPEG staging buffer (heap)
  static constexpr size_t kPlayJpegCap = 96 * 1024;
  bool playPaused_ = false;
  int playProgress_ = 0;
  int consecDecodeFails_ = 0;  // abort playback after ~30 bad frames in a row
  static constexpr int kMaxMenuRows = 6;
  static constexpr int kMaxClipList = 4;
  ClipEntry clipList_[kMaxClipList];
  int clipCount_ = 0;

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

  // ---- refined-UI primitives (theme in display_service.cpp) ---------------
  // Rounded-corner filled panel. radius clamped to h/2; corners drawn with
  // quarter steps, interior with one fillRect (cheap: 3 window writes).
  void drawCard(int x, int y, int w, int h, uint16_t color, int radius = 6);
  // Horizontal progress bar with rounded ends and a 1px darker track border.
  void drawBar(int x, int y, int w, int h, int percent, uint16_t fill,
               uint16_t track);
  // Filled circle (midpoint scanline, used for status dots and icons).
  void fillCircle(int cx, int cy, int r, uint16_t color);
  // Small glyphs: wifi arcs, rec dot, SD card, relay dish, lock. Bounded and
  // filled primitives only -- no external font dependency.
  void drawWifiIcon(int x, int y, bool ok);
  void drawRecIcon(int x, int y, bool blinkOn);
  void drawSdIcon(int x, int y, int fillPercent);
  void drawRelayIcon(int x, int y, bool live);
  // Large numeric readout (3x5 pixel digits, scale applied).
  void drawBigNum(int x, int y, const char* s, uint16_t color, int scale);

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
