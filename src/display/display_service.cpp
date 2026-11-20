#include "display_service.h"

#include <JPEGDEC.h>
#include <SPI.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <driver/ledc.h>
#include <math.h>

#include "board/board_config.h"
#include "ui/font5x7.h"

namespace {

// ST7789 command set (subset we need).
constexpr uint8_t kCmdSWRESET = 0x01;
constexpr uint8_t kCmdSLPOUT  = 0x11;
constexpr uint8_t kCmdNORON   = 0x13;
constexpr uint8_t kCmdDISPON  = 0x29;
constexpr uint8_t kCmdCASET   = 0x2A;
constexpr uint8_t kCmdRASET   = 0x2B;
constexpr uint8_t kCmdRAMWR   = 0x2C;
constexpr uint8_t kCmdMADCTL  = 0x36;
constexpr uint8_t kCmdCOLMOD  = 0x3A;

// ST7789 MADCTL, following the same convention TFT_eSPI uses for this class of
// panel (240x320 physical GRAM):
//
//   rotation 0  MADCTL 0x00   portrait  240x320   upright letterbox (default)
//   rotation 1  MADCTL 0x60   landscape 320x240   push the camera frame as-is
//   rotation 2  MADCTL 0xC0   portrait  240x320   upside-down letterbox
//   rotation 3  MADCTL 0xA0   landscape 320x240   push the camera frame as-is
//
// Panel mapping measured on this module: MADCTL 0x00 shows the pushed buffer
// 1:1 (upright), 0xC0 flips it 180 degrees, 0x60 lays a 320x240 buffer
// 90 degrees clockwise on the portrait glass, 0xA0 counter-clockwise.
//
// The important part: MV (0x20) only remaps how GRAM maps onto the glass. It
// does NOT raise the CASET limit, which stays 0..239 on a 240x320 part. An
// earlier version "swapped the spans" when MV was set, which asked for
// CASET 0..319 on a 240-wide panel: the write pointer folded back and the
// image appeared as four repeated striped tiles.
//
// Therefore the address window is ALWAYS (w-1, h-1) for the buffer being
// pushed, and the orientation table below is the only place MV is decided.
inline uint8_t madctlForRotation(uint8_t r) {
  switch (r & 3) {
    case 0: return 0x00;          // portrait,  no mirror
    case 1: return 0x20 | 0x40;   // landscape, MV | MX
    case 2: return 0x40 | 0x80;   // portrait,  mirrored both axes
    default: return 0x20 | 0x80;  // landscape, MV | MY
  }
}

// Rotations 0 and 2 address the panel in portrait (240x320). The landscape
// camera frame does not fit that aspect ratio, so it is letterboxed: scaled
// by 3/4 to 240x180 and centred with black bars. Rotating the content 90
// degrees instead would fill the glass but lay the scene on its side --
// which was exactly the mismatch between the LCD and the web preview.
inline bool isPortraitRotation(uint8_t r) { return (r & 1) == 0; }

// Orientation is now confirmed on hardware (landscape index 1 is upright), so
// the four corner markers are switched off. Set to true again to re-check the
// orientation after changing wiring or MADCTL.
constexpr bool kOrientationDebug = false;

// LEDC channel for backlight PWM.
// MUST NOT be channel 0 / timer 0: esp32-camera drives the OV2640 XCLK from
// LEDC_CHANNEL_0 on LEDC_TIMER_0 (see esp32-camera/target/xclk.c and
// esp_camera.h's default camera_config_t). Rebinding channel 0 to the
// backlight silently killed the camera clock and the backlight never lit.
constexpr ledc_channel_t kBlChannel = LEDC_CHANNEL_2;
constexpr ledc_timer_t   kBlTimer   = LEDC_TIMER_2;

// SPI bus config.
// Use Arduino SPIClass instead of raw ESP-IDF spi_master.
// The ESP-IDF driver crashes at 0x400570e5 on this Arduino 2.0.11 build,
// but the Arduino wrapper (which uses the same peripheral under the hood)
// somehow avoids the flash-cache conflict.
SPIClass* gSpi = nullptr;

void spiWrite(const uint8_t* data, size_t len) {
  if (!gSpi) return;
  gSpi->beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  digitalWrite(board::TFT_PIN_CS, LOW);
  gSpi->writeBytes(data, len);
  digitalWrite(board::TFT_PIN_CS, HIGH);
  gSpi->endTransaction();
}

void spiWriteCmd(uint8_t cmd) {
  digitalWrite(board::TFT_PIN_DC, LOW);
  spiWrite(&cmd, 1);
}

void spiWriteData(const uint8_t* data, size_t len) {
  digitalWrite(board::TFT_PIN_DC, HIGH);
  spiWrite(data, len);
}

// Single data byte. ST7789 parameters are mostly ONE byte (COLMOD, MADCTL,
// porch, etc.). Sending them through spiWriteDataU16() wrote a leading 0x00
// first, which shifted every parameter by one byte and left the controller
// uninitialised -- that is why the panel showed random RAM noise ("snow").
void spiWriteData8(uint8_t val) { spiWriteData(&val, 1); }

// Two-byte parameter, only for CASET/RASET style 16-bit arguments.
void spiWriteDataU16(uint16_t val) {
  uint8_t b[2] = {(uint8_t)(val >> 8), (uint8_t)(val & 0xFF)};
  spiWriteData(b, 2);
}

// (spiWriteDataU16 already defined above)

// JPEGDEC callback: delivers RGB565 blocks. We copy into frameBuf_ at the
// destination position, using the destination stride.
struct DispCtx {
  uint16_t* buf;
  int stride;
  int w;
  int h;
  bool overflow;
};
DispCtx gDispCtx;

// Heap-allocated decoder: see decodeAndShow() for why this cannot be a local.
JPEGDEC* gJpegDecoder = nullptr;

int dispDrawCb(JPEGDRAW* pDraw) {
  DispCtx& c = gDispCtx;
  const uint16_t* src = (const uint16_t*)pDraw->pPixels;
  const int bw = pDraw->iWidth;
  const int bh = pDraw->iHeight;

  for (int row = 0; row < bh; row++) {
    const int y = pDraw->y + row;
    if (y < 0 || y >= c.h) continue;
    const int dstBase = y * c.stride;
    for (int col = 0; col < bw; col++) {
      const int x = pDraw->x + col;
      if (x < 0 || x >= c.w) continue;
      const size_t idx = (size_t)dstBase + x;
      if (idx >= (size_t)c.w * c.h) { c.overflow = true; return 0; }
      c.buf[idx] = src[row * bw + col];
    }
  }
  return 1;
}

}  // namespace

