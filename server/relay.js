// CAM-AI cloud relay (M7) -- minimal, dependency-free Node.js server.
//
// Topology (PROJECT_PLAN.md 5.3):
//   [camera] --outbound WSS/TLS--> [this relay] <--WS/HTTP--> [browser]
//
// The camera dials OUT, so the home network keeps zero open ports. The relay
// is a 1:1 byte pump between the device channel and the viewer channels.
//
// Endpoints:
//   GET /devices/{device_id}  -- WebSocket upgrade; the device connects here
//                                and authenticates with a hello frame carrying
//                                its pre-shared token. Put TLS termination
//                                (Nginx/Caddy) in front in production.
//   GET /view                 -- browser WebSocket; receives device binary
//                                JPEG frames. First viewer triggers
//                                start_stream, last leaving triggers
//                                stop_stream ("look-to-stream").
//   GET /stream.mjpeg         -- multipart/x-mixed-replace MJPEG, works in a
//                                bare <img> tag with zero JS.
//   GET /health               -- liveness + device-connected state.
//
// Run: RELAY_TOKEN=... RELAY_VIEW_AUTH=user:pass node server/relay.js
'use strict';

const http = require('http');
const crypto = require('crypto');

// ---- configuration (env or defaults) --------------------------------------
const PORT = process.env.RELAY_PORT || 9000;
// Pre-shared device token. MUST be overridden in production.
const DEVICE_TOKEN = process.env.RELAY_TOKEN || 'change-me';
// Browser Basic Auth "user:password". Empty disables auth (dev only).
const VIEW_AUTH = process.env.RELAY_VIEW_AUTH || '';
// Device id this single-camera relay serves.
const DEVICE_ID = process.env.RELAY_DEVICE_ID || 'cam-01';
const PING_INTERVAL_MS = 30000; // server pings every 30 s
const PING_TIMEOUT_MS = 40000;  // 40 s silence = dead peer
const DEFAULT_FPS = 8;

// ---- state ----------------------------------------------------------------
let deviceSock = null;       // AUTHENTICATED device WebSocket (or null)
let viewerSocks = new Set(); // active viewer WebSockets
let mjpegClients = new Set();// active MJPEG HTTP responses
let pushRunning = false;     // start_stream currently in effect

// ---- minimal WebSocket implementation (RFC 6455) ---------------------------
// Small JSON commands + binary JPEG payloads. No fragmentation, no
// extensions, no permessage-deflate. Client->server frames are always masked
// by clients; server->client frames are unmasked per spec.

function wsAccept(key) {
  return crypto.createHash('sha1')
    .update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
    .digest('base64');
}

function wsEncode(opcode, payload) {
  const len = payload.length;
  let header;
  if (len < 126) {
    header = Buffer.alloc(2); header[1] = len;
  } else if (len < 65536) {
    header = Buffer.alloc(4); header[1] = 126; header.writeUInt16BE(len, 2);
  } else {
    header = Buffer.alloc(10); header[1] = 127; header.writeBigUInt64BE(BigInt(len), 2);
  }
  header[0] = 0x80 | opcode; // FIN + opcode
  return Buffer.concat([header, payload]);
}

// Parse all complete frames from a buffer. Returns {frames, rest, malformed}.
function wsParse(buf) {
  const frames = [];
  let off = 0;
  while (off + 2 <= buf.length) {
    const opcode = buf[off] & 0x0F;
    const masked = (buf[off + 1] & 0x80) !== 0;
    let len = buf[off + 1] & 0x7F;
    let pos = off + 2;
    if (len === 126) {
      if (pos + 2 > buf.length) break;
      len = buf.readUInt16BE(pos); pos += 2;
    } else if (len === 127) {
      if (pos + 8 > buf.length) break;
      const big = buf.readBigUInt64BE(pos); pos += 8;
      if (big > 16n * 1024n * 1024n) return { frames, rest: Buffer.alloc(0), malformed: true };
      len = Number(big);
    }
    const maskLen = masked ? 4 : 0;
    if (pos + maskLen + len > buf.length) break; // incomplete frame, wait for more
    let payload = buf.subarray(pos + maskLen, pos + maskLen + len);
    if (masked) {
      const mask = buf.subarray(pos, pos + 4);
      payload = Buffer.from(payload); // copy before unmasking in place
      for (let i = 0; i < payload.length; ++i) payload[i] ^= mask[i & 3];
    }
    frames.push({ opcode, payload });
    off = pos + maskLen + len;
  }
  return { frames, rest: buf.subarray(off) };
}

