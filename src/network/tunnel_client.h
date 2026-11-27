// TunnelClient -- transparent TCP-over-WebSocket tunnel for the cloud relay (M9).
//
// Why this exists: the LAN page is served by the device's own httpd on port 80
// and its MJPEG/replay httpd on port 81. Neither has any authentication, so
// neither may ever face the internet directly. Instead the relay server opens a
// *reverse* channel: it asks the device (over the already-authenticated control
// WebSocket) to dial its own loopback httpd and pipe the bytes back. The relay
// authenticates the browser, and the device never listens on a public port.
//
// The tunnel is deliberately dumb: it does not parse HTTP at all. It copies raw
// bytes in both directions, so every existing handler, header and quirk of the
// device httpd behaves exactly as it does on the LAN. That is the whole point --
// no second implementation of 15 endpoints that could drift.
//
// Framing on the WebSocket (see relay_client.cpp for the text commands):
//   binary device->relay : [0x01][id hi][id lo][payload...]   tunnel bytes
//   binary device->relay : [0x00][jpeg...]                   camera frame
//   text   relay->device : {"type":"http_open","id":N,"port":80,"method":"GET","path":"/x"}
//                          {"type":"http_data","id":N}          (followed by binary)
//                          {"type":"http_end","id":N}           (request body complete)
// The 0x01 tag can never collide with a JPEG (those start 0xFF 0xD8), and the
// existing relay fanout skips anything that is not a zero-tagged frame.
//
// Threading contract: ALL TLS I/O stays on the relay task. A separate task here
// only touches loopback sockets, so the two never share a socket and no TLS
// mutex is needed. Each direction gets one staging buffer with a ready flag,
// which gives natural backpressure without rings or queues.
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class TunnelClient {
 public:
  // One channel per in-flight HTTP request, and every channel costs TWO lwIP
  // sockets: the socket this client opens plus the one the device httpd
  // accepts. With CONFIG_LWIP_MAX_SOCKETS = 16 and two listeners plus the
  // relay TLS socket already spoken for, four channels is the honest ceiling.
  // Raising this fails at connect() with ENFILE (errno 23).
  static constexpr uint16_t kMaxChannels = 4;   // playback + download + API polls
  static constexpr size_t kBufSize = 2048;      // one staging chunk per direction
  static constexpr uint32_t kChannelIdleMs = 60000;

  // Start/stop the pump task. Safe to call repeatedly.
  void begin();
  void end();
  bool started() const { return taskStarted_; }

  // ---- called from the relay task only ------------------------------------
  // Opens a channel; returns false when the table is full or the dial failed.
  // contentLength > 0 makes the head advertise the body that feed() will add.
  bool open(uint16_t id, uint16_t port, const char* method, const char* path,
            size_t contentLength = 0);
  // Queue request bytes (already raw HTTP) for the device socket.
  void feed(uint16_t id, const uint8_t* data, size_t len);
  // Request body finished; nothing else will arrive for this channel.
  void finish(uint16_t id);
  // Drops a channel and its socket.
  void close(uint16_t id);

  // Pops one outbound chunk (device socket -> relay). Returns the number of
  // payload bytes written to `out` (header included by the caller is NOT here:
  // `out` receives only payload). 0 means nothing ready.
  size_t pollOut(uint16_t* id, uint8_t* out, size_t cap, bool* eof);

  uint16_t openCount() const;
  // True when a channel reached EOF/error and must be reported with http_fin.
  bool takeFinished(uint16_t* id);
  void closeAll();

 private:
  struct Channel {
    bool used = false;
    uint16_t id = 0;
    WiFiClient sock;
    uint8_t* outBuf = nullptr;   // socket -> relay staging
    size_t outLen = 0;
    volatile bool outReady = false;
    volatile bool outEof = false;
    uint8_t* inBuf = nullptr;    // relay -> socket staging
    size_t inLen = 0;
    volatile bool inReady = false;
    volatile bool inEof = false;
    uint32_t lastMs = 0;
  };

  static void taskEntry(void* arg);
  void taskLoop();
  Channel* find(uint16_t id);
  Channel* findFree();
  void release(Channel& ch, bool reportEof);

  Channel chans_[kMaxChannels];
  volatile bool running_ = false;
  bool taskStarted_ = false;
  uint16_t nextOut_ = 0;   // round-robin cursor so one busy stream cannot starve the rest
  uint16_t finished_[kMaxChannels] = {0};
  volatile uint8_t finishedCount_ = 0;
};
