#include "motion_detector.h"

#include <JPEGDEC.h>
#include <esp_heap_caps.h>

namespace {

JPEGDEC gJpeg;

// JPEGDEC hands pixels over as RGB565 in a JPEGDRAW block. We keep only luma:
// that halves memory traffic and is all motion detection needs.
struct DecodeCtx {
  uint8_t* out;
  size_t outLen;
  int w;
  int h;
  bool overflow;
};

DecodeCtx gCtx;

// Rectangular block delivered by the decoder. Blocks are NOT a full row wide,
// so the destination offset must advance by the destination stride (c.w) per
// source row -- using a flat index produces the smeared diagonal banding that
// this previously did.
int drawCallback(JPEGDRAW* pDraw) {
  DecodeCtx& c = gCtx;
  const uint16_t* pixels = (const uint16_t*)pDraw->pPixels;
  const int bw = pDraw->iWidth;
  const int bh = pDraw->iHeight;

  for (int row = 0; row < bh; row++) {
    const int y = pDraw->y + row;
    if (y < 0 || y >= c.h) continue;
    const int dstBase = y * c.w;

    for (int col = 0; col < bw; col++) {
      const int x = pDraw->x + col;
      if (x < 0 || x >= c.w) continue;
      const size_t idx = (size_t)dstBase + x;
      if (idx >= c.outLen) { c.overflow = true; return 0; }

      const uint16_t p = pixels[row * bw + col];
      // RGB565 -> luma. R and B are 5-bit, G is 6-bit, so the weights differ
      // from the usual 8-bit 0.299/0.587/0.114 form. Full white must map to
      // ~250; using 8-bit weights here crushes the image to near-black.
      const int r = (p >> 11) & 0x1F;   // 0..31
      const int g = (p >> 5) & 0x3F;    // 0..63
      const int b = p & 0x1F;           // 0..31
      const int y8 = (r * 2449 + g * 2404 + b * 934) >> 10;
      c.out[idx] = (uint8_t)(y8 > 255 ? 255 : y8);
    }
  }
  return 1;
}

}  // namespace

bool MotionDetector::begin(CameraService* camera, const Config& cfg) {
  if (!camera || !camera->isReady()) {
    Serial.println("[motion] camera not ready");
    return false;
  }
  camera_ = camera;
  cfg_ = cfg;
  recalcThreshold();

  const size_t bytes = (size_t)kImgW * kImgH;
  luma_ = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  background_ = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!luma_ || !background_) {
    Serial.println("[motion] PSRAM allocation failed");
    free(luma_);
    free(background_);
    luma_ = background_ = nullptr;
    return false;
  }
  memset(luma_, 0, bytes);
  memset(background_, 0, bytes);

  Serial.printf("[motion] ready: %dx%d luma, thr=%u, minBlocks=%u, "
                "confirm=%u, cooldown=%lums, interval=%lums\n",
                kImgW, kImgH, threshold_, cfg_.minChangedBlocks,
                cfg_.confirmSamples, (unsigned long)cfg_.cooldownMs,
                (unsigned long)cfg_.sampleIntervalMs);
  return true;
}

void MotionDetector::recalcThreshold() {
  // Higher sensitivity must mean a lower delta threshold.
  //   1   -> 40 (only large changes count)
  //   100 -> 6  (very twitchy)
  const int s = constrain((int)cfg_.sensitivity, 1, 100);
  threshold_ = (uint8_t)map(s, 1, 100, 40, 6);
}

bool MotionDetector::decodeScaledGray(camera_fb_t* fb, uint8_t* out, size_t outLen) {
  if (fb->format != PIXFORMAT_JPEG) return false;

  gCtx.out = out;
  gCtx.outLen = outLen;
  gCtx.w = kImgW;
  gCtx.h = kImgH;
  gCtx.overflow = false;

  if (!gJpeg.openRAM((uint8_t*)fb->buf, fb->len, drawCallback)) {
    return false;
  }
  gJpeg.setPixelType(RGB565_BIG_ENDIAN);
  // 1/8 scale turns 640x480 into exactly 80x60.
  const int ok = gJpeg.decode(0, 0, JPEG_SCALE_EIGHTH);
  gJpeg.close();

  return ok != 0 && !gCtx.overflow;
}