function makeWs(socket, head) {
  const ws = {
    socket,
    buf: head && head.length ? Buffer.from(head) : null,
    authed: false,
    lastRecv: Date.now(),
    sendText: (s) => socket.write(wsEncode(0x1, Buffer.from(s))),
    sendBinary: (b) => socket.write(wsEncode(0x2, b)),
  };
  return ws;
}

function checkViewAuth(req) {
  if (!VIEW_AUTH) return true; // auth disabled (dev only)
  const hdr = req.headers.authorization || '';
  const expected = 'Basic ' + Buffer.from(VIEW_AUTH).toString('base64');
  const a = Buffer.from(hdr), b = Buffer.from(expected);
  return a.length === b.length && crypto.timingSafeEqual(a, b);
}

function json(res, code, obj) {
  res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
  res.end(JSON.stringify(obj));
}

// ---- frame fan-out ---------------------------------------------------------
function fanoutBinary(jpeg) {
  for (const v of viewerSocks) { try { v.sendBinary(jpeg); } catch {} }
  for (const r of mjpegClients) {
    try {
      r.write(`Content-Type: image/jpeg\r\nContent-Length: ${jpeg.length}\r\n\r\n`);
      r.write(jpeg);
      r.write('\r\n--camai_relay\r\n');
    } catch {}
  }
}

function startPushIfViewers() {
  if (pushRunning || viewerSocks.size + mjpegClients.size === 0) return;
  if (!deviceSock || !deviceSock.authed) return;
  pushRunning = true;
  deviceSock.sendText(JSON.stringify({ type: 'start_stream', fps: DEFAULT_FPS }));
  console.log('[relay] push started');
}

function stopPushIfNoViewers() {
  if (!pushRunning) return;
  if (viewerSocks.size + mjpegClients.size > 0) return;
  pushRunning = false;
  if (deviceSock && deviceSock.authed) {
    deviceSock.sendText(JSON.stringify({ type: 'stop_stream' }));
    console.log('[relay] push stopped');
  }
}

// ---- device channel --------------------------------------------------------
function handleDevice(ws, deviceId) {
  ws.socket.on('data', (chunk) => {
    ws.buf = ws.buf ? Buffer.concat([ws.buf, chunk]) : chunk;
    const { frames, rest, malformed } = wsParse(ws.buf);
    ws.buf = rest;
    ws.lastRecv = Date.now();
    if (malformed) { ws.socket.destroy(); return; }

    for (const f of frames) {
      if (f.opcode === 0x8) { ws.socket.destroy(); return; } // close
      if (f.opcode === 0x9) { ws.socket.write(wsEncode(0xA, f.payload)); continue; } // ping
      if (f.opcode === 0x2) { // binary JPEG from an authenticated device
        if (ws.authed && deviceSock === ws) fanoutBinary(f.payload);
        continue;
      }
      if (f.opcode !== 0x1) continue; // only text remains

      let msg;
      try { msg = JSON.parse(f.payload.toString('utf8')); } catch { continue; }

      if (!ws.authed) {
        if (msg.type === 'hello' && msg.token === DEVICE_TOKEN &&
            (!msg.dev || msg.dev === DEVICE_ID)) {
          ws.authed = true;
          // Only NOW does this socket become the device channel. A failed
          // handshake must never stomp an established connection.
          if (deviceSock && deviceSock !== ws && !deviceSock.socket.destroyed) {
            console.log('[device] replacing stale connection');
            deviceSock.socket.destroy();
          }
          deviceSock = ws;
          console.log(`[device] authenticated dev=${msg.dev || deviceId}`);
          ws.sendText(JSON.stringify({ type: 'hello_ok', mode: 'push' }));
          startPushIfViewers(); // viewers may already be waiting
        } else {
          console.log('[device] auth rejected, closing');
          try { ws.sendText(JSON.stringify({ type: 'error', what: 'auth' })); } catch {}
          ws.socket.destroy();
        }
        continue;
      }
      switch (msg.type) {
        case 'pong': break; // heartbeat answer
        default: break;     // unknown device messages ignored in v1
      }
    }
  });

  const cleanup = () => {
    if (deviceSock === ws) {
      console.log('[device] disconnected');
      deviceSock = null;
      pushRunning = false;
      for (const v of viewerSocks) {
        try { v.sendText(JSON.stringify({ type: 'device_lost' })); } catch {}
      }
    }
  };
  // On Windows/Node an abruptly destroyed upgraded socket emits 'end' (FIN)
  // but NOT 'close' in some sequences; handle both or the device slot leaks.
  ws.socket.on('close', cleanup);
  ws.socket.on('end', cleanup);
  ws.socket.on('error', cleanup);
}