bool DisplayService::begin(CameraService* camera, NetworkService* network,
                           EventRecorder* recorder, const Config& cfg) {
  camera_ = camera;
  network_ = network;
  recorder_ = recorder;
  cfg_ = cfg;

  if (!camera_ || !camera_->isReady()) {
    Serial.println("[disp] camera not ready");
    return false;
  }

  // Configure control pins as outputs BEFORE any SPI traffic.
  pinMode(board::TFT_PIN_RST, OUTPUT);
  pinMode(board::TFT_PIN_DC, OUTPUT);
  pinMode(board::TFT_PIN_CS, OUTPUT);
  pinMode(board::TFT_PIN_BL, OUTPUT);

  digitalWrite(board::TFT_PIN_CS, HIGH);   // deselect
  digitalWrite(board::TFT_PIN_DC, HIGH);   // data mode default
  digitalWrite(board::TFT_PIN_RST, HIGH);  // not in reset
  // Backlight is driven by LEDC later; do not digitalWrite it here, that would
  // fight the peripheral and leave the pin latched low.

  // Allocate the frame buffer in PSRAM.
  frameBuf_ = (uint16_t*)heap_caps_malloc(
      kDispW * kDispH * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  if (!frameBuf_) {
    Serial.println("[disp] PSRAM allocation failed");
    return false;
  }
  memset(frameBuf_, 0, kDispW * kDispH * sizeof(uint16_t));

  // Letterbox buffer, only needed for the portrait orientations.
  rotBuf_ = (uint16_t*)heap_caps_malloc(
      kDispW * kDispH * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  if (!rotBuf_) {
    Serial.println("[disp] letterbox buffer alloc failed; rotation 0/2 disabled");
  }

  // One-frame JPEG staging buffer for clip playback. Recorded VGA frames are
  // ~12 KB; 96 KB leaves headroom for complex scenes (quality spikes) and for
  // the "skip oversized frame" path of AviReader to stay rare.
  playJpegBuf_ = (uint8_t*)heap_caps_malloc(kPlayJpegCap, MALLOC_CAP_SPIRAM);
  if (!playJpegBuf_) {
    Serial.println("[disp] playback jpeg buffer alloc failed; playback disabled");
  }
  avi_ = new AviReader();

  // Output geometry must match the rotation before the panel init sets the
  // address window and clears the screen.
  updateOutputGeometry();

  // Initialise bit-banged SPI pins.
  initSpi();
  Serial.println("[disp] SPI pins init done");

  // Initialise the ST7789 panel.
  initSt7789();
  Serial.println("[disp] ST7789 init done");

  // Push a solid blue test frame immediately so we can see the panel works
  // even before the display task starts.
  Serial.println("[disp] pushing test frame...");
  pushFrame(frameBuf_, outW_, outH_);
  Serial.println("[disp] test frame pushed");

  // Backlight PWM.
  ledc_timer_config_t timerCfg = {};
  timerCfg.speed_mode = LEDC_LOW_SPEED_MODE;
  timerCfg.timer_num = kBlTimer;
  timerCfg.duty_resolution = LEDC_TIMER_8_BIT;
  timerCfg.freq_hz = 5000;
  timerCfg.clk_cfg = LEDC_AUTO_CLK;
  ledc_timer_config(&timerCfg);

  ledc_channel_config_t chCfg = {};
  chCfg.speed_mode = LEDC_LOW_SPEED_MODE;
  chCfg.channel = kBlChannel;
  chCfg.timer_sel = kBlTimer;
  chCfg.gpio_num = board::TFT_PIN_BL;
  chCfg.duty = cfg_.brightness;
  chCfg.hpoint = 0;
  ledc_channel_config(&chCfg);

  setBacklight(true);
  lastActivityMs_ = millis();

  // Re-enable display task with hardware SPI on safe pins (GPIO41/GPIO2).
  Serial.println("[disp] creating display task...");
  BaseType_t ok = xTaskCreatePinnedToCore(
      taskEntry, "display", kTaskStackWords, this,
      tskIDLE_PRIORITY + 1,
      &taskHandle_, 1);
  if (ok != pdPASS) {
    Serial.println("[disp] task create failed");
    end();
    return false;
  }

  Serial.printf("[disp] ready: %dx%d, Arduino SPI, brightness %u, fps %u\n",
                kDispW, kDispH, cfg_.brightness, cfg_.fpsTarget);
  ready_ = true;
  return true;
}

void DisplayService::end() {
  if (taskHandle_) {
    vTaskDelete(taskHandle_);
    taskHandle_ = nullptr;
  }
  if (gSpi) {
    gSpi->end();
    delete gSpi;
    gSpi = nullptr;
  }
  if (frameBuf_) {
    free(frameBuf_);
    frameBuf_ = nullptr;
  }
  ready_ = false;
}

void DisplayService::taskEntry(void* arg) {
  DisplayService* self = static_cast<DisplayService*>(arg);
  self->taskLoop();
}

void DisplayService::taskLoop() {
  // Wait for the system to be fully stable before touching the camera.
  vTaskDelay(pdMS_TO_TICKS(2000));

  uint32_t lastRedrawSeq = 0xFFFFFFFF;  // force the first paint
  uint8_t lastPage = 0xFF;

  while (true) {
    if (!ready_) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

    // Auto-off backlight.
    if (backlightOn_ && cfg_.autoOffSec > 0) {
      if (millis() - lastActivityMs_ > (uint32_t)cfg_.autoOffSec * 1000) {
        setBacklight(false);
      }
    }

    // A page change must paint immediately, even while the backlight is off,
    // so the first press after auto-off both wakes the panel and shows the new
    // page instead of requiring a second press.
    const uint8_t curPage = page_;
    const uint32_t curSeq = redrawSeq_;
    const bool pageChanged = (curPage != lastPage) || (curSeq != lastRedrawSeq);

    // Clip playback owns the task while active. It is started by
    // requestPlayback() from the UI task and ends by EOF, stopPlayback(), or
    // error; afterwards the Events page is marked for repaint so the list
    // reappears.
    if (playState_ == (uint8_t)PlayState::Playing) {
      playTaskLoop();
      continue;
    }
    if (playRequest_ >= 0) {
      startPendingPlayback();
      continue;
    }

    // Text pages are static: repaint only when something changed. The preview
    // page keeps streaming frames.
    if (curPage != (uint8_t)Page::Preview) {
      if (!pageChanged) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
      lastPage = curPage;
      lastRedrawSeq = curSeq;
      renderTextPage((Page)curPage);
      // Do not spin: the next change comes from a button press.
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // Preview page.
    lastPage = curPage;
    lastRedrawSeq = curSeq;

    // Rate-limit to target fps. Bit-banged SPI at ~200 kHz takes ~2.5 s per
    // frame, so cap at 1 fps regardless of the configured target.
    uint32_t interval = 1000;  // 1 fps max for bit-banged
    if (!pageChanged && millis() - lastFrameMs_ < interval) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    lastFrameMs_ = millis();

    // Borrow a frame from the camera with a short timeout.
    camera_fb_t* fb = camera_->acquire(50);
    if (!fb) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

    // Decode and push to the panel.
    decodeAndShow(fb);
    camera_->release(fb);
    frameCount_++;

    // Yield so lower-priority tasks can run.
    taskYIELD();
  }
}

void DisplayService::initSpi() {
  // Arduino SPIClass: use FSPI (SPI2) with custom pins.
  // GPIO41=SCL, GPIO2=SDA are safe (no camera/USB conflict).
  gSpi = new SPIClass(FSPI);
  gSpi->begin(board::TFT_PIN_SCLK, -1, board::TFT_PIN_MOSI, board::TFT_PIN_CS);
  pinMode(board::TFT_PIN_CS, OUTPUT);
  digitalWrite(board::TFT_PIN_CS, HIGH);
  Serial.println("[disp] Arduino SPI ready (FSPI, custom pins)");
}

void DisplayService::initSt7789() {
  // Hardware reset pulse (datasheet: >=10 us low, then wait >=120 ms).
  digitalWrite(board::TFT_PIN_RST, LOW);
  delay(20);
  digitalWrite(board::TFT_PIN_RST, HIGH);
  delay(150);

  spiWriteCmd(kCmdSWRESET);
  delay(150);

  spiWriteCmd(kCmdSLPOUT);   // Sleep out -- required before any other setup
  delay(150);

  // Frame rate / porch control. These four are single-byte parameters; sending
  // them as 16-bit shifted the whole register set by one byte (see the
  // spiWriteData8 comment) and left the panel producing "snow".
  spiWriteCmd(0xB2);  // PORCTRL
  spiWriteData8(0x0C); spiWriteData8(0x0C); spiWriteData8(0x00);
  spiWriteData8(0x33); spiWriteData8(0x33);

  spiWriteCmd(0xB7);  // GCTRL
  spiWriteData8(0x35);

  spiWriteCmd(0xBB);  // VCOMS
  spiWriteData8(0x19);

  spiWriteCmd(0xC0);  // LCMCTRL
  spiWriteData8(0x2C);

  spiWriteCmd(0xC2);  // VDVVRHEN
  spiWriteData8(0x01);

  spiWriteCmd(0xC3);  // VRHS
  spiWriteData8(0x12);

  spiWriteCmd(0xC4);  // VDVS
  spiWriteData8(0x20);

  spiWriteCmd(0xC6);  // FRCTRL2 -- 60 Hz
  spiWriteData8(0x0F);

  spiWriteCmd(0xD0);  // PWCTRL1
  spiWriteData8(0xA4); spiWriteData8(0xA1);

  // Inversion: 0x21 = on, 0x20 = off. Measured on this GMT020 panel the image
  // came out as a photographic negative with 0x20 (white rendered black), so
  // inversion must stay ON. This is a per-panel property, not a wiring fault.
  spiWriteCmd(0x21);

  // Pixel format: 16-bit RGB565 (single byte parameter).
  spiWriteCmd(kCmdCOLMOD);
  spiWriteData8(0x55);
  delay(10);

  // Memory access control for the current rotation (single byte parameter).
  spiWriteCmd(kCmdMADCTL);
  spiWriteData8(madctlForRotation(rotation_));
  delay(10);

  // Full-screen address window for the current output geometry, then clear to
  // black so any leftover RAM content ("snow") is overwritten before the
  // first real frame.
  spiWriteCmd(kCmdCASET);
  spiWriteDataU16(0);
  spiWriteDataU16(outW_ - 1);
  spiWriteCmd(kCmdRASET);
  spiWriteDataU16(0);
  spiWriteDataU16(outH_ - 1);

  spiWriteCmd(kCmdNORON);
  delay(10);
  spiWriteCmd(kCmdDISPON);
  delay(50);

  // Clear the panel RAM to white so any area not covered by a later frame
  // (e.g. the preview letterbox bars, or a smaller landscape window on the
  // portrait glass) shows a clean white instead of leftover GRAM "snow".
  fillRect(0, 0, outW_, outH_, 0xFFFF);
}

void DisplayService::setBacklight(bool on) {
  backlightOn_ = on;
  // LEDC owns the pin once its channel is configured; calling digitalWrite on
  // a pin that LEDC drives fights the peripheral and can leave the pin parked
  // low. Drive the duty cycle only.
  ledc_set_duty(LEDC_LOW_SPEED_MODE, kBlChannel, on ? cfg_.brightness : 0);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, kBlChannel);
}

void DisplayService::updateActivity() {
  lastActivityMs_ = millis();
  if (!backlightOn_) setBacklight(true);
}

void DisplayService::setPage(Page p) {
  if ((uint8_t)p >= (uint8_t)Page::PageCount) return;
  page_ = (uint8_t)p;
  redrawSeq_++;
  updateActivity();
}

void DisplayService::setMenuRows(const MenuRow* rows, int count) {
  menuRows_ = rows;
  menuCount_ = (count < 0) ? 0 : (count > kMaxMenuRows ? kMaxMenuRows : count);
  if (page_ == (uint8_t)Page::Settings) redrawSeq_++;
}

void DisplayService::setHint(const char* text) {
  if (!text) { hint_[0] = 0; return; }
  strncpy(hint_, text, sizeof(hint_) - 1);
  hint_[sizeof(hint_) - 1] = 0;
  redrawSeq_++;
}

// ---- on-panel clip playback ----------------------------------------------

void DisplayService::setClipSelected(int i) {
  if (i < 0) i = 0;
  if (i > kMaxClipList - 1) i = kMaxClipList - 1;
  clipSelected_ = i;
  if (page_ == (uint8_t)Page::Events) redrawSeq_++;
}

void DisplayService::requestPlayback(int index) {
  if (index < 0 || index >= kMaxClipList) return;
  if (playState_ == (uint8_t)PlayState::Playing) return;  // already playing
  playRequest_ = index;
}

void DisplayService::stopPlayback() {
  playStopReq_ = true;
}

// Runs on the display task: opens the requested clip and enters the playing
// state. Kept separate from requestPlayback() because SD open + frame scan
// takes tens of milliseconds and must not run on the UI task.
void DisplayService::startPendingPlayback() {
  const int idx = playRequest_;
  playRequest_ = -1;
  if (idx < 0 || idx >= clipCount_ || !avi_ || !playJpegBuf_) return;

  snprintf(playPath_, sizeof(playPath_), "/events/%s/EVT_%s.avi",
           clipList_[idx].dir, clipList_[idx].name);
  if (!avi_->begin(SD_MMC, playPath_)) {
    Serial.printf("[disp] playback open failed: %s\n", playPath_);
    setHint("OPEN FAILED");
    redrawSeq_++;
    return;
  }
  playStopReq_ = false;
  playPaused_ = false;
  playProgress_ = 0;
  consecDecodeFails_ = 0;
  playState_ = (uint8_t)PlayState::Playing;
  Serial.printf("[disp] playing %s\n", playPath_);
}

// Decodes one JPEG into frameBuf_ and pushes it, then overlays the progress
// bar. Same decode path as the live preview (decodeAndShow), including the
// portrait letterbox composition.
bool DisplayService::renderPlaybackFrame(const uint8_t* jpeg, size_t len,
                                         int progress) {
  gDispCtx.buf = frameBuf_;
  gDispCtx.stride = kDispW;
  gDispCtx.w = kDispW;
  gDispCtx.h = kDispH;
  gDispCtx.overflow = false;

  if (!gJpegDecoder) gJpegDecoder = new JPEGDEC();
  JPEGDEC& jpegDec = *gJpegDecoder;
  if (jpegDec.openRAM((uint8_t*)jpeg, len, dispDrawCb)) {
    jpegDec.setPixelType(RGB565_BIG_ENDIAN);
    const int ok = jpegDec.decode(0, 0, JPEG_SCALE_HALF);
    jpegDec.close();
    if (ok && !gDispCtx.overflow) {
      if (isPortraitRotation(rotation_)) {
        if (rotBuf_) {
          composeLetterboxPortrait();
          pushFrame(rotBuf_, outW_, outH_);
        }
      } else {
        pushFrame(frameBuf_, outW_, outH_);
      }
      drawStatusBar();
      renderPlaybackOverlay(progress, playPaused_);
      return true;
    } else {
      decodeFails_++;
    }
  } else {
    decodeFails_++;
  }
  return false;
}

// Bottom strip: progress bar + time position + paused marker. Drawn on top of
// the pushed frame, like drawStatusBar.
void DisplayService::renderPlaybackOverlay(int progress, bool paused) {
  const uint16_t kAccent = 0x07E0;
  const uint16_t kWarn = 0xFFE0;
  const int w = outW_;
  const int h = outH_;
  const int barY = h - 18;
  const int barX = 8;
  const int barW = w - 16;

  // Dark pill behind the bar so it reads over any video content.
  drawCard(barX - 4, barY - 6, barW + 8, 20, 0x0000, 8);
  drawBar(barX, barY, barW, 8, progress, kAccent, 0x2104);
  if (paused) {
    // Two vertical bars = pause glyph, at the left inside the pill.
    fillRect(barX + barW - 22, barY - 3, 3, 12, kWarn);
    fillRect(barX + barW - 16, barY - 3, 3, 12, kWarn);
  }
}

// Frame pump for the Playing state. Runs only on the display task. Each
// iteration decodes and pushes one frame, paced to roughly the recorded
// nominal fps; the buttons can pause or stop.
void DisplayService::playTaskLoop() {
  // Nominal 10 fps matches EventRecorder::Config::nominalFps; AVI strh rate
  // is per-clip but re-parsing it would cost another seek, and 10 fps is what
  // every clip is recorded with.
  constexpr uint32_t kFrameIntervalMs = 100;   // 10 fps
  constexpr uint32_t kPausedPollMs = 120;
  static uint32_t lastFrameMs = 0;

  if (playStopReq_) {
    endPlayback("STOPPED");
    return;
  }

  if (playPaused_) {
    vTaskDelay(pdMS_TO_TICKS(kPausedPollMs));
    return;
  }

  if (millis() - lastFrameMs < kFrameIntervalMs) {
    vTaskDelay(pdMS_TO_TICKS(5));
    return;
  }
  lastFrameMs = millis();

  const int n = avi_->nextFrame(playJpegBuf_, kPlayJpegCap);
  if (n == 0) {
    endPlayback("END");
    return;
  }
  if (n < 0) return;  // oversized frame skipped; try the next one

  // Abort after consecutive decode failures: a corrupt clip (or an old file
  // whose index got misread) must not leave the panel frozen for minutes with
  // keys looking dead. ~3 s of failures = give up.
  playProgress_ = avi_->progressPercent();
  if (!renderPlaybackFrame(playJpegBuf_, (size_t)n, playProgress_)) {
    if (++consecDecodeFails_ >= 30) endPlayback("BAD FILE");
    return;
  }
  consecDecodeFails_ = 0;
  taskYIELD();
}

// Common exit: close the file, drop back to the Events list and repaint it.
void DisplayService::endPlayback(const char* reason) {
  Serial.printf("[disp] playback %s\n", reason);
  if (avi_) avi_->end();
  playState_ = (uint8_t)PlayState::Idle;
  playStopReq_ = false;
  playPaused_ = false;
  // Back to the Events page and force a repaint of the list.
  page_ = (uint8_t)Page::Events;
  redrawSeq_++;
}


// Draws one string with the built-in 5x7 font. Each glyph column is sent as a
// run of fillRect() calls: the driver has no partial-buffer API, so this is the
// only way to blend text over an existing image. Cost is ~35 small window
// writes per character, which is fine for a page repaint (not per frame).
void DisplayService::drawText(int x, int y, const char* s, uint16_t fg,
                              uint16_t bg, int scale) {
  if (!s) return;
  if (scale < 1) scale = 1;

  const int gw = font5x7::kGlyphW;
  const int gh = font5x7::kGlyphH;

  // Background slab first, so glyph gaps do not show the camera image through.
  const int totalW = font5x7::textWidth(s, scale);
  if (bg != 0xFFFF && totalW > 0) {
    fillRect(x, y, totalW, gh * scale, bg);
  }

  int cx = x;
  for (const char* p = s; *p; p++, cx += (gw + 1) * scale) {
    const uint8_t* g = font5x7::glyph(*p);
    if (!g) continue;  // unsupported char: advance only
    for (int col = 0; col < gw; col++) {
      const uint8_t bits = g[col];
      for (int row = 0; row < gh; row++) {
        if (!(bits & (1u << row))) continue;
        fillRect(cx + col * scale, y + row * scale, scale, scale, fg);
      }
    }
  }
}

void DisplayService::drawFrameRect(int x, int y, int w, int h, uint16_t color) {
  fillRect(x, y, w, 1, color);
  fillRect(x, y + h - 1, w, 1, color);
  fillRect(x, y, 1, h, color);
  fillRect(x + w - 1, y, 1, h, color);
}

// ---- refined-UI primitives -------------------------------------------------
//
// Palette. A dark background with one accent keeps the 240-px-wide panel
// readable at arm's length and in daylight; green accents match the web UI.
namespace uitheme {
constexpr uint16_t kBg      = 0x0000;  // near-black page background
constexpr uint16_t kCard    = 0x1082;  // dark slate card (r 16 g 32 b 32 approx)
constexpr uint16_t kCardHi  = 0x18C3;  // slightly lighter card top edge
constexpr uint16_t kBorder  = 0x2104;  // 33% grey border/track
constexpr uint16_t kFg      = 0xFFFF;  // white
constexpr uint16_t kDim     = 0x8410;  // 50% grey secondary text
constexpr uint16_t kAccent  = 0x07E0;  // green
constexpr uint16_t kAccent2 = 0x05FF;  // cyan (info)
constexpr uint16_t kWarn    = 0xFFE0;  // yellow
constexpr uint16_t kErr     = 0xF800;  // red
constexpr uint16_t kTitleBg = 0x0000;  // title bar blends into the page
}  // namespace uitheme

void DisplayService::drawCard(int x, int y, int w, int h, uint16_t color,
                              int radius) {
  if (radius < 1) { fillRect(x, y, w, h, color); return; }
  if (radius > h / 2) radius = h / 2;
  if (radius > w / 2) radius = w / 2;

  // Interior slab in one window write; corners stepped inward per row.
  fillRect(x, y + radius, w, h - 2 * radius, color);
  for (int i = 0; i < radius; i++) {
    // Corner inset follows a quarter-circle: dx = r - sqrt(r^2 - dy^2).
    const int dy = radius - i;                    // 1..r from the flat edge
    const int dx = radius - (int)sqrtf((float)radius * radius -
                                       (float)dy * dy);
    fillRect(x + dx, y + i, w - 2 * dx, 1, color);              // top edge row
    fillRect(x + dx, y + h - 1 - i, w - 2 * dx, 1, color);      // bottom edge row
  }
}

void DisplayService::fillCircle(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) { fillRect(cx, cy, 1, 1, color); return; }
  for (int dy = -r; dy <= r; dy++) {
    const int dx = (int)sqrtf((float)r * r - (float)dy * dy);
    fillRect(cx - dx, cy + dy, dx * 2 + 1, 1, color);
  }
}

void DisplayService::drawBar(int x, int y, int w, int h, int percent,
                             uint16_t fill, uint16_t track) {
  if (w < 3 || h < 3) return;
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  // Rounded track.
  drawCard(x, y, w, h, track, h / 2);
  if (percent <= 0) return;
  const int fw = (int)((long)w * percent / 100);
  if (fw <= h) {
    // Round cap for a short fill.
    drawCard(x, y, fw, h, fill, fw / 2);
  } else {
    drawCard(x, y, h, h, fill, h / 2);            // left cap
    fillRect(x + h / 2, y, fw - h, h, fill);      // body
    drawCard(x + fw - h, y, h, h, fill, h / 2);   // right cap
  }
}

// WiFi: filled dot + two arcs approximated by concentric quarter bands. ok=false
// draws a red cross instead so the failure mode is obvious.
void DisplayService::drawWifiIcon(int x, int y, bool ok) {
  const uint16_t c = ok ? uitheme::kAccent : uitheme::kErr;
  if (!ok) {
    // 9x9 red X.
    for (int i = 0; i < 7; i++) {
      fillRect(x + i, y + i, 2, 2, c);
      fillRect(x + 7 - i, y + i, 2, 2, c);
    }
    return;
  }
  fillCircle(x + 5, y + 10, 2, c);                       // dot
  for (int i = 0; i < 5; i++) fillRect(x + 1 + i, y + 8 - i, 2, 2, c);
  for (int i = 0; i < 5; i++) fillRect(x + 8 - i, y + 8 - i, 2, 2, c);
  for (int i = 0; i < 7; i++) fillRect(x + i, y + 4, 2, 2, c);
  for (int i = 0; i < 4; i++) { fillRect(x + i, y + 4 - i, 2, 2, c); }
  for (int i = 0; i < 4; i++) { fillRect(x + 8 - i, y + 4 - i, 2, 2, c); }
}

// Recording: pulsing filled circle inside a ring.
void DisplayService::drawRecIcon(int x, int y, bool blinkOn) {
  const uint16_t ring = uitheme::kBorder;
  // 14px ring.
  for (int dy = -7; dy <= 7; dy++) {
    const int dx = (int)sqrtf(49.0f - (float)dy * dy);
    if (dx < 6) continue;
    fillRect(x + 7 - dx, y + 7 + dy, 2, 1, ring);
    fillRect(x + 7 + dx, y + 7 + dy, 2, 1, ring);
  }
  if (blinkOn) fillCircle(x + 7, y + 7, 5, uitheme::kErr);
  else {
    fillCircle(x + 7, y + 7, 5, uitheme::kCard);
    fillCircle(x + 7, y + 7, 3, uitheme::kErr);
  }
}

// SD card glyph with a fill bar showing free space.
void DisplayService::drawSdIcon(int x, int y, int fillPercent) {
  // Card body 12x15, notched top-right corner.
  fillRect(x + 2, y, 10, 15, uitheme::kCardHi);
  fillRect(x, y + 2, 12, 13, uitheme::kCardHi);
  // Contact pins.
  for (int i = 0; i < 4; i++) fillRect(x + 3 + i * 2, y + 1, 1, 2, uitheme::kCard);
  // Free-space bar inside the card.
  if (fillPercent < 0) fillPercent = 0;
  if (fillPercent > 100) fillPercent = 100;
  fillRect(x + 2, y + 9, 8, 4, uitheme::kCard);
  const int fw = 8 * fillPercent / 100;
  if (fw > 0) fillRect(x + 2, y + 9, fw, 4, uitheme::kAccent2);
}

// Relay: a small dish with a signal wedge; live adds an expanding arc dot.
void DisplayService::drawRelayIcon(int x, int y, bool live) {
  const uint16_t c = live ? uitheme::kWarn : uitheme::kDim;
  // Dish: bottom-filled triangle-ish wedge 11x7.
  for (int row = 0; row < 7; row++) {
    const int wide = 11 - row * 2;
    if (wide <= 0) break;
    fillRect(x + (11 - wide) / 2, y + row, wide, 1, c);
  }
  if (live) {
    fillRect(x + 5, y - 4, 2, 2, uitheme::kAccent);
    fillRect(x + 4, y - 2, 4, 1, uitheme::kAccent);
  }
}

// 3x5 pixel digits for the big readout, row-major bitmaps, '1'..'9','0',':','%'.
void DisplayService::drawBigNum(int x, int y, const char* s, uint16_t color,
                                int scale) {
  static const uint8_t kDig[16][5] = {
    {0x7,0x5,0x5,0x5,0x7}, // 0
    {0x2,0x6,0x2,0x2,0x7}, // 1
    {0x7,0x1,0x7,0x4,0x7}, // 2
    {0x7,0x1,0x7,0x1,0x7}, // 3
    {0x5,0x5,0x7,0x1,0x1}, // 4
    {0x7,0x4,0x7,0x1,0x7}, // 5
    {0x7,0x4,0x7,0x5,0x7}, // 6
    {0x7,0x1,0x2,0x2,0x2}, // 7
    {0x7,0x5,0x7,0x5,0x7}, // 8
    {0x7,0x5,0x7,0x1,0x7}, // 9
    {0x0,0x2,0x0,0x2,0x0}, // :
    {0x5,0x5,0x2,0x0,0x2}, // %
    {0x7,0x4,0x6,0x4,0x7}, // K
    {0x2,0x5,0x7,0x5,0x2}, // M
    {0x2,0x5,0x5,0x5,0x2}, // B
    {0x0,0x0,0x7,0x0,0x0}, // - (dash fallback)
  };
  if (scale < 1) scale = 1;
  int cx = x;
  for (const char* p = s; *p; ++p) {
    int idx = -1;
    if (*p >= '0' && *p <= '9') idx = *p - '0';
    else if (*p == ':') idx = 10;
    else if (*p == '%') idx = 11;
    else if (*p == 'K') idx = 12;
    else if (*p == 'M') idx = 13;
    else if (*p == 'B') idx = 14;
    else if (*p == '-' || *p == ' ') idx = 15;
    if (idx >= 0) {
      const uint8_t* g = kDig[idx];
      for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 3; col++) {
          if (g[row] & (1u << (2 - col))) {
            fillRect(cx + col * scale, y + row * scale, scale, scale, color);
          }
        }
      }
      cx += 4 * scale;
    } else {
      cx += 4 * scale;  // unsupported: advance only
    }
  }
}

