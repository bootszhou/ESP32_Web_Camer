#include "relay_client.h"

#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>

#include <time.h>

// Relay endpoint constants live in secrets.h (not committed). Provide harmless
// fallbacks so the tree still compiles when secrets.h predates M7.
#include "secrets.h"
#ifndef RELAY_CA_CERT
#define RELAY_CA_CERT ""
#endif

namespace {

// WS frame opcode constants (RFC 6455).
constexpr uint8_t kOpText = 0x1;
constexpr uint8_t kOpBinary = 0x2;
constexpr uint8_t kOpPing = 0x9;
constexpr uint8_t kOpPong = 0xA;
constexpr uint8_t kOpClose = 0x8;

// Masking key for client->server frames. RFC 6455 requires client frames to be
// masked; using an all-zero key is protocol-legal (the mask is obfuscation, not
// security) and avoids per-byte XOR cost on ~12 KB JPEG frames.
constexpr uint8_t kMaskKey[4] = {0, 0, 0, 0};

// Task-local pointer to the live TLS connection. Only the relay task touches
// it, so no locking is needed; it simply lets the small send helpers below
// reach the socket created inside runSession().
WiFiClientSecure* gActiveTls = nullptr;

// Nonce source for Sec-WebSocket-Key. Entropy quality does not matter here
// (the server just echoes it back), but it must differ per connection.
uint32_t gWsNonce = 0x1234567;

void makeWsKey(char out[25]) {
  uint8_t raw[16];
  for (int i = 0; i < 4; ++i) {
    gWsNonce = gWsNonce * 1664525u + 1013904223u;
    raw[i * 4 + 0] = (gWsNonce >> 0) & 0xFF;
    raw[i * 4 + 1] = (gWsNonce >> 8) & 0xFF;
    raw[i * 4 + 2] = (gWsNonce >> 16) & 0xFF;
    raw[i * 4 + 3] = (gWsNonce >> 24) & 0xFF;
  }
  size_t olen = 0;
  mbedtls_base64_encode((uint8_t*)out, 25, &olen, raw, 16);
}

// RFC 6455 section 4.2.1: Sec-WebSocket-Accept must equal
// base64(SHA1(key + GUID)). Verifying it catches misbehaving proxies.
bool checkAccept(const char* key, const char* acceptVal) {
  const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  char cat[64];
  snprintf(cat, sizeof(cat), "%.24s%s", key, kGuid);

  uint8_t sha[20];
  mbedtls_sha1((const uint8_t*)cat, strlen(cat), sha);

  uint8_t b64[32];
  size_t olen = 0;
  mbedtls_base64_encode(b64, sizeof(b64), &olen, sha, 20);
  return acceptVal && strcasecmp((const char*)b64, acceptVal) == 0;
}

bool sendFrameParts(uint8_t op, const uint8_t* prefix, size_t prefixLen,
                    const uint8_t* data, size_t len) {
  if (!gActiveTls || !gActiveTls->connected()) return false;
  const size_t total = prefixLen + len;
  uint8_t hdr[10];
  size_t hdrLen = 0;
  hdr[hdrLen++] = 0x80 | op;  // FIN + opcode
  if (total < 126) {
    hdr[hdrLen++] = 0x80 | (uint8_t)total;  // MASK bit + 7-bit length
  } else if (total < 65536) {
    hdr[hdrLen++] = 0x80 | 126;
    hdr[hdrLen++] = (total >> 8) & 0xFF;
    hdr[hdrLen++] = total & 0xFF;
  } else {
    return false;  // 12 KB JPEGs fit 16-bit length; larger means a bug
  }
  memcpy(hdr + hdrLen, kMaskKey, 4);
  hdrLen += 4;

  if (gActiveTls->write(hdr, hdrLen) != hdrLen) return false;
  if (prefixLen && gActiveTls->write(prefix, prefixLen) != prefixLen) return false;
  if (len == 0) return true;
  size_t off = 0;
  while (off < len) {
    size_t chunk = (len - off) > 4096 ? 4096 : (len - off);
    if (gActiveTls->write(data + off, chunk) != chunk) return false;
    off += chunk;
  }
  return true;
}

bool sendFrame(uint8_t op, const uint8_t* data, size_t len) {
  return sendFrameParts(op, nullptr, 0, data, len);
}

// Binary frames sent to the relay always carry a one-byte tag so the server can
// tell camera JPEGs from tunnel bytes without guessing. A JPEG starts 0xFF 0xD8,
// so 0x00/0x01 can never be confused with one.
constexpr uint8_t kTagJpeg = 0x00;
constexpr uint8_t kTagTunnel = 0x01;

bool sendFrameTagged(uint8_t tag, uint16_t id, const uint8_t* data, size_t len) {
  uint8_t prefix[3] = {tag, (uint8_t)(id >> 8), (uint8_t)(id & 0xFF)};
  const size_t prefixLen = (tag == kTagTunnel) ? 3 : 1;
  return sendFrameParts(kOpBinary, prefix, prefixLen, data, len);
}

}  // namespace