// ---- viewer channels -------------------------------------------------------
function addViewerHook() {
  // Look-to-stream: first viewer starts the push.
  startPushIfViewers();
}
function removeViewerHook() {
  // Last viewer leaving stops the push.
  stopPushIfNoViewers();
}

function handleViewer(ws) {
  viewerSocks.add(ws);
  console.log(`[viewer] ws connected (${viewerSocks.size} active)`);
  addViewerHook();

  ws.socket.on('data', (chunk) => {
    ws.buf = ws.buf ? Buffer.concat([ws.buf, chunk]) : chunk;
    const { frames, rest, malformed } = wsParse(ws.buf);
    ws.buf = rest;
    ws.lastRecv = Date.now();
    if (malformed) { ws.socket.destroy(); return; }
    for (const f of frames) {
      if (f.opcode === 0x8) { ws.socket.destroy(); return; }
      if (f.opcode === 0x9) ws.socket.write(wsEncode(0xA, f.payload));
      // Viewer commands (fps adjust etc.) reserved for v2.
    }
  });

  const cleanup = () => {
    viewerSocks.delete(ws);
    console.log(`[viewer] ws gone (${viewerSocks.size} active)`);
    removeViewerHook();
  };
  // 'end' as well as 'close': see the note in handleDevice.
  ws.socket.on('close', cleanup);
  ws.socket.on('end', cleanup);
  ws.socket.on('error', cleanup);
}

function handleMjpeg(req, res) {
  res.writeHead(200, {
    'Content-Type': 'multipart/x-mixed-replace; boundary=camai_relay',
    'Cache-Control': 'no-store',
    'Connection': 'close',
  });
  res.write('--camai_relay\r\n');
  mjpegClients.add(res);
  console.log(`[viewer] mjpeg connected (${mjpegClients.size} active)`);
  addViewerHook();
  res.on('close', () => {
    mjpegClients.delete(res);
    console.log(`[viewer] mjpeg gone (${mjpegClients.size} active)`);
    removeViewerHook();
  });
}

// ---- HTTP server -----------------------------------------------------------
const server = http.createServer((req, res) => {
  const path = new URL(req.url, 'http://x').pathname;

  if (path === '/health') {
    return json(res, 200, {
      ok: true,
      device: !!(deviceSock && deviceSock.authed),
      viewers: viewerSocks.size + mjpegClients.size,
      push: pushRunning,
      uptime_s: Math.floor(process.uptime()),
    });
  }

  if (path === '/stream.mjpeg') {
    if (!checkViewAuth(req)) {
      res.writeHead(401, { 'WWW-Authenticate': 'Basic realm="cam-ai"' });
      return res.end('auth required');
    }
    return handleMjpeg(req, res);
  }

  res.writeHead(404);
  res.end('not found');
});

// Node routes Upgrade requests here; the createServer handler never sees them.
server.on('upgrade', (req, socket, head) => {
  const path = new URL(req.url, 'http://x').pathname;
  const key = req.headers['sec-websocket-key'];
  if (!key) { socket.destroy(); return; }

  const devMatch = path.match(/^\/devices\/([A-Za-z0-9_-]+)$/);
  const isView = path === '/view';

  // Viewer WS endpoints require Basic auth when configured; the device
  // endpoint authenticates itself via the hello token instead.
  if (isView && !checkViewAuth(req)) {
    socket.write('HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm="cam-ai"\r\n\r\n');
    return socket.destroy();
  }
  if (!devMatch && !isView) { socket.destroy(); return; }

  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
    'Upgrade: websocket\r\n' +
    'Connection: Upgrade\r\n' +
    `Sec-WebSocket-Accept: ${wsAccept(key)}\r\n` +
    '\r\n');

  const ws = makeWs(socket, head);
  if (devMatch) handleDevice(ws, devMatch[1]);
  else handleViewer(ws);
});

// ---- heartbeat -------------------------------------------------------------
setInterval(() => {
  const now = Date.now();
  if (deviceSock) {
    deviceSock.sendText(JSON.stringify({ type: 'ping' }));
    if (now - deviceSock.lastRecv > PING_TIMEOUT_MS) {
      console.log('[device] heartbeat timeout, closing');
      deviceSock.socket.destroy();
    }
  }
  for (const v of [...viewerSocks]) {
    if (now - v.lastRecv > PING_TIMEOUT_MS) v.socket.destroy();
  }
}, PING_INTERVAL_MS);

server.listen(PORT, () => {
  console.log(`[relay] listening on :${PORT} (device token ${DEVICE_TOKEN === 'change-me' ? 'DEFAULT - INSECURE' : 'set'})`);
  if (!VIEW_AUTH) console.log('[relay] WARNING: no viewer auth (RELAY_VIEW_AUTH unset) - dev only');
});