// Renders one of the text pages. Layout is deliberately independent of
// orientation: the whole page is composed from the current outW_/outH_, so it
// works both letterboxed (240x320) and landscape (320x240).
//
// Refined design (v2): dark page, rounded cards, icon chips, big numeric
// readouts and progress bars instead of raw label/value lines.
void DisplayService::renderTextPage(Page p) {
  using namespace uitheme;
  const int w = outW_;
  const int h = outH_;
  const int lineH = 12;
  const int marginX = 6;

  fillRect(0, 0, w, h, kBg);

  // ---- title bar: accent pill + page name + page index ----------------------
  const char* title = "";
  uint16_t titleAccent = kAccent;
  switch (p) {
    case Page::Events:   title = "EVENTS";   titleAccent = kAccent2; break;
    case Page::Settings: title = "SETTINGS"; titleAccent = kWarn;    break;
    case Page::Network:  title = "NETWORK";  titleAccent = kAccent;  break;
    default:             title = "CAM-AI";   titleAccent = kAccent;  break;
  }
  // Accent tick left of the title (6x15 rounded pill).
  drawCard(marginX, 4, 6, 15, titleAccent, 3);
  drawText(marginX + 11, 6, title, kFg, kBg, 2);
  char idx[8];
  snprintf(idx, sizeof(idx), "%u/%u", (unsigned)p + 1,
           (unsigned)Page::PageCount);
  const int idxW = font5x7::textWidth(idx);
  drawText(w - marginX - idxW, 10, idx, kDim, kBg);

  int y = 28;

  if (p == Page::Events) {
    const EventRecorder::Status* rs = recorder_ ? &recorder_->status() : nullptr;
    char buf[32];

    // ---- compact storage card ----------------------------------------------
    const int cardX = marginX, cardW = w - 2 * marginX;
    int cardY = y;
    drawCard(cardX, cardY, cardW, 38, kCard, 7);
    drawSdIcon(cardX + 8, cardY + 7,
               rs && rs->totalMb
                   ? (int)((uint64_t)rs->freeMb * 100 / rs->totalMb)
                   : 0);
    if (rs && rs->mounted) {
      snprintf(buf, sizeof(buf), "%luM/%luM FREE",
               (unsigned long)rs->freeMb, (unsigned long)rs->totalMb);
      drawText(cardX + 28, cardY + 7, buf, kFg, kCard);
      // Free-ratio bar under the text.
      drawBar(cardX + 28, cardY + 22, cardW - 40, 7,
              rs->totalMb
                  ? (int)((uint64_t)rs->freeMb * 100 / rs->totalMb)
                  : 0,
              kAccent2, kBorder);
    } else {
      drawText(cardX + 28, cardY + 7, "NO SD CARD", kErr, kCard);
      drawText(cardX + 28, cardY + 20, "recording off", kDim, kCard);
    }
    y = cardY + 44;

    // ---- clips summary row: written | failed, inline -----------------------
    cardY = y;
    drawCard(cardX, cardY, cardW, 30, kCard, 7);
    if (rs) {
      snprintf(buf, sizeof(buf), "CLIPS %lu", (unsigned long)rs->clipsWritten);
      drawText(cardX + 8, cardY + 10, buf, kAccent, kCard);
      snprintf(buf, sizeof(buf), "FAIL %lu", (unsigned long)rs->clipsFailed);
      drawText(cardX + cardW - 10 - font5x7::textWidth(buf), cardY + 10, buf,
               rs->clipsFailed ? (uint16_t)kErr : (uint16_t)kDim, kCard);
    } else {
      drawText(cardX + 8, cardY + 10, "recorder offline", kDim, kCard);
    }
    y = cardY + 36;

    // ---- recent clips list -------------------------------------------------
    // SD scan only on repaint (page change), never per frame.
    scanClips();
    if (clipSelected_ >= clipCount_) clipSelected_ = clipCount_ - 1;
    if (clipSelected_ >= clipCount_) clipSelected_ = clipCount_ - 1;
    cardY = y;
    if (clipCount_ > 0) {
      const int rowH = 16;
      const int listH = clipCount_ * rowH + 18;
      drawCard(cardX, cardY, cardW, listH, kCard, 7);
      drawText(cardX + 8, cardY + 5, "RECENT", kDim, kCard);
      int ry = cardY + 18;
      for (int i = 0; i < clipCount_; i++) {
        const ClipEntry& e = clipList_[i];
        const bool sel = ((int)clipSelected_ == i);
        // Selected row: full-width inset highlight pill + accent border on the
        // play glyph, so the choice is unambiguous before starting playback.
        const uint16_t rowBg = sel ? (uint16_t)0x1284 : (uint16_t)kCard;
        if (sel) drawCard(cardX + 4, ry - 1, cardW - 8, rowH, rowBg, 6);
        // Play glyph: small triangle in a circle (accent when selected).
        fillCircle(cardX + 13, ry + 6, 6, sel ? (uint16_t)kAccent : (uint16_t)0x2104);
        fillRect(cardX + 11, ry + 3, 2, 7, sel ? (uint16_t)0x0000 : (uint16_t)kAccent);
        fillRect(cardX + 13, ry + 4, 1, 5, sel ? (uint16_t)0x0000 : (uint16_t)kAccent);
        fillRect(cardX + 14, ry + 5, 1, 3, sel ? (uint16_t)0x0000 : (uint16_t)kAccent);
        // Name + size. e.name is "20261120_161500_001" or "uptime_x_001";
        // show the tail that fits.
        const char* nm = e.name;
        const int maxChars = (cardW - 60) / (font5x7::kGlyphW + 1);
        const int len = (int)strlen(nm);
        if (len > maxChars) nm += (len - maxChars);
        drawText(cardX + 24, ry + 2, nm, sel ? (uint16_t)kFg : (uint16_t)kDim, rowBg);
        snprintf(buf, sizeof(buf), "%luK", (unsigned long)e.kb);
        drawText(cardX + cardW - 10 - font5x7::textWidth(buf), ry + 2, buf,
                 sel ? (uint16_t)kAccent2 : (uint16_t)kDim, rowBg);
        ry += rowH;
      }
      y = cardY + listH + 6;
    } else {
      drawCard(cardX, cardY, cardW, 26, kCard, 7);
      drawText(cardX + 8, cardY + 8, rs && rs->mounted ? "NO CLIPS YET"
                                                       : "insert SD card",
               kDim, kCard);
      y = cardY + 32;
    }
  } else if (p == Page::Settings) {
    const int cardX = marginX, cardW = w - 2 * marginX;
    if (menuRows_ && menuCount_ > 0) {
      // Menu rows live inside one card; the selected row gets an inset
      // highlight pill; an editing row turns the value yellow with a caret.
      // Rows use scale-2 text (12x14 glyphs) after user feedback that the
      // 5x7 font was too small: row height 26, values right-aligned.
      const int rowH = 26;
      const int listH = menuCount_ * rowH + 14;
      drawCard(cardX, y, cardW, listH, kCard, 7);
      int ry = y + 8;
      for (int i = 0; i < menuCount_; i++) {
        const MenuRow& r = menuRows_[i];
        if (r.selected) {
          const uint16_t hiBg = r.editing ? (uint16_t)0x8200 : (uint16_t)0x1284;
          drawCard(cardX + 4, ry - 3, cardW - 8, rowH, hiBg, 6);
          drawText(cardX + 10, ry + 4, r.label, kFg, hiBg, 2);
          char val[24];
          snprintf(val, sizeof(val), "%s%s", r.value, r.editing ? "<" : "");
          const int vw = font5x7::textWidth(val, 2);
          drawText(cardX + cardW - 10 - vw, ry + 4, val,
                   r.editing ? (uint16_t)kWarn : (uint16_t)kAccent, hiBg, 2);
        } else {
          // Dim labels keep the unselected rows visually quieter, but at the
          // same large size so the column of rows stays aligned.
          drawText(cardX + 10, ry + 4, r.label, kDim, kCard, 2);
          const int vw = font5x7::textWidth(r.value, 2);
          drawText(cardX + cardW - 10 - vw, ry + 4, r.value, kFg, kCard, 2);
        }
        ry += rowH;
        if (ry > y + listH - rowH) break;
      }
      y += listH + 8;
    } else {
      drawCard(cardX, y, cardW, 30, kCard, 7);
      drawText(cardX + 10, y + 10, "NO ITEMS", kDim, kCard);
      y += 38;
    }
  } else if (p == Page::Network) {
    char buf[40];
    const bool up = network_ && network_->isConnected();
    const int cardX = marginX, cardW = w - 2 * marginX;

    // ---- connection card ----------------------------------------------------
    int cardY = y;
    drawCard(cardX, cardY, cardW, 36, kCard, 7);
    drawWifiIcon(cardX + 8, cardY + 9, up);
    drawText(cardX + 28, cardY + 8, up ? "CONNECTED" : "OFFLINE",
             up ? (uint16_t)kAccent : (uint16_t)kErr, kCard);
    if (up) {
      snprintf(buf, sizeof(buf), "RSSI %d dBm", (int)network_->rssi());
      drawText(cardX + 28, cardY + 21, buf, kDim, kCard);
    } else {
      drawText(cardX + 28, cardY + 21,
               network_ && network_->provisioningActive() ? "AP CONFIG MODE"
                                                          : "waiting...",
               kDim, kCard);
    }
    y = cardY + 42;

    // ---- address card: IP at scale 2 ---------------------------------------
    if (up) {
      cardY = y;
      drawCard(cardX, cardY, cardW, 40, kCard, 7);
      drawText(cardX + 8, cardY + 5, "IP", kDim, kCard);
      snprintf(buf, sizeof(buf), "%s",
               network_->localIp().toString().c_str());
      drawText(cardX + 8, cardY + 19, buf, kFg, kCard, 2);
      y = cardY + 46;
    }

    // ---- system card --------------------------------------------------------
    cardY = y;
    drawCard(cardX, cardY, cardW, 46, kCard, 7);
    snprintf(buf, sizeof(buf), "UPTIME %lus",
             (unsigned long)(millis() / 1000));
    drawText(cardX + 8, cardY + 6, buf, kFg, kCard);
    snprintf(buf, sizeof(buf), "HEAP %uK",
             (unsigned)(ESP.getFreeHeap() / 1024));
    drawText(cardX + 8, cardY + 19, buf, kDim, kCard);
    // Heap health bar: 320 KB budget, green above 30%.
    {
      uint32_t fh = ESP.getFreeHeap();
      int pct = (int)((uint64_t)fh * 100 / 327680UL);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      drawBar(cardX + cardW - 78, cardY + 20, 70, 8, pct,
              pct > 30 ? kAccent : kErr, kBorder);
    }
    drawText(cardX + 8, cardY + 32, "FW 0.6.0 M7", kDim, kCard);
    y = cardY + 52;
  }

  // Hint pill at the bottom, e.g. "GPIO0 LONG = PROVISION".
  if (hint_[0]) {
    const int hy = h - lineH - 8;
    const int hw = font5x7::textWidth(hint_) + 14;
    drawCard(marginX, hy, hw < w - 2 * marginX ? hw : w - 2 * marginX,
             lineH + 5, 0x1082, 7);
    drawText(marginX + 7, hy + 4, hint_, 0xFFE0, 0x1082);
  }
}