uint8_t MotionDetector::blockMean(const uint8_t* gray, int bx, int by) const {
  const int x0 = bx * kBlockSize;
  const int y0 = by * kBlockSize;
  const int y1 = min(y0 + kBlockSize, kImgH);
  const int x1 = min(x0 + kBlockSize, kImgW);

  uint32_t sum = 0;
  uint16_t n = 0;
  for (int y = y0; y < y1; y++) {
    const uint8_t* row = gray + y * kImgW;
    for (int x = x0; x < x1; x++) {
      sum += row[x];
      n++;
    }
  }
  return n ? (uint8_t)(sum / n) : 0;
}

void MotionDetector::updateBackground(const uint8_t* gray) {
  // Background adaptation must be slow enough that a continuous wave cannot be
  // absorbed into it. At a 250 ms sample interval, >>6 gives a time constant of
  // roughly 16 s; >>5 (about 8 s) let sustained motion fade into the model,
  // which is why a hand waved continuously stopped registering.
  //
  // Deviating blocks adapt even slower (>>8, ~64 s) so a person walking past
  // is not learned. But slow adaptation alone is not enough: a hard freeze was
  // tried and proved to be a one-way latch — after the camera moved, those
  // blocks stayed "changed" forever, the detector never went idle, and it
  // recorded a clip every cooldown indefinitely. So a block that has been
  // deviating continuously for kAdoptAfterSamples is force-adopted: by then
  // the "motion" is almost certainly a genuinely changed scene (moved camera,
  // shifted lighting), not a passing object.
  for (int by = 0; by < kBlocksY; by++) {
    for (int bx = 0; bx < kBlocksX; bx++) {
      const int b = by * kBlocksX + bx;
      const int delta = abs((int)blockMean(gray, bx, by) -
                            (int)blockMean(background_, bx, by));

      const int x0 = bx * kBlockSize;
      const int y0 = by * kBlockSize;
      const int y1 = min(y0 + kBlockSize, kImgH);
      const int x1 = min(x0 + kBlockSize, kImgW);

      if (delta >= (int)threshold_) {
        if (++deviatingSamples_[b] >= kAdoptAfterSamples) {
          // The scene itself changed: adopt the current content outright.
          for (int y = y0; y < y1; y++) {
            memcpy(&background_[y * kImgW + x0], &gray[y * kImgW + x0],
                   x1 - x0);
          }
          deviatingSamples_[b] = 0;
        } else {
          // Ordinary motion: adapt very slowly so it is not learned.
          for (int y = y0; y < y1; y++) {
            const int row = y * kImgW;
            for (int x = x0; x < x1; x++) {
              const int bg = background_[row + x];
              background_[row + x] = (uint8_t)(bg + ((gray[row + x] - bg) >> 8));
            }
          }
        }
        continue;
      }

      deviatingSamples_[b] = 0;
      const int shift = 6;
      for (int y = y0; y < y1; y++) {
        const int row = y * kImgW;
        for (int x = x0; x < x1; x++) {
          const int bg = background_[row + x];
          background_[row + x] = (uint8_t)(bg + ((gray[row + x] - bg) >> shift));
        }
      }
    }
  }
}

