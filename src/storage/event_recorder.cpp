#include "event_recorder.h"

#include <SD_MMC.h>
#include <time.h>

#include "board/board_config.h"

namespace {
// FAT32 timestamps are local time; without NTP we fall back to a counter so
// filenames stay unique and sortable rather than claiming a fake date.
const char* kEventsRoot = "/events";
}  // namespace

bool EventRecorder::begin(CameraService* camera, MotionDetector* motion,
                          const Config& cfg) {
  camera_ = camera;
  motion_ = motion;
  cfg_ = cfg;

  return remount();
}

bool EventRecorder::remount() {
  SD_MMC.end();
  delay(50);

  SD_MMC.setPins(board::SD_PIN_CLK, board::SD_PIN_CMD, board::SD_PIN_D0);
  // 1-bit bus: the board is wired for it and 4-bit would need D1..D3.
  if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 3)) {
    Serial.println("[rec] SD mount failed");
    status_.mounted = false;
    snprintf(status_.lastError, sizeof(status_.lastError), "SD mount failed");
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("[rec] no SD card");
    status_.mounted = false;
    snprintf(status_.lastError, sizeof(status_.lastError), "no card");
    SD_MMC.end();
    return false;
  }

  if (!SD_MMC.exists(kEventsRoot)) SD_MMC.mkdir(kEventsRoot);

  status_.mounted = true;
  refreshSpace();
  Serial.printf("[rec] SD ready: %lu MB total, %lu MB free\n",
                (unsigned long)status_.totalMb, (unsigned long)status_.freeMb);
  return true;
}

void EventRecorder::refreshSpace() {
  if (!status_.mounted) return;
  status_.totalMb = (uint32_t)(SD_MMC.totalBytes() / (1024ULL * 1024ULL));
  status_.freeMb = (uint32_t)((SD_MMC.totalBytes() - SD_MMC.usedBytes()) /
                              (1024ULL * 1024ULL));
}

bool EventRecorder::makeEventPaths(char* avi, size_t aviLen, char* json,
                                   size_t jsonLen) {
  // Prefer a real date directory; fall back to an uptime-based name.
  char dir[32];
  time_t now = time(nullptr);
  struct tm tmv;
  bool haveTime = false;
  if (now > 1700000000) {  // sanity: after 2023-11-14 means NTP worked
    localtime_r(&now, &tmv);
    strftime(dir, sizeof(dir), "%Y-%m-%d", &tmv);
    haveTime = true;
  } else {
    snprintf(dir, sizeof(dir), "uptime");
  }

  char dirPath[48];
  snprintf(dirPath, sizeof(dirPath), "%s/%s", kEventsRoot, dir);
  if (!SD_MMC.exists(dirPath)) SD_MMC.mkdir(dirPath);

  // Find the next free index rather than trusting a counter alone, so a reboot
  // cannot overwrite existing clips.
  int idx = (int)clipIndex_ + 1;
  char candidate[80];
  for (int attempt = 0; attempt < 1000; attempt++, idx++) {
    if (haveTime) {
      char stamp[24];
      strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
      snprintf(candidate, sizeof(candidate), "%s/EVT_%s_%03d.avi",
               dirPath, stamp, idx);
    } else {
      snprintf(candidate, sizeof(candidate), "%s/EVT_%08lu_%03d.avi",
               dirPath, (unsigned long)(millis() / 1000), idx);
    }
    if (!SD_MMC.exists(candidate)) break;
  }
  clipIndex_ = (uint32_t)idx;

  strncpy(avi, candidate, aviLen - 1);
  // Metadata path shares the basename with a .json suffix.
  strncpy(json, candidate, jsonLen - 1);
  char* dot = strrchr(json, '.');
  if (dot && (size_t)(dot - json + 6) < jsonLen) {
    strcpy(dot, ".json");
  }
  return true;
}

