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
// Run: RELAY_TOKEN=... RELAY_LOGIN_USER=... RELAY_LOGIN_PASS=... \
//      RELAY_SESSION_SECRET=... RELAY_VIEW_AUTH=user:pass node server/relay.js
//
// M9 adds the management tunnel: the browser reaches the device's own LAN page
// through /dev80/... and its MJPEG/replay httpd through /dev81/..., with a
// cookie session enforced here (and again in nginx). See tunnel_*() below.
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

// ---- web login (M9) --------------------------------------------------------
// nginx terminates TLS and asks this server, via auth_request, whether a
// request carries a valid session. Credentials therefore never reach the
// device, which has no authentication of its own at all -- that is the whole
// reason the tunnel exists rather than just exposing port 80.
const LOGIN_USER = process.env.RELAY_LOGIN_USER || '';
const LOGIN_PASS = process.env.RELAY_LOGIN_PASS || '';
const SESSION_SECRET = process.env.RELAY_SESSION_SECRET || '';
const SESSION_TTL_MS = 12 * 60 * 60 * 1000; // 12 h
const COOKIE_NAME = 'camai_sess';

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

// ---- session cookies -------------------------------------------------------
// Stateless signed tokens: "<expires>.<hmac>" so a restart does not log
// everyone out and no session table has to be kept or cleaned up.
function signSession() {
  const exp = Date.now() + SESSION_TTL_MS;
  const mac = crypto.createHmac('sha256', SESSION_SECRET)
    .update(String(exp)).digest('base64url');
  return `${exp}.${mac}`;
}

function verifySession(token) {
  if (!token || !SESSION_SECRET) return false;
  const dot = token.indexOf('.');
  if (dot <= 0) return false;
  const exp = token.slice(0, dot);
  if (!/^\d+$/.test(exp) || Number(exp) < Date.now()) return false;
  const want = crypto.createHmac('sha256', SESSION_SECRET)
    .update(exp).digest('base64url');
  const got = token.slice(dot + 1);
  const a = Buffer.from(got), b = Buffer.from(want);
  return a.length === b.length && crypto.timingSafeEqual(a, b);
}

function parseCookies(header) {
  const out = {};
  if (!header) return out;
  for (const part of header.split(';')) {
    const eq = part.indexOf('=');
    if (eq < 0) continue;
    out[part.slice(0, eq).trim()] = decodeURIComponent(part.slice(eq + 1).trim());
  }
  return out;
}

function hasSession(req) {
  return verifySession(parseCookies(req.headers.cookie)[COOKIE_NAME]);
}

function checkLogin(user, pass) {
  if (!LOGIN_USER || !LOGIN_PASS) return false;
  // Compare fixed-length digests so neither value leaks through timing.
  const a = crypto.createHash('sha256').update(`${user}\0${pass}`).digest();
  const b = crypto.createHash('sha256').update(`${LOGIN_USER}\0${LOGIN_PASS}`).digest();
  return crypto.timingSafeEqual(a, b);
}

// Minimal self-contained login page. It lives here rather than in a static
// file so the page and the cookie logic can never drift apart.
const LOGIN_HTML = `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>登录 · CAM-AI</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body {
    margin: 0; min-height: 100dvh; display: grid; place-items: center;
    background: radial-gradient(circle at 30% 20%, #1b2430, #0b0e12 70%);
    font: 15px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif;
    color: #e8edf3; padding: max(16px, env(safe-area-inset-top)) 16px;
  }
  form {
    width: min(360px, 100%); padding: 28px 24px; border-radius: 16px;
    background: rgba(22, 28, 36, .92); border: 1px solid #2a3441;
    box-shadow: 0 18px 48px rgba(0, 0, 0, .55);
  }
  h1 { margin: 0 0 4px; font-size: 19px; letter-spacing: .02em; }
  p.sub { margin: 0 0 22px; color: #8b99a8; font-size: 13px; }
  label { display: block; margin: 0 0 6px; font-size: 13px; color: #a9b6c4; }
  input {
    width: 100%; margin: 0 0 16px; padding: 11px 13px; border-radius: 9px;
    border: 1px solid #33404f; background: #0e1319; color: #e8edf3;
    font-size: 15px;
  }
  input:focus { outline: 2px solid #2f81f7; outline-offset: -1px; }
  button {
    width: 100%; padding: 12px; border: 0; border-radius: 9px;
    background: #2f81f7; color: #fff; font-size: 15px; font-weight: 600;
    cursor: pointer;
  }
  button:active { background: #2568cc; }
  .err {
    margin: 0 0 16px; padding: 9px 12px; border-radius: 8px;
    background: #3d1d20; border: 1px solid #6b2b30; color: #ff9ba2;
    font-size: 13px;
  }
</style>
</head>
<body>
<form method="POST" action="/login">
  <h1>CAM-AI 监控</h1>
  <p class="sub">请登录后查看与管理摄像头</p>
  <!--ERR-->
  <label for="u">用户名</label>
  <input id="u" name="user" autocomplete="username" required autofocus>
  <label for="p">密码</label>
  <input id="p" name="pass" type="password" autocomplete="current-password" required>
  <button type="submit">登录</button>
</form>
</body>
</html>`;

