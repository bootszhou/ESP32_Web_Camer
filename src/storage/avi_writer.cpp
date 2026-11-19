#include "avi_writer.h"

namespace {

// AVI header layout (all little-endian RIFF chunks).
//
//   RIFF <size> AVI
//     LIST <size> hdrl
//       avih <56>  main header
//       LIST <size> strl
//         strh <56>  stream header (vids / MJPG)
//         strf <40>  BITMAPINFOHEADER
//     LIST <size> movi
//       00dc <len> <jpeg data>   <- frames
//     idx1 <size> <entries>       <- index
//
// Sizes and frame counts are unknown while recording, so fixed placeholders
// are written first and patched by finish().
constexpr uint32_t kAvihSize = 56;
constexpr uint32_t kStrhSize = 56;
constexpr uint32_t kStrfSize = 40;

}  // namespace

void AviWriter::writeLE16(uint16_t v) {
  uint8_t b[2] = {(uint8_t)(v & 0xFF), (uint8_t)(v >> 8)};
  file_->write(b, 2);
}

void AviWriter::writeLE32(uint32_t v) {
  uint8_t b[4] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                  (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF)};
  file_->write(b, 4);
}

void AviWriter::writeFourCC(const char* tag) {
  file_->write((const uint8_t*)tag, 4);
}

bool AviWriter::begin(fs::FS& fs, const char* path, uint16_t width,
                      uint16_t height, uint8_t fps) {
  if (file_) return false;

  file_ = new fs::File(fs.open(path, FILE_WRITE));
  if (!file_ || !*file_) {
    Serial.printf("[avi] cannot open %s\n", path);
    delete file_;
    file_ = nullptr;
    return false;
  }
  strncpy(path_, path, sizeof(path_) - 1);

  stats_ = Stats{};
  indexCount_ = 0;
  finished_ = false;
  startMs_ = millis();
  lastTsMs_ = 0;

  if (!writeHeaderPlaceholder(width, height, fps)) {
    file_->close();
    delete file_;
    file_ = nullptr;
    return false;
  }
  return true;
}

bool AviWriter::writeHeaderPlaceholder(uint16_t width, uint16_t height, uint8_t fps) {
  const uint32_t usecPerFrame = 1000000UL / (fps ? fps : 10);

  // ---- RIFF header ----
  writeFourCC("RIFF");
  riffSizePos_ = file_->position();
  writeLE32(0);              // patched later
  writeFourCC("AVI ");

  // ---- LIST hdrl ----
  // Payload: 'hdrl' + avih chunk + strl LIST (12-byte header + strh + strf).
  // A previous version of this expression had a trailing "+ 4", making the
  // size field 4 bytes larger than the actual content; players tolerate it,
  // but strict RIFF walkers land mid-header, so keep it exact now. Clips
  // written before this fix still carry the overshoot -- the playback reader
  // pattern-matches 'LIST????movi' instead of trusting size fields.
  writeFourCC("LIST");
  writeLE32(4 + (8 + kAvihSize) + (4 + 8) + (8 + kStrhSize) + (8 + kStrfSize));
  writeFourCC("hdrl");

  // ---- avih ----
  writeFourCC("avih");
  writeLE32(kAvihSize);
  writeLE32(usecPerFrame);       // dwMicroSecPerFrame
  avihMaxBpsPos_ = file_->position();
  writeLE32(0);                  // dwMaxBytesPerSec - patched
  writeLE32(0);                  // dwPaddingGranularity
  writeLE32(0x10);               // dwFlags: AVIF_HASINDEX
  avihFramesPos_ = file_->position();
  writeLE32(0);                  // dwTotalFrames - patched
  writeLE32(0);                  // dwInitialFrames
  writeLE32(1);                  // dwStreams
  writeLE32(0);                  // dwSuggestedBufferSize
  writeLE32(width);              // dwWidth
  writeLE32(height);             // dwHeight
  writeLE32(0); writeLE32(0); writeLE32(0); writeLE32(0);  // reserved

  // ---- LIST strl ----
  writeFourCC("LIST");
  writeLE32(4 + (8 + kStrhSize) + (8 + kStrfSize));
  writeFourCC("strl");

  // ---- strh ----
  writeFourCC("strh");
  writeLE32(kStrhSize);
  writeFourCC("vids");
  writeFourCC("MJPG");
  writeLE32(0);                  // dwFlags
  writeLE16(0);                  // wPriority
  writeLE16(0);                  // wLanguage
  writeLE32(0);                  // dwInitialFrames
  // AVI timing is expressed as dwScale / dwRate seconds per frame.
  // For 10 fps that is 1/10 s, so use scale=1, rate=fps.
  // (Using usecPerFrame here would make players think each frame is seconds
  // long, which is the classic "video plays 100x too slow" AVI bug.)
  strhRatePos_ = file_->position();
  writeLE32(1);                            // dwScale
  writeLE32(fps ? fps : 10);               // dwRate
  writeLE32(0);                            // dwStart
  strhLengthPos_ = file_->position();
  writeLE32(0);                  // dwLength - patched
  writeLE32(0);                  // dwSuggestedBufferSize
  writeLE32(0xFFFFFFFF);         // dwQuality
  writeLE32(0);                  // dwSampleSize
  writeLE16(0); writeLE16(0);    // rcFrame left/top
  writeLE16(width); writeLE16(height);  // rcFrame right/bottom

  // ---- strf (BITMAPINFOHEADER) ----
  writeFourCC("strf");
  writeLE32(kStrfSize);
  writeLE32(kStrfSize);          // biSize
  writeLE32(width);              // biWidth
  writeLE32(height);             // biHeight
  writeLE16(1);                  // biPlanes
  writeLE16(24);                 // biBitCount (informational for MJPG)
  writeFourCC("MJPG");           // biCompression
  writeLE32(width * height * 3); // biSizeImage
  writeLE32(0); writeLE32(0);    // pel aspect / resolution
  writeLE32(0); writeLE32(0);    // colors used / important

  // ---- LIST movi ----
  writeFourCC("LIST");
  moviListSizePos_ = file_->position();
  writeLE32(0);                  // patched
  writeFourCC("movi");
  moviDataStart_ = file_->position();

  file_->flush();
  return true;
}