void EventRecorder::startRecording() {
  if (!status_.mounted) return;
  if (status_.freeMb < cfg_.minFreeMb) {
    snprintf(status_.lastError, sizeof(status_.lastError),
             "low space: %lu MB free", (unsigned long)status_.freeMb);
    Serial.printf("[rec] refusing to record, only %lu MB free\n",
                  (unsigned long)status_.freeMb);
    return;
  }

  if (!makeEventPaths(clipPath_, sizeof(clipPath_), jsonPath_, sizeof(jsonPath_))) {
    return;
  }

  // Write to .tmp so an interrupted clip is never mistaken for a good one.
  char tmpPath[72];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", clipPath_);

  if (!writer_.begin(SD_MMC, tmpPath, camera_->width(), camera_->height(),
                     cfg_.nominalFps)) {
    snprintf(status_.lastError, sizeof(status_.lastError), "open failed");
    status_.clipsFailed++;
    return;
  }

  status_.recording = true;
  clipStartMs_ = millis();
  // Freeze the target duration now. Reading cfg_ live meant a settings change
  // during a clip could end it early or stretch it unexpectedly.
  clipTargetMs_ = cfg_.clipDurationMs;
  lastMotionMs_ = clipStartMs_;
  Serial.printf("[rec] recording -> %s (target %lu ms)\n", clipPath_,
                (unsigned long)clipTargetMs_);
}

void EventRecorder::finishRecording() {
  if (!status_.recording) return;
  status_.recording = false;

  const uint32_t frames = writer_.stats().frames;
  const uint32_t bytes = writer_.stats().bytes;
  const bool ok = writer_.finish();

  status_.lastClipFrames = frames;
  status_.lastClipBytes = bytes;
  status_.framesWritten += frames;

  if (!ok || frames == 0) {
    status_.clipsFailed++;
    snprintf(status_.lastError, sizeof(status_.lastError), "clip had no frames");
    return;
  }

  // Only now promote the temporary file to its final name.
  char tmpPath[72];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", clipPath_);
  if (!SD_MMC.rename(tmpPath, clipPath_)) {
    Serial.printf("[rec] rename failed %s\n", tmpPath);
    snprintf(status_.lastError, sizeof(status_.lastError), "rename failed");
    status_.clipsFailed++;
    return;
  }

  writeMetadata(jsonPath_);

  status_.clipsWritten++;
  snprintf(status_.lastClip, sizeof(status_.lastClip), "%s",
           strrchr(clipPath_, '/') ? strrchr(clipPath_, '/') + 1 : clipPath_);
  snprintf(status_.lastError, sizeof(status_.lastError), "%s", "");
  refreshSpace();

  Serial.printf("[rec] saved %s: %u frames, %u KB, %lu MB free\n",
                status_.lastClip, frames, bytes / 1024,
                (unsigned long)status_.freeMb);
}

void EventRecorder::writeMetadata(const char* jsonPath) {
  File f = SD_MMC.open(jsonPath, FILE_WRITE);
  if (!f) return;

  time_t now = time(nullptr);
  char stamp[32] = "unknown";
  if (now > 1700000000) {
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
  }

  const float avgFps = status_.lastClipFrames
                           ? (status_.lastClipFrames * 1000.0f) /
                                 (millis() - clipStartMs_ + 1)
                           : 0.0f;

  f.printf("{\n");
  f.printf("  \"file\": \"%s\",\n",
           strrchr(clipPath_, '/') ? strrchr(clipPath_, '/') + 1 : clipPath_);
  f.printf("  \"trigger\": \"motion\",\n");
  f.printf("  \"recorded_at\": \"%s\",\n", stamp);
  f.printf("  \"width\": %u,\n", camera_->width());
  f.printf("  \"height\": %u,\n", camera_->height());
  f.printf("  \"nominal_fps\": %u,\n", cfg_.nominalFps);
  f.printf("  \"measured_fps\": %.1f,\n", avgFps);
  f.printf("  \"frames\": %lu,\n", (unsigned long)status_.lastClipFrames);
  f.printf("  \"bytes\": %lu,\n", (unsigned long)status_.lastClipBytes);
  f.printf("  \"complete\": true\n");
  f.printf("}\n");
  f.close();
}

void EventRecorder::onMotionTrigger() {
  if (!status_.mounted) return;
  if (!enabled_) return;   // user switched recording off

  // Ignore triggers briefly after a manual stop. The detector may still be
  // active (motion continues), and without this the next cooldown expiry
  // restarted a clip within seconds, making the stop button look broken.
  if ((int32_t)(millis() - suppressTriggersUntilMs_) < 0) return;

  if (!status_.recording) {
    startRecording();
    return;
  }

  // Already recording: remember the latest motion so the clip can be extended
  // rather than split into fragments.
  lastMotionMs_ = millis();
}

