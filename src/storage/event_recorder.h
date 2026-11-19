// EventRecorder -- turns a motion trigger into a 10 second MJPEG AVI on SD.
//
// State machine:
//   Idle --trigger--> Recording --duration elapsed--> Finalise --> Idle
//
// Write strategy, chosen after measuring the card at ~1.07 MB/s:
//   Recording needs about 125 KB/s at VGA/10fps, roughly 8x headroom, so the
//   writer runs synchronously on the recorder's own loop rather than on a
//   separate task with an unbounded queue. That keeps memory flat and makes
//   failures visible immediately instead of hiding them behind a backlog.
//
//   The camera buffer is recycled by the driver the moment it is returned, so
//   a pointer cannot be kept for later. Frames are written to the card inside
//   the same call that offers them, before releasing the buffer.
#pragma once

#include <Arduino.h>
#include <FS.h>

#include "camera/camera_service.h"
#include "detection/motion_detector.h"
#include "storage/avi_writer.h"

class EventRecorder {
 public:
  struct Config {
    uint32_t clipDurationMs = 10000;   // record 10 s from the trigger
    uint8_t nominalFps = 10;
    // Motion during a clip extends it instead of starting a second file.
    bool extendOnMotion = true;
    uint32_t maxClipDurationMs = 60000;  // hard ceiling for one clip
    uint32_t minFreeMb = 200;            // stop before the card fills up
  };

  struct Status {
    bool mounted = false;
    bool recording = false;
    uint32_t clipsWritten = 0;
    uint32_t clipsFailed = 0;
    uint32_t framesWritten = 0;
    uint32_t framesDropped = 0;
    uint32_t lastClipBytes = 0;
    uint32_t lastClipFrames = 0;
    uint32_t freeMb = 0;
    uint32_t totalMb = 0;
    char lastClip[40] = {0};
    char lastError[64] = {0};
  };

  bool begin(CameraService* camera, MotionDetector* motion, const Config& cfg);
  void loop();

  // Applies new settings immediately. A clip already in progress keeps the
  // duration it started with, so a settings change never truncates a recording
  // mid-write.
  void setConfig(const Config& cfg) { cfg_ = cfg; }
  const Config& config() const { return cfg_; }

  // Deletes the oldest completed clips until at least targetFreeMb is available.
  // Returns the number of files removed.
  int cleanOldest(int targetFreeMb, int maxRemove = 20);

  // Called when the detector fires; starts or extends a clip.
  void onMotionTrigger();

  // Stops the current clip early and finalises it, so the file is playable.
  // Returns true if a clip was actually closed.
  bool stopRecording();

  // Blocks new clips from starting. An in-progress clip keeps recording.
  void setRecordingEnabled(bool enabled) { enabled_ = enabled; }
  bool recordingEnabled() const { return enabled_; }

  // Called every loop while the detector reports movement, so a long episode
  // keeps extending the clip instead of ending early.
  void noteActivity();

  // Feed a frame. Returns immediately when not recording.
  void offerFrame(camera_fb_t* fb);

  bool isRecording() const { return status_.recording; }
  const Status& status() const { return status_; }

  // Re-scan the card (after insertion/removal).
  bool remount();

 private:
  void startRecording();
  void finishRecording();
  void refreshSpace();
  bool makeEventPaths(char* avi, size_t aviLen, char* json, size_t jsonLen);
  void writeMetadata(const char* jsonPath);

  CameraService* camera_ = nullptr;
  MotionDetector* motion_ = nullptr;
  Config cfg_{};
  Status status_{};

  AviWriter writer_;

  uint32_t clipStartMs_ = 0;
  uint32_t clipTargetMs_ = 0;   // frozen at clip start, so edits do not truncate
  uint32_t lastMotionMs_ = 0;
  bool enabled_ = true;         // master switch for starting new clips
  // After a manual stop, suppress new clips briefly. Without this the detector
  // (which can legitimately still be active) re-triggered within the 2 s UI
  // poll interval, so the page looked as if the stop button did nothing.
  uint32_t suppressTriggersUntilMs_ = 0;
  uint32_t clipIndex_ = 0;
  char clipPath_[64] = {0};
  char jsonPath_[64] = {0};
  char dateDir_[32] = {0};
};