bool AviWriter::addFrame(const uint8_t* jpeg, size_t len, uint32_t timestampMs) {
  if (!file_ || finished_) return false;
  if (indexCount_ >= kMaxIndex) {
    stats_.dropped++;
    return false;
  }

  const uint32_t chunkOffset = file_->position() - moviDataStart_;

  writeFourCC("00dc");
  writeLE32((uint32_t)len);
  const size_t written = file_->write(jpeg, len);
  if (written != len) {
    Serial.printf("[avi] short write %u/%u\n", (unsigned)written, (unsigned)len);
    return false;
  }
  // RIFF chunks must be word-aligned.
  if (len & 1) {
    const uint8_t pad = 0;
    file_->write(&pad, 1);
  }

  index_[indexCount_].offset = chunkOffset;
  index_[indexCount_].size = (uint32_t)len;
  indexCount_++;

  stats_.frames++;
  stats_.bytes += len;
  lastTsMs_ = timestampMs;
  return true;
}

bool AviWriter::finish() {
  if (!file_ || finished_) return false;
  finished_ = true;

  if (stats_.frames == 0) {
    Serial.println("[avi] no frames recorded, discarding");
    file_->close();
    delete file_;
    file_ = nullptr;
    return false;
  }

  // Sizes must reflect the file AFTER idx1 is appended, so write the index
  // first and patch the headers last. Patching before the index leaves the
  // RIFF size short by exactly the index length, which truncates playback.
  const uint32_t moviEnd = file_->position();

  // ---- idx1 ----
  writeFourCC("idx1");
  writeLE32(indexCount_ * 16);
  for (int i = 0; i < indexCount_; i++) {
    writeFourCC("00dc");
    writeLE32(0x10);  // AVIIF_KEYFRAME
    writeLE32(index_[i].offset);
    writeLE32(index_[i].size);
  }
  file_->flush();

  const uint32_t fileEnd = file_->position();
  const uint32_t moviSize = moviEnd - moviListSizePos_ - 8;
  const uint32_t riffSize = fileEnd - 8;

  struct Patch {
    uint32_t pos;
    uint32_t value;
  };
  const Patch patches[] = {
      {riffSizePos_, riffSize},
      {avihFramesPos_, stats_.frames},
      {strhLengthPos_, stats_.frames},
      {moviListSizePos_, moviSize},
      {avihMaxBpsPos_,
       (uint32_t)((uint64_t)stats_.bytes * 8 / (stats_.frames ? stats_.frames : 1))},
  };
  for (const Patch& p : patches) {
    file_->seek(p.pos);
    writeLE32(p.value);
  }

  file_->seek(fileEnd);
  file_->flush();
  file_->close();
  delete file_;
  file_ = nullptr;

  Serial.printf("[avi] closed %s: %u frames, %u KB, riff=%u\n", path_,
                stats_.frames, stats_.bytes / 1024, riffSize);
  return true;
}
