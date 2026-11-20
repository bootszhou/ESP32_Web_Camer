// AviReader -- minimal sequential MJPEG-AVI reader for on-panel playback.
//
// Purpose: the Events page lists recorded clips and the user can select one
// with the buttons and play it back on the panel. The recorder (AviWriter)
// produces RIFF AVI with a single MJPG stream whose frames are "00dc" chunks
// inside the "movi" LIST; this reader walks that structure forward without
// loading anything into RAM except one frame at a time.
//
// Why not parse the idx1 index: clips written before the header-size fix in
// AviWriter::writeHeaderPlaceholder carry a slightly oversized hdrl LIST, and
// walking forward pattern-matching chunk tags is tolerant of that (the same
// approach the web server's stream endpoint uses). We simply scan for
// "00dc" tags from the current position, which lands on the first frame
// regardless of header quirks, and keep going until "movi" ends.
//
// Chunk grammar we accept (little-endian):
//   <fourcc><u32 size>[size bytes][pad to even]
// Frames are fourcc "00dc" + JPEG bytes; anything else is skipped over.
//
// Thread-safety: one instance, one task. The display task is the only caller
// (playback replaces the preview loop on the same task, so SPI stays
// single-owner and no mutex is needed).
#pragma once

#include <Arduino.h>
#include <FS.h>

class AviReader {
 public:
  // Opens the file and locates the first frame. Returns false if the file is
  // missing or has no recognizable frame data.
  bool begin(fs::FS& fs, const char* path) {
    end();
    file_ = new File(fs.open(path, FILE_READ));
    if (!file_ || !*file_) {
      end();
      return false;
    }
    // Sanity check: RIFF....AVI . Anything else is not ours.
    char tag[4] = {0};
    if (file_->read((uint8_t*)tag, 4) != 4 || memcmp(tag, "RIFF", 4) != 0) {
      end();
      return false;
    }
    file_->seek(8);
    if (file_->read((uint8_t*)tag, 4) != 4 || memcmp(tag, "AVI ", 4) != 0) {
      end();
      return false;
    }
    // Find the first "00dc" frame tag with a raw byte scan bounded to the
    // header region. Why not walk the RIFF chunk tree: (a) clips written
    // before the AviWriter header-size fix carry an oversized hdrl LIST, so
    // skipping by declared sizes lands misaligned; (b) skipping the "LIST
    // movi" chunk by its size field jumps over ALL frames straight into idx1,
    // where "00dc"-looking index entries would be misread as frames (16-byte
    // garbage that decode-fails every time). A raw scan for the first "00dc"
    // in the file is the first real frame tag -- no frame data precedes it.
    // Once inside movi, frames are contiguous chunks and the chunk-walking
    // in seekNextFrame() stays exact.
    if (!findFirstFrame()) {
      end();
      return false;
    }
    open_ = true;
    return true;
  }

  // Reads the next frame's JPEG bytes into buf (up to cap). Returns the frame
  // length, 0 at end of stream, -1 when the frame does not fit the buffer
  // (the frame is skipped and the reader moves on to the next one).
  int nextFrame(uint8_t* buf, size_t cap) {
    if (!open_ || pendingSize_ == 0) return 0;
    const uint32_t size = pendingSize_;
    pendingSize_ = 0;
    if (size > cap) {
      // Skip the oversized frame rather than abort: the next frame may fit.
      file_->seek(file_->position() + size + (size & 1));
      if (!seekNextFrame()) open_ = false;
      return -1;
    }
    if (file_->read(buf, size) != (int)size) {
      open_ = false;
      return 0;
    }
    if (size & 1) file_->seek(file_->position() + 1);  // RIFF word pad
    if (!seekNextFrame()) open_ = false;
    return (int)size;
  }

  // Rough progress 0..100 from the read position vs file size. Chunk-level
  // exactness is not worth a second pass over the file.
  int progressPercent() {
    if (!file_) return 0;
    const uint32_t pos = file_->position();
    const uint32_t total = file_->size();
    return total ? (int)((uint64_t)pos * 100 / total) : 0;
  }

  void end() {
    if (file_) {
      file_->close();
      delete file_;
      file_ = nullptr;
    }
    open_ = false;
    pendingSize_ = 0;
  }

  bool isOpen() const { return open_; }

  ~AviReader() { end(); }

 private:
  // Raw byte scan for the first "00dc" tag, bounded to the first 64 KB so a
  // frame-less file cannot make us read the whole card. On success the
  // position sits on the tag's 8-byte header (so pendingSize_ is the payload
  // length and nextFrame() reads from the payload).
  bool findFirstFrame() {
    constexpr uint32_t kScanLimit = 64 * 1024;
    const uint32_t fileSize = file_->size();
    const uint32_t limit = (fileSize < kScanLimit) ? fileSize : kScanLimit;
    uint8_t window[256];
    uint32_t pos = 4;  // after "RIFF"
    while (pos + 8 <= limit) {
      file_->seek(pos);
      const size_t want = (limit - pos < sizeof(window)) ? (limit - pos) : sizeof(window);
      const int got = file_->read(window, want);
      if (got < 4) return false;
      for (int i = 0; i + 4 <= got; i++) {
        if (memcmp(window + i, "00dc", 4) == 0 && pos + (uint32_t)i + 8 <= fileSize) {
          file_->seek(pos + i);
          uint8_t hdr[8];
          if (file_->read(hdr, 8) != 8) return false;
          pendingSize_ = le32(hdr + 4);
          if (pendingSize_ == 0 || pendingSize_ > 512 * 1024) return false;
          return true;  // positioned on the payload
        }
      }
      pos += (uint32_t)got - 3;  // overlap 3 bytes: tag may straddle reads
    }
    return false;
  }

  // From the current position, scan forward to the next "00dc" chunk tag.
  // Sets pendingSize_ to its payload size and leaves the position on the
  // payload. Returns false at end of movi (EOF, or a chunk whose declared
  // size is implausible -- corrupt tail).
  bool seekNextFrame() {
    uint8_t hdr[8];
    while (true) {
      const int got = file_->read(hdr, 8);
      if (got != 8) return false;
      if (memcmp(hdr, "00dc", 4) == 0) {
        pendingSize_ = le32(hdr + 4);
        // Reject absurd sizes (corrupt tail) instead of trusting them.
        if (pendingSize_ == 0 || pendingSize_ > 512 * 1024) return false;
        return true;
      }
      // Skip this non-frame chunk: payload + word pad. Bounded by the file
      // size so a bogus size field cannot run off the end.
      const uint32_t sz = le32(hdr + 4);
      const uint32_t remain = file_->size() - file_->position();
      if (sz > remain) return false;
      file_->seek(file_->position() + sz + (sz & 1));
    }
  }

  static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
  }

  File* file_ = nullptr;
  bool open_ = false;
  uint32_t pendingSize_ = 0;
};