void MotionDetector::analyse(const uint8_t* gray) {
  if (!backgroundSeeded_) {
    memcpy(background_, gray, (size_t)kImgW * kImgH);
    backgroundSeeded_ = true;
    return;
  }

  // Compensate for a global brightness shift (lights on/off, auto-exposure).
  //
  // The naive version -- subtract the frame-wide mean offset from every block --
  // is wrong when an object covers a large part of the view: a hand held up to
  // the lens shifts the mean so much that subtracting it erases the very
  // difference we are looking for. That is exactly why a full-frame wave
  // produced 0 changed blocks.
  //
  // Use the MEDIAN of the per-block offsets instead. With a hand covering a
  // minority of blocks, the median stays near the true lighting shift while the
  // blocks showing the hand remain far from their background.
  int16_t blockDelta[kBlockCount];
  int n = 0;
  for (int by = 0; by < kBlocksY; by++) {
    for (int bx = 0; bx < kBlocksX; bx++) {
      blockDelta[n++] = (int16_t)((int)blockMean(gray, bx, by) -
                                  (int)blockMean(background_, bx, by));
    }
  }

  // Insertion sort on a 70-element array is cheaper than pulling in std::sort.
  int16_t sorted[kBlockCount];
  memcpy(sorted, blockDelta, sizeof(sorted));
  for (int i = 1; i < kBlockCount; i++) {
    const int16_t key = sorted[i];
    int j = i - 1;
    while (j >= 0 && sorted[j] > key) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = key;
  }
  const int globalOffset = sorted[kBlockCount / 2];

  uint16_t changed = 0;
  for (int i = 0; i < kBlockCount; i++) {
    if (abs((int)blockDelta[i] - globalOffset) >= (int)threshold_) changed++;
  }

  lastChangedBlocks_ = changed;
  lastChangePercent_ = (uint8_t)((changed * 100) / kBlockCount);

  // A trigger must persist across consecutive samples so sensor noise does not
  // raise events on its own.
  const bool hot = changed >= cfg_.minChangedBlocks;
  if (hot) {
    if (confirmCount_ < 255) confirmCount_++;
  } else {
    confirmCount_ = 0;
  }

  const bool nowActive = (confirmCount_ >= cfg_.confirmSamples);
  const uint32_t now = millis();

  // Trigger on the rising edge, and again on a sustained-motion cadence.
  //
  // Edge-only detection (the previous behaviour) fired once per motion episode
  // and then stayed silent: while something kept moving, nowActive and active_
  // were both true, so the edge condition could never re-arm. Continuous
  // activity therefore produced a single clip and nothing afterwards, which
  // reads as "it only recorded once".
  //
  // A trigger count of zero guarantees the very first event, because
  // lastTriggerMs_ starts at 0 and "now - 0 >= cooldown" is false for the
  // first 30 s of uptime.
  if (nowActive) {
    const bool risingEdge = !active_;
    const bool cooled = (triggerCount_ == 0) ||
                        ((int32_t)(now - lastTriggerMs_) >= (int32_t)cfg_.cooldownMs);

    // Fire on the rising edge (a new episode) OR after the cooldown elapses
    // while motion continues. Requiring risingEdge in both cases was the bug:
    // during sustained movement the edge never recurs, so only one clip was
    // ever produced no matter how long the movement lasted.
    if (cooled) {
      hasTriggered_ = true;
      triggerCount_++;
      lastTriggerMs_ = now;
      Serial.printf("[motion] TRIGGER %s %u/%u blocks (%.0f%%), offset=%d\n",
                    risingEdge ? "edge" : "sustained",
                    changed, kBlockCount, (changed * 100.0f) / kBlockCount,
                    globalOffset);
    }
  } else if (active_) {
    // Motion has stopped; the next movement is a fresh episode and must be able
    // to trigger immediately rather than waiting out the remaining cooldown.
    Serial.printf("[motion] idle again (%u blocks)\n", changed);
    lastTriggerMs_ = now - cfg_.cooldownMs;
  }

  active_ = nowActive;
  updateBackground(gray);
}

void MotionDetector::loop() {
  if (!camera_) return;

  const uint32_t now = millis();
  if ((int32_t)(now - lastSampleMs_) < (int32_t)cfg_.sampleIntervalMs) return;

  camera_fb_t* fb = camera_->acquire(500);
  if (!fb) {
    // Do NOT advance lastSampleMs_ here. Doing so meant a missed buffer (the
    // stream task can hold both of them) consumed the slot and the detector
    // silently skipped that sample; with a viewer connected it skipped them all
    // and the reported values froze. Leaving the timer untouched makes it retry
    // immediately on the next loop iteration.
    acquireFails_++;
    return;
  }
  lastSampleMs_ = now;

  const bool ok = decodeScaledGray(fb, luma_, (size_t)kImgW * kImgH);
  camera_->release(fb);
  if (!ok) {
    decodeFails_++;
    return;
  }

  analyse(luma_);
  analysedFrames_++;
  lastAnalysisMs_ = millis();

  // Cheap fingerprint of the current luma surface: if this stops changing while
  // the scene moves, the detector is being handed a stale frame rather than
  // the comparison logic being wrong.
  uint32_t sum = 0;
  for (int i = 0; i < kImgW * kImgH; i += 17) sum += luma_[i];
  lumaSum_ = sum;
}

bool MotionDetector::consumeTrigger() {
  if (!hasTriggered_) return false;
  hasTriggered_ = false;
  return true;
}