// Scan /events/<date>/EVT_*.avi newest-first into clipList_. Runs on the
// display task only when the Events page repaints: an SD directory walk takes
// tens of milliseconds per folder and must never run per frame. FAT returns
// entries in creation order, so reading directories last-to-first and files
// last-to-first approximates newest-first without sorting.
int DisplayService::scanClips() {
  clipCount_ = 0;

  File root = SD_MMC.open("/events");
  if (!root || !root.isDirectory()) return 0;

  // Collect date dirs (max 8, enough history for a "recent" list).
  constexpr int kMaxDirs = 8;
  String dirs[kMaxDirs];
  int dirCount = 0;
  File d = root.openNextFile();
  while (d && dirCount < kMaxDirs) {
    if (d.isDirectory()) {
      String name = d.name();
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      dirs[dirCount++] = name;
    }
    d = root.openNextFile();
  }
  root.close();

  // Newest date dir first; within a dir, collect all .avi names then take the
  // tail (FAT order is oldest-first, so the tail is the newest).
  for (int i = dirCount - 1; i >= 0 && clipCount_ < kMaxClipList; i--) {
    String dirPath = String("/events/") + dirs[i];
    File dir = SD_MMC.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) continue;

    String names[10];
    uint32_t sizes[10];
    int n = 0;
    File f = dir.openNextFile();
    while (f && n < 10) {
      String fname = f.name();
      int slash = fname.lastIndexOf('/');
      if (slash >= 0) fname = fname.substring(slash + 1);
      if (fname.endsWith(".avi")) {
        names[n] = fname;
        sizes[n] = (uint32_t)(f.size() / 1024);  // KB
        n++;
      }
      f = dir.openNextFile();
    }
    dir.close();

    for (int j = n - 1; j >= 0 && clipCount_ < kMaxClipList; j--) {
      ClipEntry& e = clipList_[clipCount_];
      // Short label: strip "EVT_" prefix and ".avi" suffix; the date is the
      // card header, so the time part is what distinguishes entries.
      String short_ = names[j];
      short_.replace("EVT_", "");
      short_.replace(".avi", "");
      strncpy(e.name, short_.c_str(), sizeof(e.name) - 1);
      e.name[sizeof(e.name) - 1] = 0;
      strncpy(e.dir, dirs[i].c_str(), sizeof(e.dir) - 1);
      e.dir[sizeof(e.dir) - 1] = 0;
      e.kb = sizes[j];
      clipCount_++;
    }
  }
  return clipCount_;
}

