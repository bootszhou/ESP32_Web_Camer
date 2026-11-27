// TunnelClient implementation. See tunnel_client.h for the protocol rationale.
//
// Buffer policy: every staging buffer lives in PSRAM (the device has ~8 MB
// free). The relay TLS task only has a 12 KB stack and 156 KB of internal heap
// is precious, so the tunnel must not spend any of it.

#include "network/tunnel_client.h"

#include <esp_heap_caps.h>

namespace {

// Dial the device's own httpd. Loopback is tried first because it never leaves
// the chip; the station address is the fallback for builds/SDKs where the lwIP
// loopback netif does not route TCP to 127.0.0.1.
bool dial(WiFiClient& sock, uint16_t port, uint32_t timeoutMs) {
  static const uint32_t kAttempts[] = {0, 1};  // 0 = loopback, 1 = own IP
  for (uint8_t i = 0; i < 2; ++i) {
    IPAddress addr = (kAttempts[i] == 0) ? IPAddress(127, 0, 0, 1) : WiFi.localIP();
    if (kAttempts[i] == 1 && addr == IPAddress(0, 0, 0, 0)) continue;
    sock.stop();
    if (sock.connect(addr, port, timeoutMs)) return true;
  }
  return false;
}

uint8_t* allocBuf() {
  uint8_t* p = (uint8_t*)heap_caps_malloc(TunnelClient::kBufSize, MALLOC_CAP_SPIRAM);
  if (!p) p = (uint8_t*)malloc(TunnelClient::kBufSize);  // degraded but alive
  return p;
}

}  // namespace

void TunnelClient::begin() {
  running_ = true;
  if (taskStarted_) return;
  // 8 KB words = 32 KB stack: only plain socket writes happen here, but the
  // staging copies and printf formatting are not trivial.
  if (xTaskCreate(taskEntry, "tunnel", 8192, this, 2, nullptr) == pdPASS) {
    taskStarted_ = true;
  } else {
    running_ = false;
    Serial.println("[tunnel] task create failed");
  }
}

void TunnelClient::end() {
  running_ = false;
}

void TunnelClient::taskEntry(void* arg) {
  static_cast<TunnelClient*>(arg)->taskLoop();
}

TunnelClient::Channel* TunnelClient::find(uint16_t id) {
  for (auto& ch : chans_) {
    if (ch.used && ch.id == id) return &ch;
  }
  return nullptr;
}

TunnelClient::Channel* TunnelClient::findFree() {
  for (auto& ch : chans_) {
    if (!ch.used) return &ch;
  }
  return nullptr;
}

bool TunnelClient::open(uint16_t id, uint16_t port, const char* method, const char* path,
                        size_t contentLength) {
  if (find(id)) close(id);  // the relay reused an id we still hold: reset it
  Channel* ch = findFree();
  if (!ch) {
    Serial.printf("[tunnel] no free channel for id=%u\n", (unsigned)id);
    return false;
  }
  if (!ch->outBuf && !(ch->outBuf = allocBuf())) return false;
  if (!ch->inBuf && !(ch->inBuf = allocBuf())) return false;

  if (!dial(ch->sock, port, 3000)) {
    ch->sock.stop();
    return false;
  }

  // A tunnel request is one-shot: ask the device httpd to close when done so
  // the byte stream gets a clean EOF instead of hanging on keep-alive. The
  // head is staged but NOT released to the pump yet: a POST body may still be
  // appended, and head+body must go out as one contiguous write.
  char req[512];
  int n;
  if (contentLength) {
    n = snprintf(req, sizeof(req),
                 "%s %s HTTP/1.1\r\n"
                 "Host: 127.0.0.1:%u\r\n"
                 "Connection: close\r\n"
                 "Content-Length: %u\r\n"
                 "X-CamAI-Tunnel: 1\r\n"
                 "\r\n",
                 method, path, (unsigned)port, (unsigned)contentLength);
  } else {
    n = snprintf(req, sizeof(req),
                 "%s %s HTTP/1.1\r\n"
                 "Host: 127.0.0.1:%u\r\n"
                 "Connection: close\r\n"
                 "X-CamAI-Tunnel: 1\r\n"
                 "\r\n",
                 method, path, (unsigned)port);
  }
  if (n < 0 || n >= (int)sizeof(req) || (size_t)n > kBufSize) {
    ch->sock.stop();
    return false;
  }
  const size_t headLen = (size_t)n;

  ch->used = true;
  ch->id = id;
  ch->outLen = 0;
  ch->outReady = false;
  ch->outEof = false;
  ch->inLen = headLen;
  memcpy(ch->inBuf, req, headLen);
  ch->inReady = false;  // flushed by finish(), or appended to by feed()
  ch->inEof = false;
  ch->lastMs = millis();
  return true;
}