function loginPage(res, code, msg) {
  const html = LOGIN_HTML.replace('<!--ERR-->',
    msg ? `<div class="err">${msg.replace(/[<>&"]/g, '')}</div>` : '');
  res.writeHead(code, {
    'Content-Type': 'text/html; charset=utf-8',
    'Cache-Control': 'no-store',
    'X-Frame-Options': 'DENY',
    'Referrer-Policy': 'no-referrer',
  });
  res.end(html);
}

function json(res, code, obj) {
  res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
  res.end(JSON.stringify(obj));
}

// ---- tunnel (M9) -----------------------------------------------------------
// The device dials its own loopback httpd on our behalf: the relay names a
// method/path/port, the device opens the socket and pipes raw bytes back. The
// relay therefore never speaks HTTP to the device -- it only multiplexes
// byte streams over the WebSocket it already has. That is what makes the LAN
// page work unmodified in the cloud, with all 15 endpoints intact.
//
// Binary frames from the device carry a one-byte tag:
//   0x00 + JPEG            camera frame (MJPEG over WS)
//   0x01 + id(2, BE) + N   tunnel payload for channel id
// A JPEG starts 0xFF 0xD8, so the two can never be confused.
const tunnels = new Map();   // id -> { req, res, state, ... }
let nextTunnelId = 1;
// Match the firmware's TunnelClient::kMaxChannels, which is in turn limited by
// lwIP's 16-socket pool (each tunnel needs two: ours plus the device httpd's
// accepted socket). Requests beyond the limit wait in tunnelQueue instead of
// failing, because a page load fires four pollers at once.
const TUNNEL_MAX = 4;
const TUNNEL_QUEUE_MAX = 16;
const tunnelQueue = [];

function deviceSendTunnel(id, payload) {
  if (!deviceSock || !deviceSock.authed) return false;
  const frame = Buffer.allocUnsafe(3 + payload.length);
  frame[0] = 0x01;
  frame.writeUInt16BE(id & 0xFFFF, 1);
  payload.copy(frame, 3);
  try { deviceSock.sendBinary(frame); } catch { return false; }
  return true;
}

function tunnelCmd(obj) {
  if (!deviceSock || !deviceSock.authed) return false;
  try { deviceSock.sendText(JSON.stringify(obj)); return true; } catch { return false; }
}

function tunnelFail(t, why) {
  if (!t || t.done) return;
  t.done = true;
  tunnels.delete(t.id);
  if (t.res && !t.res.headersSent) {
    json(t.res, 502, { ok: false, error: why });
  } else if (t.res) {
    try { t.res.end(); } catch {}
  }
  drainTunnelQueue();
}

// The device can only carry TUNNEL_MAX requests at once, but one page load
// fires four pollers in parallel and a real browser adds a few more. Queueing
// briefly is far better than answering 503 to a request that would have
// succeeded a few milliseconds later.
function drainTunnelQueue() {
  while (tunnelQueue.length && tunnels.size < TUNNEL_MAX) {
    const job = tunnelQueue.shift();
    if (job.cancelled) continue;
    startTunnel(job.req, job.res, job.body, job.port);
  }
}