void DisplayService::updateOutputGeometry() {
  // Portrait rotations (0 and 2) push a 240x320 letterboxed buffer; landscape
  // rotations (1 and 3) push the 320x240 camera frame unchanged.
  if (isPortraitRotation(rotation_)) {
    outW_ = kDispH;  // 240
    outH_ = kDispW;  // 320
  } else {
    outW_ = kDispW;  // 320
    outH_ = kDispH;  // 240
  }
}

void DisplayService::setRotation(uint8_t r) {
  rotation_ = r & 3;
  updateOutputGeometry();

  spiWriteCmd(kCmdMADCTL);
  spiWriteData8(madctlForRotation(rotation_));
  delay(10);

  Serial.printf("[disp] rotation %u -> window %dx%d, MADCTL 0x%02X\n",
                rotation_, outW_, outH_, madctlForRotation(rotation_));
}

// Letterbox the 320x240 camera frame into a 240x320 portrait buffer: the
// image is scaled by 3/4 to 240x180 and centred vertically; the rows above
// and below stay black. This keeps the LCD preview upright and matching the
// web page on a portrait-mounted panel -- rotating the content 90 degrees
// would fill the glass but lay the scene on its side, which was the
// mismatch seen on hardware.
void DisplayService::composeLetterboxPortrait() {
  // Source frame is kDispW x kDispH (320x240). Scale by 3/4 to fit the
  // 240-pixel-wide portrait glass -> 240x180. Using kDispW here (320*3/4=240)
  // made us draw 240 rows whose source rows ran to 318, i.e. ~50 KB past the
  // end of frameBuf_, which showed up as "snow" in the lower image band.
  const int imgW = kDispW * 3 / 4;       // 240
  const int imgH = kDispH * 3 / 4;       // 180
  const int yOff = (kDispW - imgH) / 2;  // (320 - 180) / 2 = 70

  // Fill the whole portrait buffer with white first. The rows above/below the
  // letterboxed image (the area "outside" the 240x180 camera picture) then show
  // a clean white instead of leftover GRAM/PSRAM noise ("snow").
  for (int i = 0; i < kDispW * kDispH; i++) rotBuf_[i] = 0xFFFF;
  for (int gy = 0; gy < imgH; gy++) {
    const uint16_t* srcRow = frameBuf_ + (size_t)(gy * 4 / 3) * kDispW;
    uint16_t* dstRow = rotBuf_ + (size_t)(yOff + gy) * imgW;
    for (int gx = 0; gx < imgW; gx++) {
      dstRow[gx] = srcRow[gx * 4 / 3];
    }
  }
}