bool RelayClient::begin(CameraService* camera, const Config& cfg) {
  camera_ = camera;
  cfg_ = cfg;
  status_.enabled = cfg.enabled;
  if (!cfg.enabled) return true;  // dormant until enabled in settings

  if (!cfg.host || !*cfg.host) {
    setLastError("relay host not set");
    return false;
  }

  running_ = true;
  tunnel_.begin();
  if (!taskStarted_) {
    // 12 KB words = 48 KB stack: WiFiClientSecure handshake is deep.
    if (xTaskCreate(taskEntry, "relay", 12288, this, 1, nullptr) != pdPASS) {
      running_ = false;
      setLastError("relay task create failed");
      return false;
    }
    taskStarted_ = true;
  }
  return true;
}

void RelayClient::end() {
  running_ = false;
  tunnel_.end();
  tunnel_.closeAll();
}

void RelayClient::setConfig(const Config& cfg) {
  const bool wasEnabled = cfg_.enabled;
  cfg_ = cfg;
  status_.enabled = cfg.enabled;
  if (cfg.enabled && !taskStarted_) {
    running_ = true;
    tunnel_.begin();
    if (xTaskCreate(taskEntry, "relay", 12288, this, 1, nullptr) == pdPASS) {
      taskStarted_ = true;
    } else {
      running_ = false;
      setLastError("relay task create failed");
    }
  } else if (!cfg.enabled) {
    running_ = false;  // task loop idles until re-enabled
  } else {
    running_ = true;
  }
}

void RelayClient::taskEntry(void* arg) {
  static_cast<RelayClient*>(arg)->taskLoop();
}

