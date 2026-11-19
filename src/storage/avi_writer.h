// AviWriter -- wraps MJPEG frames into an AVI (RIFF) container.
//
// Why AVI + MJPEG: the OV2640 already produces JPEG, so recording needs no
// transcoding. AVI is the simplest container that both VLC and ffmpeg accept
// for a Motion-JPEG stream, and it lets a browser replay the clip by pulling
// individual JPEG frames back out.
//
// The file is written in three passes because total size is unknown up front:
//   1. reserve a fixed-size header with placeholder lengths
//   2. append frame chunks as they arrive
//   3. seek back and patch the real sizes, then append the index
// A crash mid-recording therefore leaves an unpatched header, which is exactly
// why the recorder writes to .tmp and renames on success.
#pragma once

#include <Arduino.h>
#include <FS.h>

class AviWriter {
 public:
  struct Stats {
    uint32_t frames = 0;
    uint32_t bytes = 0;
    uint32_t dropped = 0;
  };

  // Opens (or creates) the target file and writes a placeholder header.
  bool begin(fs::FS& fs, const char* path, uint16_t width, uint16_t height,
             uint8_t nominalFps = 10);

  // Appends one JPEG frame. Returns false if the file rejected the write.
  bool addFrame(const uint8_t* jpeg, size_t len, uint32_t timestampMs);

  // Patches sizes, writes the index and closes. Safe to call twice.
  // Returns false if any stage failed, meaning the clip is not trustworthy.
  bool finish();

  bool isOpen() const { return file_ != nullptr; }
  const Stats& stats() const { return stats_; }
  const char* path() const { return path_; }

 private:
  struct IndexEntry {
    uint32_t offset;  // relative to 'movi' data start
    uint32_t size;
  };

  void writeLE16(uint16_t v);
  void writeLE32(uint32_t v);
  void writeFourCC(const char* tag);
  bool writeHeaderPlaceholder(uint16_t width, uint16_t height, uint8_t fps);

  fs::File* file_ = nullptr;
  char path_[64] = {0};

  uint32_t riffSizePos_ = 0;   // where RIFF size field sits
  uint32_t avihFramesPos_ = 0; // dwTotalFrames inside avih
  uint32_t avihMaxBpsPos_ = 0; // dwMaxBytesPerSec
  uint32_t strhLengthPos_ = 0; // dwLength inside strh
  uint32_t strhRatePos_ = 0;
  uint32_t moviListSizePos_ = 0;
  uint32_t moviDataStart_ = 0; // first byte after 'movi'

  // 100 frames for a 10 s clip, with headroom for the extend-on-motion case.
  static constexpr int kMaxIndex = 600;
  IndexEntry index_[kMaxIndex];
  int indexCount_ = 0;

  Stats stats_{};
  uint32_t startMs_ = 0;
  uint32_t lastTsMs_ = 0;
  bool finished_ = false;
};