// loop() is now empty: all work moved to taskLoop() on a dedicated task.
void DisplayService::loop() {}

void DisplayService::decodeAndShow(camera_fb_t* fb) {
  if (fb->format != PIXFORMAT_JPEG) return;

  gDispCtx.buf = frameBuf_;
  gDispCtx.stride = kDispW;
  gDispCtx.w = kDispW;
  gDispCtx.h = kDispH;
  gDispCtx.overflow = false;

  // JPEGDEC must NOT be a stack local: the object embeds ~16 KB of inline
  // buffers (ucFileBuf 2K, ucHuffDC 2K, usHuffAC 8K, usUnalignedPixels 4K).
  // Declaring it here exhausted the display task stack and produced
  // "Stack canary watchpoint triggered (display)" -- the crash that was
  // previously misread as an SPI/flash conflict.
  if (!gJpegDecoder) gJpegDecoder = new JPEGDEC();
  JPEGDEC& jpeg = *gJpegDecoder;
  if (!jpeg.openRAM((uint8_t*)fb->buf, fb->len, dispDrawCb)) {
    decodeFails_++;
    return;
  }
  jpeg.setPixelType(RGB565_BIG_ENDIAN);
  // 1/2 scale: 640x480 -> 320x240 exactly.
  int ok = jpeg.decode(0, 0, JPEG_SCALE_HALF);
  jpeg.close();

  if (!ok || gDispCtx.overflow) {
    decodeFails_++;
    return;
  }

  // Push to panel. Portrait rotations (0 and 2) letterbox the landscape
  // camera frame into the 240x320 portrait buffer so the preview stays
  // upright and matches the web page; landscape rotations (1 and 3) push the
  // decoded frame unchanged.
  if (isPortraitRotation(rotation_)) {
    if (rotBuf_) {
      composeLetterboxPortrait();
      pushFrame(rotBuf_, outW_, outH_);
    }
  } else {
    pushFrame(frameBuf_, outW_, outH_);
  }

  // Draw status bar on top (after push, so it overlays the image).
  drawStatusBar();
}

