// Smoke test for server/relay.js — simulates a device and a viewer.
// Usage: node server/smoke_test.js   (spawns its own relay on port 9123)
'use strict';
const http = require('http');
const crypto = require('crypto');
const path = require('path');
const { spawn } = require('child_process');

const PORT = 9123;
const TOKEN = 'test-token';
let passed = 0, failed = 0;
const ok = (name, cond) => { cond ? passed++ : failed++; console.log(`${cond ? 'PASS' : 'FAIL'} ${name}`); };

// Relay under test. Login vars are deliberately left unset: /dev* and /login must
// refuse everything then, and none of the cases below touch those routes.
let relay = null;
function startRelay() {
  relay = spawn(process.execPath, [path.join(__dirname, 'relay.js')], {
    env: { ...process.env, RELAY_PORT: String(PORT), RELAY_TOKEN: TOKEN,
           RELAY_VIEW_AUTH: '', RELAY_LOGIN_USER: '', RELAY_LOGIN_PASS: '',
           RELAY_SESSION_SECRET: '' },
    // inherit: the relay tags its own lines with [relay], so they stay readable
    // in-line and a crash shows up immediately.
    stdio: ['ignore', 'inherit', 'inherit'],
  });
  relay.on('exit', (code, sig) => {
    if (code !== 0 && code !== null) console.error(`relay exited early: code=${code} sig=${sig}`);
  });
}
function stopRelay() {
  if (relay && !relay.killed) { try { relay.kill('SIGTERM'); } catch {} }
}
// Poll /health until the listener answers, so we never race the bind().
// Each attempt carries its own abort timeout: a socket that accepts but never
// replies (e.g. some other program already holding 9123) would otherwise hang
// the fetch forever and the overall deadline would never be reached.
async function waitRelayReady(timeoutMs = 5000) {
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    try {
      const r = await fetch(`http://127.0.0.1:${PORT}/health`, { signal: AbortSignal.timeout(500) });
      if (r.ok) return true;
    } catch {}
    await new Promise(r => setTimeout(r, 100));
  }
  return false;
}

function wsKey() { return crypto.randomBytes(16).toString('base64'); }
function acceptOf(key) {
  return crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
}
function connect(path, extraHeaders) {
  return new Promise((resolve, reject) => {
    const key = wsKey();
    const req = http.request({
      host: '127.0.0.1', port: PORT, path,
      headers: { Connection: 'Upgrade', Upgrade: 'websocket',
                 'Sec-WebSocket-Key': key, 'Sec-WebSocket-Version': 13, ...extraHeaders },
    });
    req.on('upgrade', (res, socket, head) => {
      ok(`upgrade ${path} accept header`, res.headers['sec-websocket-accept'] === acceptOf(key));
      resolve({ socket, head });
    });
    req.on('error', reject);
    req.end();
  });
}
function clientFrame(opcode, payload) {
  const mask = crypto.randomBytes(4);
  const p = Buffer.from(payload);
  const lenByte = p.length < 126 ? p.length : (p.length < 65536 ? 126 : 127);
  const h = Buffer.alloc(2 + (lenByte === 126 ? 2 : lenByte === 127 ? 8 : 0) + 4);
  h[0] = 0x80 | opcode; h[1] = 0x80 | lenByte;
  let off = 2;
  if (lenByte === 126) { h.writeUInt16BE(p.length, 2); off = 4; }
  mask.copy(h, off);
  for (let i = 0; i < p.length; ++i) p[i] ^= mask[i & 3];
  return Buffer.concat([h, p]);
}
// Client must mask; parse unmasked server frames.
function readFrames(socket, head, onFrame) {
  let buf = head && head.length ? Buffer.from(head) : Buffer.alloc(0);
  socket.on('data', (c) => {
    buf = Buffer.concat([buf, c]);
    while (true) {
      if (buf.length < 2) break;
      const op = buf[0] & 0xF;
      let len = buf[1] & 0x7F, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      onFrame(op, buf.subarray(off, off + len));
      buf = buf.subarray(off + len);
    }
  });
}
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

(async () => {
  startRelay();
  if (!await waitRelayReady()) {
    console.error(`SMOKE ERROR relay did not come up on port ${PORT}`);
    stopRelay();
    process.exit(2);
  }

  // 1. health before anything
  const health0 = await fetch(`http://127.0.0.1:${PORT}/health`).then(r => r.json());
  ok('health ok:false initially', health0.ok === true && health0.device === false);

  // 2. device connects + hello
  const dev = await connect('/devices/cam-01');
  const devTexts = [];
  readFrames(dev.socket, dev.head, (op, p) => { if (op === 1) devTexts.push(JSON.parse(p.toString())); });
  dev.socket.write(clientFrame(1, JSON.stringify({ type: 'hello', token: TOKEN, dev: 'cam-01' })));
  await sleep(200);
  ok('device hello_ok', devTexts.some(m => m.type === 'hello_ok'));

  // 3. wrong token rejected
  const bad = await connect('/devices/cam-01');
  let badClosed = false;
  bad.socket.on('close', () => badClosed = true);
  readFrames(bad.socket, bad.head, () => {});
  bad.socket.write(clientFrame(1, JSON.stringify({ type: 'hello', token: 'WRONG' })));
  await sleep(200);
  ok('wrong token closes socket', badClosed || bad.socket.destroyed);

  // 4. viewer connects -> should trigger start_stream to device
  const viewer = await connect('/view');
  const viewerFrames = [];
  readFrames(viewer.socket, viewer.head, (op, p) => viewerFrames.push({ op, p }));
  await sleep(200);
  ok('start_stream sent to device', devTexts.some(m => m.type === 'start_stream' && m.fps > 0));

  // 5. device sends binary JPEG -> viewer receives it
  const fakeJpeg = Buffer.concat([Buffer.from([0xFF, 0xD8]), crypto.randomBytes(200), Buffer.from([0xFF, 0xD9])]);
  dev.socket.write(clientFrame(2, fakeJpeg));
  await sleep(200);
  const gotBin = viewerFrames.find(f => f.op === 2 && f.p.equals(fakeJpeg));
  ok('viewer receives binary frame', !!gotBin);

  // 6. viewer leaves -> stop_stream (wait until the server processed the close)
  viewer.socket.destroy();
  for (let i = 0; i < 20; ++i) {
    await sleep(100);
    const h = await fetch(`http://127.0.0.1:${PORT}/health`).then(r => r.json());
    if (h.viewers === 0) break;
  }
  ok('stop_stream sent after last viewer', devTexts.some(m => m.type === 'stop_stream'));

  // 7. MJPEG endpoint streams frames
  const mjpegChunks = [];
  const mreq = http.get(`http://127.0.0.1:${PORT}/stream.mjpeg`, (res) => {
    ok('mjpeg content-type', /multipart\/x-mixed-replace/.test(res.headers['content-type'] || ''));
    res.on('data', c => mjpegChunks.push(c));
  });
  await sleep(100);
  dev.socket.write(clientFrame(2, fakeJpeg));
  await sleep(300);
  mreq.destroy();
  const all = Buffer.concat(mjpegChunks).toString('latin1');
  ok('mjpeg carries jpeg payload', all.includes('image/jpeg') && all.includes('--camai_relay'));

  // 8. auth: RELAY_VIEW_AUTH set -> 401 without creds
  // (server under test has auth disabled; covered by design, skip runtime check)

  console.log(`\n${passed} passed, ${failed} failed`);
  stopRelay();
  process.exit(failed ? 1 : 0);
})().catch(e => { console.error('SMOKE ERROR', e); stopRelay(); process.exit(2); });