// Rebuild the browser's request as a raw HTTP/1.1 message and hand it to the
// device. Headers that describe the nginx hop are dropped: the device only
// cares about method, path, cookies and content type/length.
function startTunnel(req, res, body, port) {
  if (!deviceSock || !deviceSock.authed) {
    return json(res, 503, { ok: false, error: 'device offline' });
  }
  if (tunnels.size >= TUNNEL_MAX) {
    if (tunnelQueue.length >= TUNNEL_QUEUE_MAX) {
      return json(res, 503, { ok: false, error: 'tunnel busy' });
    }
    const job = { req, res, body, port, cancelled: false };
    tunnelQueue.push(job);
    // A client that gives up while queued must not hold a queue slot.
    res.on('close', () => { job.cancelled = true; });
    return;
  }
  const id = nextTunnelId++;
  if (nextTunnelId > 0xFFFF) nextTunnelId = 1;

  // Path as the device should see it: everything after the /dev80 or /dev81
  // prefix, plus the original query string.
  let path = req.url || '/';
  path = path.replace(/^\/dev(80|81)(?=\/|\?|$)/, '') || '/';

  const t = { id, req, res, done: false, buf: Buffer.alloc(0), headerDone: false };
  tunnels.set(id, t);

  const ok = tunnelCmd({
    type: 'http_open',
    id,
    port,
    method: req.method,
    path,
    len: body ? body.length : 0,
  });
  if (!ok) { tunnelFail(t, 'device offline'); return; }

  // Body (if any) follows as a separate binary frame; the device appends it to
  // the staged request head before writing anything to its socket.
  if (body && body.length) {
    tunnelCmd({ type: 'http_data', id });
    deviceSendTunnel(id, body);
  }
  tunnelCmd({ type: 'http_end', id });

  res.on('close', () => {
    if (!t.done) {
      t.done = true;
      tunnels.delete(id);
      tunnelCmd({ type: 'http_close', id });
      drainTunnelQueue();
    }
  });
}

// Device -> browser: the bytes are a complete HTTP response and are written
// straight to the socket, so status line, headers, chunking and MJPEG bodies
// all reach the browser exactly as the device produced them. res.write() would
// re-frame the body with our own chunked encoding, so the socket is used raw.
function tunnelData(id, payload) {
  const t = tunnels.get(id);
  if (!t || t.done) return;
  const sock = t.res.socket;
  if (!sock || sock.destroyed) { tunnelFail(t, 'socket gone'); return; }
  try {
    sock.write(payload);
  } catch {
    tunnelFail(t, 'write failed');
  }
}