void DisplayService::pushFrame(const uint16_t* frame, int w, int h) {
  // Address window is always the buffer's own dimensions. See madctlForRotation
  // for why the window must NOT be swapped when MV is set: CASET stays limited
  // to 0..239 on a 240x320 GRAM, so asking for a wider window folds back and
  // produces the repeated striped tiles seen during bring-up.
  spiWriteCmd(kCmdCASET);
  spiWriteDataU16(0);
  spiWriteDataU16(w - 1);
  spiWriteCmd(kCmdRASET);
  spiWriteDataU16(0);
  spiWriteDataU16(h - 1);
  spiWriteCmd(kCmdRAMWR);

  // Chunked transfer: one 153,600-byte transaction exceeds the driver's
  // queue/transfer limits and would silently truncate the image.
  const uint8_t* p = (const uint8_t*)frame;
  const size_t total = (size_t)w * h * 2;
  constexpr size_t kChunk = 4096;

  digitalWrite(board::TFT_PIN_DC, HIGH);   // data mode for the whole burst
  digitalWrite(board::TFT_PIN_CS, LOW);
  gSpi->beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  for (size_t off = 0; off < total; off += kChunk) {
    size_t n = (off + kChunk <= total) ? kChunk : (total - off);
    gSpi->writeBytes(p + off, n);
  }
  gSpi->endTransaction();
  digitalWrite(board::TFT_PIN_CS, HIGH);
}

