#include "display_service.h"

#include <JPEGDEC.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <driver/ledc.h>

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

  // Clear the panel RAM.
  fillRect(0, 0, outW_, outH_, 0x0000);
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

// Renders one of the text pages. Layout is deliberately independent of
// orientation: the whole page is composed from the current outW_/outH_, so it
// works both letterboxed (240x320) and landscape (320x240).
void DisplayService::renderTextPage(Page p) {
  constexpr uint16_t kBg = 0x0000;
  constexpr uint16_t kFg = 0xFFFF;
  constexpr uint16_t kDim = 0x8410;   // grey
  constexpr uint16_t kAccent = 0x07E0;  // green
  constexpr uint16_t kTitleBg = 0x0010;  // dark blue

  const int w = outW_;
  const int h = outH_;
  const int lineH = 12;
  const int marginX = 4;

  fillRect(0, 0, w, h, kBg);

  // Title bar.
  const char* title = "";
  switch (p) {
    case Page::Events:   title = "EVENTS"; break;
    case Page::Settings: title = "SETTINGS"; break;
    case Page::Network:  title = "NETWORK"; break;
    default:             title = "CAM-AI"; break;
  }
  fillRect(0, 0, w, lineH + 2, kTitleBg);
  drawText(marginX, 4, title, kFg, kTitleBg);
  // Page indicator on the right of the title bar.
  char idx[8];
  snprintf(idx, sizeof(idx), "%u/%u", (unsigned)p + 1,
           (unsigned)Page::PageCount);
  const int idxW = font5x7::textWidth(idx);
  drawText(w - marginX - idxW, 4, idx, kDim, kTitleBg);

  int y = lineH + 8;

  if (p == Page::Events) {
    const EventRecorder::Status* rs = recorder_ ? &recorder_->status() : nullptr;
    char buf[32];

    drawText(marginX, y, "STORAGE", kDim, kBg); y += lineH;
    if (rs && rs->mounted) {
      snprintf(buf, sizeof(buf), "FREE %lu MB", (unsigned long)rs->freeMb);
      drawText(marginX, y, buf, kFg, kBg); y += lineH;
      snprintf(buf, sizeof(buf), "TOTAL %lu MB", (unsigned long)rs->totalMb);
      drawText(marginX, y, buf, kFg, kBg); y += lineH;
    } else {
      drawText(marginX, y, "NO SD CARD", 0xF800, kBg); y += lineH;
    }

    y += 4;
    drawText(marginX, y, "CLIPS", kDim, kBg); y += lineH;
    if (rs) {
      snprintf(buf, sizeof(buf), "WRITTEN %lu", (unsigned long)rs->clipsWritten);
      drawText(marginX, y, buf, kFg, kBg); y += lineH;
      snprintf(buf, sizeof(buf), "FAILED %lu", (unsigned long)rs->clipsFailed);
      drawText(marginX, y, buf,
               rs->clipsFailed ? (uint16_t)0xF800 : (uint16_t)kDim, kBg);
      y += lineH;
      if (rs->lastClip[0]) {
        drawText(marginX, y, "LAST", kDim, kBg); y += lineH;
        // EVT_20261113_161500_001 -> keep the tail that still fits the panel
        const char* name = rs->lastClip;
        const int maxChars = (w - 2 * marginX) / (font5x7::kGlyphW + 1);
        const int len = (int)strlen(name);
        if (len > maxChars) name += (len - maxChars);
        drawText(marginX, y, name, kAccent, kBg); y += lineH;
      }
    }
  } else if (p == Page::Settings) {
    if (menuRows_ && menuCount_ > 0) {
      for (int i = 0; i < menuCount_; i++) {
        const MenuRow& r = menuRows_[i];
        const uint16_t bg = r.selected ? (r.editing ? (uint16_t)0x001F
                                                    : (uint16_t)0x0320)
                                       : (uint16_t)kBg;
        if (r.selected) fillRect(0, y - 2, w, lineH, bg);

        drawText(marginX, y, r.label, r.selected ? kFg : kDim, bg);
        const int vw = font5x7::textWidth(r.value);
        drawText(w - marginX - vw, y, r.value,
                 r.editing ? (uint16_t)0xFFE0 : (r.selected ? (uint16_t)kAccent
                                                            : (uint16_t)kFg),
                 bg);
        y += lineH;
        if (y > h - 2 * lineH) break;
      }
    } else {
      drawText(marginX, y, "NO ITEMS", kDim, kBg); y += lineH;
    }
  } else if (p == Page::Network) {
    char buf[40];
    const bool up = network_ && network_->isConnected();

    drawText(marginX, y, "STATUS", kDim, kBg); y += lineH;
    drawText(marginX, y, up ? "CONNECTED" : "OFFLINE",
             up ? (uint16_t)kAccent : (uint16_t)0xF800, kBg);
    y += lineH;

    if (up) {
      snprintf(buf, sizeof(buf), "IP %s",
               network_->localIp().toString().c_str());
      drawText(marginX, y, buf, kFg, kBg); y += lineH;
      snprintf(buf, sizeof(buf), "RSSI %d dBm", (int)network_->rssi());
      drawText(marginX, y, buf, kFg, kBg); y += lineH;
    }

    y += 4;
    snprintf(buf, sizeof(buf), "UPTIME %lus",
             (unsigned long)(millis() / 1000));
    drawText(marginX, y, buf, kFg, kBg); y += lineH;
    drawText(marginX, y, "FW 0.5.0 M6", kDim, kBg); y += lineH;

    // Heap is the number that matters when the stream and a clip run together.
    snprintf(buf, sizeof(buf), "HEAP %uK", (unsigned)(ESP.getFreeHeap() / 1024));
    drawText(marginX, y, buf, kDim, kBg); y += lineH;
  }

  // Hint line at the bottom, e.g. "GPIO0 LONG = PROVISION".
  if (hint_[0]) {
    const int hy = h - lineH - 3;
    fillRect(0, hy, w, lineH + 3, 0x2104);
    drawText(marginX, hy + 2, hint_, 0xFFE0, 0x2104);
  }
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
  const int imgW = kDispH;               // 240
  const int imgH = kDispW * 3 / 4;       // 180
  const int yOff = (kDispW - imgH) / 2;  // (320 - 180) / 2 = 70

  memset(rotBuf_, 0, kDispW * kDispH * sizeof(uint16_t));
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

  // Top 20-pixel bar with: Wi-Fi dBm, rec indicator, SD free space.
  int barH = 20;

  // Background.
  fillRect(0, 0, outW_, barH, 0x0000);  // black

  // We do not have font rendering in this minimal driver; instead we draw
  // simple colored blocks as indicators:
  //   Green  = Wi-Fi connected, red = disconnected
  //   Red    = recording dot, blinks via frame count parity
  //   Blue   = SD free bar width proportional to free MB

  // Wi-Fi indicator: 12x12 square at x=4,y=4.
  uint16_t wifiColor = (network_ && network_->isConnected()) ? 0x07E0 : 0xF800;
  fillRect(4, 4, 12, 12, wifiColor);

  // Recording indicator: 12x12 red dot at x=20,y=4, blinks at ~2 Hz.
  if (recorder_ && recorder_->isRecording() && (frameCount_ & 0x08)) {
    fillRect(20, 4, 12, 12, 0xF800);
  } else {
    fillRect(20, 4, 12, 12, 0x0000);
  }

  // SD free bar: width proportional to free MB, max 200 MB.
  if (recorder_) {
    uint32_t freeMb = recorder_->status().freeMb;
    int barW = (int)(freeMb > 200 ? 200 : freeMb);
    fillRect(40, 4, barW, 12, 0x001F);  // blue bar
    // Remaining as dark grey.
    if (barW < 200) fillRect(40 + barW, 4, 200 - barW, 12, 0x2104);
  }
}