function tunnelFin(id) {
  const t = tunnels.get(id);
  if (!t || t.done) return;
  t.done = true;
  tunnels.delete(id);
  const sock = t.res.socket;
  try { if (sock && !sock.destroyed) sock.end(); } catch {}
  drainTunnelQueue();
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
  ws.tunnelHead = null; // set by {"type":"http_data"} until its binary arrives
  ws.socket.on('data', (chunk) => {
    ws.buf = ws.buf ? Buffer.concat([ws.buf, chunk]) : chunk;
    const { frames, rest, malformed } = wsParse(ws.buf);
    ws.buf = rest;
    ws.lastRecv = Date.now();
    if (malformed) { ws.socket.destroy(); return; }

    for (const f of frames) {
      if (f.opcode === 0x8) { ws.socket.destroy(); return; } // close
      if (f.opcode === 0x9) { ws.socket.write(wsEncode(0xA, f.payload)); continue; } // ping
      if (f.opcode === 0x2) { // binary from an authenticated device
        if (!ws.authed || deviceSock !== ws) continue;
        if (f.payload.length === 0) continue;
        const tag = f.payload[0];
        if (tag === 0x00) {
          fanoutBinary(f.payload.subarray(1));
        } else if (tag === 0xFF) {
          // Firmware from before M9 sends the bare JPEG. 0xFF can only be the
          // start of a JPEG (0xFF 0xD8), so accepting it here keeps a rolling
          // upgrade working: the relay does not need to be deployed in lockstep
          // with the device.
          fanoutBinary(f.payload);
        } else if (tag === 0x01) {
          if (f.payload.length < 3) continue;
          tunnelData(f.payload.readUInt16BE(1), f.payload.subarray(3));
        }
        // Unknown tags are ignored so an older relay can talk to a newer
        // device without dropping the session.
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
        case 'http_fin': tunnelFin(msg.id); break;
        // The device has fewer tunnel channels than this relay accepts. Report
        // that as a real 503 right away instead of letting the browser hang
        // until its own timeout.
        case 'http_open_failed': tunnelFail(tunnels.get(msg.id), 'device busy'); break;
        default: break;     // unknown device messages ignored in v1
      }
    }
  });

  const cleanup = () => {
    if (deviceSock === ws) {
      console.log('[device] disconnected');
      deviceSock = null;
      pushRunning = false;
      // Every in-flight tunnel is downstream of this socket: fail them all so
      // the browser sees an error instead of hanging until its own timeout.
      for (const t of [...tunnels.values()]) tunnelFail(t, 'device disconnected');
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
// Small helper: buffer the request body (bounded, these are all form/JSON
// payloads) and only then build the tunnel. Streaming uploads are not needed
// by any endpoint the device exposes.
const MAX_BODY = 64 * 1024;

function readBody(req, cb) {
  let total = 0;
  const chunks = [];
  req.on('data', (c) => {
    total += c.length;
    if (total > MAX_BODY) { req.destroy(); return; }
    chunks.push(c);
  });
  req.on('end', () => cb(chunks.length ? Buffer.concat(chunks) : null));
  req.on('error', () => cb(null));
}

const server = http.createServer((req, res) => {
  const path = new URL(req.url, 'http://x').pathname;

  if (path === '/health') {
    return json(res, 200, {
      ok: true,
      device: !!(deviceSock && deviceSock.authed),
      viewers: viewerSocks.size + mjpegClients.size,
      push: pushRunning,
      tunnels: tunnels.size,
      uptime_s: Math.floor(process.uptime()),
    });
  }

  // ---- auth endpoints used by nginx auth_request ---------------------------
  // 204 lets nginx continue; 401 makes it serve the login page instead. This
  // endpoint must never be cached and must never leak the reason it failed.
  if (path === '/auth') {
    if (hasSession(req)) { res.writeHead(204); return res.end(); }
    res.writeHead(401, { 'Cache-Control': 'no-store' });
    return res.end('no session');
  }

  if (path === '/login') {
    if (req.method === 'GET') {
      if (hasSession(req)) { res.writeHead(302, { Location: '/' }); return res.end(); }
      return loginPage(res, 200, '');
    }
    if (req.method !== 'POST') { res.writeHead(405); return res.end(); }
    return readBody(req, (body) => {
      const form = new URLSearchParams(body ? body.toString('utf8') : '');
      if (!checkLogin(form.get('user') || '', form.get('pass') || '')) {
        console.log('[auth] login failed');
        return loginPage(res, 401, '用户名或密码错误');
      }
      const cookie = `${COOKIE_NAME}=${signSession()}; Path=/; HttpOnly; Secure; ` +
                     `SameSite=Lax; Max-Age=${Math.floor(SESSION_TTL_MS / 1000)}`;
      console.log('[auth] login ok');
      res.writeHead(302, { 'Set-Cookie': cookie, Location: '/' });
      res.end();
    });
  }

  if (path === '/logout') {
    res.writeHead(302, {
      'Set-Cookie': `${COOKIE_NAME}=; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=0`,
      Location: '/login',
    });
    return res.end();
  }

  if (path === '/stream.mjpeg') {
    if (!hasSession(req) && !checkViewAuth(req)) {
      res.writeHead(401, { 'WWW-Authenticate': 'Basic realm="cam-ai"' });
      return res.end('auth required');
    }
    return handleMjpeg(req, res);
  }

  // Everything under /dev80 reaches the device's control httpd (port 80) and
  // everything under /dev81 reaches its stream/replay httpd (port 81). The
  // prefixes exist so one browser origin can serve both device ports.
  if (path === '/dev80' || path.startsWith('/dev80/') ||
      path === '/dev81' || path.startsWith('/dev81/')) {
    // The tunnel bypasses the device's (nonexistent) auth, so it must never be
    // reachable without a session -- nginx enforces this too, but a second
    // check here means a misconfigured vhost cannot silently expose the camera.
    if (!hasSession(req) && !checkViewAuth(req)) {
      res.writeHead(401, { 'Cache-Control': 'no-store' });
      return res.end('auth required');
    }
    const port = path.startsWith('/dev81') ? 81 : 80;
    if (req.method === 'GET' || req.method === 'HEAD') {
      return startTunnel(req, res, null, port);
    }
    return readBody(req, (body) => startTunnel(req, res, body, port));
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
  if (!LOGIN_USER || !LOGIN_PASS || !SESSION_SECRET) {
    console.log('[relay] WARNING: login not configured (RELAY_LOGIN_USER/PASS/' +
                'SESSION_SECRET) - /login and /dev* will refuse every request');
  }
});