void DisplayService::fillRect(int x, int y, int w, int h, uint16_t color) {
  spiWriteCmd(kCmdCASET);
  spiWriteDataU16(x);
  spiWriteDataU16(x + w - 1);
  spiWriteCmd(kCmdRASET);
  spiWriteDataU16(y);
  spiWriteDataU16(y + h - 1);
  spiWriteCmd(kCmdRAMWR);

  // Use a static buffer to avoid blowing the loopTask stack (default 4 KB,
  // already partly consumed by JPEGDEC and SPI driver frames).
  static uint8_t buf[64];
  for (int i = 0; i < 32; i++) {
    buf[i * 2] = color >> 8;
    buf[i * 2 + 1] = color & 0xFF;
  }
  size_t total = (size_t)w * h * 2;
  for (size_t off = 0; off < total; off += sizeof(buf)) {
    size_t n = (off + sizeof(buf) <= total) ? sizeof(buf) : (total - off);
    spiWriteData(buf, n);
  }
}

void DisplayService::drawStatusBar() {
  // Bring-up aid: four corner blocks make the panel orientation obvious
  // without depending on scene content.
  //   TL red, TR green, BL blue, BR yellow
  if (kOrientationDebug) {
    constexpr int m = 28;  // marker size in pixels
    fillRect(0, 0, m, m, 0xF800);                        // TL red
    fillRect(outW_ - m, 0, m, m, 0x07E0);                // TR green
    fillRect(0, outH_ - m, m, m, 0x001F);                // BL blue
    fillRect(outW_ - m, outH_ - m, m, m, 0xFFE0);        // BR yellow
    return;
  }

  using namespace uitheme;

  // Refined status bar: 22 px translucent-style black strip with icon chips.
  // wifi | rec | relay on the left, SD free bar (with icon) on the right.
  const int barH = 22;
  fillRect(0, 0, outW_, barH, kBg);

  int x = 4;
  // Wi-Fi icon (9x12 glyph).
  const bool wifiUp = network_ && network_->isConnected();
  drawWifiIcon(x, 5, wifiUp);
  x += 14;

  // Recording indicator: ring + dot, blinks at ~2 Hz via frame parity.
  const bool rec = recorder_ && recorder_->isRecording();
  drawRecIcon(x, 4, rec && (frameCount_ & 0x08));
  x += 18;

  // Relay chip: dish with a live wedge when a viewer watches the cloud stream.
  bool relayLive = false;
  bool relayOn = false;
  // relay_ is not wired into DisplayService (it lives in main/web); derive the
  // "live" look from the recorder-free heuristic: no relay pointer here yet,
  // so keep the slot dim until M7 display wiring lands.
  relayOn = false;
  (void)relayLive;
  drawRelayIcon(x, 6, relayOn);
  x += 16;

  // SD free bar with card glyph on the right edge.
  if (recorder_) {
    const EventRecorder::Status& st = recorder_->status();
    if (st.totalMb > 0) {
      const int pct = (int)((uint64_t)st.freeMb * 100 / st.totalMb);
      drawSdIcon(outW_ - 18, 3, pct);
      // Bar next to the icon.
      const int barW = outW_ - x - 26;
      if (barW > 20) {
        drawBar(x, 8, barW, 8, pct, pct > 20 ? kAccent : kErr, kBorder);
      }
      return;
    }
  }
  // No SD: red dot instead of the bar.
  fillCircle(outW_ - 12, 11, 4, kErr);
}
