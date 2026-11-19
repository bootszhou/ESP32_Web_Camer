// MotionDetector -- decides whether the picture actually changed.
//
// Why not just diff JPEG bytes: JPEG is a lossy DCT codec, so identical scenes
// still produce different bytes frame to frame, while a small real change can
// barely move the byte count. Any byte-level comparison is therefore useless.
//
// Why not fmt2rgb888 at full res: decoding a 640x480 JPEG allocates ~921 KB and
// costs far more CPU than the whole streaming path. Instead we ask the decoder
// for a 1/8 scale luma surface (80x60), which is enough to spot real motion and
// cheap enough to run a couple of times per second alongside streaming.
//
// The detector works on 8x8 blocks of that small image:
//   - a block is "changed" when it deviates from its own slowly-adapted
//     background value by more than the sensitivity threshold
//   - global brightness shifts (auto-exposure, lights on/off) are compensated
//     before block comparison, so they do not masquerade as motion
//   - a trigger needs several changed blocks across two consecutive samples,
//     which rejects sensor noise and rolling-shutter flicker
#pragma once

#include <Arduino.h>

#include "camera/camera_service.h"

class MotionDetector {
 public:
  struct Config {
    // 1 (least sensitive) .. 100 (most sensitive). Mapped to a luma delta.
    uint8_t sensitivity = 50;

    // Blocks that must change for a trigger, out of kBlocksX * kBlocksY.
    uint8_t minChangedBlocks = 6;

    // How many consecutive sampled frames must agree before triggering.
    uint8_t confirmSamples = 2;

    // Minimum gap between two triggers, so one episode yields one event.
    uint32_t cooldownMs = 30000;

    // Minimum spacing between analysed frames; the camera may run faster.
    uint32_t sampleIntervalMs = 250;
  };

  enum class Level { kIdle, kActive, kTriggered };

  bool begin(CameraService* camera, const Config& cfg);
  void setConfig(const Config& cfg) { cfg_ = cfg; recalcThreshold(); }
  const Config& config() const { return cfg_; }

  // Call frequently. Analysis is rate-limited internally.
  void loop();

  // Reports whether motion is happening right now.
  bool isActive() const { return active_; }

  // One-shot: true for exactly one call after a trigger fired.
  bool consumeTrigger();

  // 0..100, how much of the frame changed on the last analysed sample.
  uint8_t lastChangePercent() const { return lastChangePercent_; }

  // Diagnostics for the status API.
  uint32_t analysedFrames() const { return analysedFrames_; }
  uint16_t lastChangedBlocks() const { return lastChangedBlocks_; }
  uint32_t lastAnalysisMs() const { return lastAnalysisMs_; }
  uint32_t triggerCount() const { return triggerCount_; }
  uint32_t decodeFails() const { return decodeFails_; }
  uint32_t acquireFails() const { return acquireFails_; }
  bool backgroundSeeded() const { return backgroundSeeded_; }
  uint32_t lumaSum() const { return lumaSum_; }
  uint8_t confirmCount() const { return confirmCount_; }

  // Small luma preview of what the detector sees (kImgW x kImgH), for tuning.
  const uint8_t* lumaBuffer() const { return luma_; }

  static constexpr int kImgW = 80;
  static constexpr int kImgH = 60;
  static constexpr int kBlockSize = 8;
  static constexpr int kBlocksX = kImgW / kBlockSize;  // 10
  static constexpr int kBlocksY = kImgH / kBlockSize;  // 7 (last row partial)
  static constexpr int kBlockCount = kBlocksX * kBlocksY;

 private:
  void recalcThreshold();
  bool decodeScaledGray(camera_fb_t* fb, uint8_t* out, size_t outLen);
  void analyse(const uint8_t* gray);
  void updateBackground(const uint8_t* gray);
  uint8_t blockMean(const uint8_t* gray, int bx, int by) const;

  CameraService* camera_ = nullptr;
  Config cfg_{};

  uint8_t* luma_ = nullptr;      // current sample, PSRAM
  uint8_t* background_ = nullptr;  // running background, PSRAM
  bool backgroundSeeded_ = false;

  // Per-block count of consecutive deviating samples. A hard freeze on
  // deviation was a one-way latch: after the camera moved, those blocks stayed
  // "changed" forever and the detector recorded every cooldown indefinitely.
  // Now a block deviating for longer than kAdoptAfterSamples is force-adopted
  // into the background (the scene genuinely changed), while ordinary motion
  // is still ignored by the slow adaptation.
  static constexpr uint16_t kAdoptAfterSamples = 120;  // 30 s at 250 ms
  uint16_t deviatingSamples_[kBlockCount] = {0};

  uint8_t threshold_ = 20;
  uint8_t confirmCount_ = 0;
  bool active_ = false;
  bool triggered_ = false;
  bool hasTriggered_ = false;

  uint32_t lastSampleMs_ = 0;
  uint32_t lastTriggerMs_ = 0;
  uint32_t analysedFrames_ = 0;
  uint32_t lastAnalysisMs_ = 0;
  uint16_t lastChangedBlocks_ = 0;
  uint16_t prevChangedBlocks_ = 0;
  uint8_t lastChangePercent_ = 0;
  uint32_t triggerCount_ = 0;
  uint32_t decodeFails_ = 0;
  uint32_t acquireFails_ = 0;
  uint32_t lumaSum_ = 0;
};