void RelayClient::taskLoop() {
  while (true) {
    if (!running_ || !cfg_.enabled) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (!camera_ || !camera_->isReady()) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    // TLS needs a real clock (see runSession). Poll without touching the
    // reconnect backoff, otherwise boot-time waiting would inflate it to 60 s
    // and delay the first successful connection by a whole minute.
    if (time(nullptr) <= 1704067200) {  // 2024-01-01
      status_.lastErrorMs = millis();
      strncpy(status_.lastError, "waiting for clock sync", sizeof(status_.lastError) - 1);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    runSession();
    // runSession returned: the connection dropped. Back off, then retry.
    wsConnected_ = false;
    viewerActive_ = false;
    status_.viewerActive = false;
    ++status_.reconnects;
    nextAttemptMs_ = millis() + backoffMs_;
    backoffMs_ = (backoffMs_ < kMaxBackoffMs) ? backoffMs_ * 2 : kMaxBackoffMs;
    while (running_ && millis() < nextAttemptMs_) {
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (wsConnected_) backoffMs_ = 2000;
  }
}

void RelayClient::runSession() {
  // Certificate validation is time-based: the ESP32 boots at 1970 and every
  // real server cert has a notBefore after that, so mbedtls rejects the chain
  // with -9984 until SNTP sets the clock. Waiting here is cheaper and far less
  // confusing than surfacing "TLS connect failed" at boot.
  if (time(nullptr) <= 1704067200) {  // 2024-01-01
    setLastError("waiting for clock sync");
    return;
  }

  WiFiClientSecure tls;
  tls.setTimeout(10);  // seconds on the Stream interface
#if defined(RELAY_INSECURE_SKIP_CERT_VERIFY)
  tls.setInsecure();  // DEV ONLY: requires an explicit build flag
#else
  // Production: pin the relay CA. RELAY_CA_CERT comes from secrets.h. Without
  // a pinned cert the device refuses to connect rather than trust anyone.
  if (RELAY_CA_CERT[0]) {
    tls.setCACert(RELAY_CA_CERT);
  } else {
    setLastError("no relay CA cert pinned");
    return;
  }
#endif

  Serial.printf("[relay] connecting to %s:%u\n", cfg_.host, (unsigned)cfg_.port);
  if (!tls.connect(cfg_.host, cfg_.port)) {
    setLastError("TLS connect failed");
    return;
  }
  gActiveTls = &tls;

  if (!wsHandshake(tls, cfg_.host, cfg_.port)) {
    gActiveTls = nullptr;
    return;  // reason already logged
  }

  // Authenticate. The relay closes the socket when the token is wrong.
  char hello[160];
  snprintf(hello, sizeof(hello),
           "{\"type\":\"hello\",\"token\":\"%s\",\"dev\":\"%s\",\"ver\":\"0.8.0\"}",
           cfg_.token, cfg_.deviceId);
  if (!sendText(hello)) {
    setLastError("hello send failed");
    gActiveTls = nullptr;
    return;
  }

  char line[256];
  if (!readServerFrame(line, sizeof(line), 10000)) {
    setLastError("no hello_ok");
    gActiveTls = nullptr;
    return;
  }
  if (!strstr(line, "hello_ok")) {
    setLastError(line[0] ? line : "auth rejected");
    gActiveTls = nullptr;
    return;
  }

  wsConnected_ = true;
  status_.connected = true;
  backoffMs_ = 2000;
  // Clear the stale "waiting for clock sync" / previous failure so the web UI
  // does not show an error next to a healthy connection.
  status_.lastError[0] = 0;
  status_.lastErrorMs = 0;
  Serial.println("[relay] authenticated, idle (look-to-stream)");

  // ---- steady state -------------------------------------------------------
  uint32_t lastRecvMs = millis();
  uint32_t lastFrameMs = 0;
  frameIntervalMs_ = 1000 / (cfg_.fps ? cfg_.fps : 8);

  while (running_ && cfg_.enabled && tls.connected()) {
    // 1. Drain server frames. One loop pass handles everything readable.
    while (tls.available()) {
      lastRecvMs = millis();
      uint8_t h[2];
      if (tls.readBytes(h, 2) != 2) break;
      const bool masked = h[1] & 0x80;
      size_t len = h[1] & 0x7F;
      if (len == 126) {
        uint8_t ext[2];
        if (tls.readBytes(ext, 2) != 2) break;
        len = ((size_t)ext[0] << 8) | ext[1];
      } else if (len == 127) {
        // This protocol never uses 64-bit lengths; treat as malformed so a
        // bad frame cannot make us wait or allocate forever.
        setLastError("ws frame too large");
        tls.stop();
        gActiveTls = nullptr;
        return;
      }
      uint8_t mask[4] = {0};
      if (masked && tls.readBytes(mask, 4) != 4) break;

      // Server->device text frames are tiny JSON commands. Binary frames are
      // only used as the payload half of {"type":"http_data"}: the relay sends
      // the JSON header, then one binary frame with the raw request bytes.
      static uint8_t buf[512];
      static uint8_t binBuf[TunnelClient::kBufSize];
      char text[513];
      const bool isText = (h[0] & 0x0F) == kOpText;
      const bool isBin = (h[0] & 0x0F) == kOpBinary;
      uint8_t* sink = isText ? (uint8_t*)text : (isBin ? binBuf : nullptr);
      const size_t sinkCap = isText ? (sizeof(text) - 1) : (isBin ? sizeof(binBuf) : 0);
      size_t kept = 0;
      size_t got = 0;
      while (got < len) {
        const size_t want = (len - got) < sizeof(buf) ? (len - got) : sizeof(buf);
        const size_t n = tls.readBytes(buf, want);
        if (n == 0) break;
        if (sink && kept < sinkCap) {
          const size_t room = sinkCap - kept;
          const size_t take = (n < room) ? n : room;
          memcpy(sink + kept, buf, take);
          kept += take;
        }
        got += n;
      }
      if (got < len) {
        setLastError("short ws read");
        tls.stop();
        gActiveTls = nullptr;
        return;
      }
      if (isText) text[kept] = 0;

      const uint8_t op = h[0] & 0x0F;
      if (op == kOpPing) {
        // Echo a pong. Empty and unmasked is fine server->client->client side:
        // client->server pongs MUST be masked, which sendFrame does.
        sendFrame(kOpPong, nullptr, 0);
      } else if (op == kOpClose) {
        Serial.println("[relay] server sent close");
        tls.stop();
        gActiveTls = nullptr;
        return;
      } else if (op == kOpText) {
        handleTextCommand(text);
      } else if (op == kOpBinary) {
        // Payload is [0x01][id hi][id lo][raw request bytes]; the tag is ours,
        // not part of the HTTP body, so it must be stripped before forwarding.
        if (pendingTunnelId_ && kept > 3) {
          tunnel_.feed(pendingTunnelId_, binBuf + 3, kept - 3);
          pendingTunnelId_ = 0;
        }
      }
    }

    // 2. Heartbeat watchdog: the relay pings every 30 s; 40 s silent = dead.
    if (millis() - lastRecvMs > kPingTimeoutMs) {
      setLastError("relay heartbeat timeout");
      tls.stop();
      gActiveTls = nullptr;
      return;
    }

    // 3. Push video only while a viewer is watching (look-to-stream).
    if (viewerActive_ && millis() - lastFrameMs >= frameIntervalMs_) {
      camera_fb_t* fb = camera_->acquire(500);
      if (fb) {
        lastFrameMs = millis();
        if (!sendFrameTagged(kTagJpeg, 0, fb->buf, fb->len)) {
          camera_->release(fb);
          setLastError("frame send failed");
          tls.stop();
          gActiveTls = nullptr;
          return;
        }
        ++status_.framesSent;
        status_.bytesSent += fb->len;
        camera_->release(fb);
      }
    }

    // 4. Drain tunnel traffic. This uses the same TLS socket as the frame push
    // above, but only ever from this task, so no lock is required.
    pumpTunnel();

    vTaskDelay(pdMS_TO_TICKS(5));
  }

  tunnel_.closeAll();
  status_.connected = false;
  gActiveTls = nullptr;
  setLastError("connection closed");
}

bool RelayClient::wsHandshake(WiFiClientSecure& tls, const char* host, uint16_t port) {
  char key[25];
  makeWsKey(key);

  char req[512];
  snprintf(req, sizeof(req),
           "GET /devices/%s HTTP/1.1\r\n"
           "Host: %s:%u\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "\r\n",
           cfg_.deviceId, host, (unsigned)port, key);

  if (tls.write((const uint8_t*)req, strlen(req)) != strlen(req)) {
    setLastError("handshake write failed");
    return false;
  }

  // Read the response header block (2 KB is plenty for a relay).
  char resp[2048];
  size_t used = 0;
  const uint32_t deadline = millis() + 10000;
  while (millis() < deadline && used < sizeof(resp) - 1) {
    if (tls.available()) {
      const int c = tls.read();
      if (c < 0) break;
      resp[used++] = (char)c;
      if (used >= 4 && memcmp(resp + used - 4, "\r\n\r\n", 4) == 0) break;
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  resp[used] = 0;

  if (used == 0) {
    setLastError("handshake: no response");
    return false;
  }
  if (!strstr(resp, "101")) {
    setLastError("handshake: not 101");
    Serial.println(resp);
    return false;
  }
  const char* accept = strstr(resp, "Sec-WebSocket-Accept:");
  if (!accept) {
    setLastError("handshake: no accept header");
    return false;
  }
  accept += strlen("Sec-WebSocket-Accept:");
  while (*accept == ' ') ++accept;
  char acceptVal[32] = {0};
  for (size_t i = 0; i < sizeof(acceptVal) - 1 && accept[i] && accept[i] != '\r'; ++i) {
    acceptVal[i] = accept[i];
  }
  if (!checkAccept(key, acceptVal)) {
    setLastError("handshake: accept mismatch");
    return false;
  }
  return true;
}

bool RelayClient::sendText(const char* json) {
  return sendFrame(kOpText, (const uint8_t*)json, strlen(json));
}

// Small helpers for the tunnel commands. The device side only ever needs a
// handful of integers out of a dozen-byte JSON object, so pulling in a parser
// would cost more flash than it saves.
namespace {

// Returns the value of `"key":<int>` or `fallback` when absent.
long jsonInt(const char* json, const char* key, long fallback) {
  const char* p = strstr(json, key);
  if (!p) return fallback;
  p += strlen(key);
  while (*p == ':' || *p == ' ' || *p == '"') ++p;
  if (*p < '0' || *p > '9') return fallback;
  return strtol(p, nullptr, 10);
}

// Copies the value of `"key":"value"` into out. Escapes are not needed here:
// the only string fields are HTTP methods and paths, which the relay encodes
// with percent-escapes instead of JSON escapes.
void jsonStr(const char* json, const char* key, char* out, size_t cap) {
  out[0] = 0;
  const char* p = strstr(json, key);
  if (!p) return;
  p += strlen(key);
  while (*p == ':' || *p == ' ') ++p;
  if (*p != '"') return;
  ++p;
  size_t n = 0;
  while (*p && *p != '"' && n < cap - 1) out[n++] = *p++;
  out[n] = 0;
}

}  // namespace

void RelayClient::handleTextCommand(const char* text) {
  if (strstr(text, "start_stream")) {
    viewerActive_ = true;
    status_.viewerActive = true;  // mirrored into the status snapshot
    Serial.println("[relay] viewer connected: start push");
  } else if (strstr(text, "stop_stream")) {
    viewerActive_ = false;
    status_.viewerActive = false;
    Serial.println("[relay] viewer left: stop push");
  } else if (strstr(text, "http_open")) {
    const uint16_t id = (uint16_t)jsonInt(text, "\"id\"", 0);
    const uint16_t port = (uint16_t)jsonInt(text, "\"port\"", 80);
    const long len = jsonInt(text, "\"len\"", 0);
    char method[8];
    char path[400];
    jsonStr(text, "\"method\"", method, sizeof(method));
    jsonStr(text, "\"path\"", path, sizeof(path));
    if (!id || !method[0] || !path[0]) {
      Serial.println("[relay] bad http_open");
      return;
    }
    if (!tunnel_.open(id, port, method, path, len > 0 ? (size_t)len : 0)) {
      char msg[96];
      snprintf(msg, sizeof(msg), "{\"type\":\"http_open_failed\",\"id\":%u}", (unsigned)id);
      sendText(msg);
    }
  } else if (strstr(text, "http_data")) {
    // The body arrives as the next binary frame, tagged with this channel id.
    pendingTunnelId_ = (uint16_t)jsonInt(text, "\"id\"", 0);
  } else if (strstr(text, "http_end")) {
    tunnel_.finish((uint16_t)jsonInt(text, "\"id\"", 0));
  } else if (strstr(text, "http_close")) {
    tunnel_.close((uint16_t)jsonInt(text, "\"id\"", 0));
  } else if (strstr(text, "ping")) {
    sendText("{\"type\":\"pong\"}");
  } else if (strstr(text, "set_quality")) {
    // {"type":"set_quality","fps":N} -- runtime fps adjust from relay.
    const long v = jsonInt(text, "\"fps\"", 0);
    if (v >= 1 && v <= 15) frameIntervalMs_ = 1000 / v;
  }
}

void RelayClient::pumpTunnel() {
  // Relay -> device: one staged chunk at a time, tagged with its channel id.
  uint16_t finId = 0;
  while (tunnel_.takeFinished(&finId)) {
    char msg[64];
    snprintf(msg, sizeof(msg), "{\"type\":\"http_fin\",\"id\":%u}", (unsigned)finId);
    if (!sendText(msg)) return;  // the session loop will notice on its own
  }

  // Device -> relay: copy whatever the socket task staged. More than one chunk
  // per pass matters for downloads: the loop sleeps 5 ms afterwards, so a
  // single 2 KB chunk per pass would cap throughput at roughly 400 KB/s.
  static uint8_t chunk[TunnelClient::kBufSize];
  for (uint8_t i = 0; i < 8; ++i) {
    uint16_t id = 0;
    bool eof = false;
    const size_t n = tunnel_.pollOut(&id, chunk, sizeof(chunk), &eof);
    if (!n && !eof) return;
    if (n && !sendFrameTagged(kTagTunnel, id, chunk, n)) return;
    if (eof) {
      char msg[64];
      snprintf(msg, sizeof(msg), "{\"type\":\"http_fin\",\"id\":%u}", (unsigned)id);
      if (!sendText(msg)) return;
    }
  }
}

bool RelayClient::readServerFrame(char* out, size_t cap, uint32_t timeoutMs) {
  // Blocking read of one text frame, used for hello_ok before the main loop.
  const uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    if (!gActiveTls || !gActiveTls->connected()) return false;
    if (gActiveTls->available()) {
      uint8_t h[2];
      if (gActiveTls->readBytes(h, 2) != 2) return false;
      const bool masked = h[1] & 0x80;
      size_t len = h[1] & 0x7F;
      if (len == 126) {
        uint8_t ext[2];
        if (gActiveTls->readBytes(ext, 2) != 2) return false;
        len = ((size_t)ext[0] << 8) | ext[1];
      }
      uint8_t mask[4] = {0};
      if (masked && gActiveTls->readBytes(mask, 4) != 4) return false;
      if (len + 1 > cap) len = cap - 1;  // commands are small; truncation is ok
      const size_t n = gActiveTls->readBytes((uint8_t*)out, len);
      if (n != len) return false;
      out[n] = 0;
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return false;
}

void RelayClient::setLastError(const char* what) {
  status_.lastErrorMs = millis();
  status_.connected = wsConnected_;
  snprintf(status_.lastError, sizeof(status_.lastError), "%s", what);
  Serial.printf("[relay] error: %s\n", what);
}