// Closes the active clip immediately, finalising the AVI so the partial
// recording is still playable. The file keeps its normal name; nothing is
// discarded, because a stopped clip is still evidence of what happened.
bool EventRecorder::stopRecording() {
  if (!status_.recording) return false;
  Serial.println("[rec] stop requested by user");
  // Suppress re-triggering for a while so the stop is actually visible on the
  // page instead of a new clip starting on the next cooldown expiry.
  suppressTriggersUntilMs_ = millis() + 15000;
  finishRecording();
  return true;
}

// Keeps the clip alive while the detector still reports movement.
//
// Relying only on onMotionTrigger() was not enough: the detector fires on a
// 30 s cooldown, so during a long episode no new trigger arrived and the clip
// ended after its base duration even though movement continued.
void EventRecorder::noteActivity() {
  if (!status_.recording) return;
  lastMotionMs_ = millis();
}

void EventRecorder::offerFrame(camera_fb_t* fb) {
  if (!status_.recording || !fb) return;
  if (fb->format != PIXFORMAT_JPEG) return;

  if (!writer_.addFrame(fb->buf, fb->len, millis() - clipStartMs_)) {
    status_.framesDropped++;
    return;
  }

  // One frame per call is all we take; the writer runs inline.
  if (writer_.stats().frames % 11 == 0) {
    // Check space occasionally rather than every frame (statfs is not free).
    refreshSpace();
  }
}

void EventRecorder::loop() {
  if (!status_.recording) return;

  const uint32_t now = millis();
  const uint32_t elapsed = now - clipStartMs_;

  uint32_t target = clipTargetMs_;

  // Extend while motion continues, up to the ceiling. Uses the frozen target
  // plus live extension logic, so the base duration never changes mid-clip.
  if (cfg_.extendOnMotion) {
    const uint32_t sinceMotion = now - lastMotionMs_;
    if (sinceMotion < clipTargetMs_) {
      const uint32_t extended = (lastMotionMs_ - clipStartMs_) + clipTargetMs_;
      if (extended > target) target = extended;
    }
  }
  if (target > cfg_.maxClipDurationMs) target = cfg_.maxClipDurationMs;

  // Stop early if the card is nearly full rather than corrupting the file.
  if (status_.freeMb < 20) {
    Serial.println("[rec] free space critical, closing clip");
    finishRecording();
    return;
  }

  if (elapsed >= target) {
    finishRecording();
  }
}

// Deletes the oldest clips until the requested free space is available.
//
// "Oldest" is determined by the date directory name and then the filename, both
// of which sort lexicographically because they carry zero-padded timestamps.
// The currently recording clip lives under a .tmp name and is skipped, and a
// clip is only removed after its .json sidecar so a crash mid-cleanup cannot
// leave the listing pointing at a missing video.
int EventRecorder::cleanOldest(int targetFreeMb, int maxRemove) {
  if (!status_.mounted || maxRemove <= 0) return 0;

  refreshSpace();
  if ((int)status_.freeMb >= targetFreeMb) return 0;

  constexpr int kMaxDirs = 20;
  String dirs[kMaxDirs];
  int dirCount = 0;

  File root = SD_MMC.open("/events");
  if (!root || !root.isDirectory()) return 0;

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

  int removed = 0;
  // Oldest directory first; filenames inside are also oldest-first.
  for (int i = 0; i < dirCount && removed < maxRemove; i++) {
    String dirPath = String("/events/") + dirs[i];
    File dir = SD_MMC.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) continue;

    File f = dir.openNextFile();
    while (f && removed < maxRemove) {
      String fname = f.name();
      int slash = fname.lastIndexOf('/');
      if (slash >= 0) fname = fname.substring(slash + 1);

      if (fname.endsWith(".avi")) {
        String path = dirPath + "/" + fname;
        // Sidecar first, then the video.
        String jsonPath = path;
        jsonPath.replace(".avi", ".json");
        if (SD_MMC.exists(jsonPath.c_str())) SD_MMC.remove(jsonPath.c_str());

        if (SD_MMC.remove(path.c_str())) {
          removed++;
          Serial.printf("[rec] auto-clean removed %s\n", path.c_str());
        }
      }
      f = dir.openNextFile();
      delay(1);  // yield so the HTTP task keeps running during a long cleanup
    }
    dir.close();

    if ((int)status_.freeMb >= targetFreeMb) break;
    refreshSpace();
  }

  if (removed > 0) {
    refreshSpace();
    Serial.printf("[rec] auto-clean freed %d file(s), now %lu MB free\n",
                  removed, (unsigned long)status_.freeMb);
  }
  return removed;
}