void TunnelClient::feed(uint16_t id, const uint8_t* data, size_t len) {
  Channel* ch = find(id);
  if (!ch || !len) return;
  // One staging buffer holds head + body: the two are written together so the
  // device never sees a half request. Request bodies here are config JSON or
  // WiFi credentials, all far below 2 KB; anything larger is a caller bug.
  if (len > kBufSize || ch->inLen + len > kBufSize) {
    Serial.printf("[tunnel] id=%u feed overflow (%u bytes)\n", (unsigned)id, (unsigned)len);
    release(*ch, true);
    return;
  }
  memcpy(ch->inBuf + ch->inLen, data, len);
  ch->inLen += len;
  ch->inReady = true;
  ch->lastMs = millis();
}

void TunnelClient::finish(uint16_t id) {
  Channel* ch = find(id);
  if (!ch) return;
  if (ch->inLen) ch->inReady = true;  // flush a head-only (GET) request
  ch->inEof = true;
  ch->lastMs = millis();
}

void TunnelClient::close(uint16_t id) {
  Channel* ch = find(id);
  if (!ch) return;
  release(*ch, false);
}

size_t TunnelClient::pollOut(uint16_t* id, uint8_t* out, size_t cap, bool* eof) {
  *eof = false;
  // Round-robin instead of always serving channel 0: an MJPEG stream keeps one
  // channel permanently readable, and a fixed scan order would starve the
  // small status/config requests queued behind it.
  for (uint16_t i = 0; i < kMaxChannels; ++i) {
    const uint16_t idx = (uint16_t)((nextOut_ + i) % kMaxChannels);
    Channel& ch = chans_[idx];
    if (!ch.used || !ch.outReady) continue;
    const size_t n = ch.outLen < cap ? ch.outLen : cap;
    memcpy(out, ch.outBuf, n);
    if (id) *id = ch.id;
    *eof = ch.outEof;
    ch.outReady = false;  // lets the pump task refill it
    nextOut_ = (uint16_t)((idx + 1) % kMaxChannels);
    return n;
  }
  return 0;
}

uint16_t TunnelClient::openCount() const {
  uint16_t n = 0;
  for (const auto& ch : chans_) {
    if (ch.used) ++n;
  }
  return n;
}

bool TunnelClient::takeFinished(uint16_t* id) {
  if (!finishedCount_) return false;
  *id = finished_[0];
  for (uint8_t i = 1; i < finishedCount_; ++i) finished_[i - 1] = finished_[i];
  --finishedCount_;
  return true;
}

void TunnelClient::release(Channel& ch, bool reportEof) {
  if (!ch.used) return;
  if (reportEof && finishedCount_ < kMaxChannels) {
    finished_[finishedCount_++] = ch.id;
  }
  ch.sock.stop();
  ch.used = false;
  ch.outReady = false;
  ch.inReady = false;
  ch.outEof = false;
  ch.inEof = false;
  ch.outLen = 0;
  ch.inLen = 0;
}

void TunnelClient::closeAll() {
  for (auto& ch : chans_) {
    if (ch.used) release(ch, false);
  }
  finishedCount_ = 0;
}

void TunnelClient::taskLoop() {
  while (true) {
    if (!running_) {
      closeAll();
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    bool busy = false;
    for (auto& ch : chans_) {
      if (!ch.used) continue;

      // ---- relay -> socket ------------------------------------------------
      if (ch.inReady) {
        const size_t n = ch.sock.write(ch.inBuf, ch.inLen);
        if (n == 0) {
          if (!ch.sock.connected()) release(ch, true);
        } else {
          if (n < ch.inLen) {
            memmove(ch.inBuf, ch.inBuf + n, ch.inLen - n);
            ch.inLen -= n;
          } else {
            ch.inLen = 0;
            ch.inReady = false;
          }
          ch.lastMs = millis();
          busy = true;
        }
      }

      // ---- socket -> relay ------------------------------------------------
      if (!ch.inReady && !ch.outReady && !ch.outEof) {
        const int avail = ch.sock.available();
        if (avail > 0) {
          const size_t want = (size_t)avail < kBufSize ? (size_t)avail : kBufSize;
          const size_t n = ch.sock.read(ch.outBuf, want);
          if (n > 0) {
            ch.outLen = n;
            ch.outReady = true;
            ch.lastMs = millis();
            busy = true;
          }
        } else if (!ch.sock.connected()) {
          // Peer closed and nothing buffered: hand the tail to the relay once.
          ch.outLen = 0;
          ch.outReady = true;
          ch.outEof = true;
          busy = true;
        }
      }

      // The relay consumes outReady; once it reports eof the channel is done.
      if (ch.outEof && !ch.outReady) {
        release(ch, true);
        continue;
      }

      if (millis() - ch.lastMs > kChannelIdleMs) {
        Serial.printf("[tunnel] id=%u idle timeout\n", (unsigned)ch.id);
        release(ch, true);
      }
    }

    if (!busy) vTaskDelay(pdMS_TO_TICKS(4));
  }
}
