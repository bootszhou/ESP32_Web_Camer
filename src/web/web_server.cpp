#include "web_server.h"

#include <esp_timer.h>
#include <SD_MMC.h>
#include <sys/socket.h>   // setsockopt / SO_SNDTIMEO for the stream socket

#include "network/relay_client.h"

namespace {

constexpr const char* kBoundary = "camai_frame_boundary";
constexpr const char* kStreamContentType =
    "multipart/x-mixed-replace; boundary=camai_frame_boundary";

// Only one viewer at a time for now. Each extra stream multiplies the WiFi
// airtime cost, and a single camera pipeline cannot serve a crowd.
constexpr uint32_t kMaxStreamClients = 1;

// The MJPEG stream lives on its own httpd instance (own task), see
// web_server.h. The page and API stay on port 80.
constexpr uint16_t kStreamPort = 81;

// Playback is independent of the live stream. Normally just one replay runs,
// but switching clips mid-play briefly overlaps the outgoing connection with
// the incoming one until the old loop notices its token was superseded. Allow
// a small headroom for that transition; token preemption (not this counter)
// is what guarantees only one replay actually streams at a time.
constexpr uint32_t kMaxPlaybackClients = 4;

// Pause between streamed frames.
//
// The stream runs on its own httpd instance now, so this yield is no longer
// what keeps the UI alive. It now serves the other camera consumers: with the
// loop re-acquiring immediately, the stream could hold all frame buffers and
// the motion detector's acquire() in the main loop would starve (its readings
// froze whenever a viewer was connected). 40 ms caps the stream near 20 fps --
// well past what WiFi delivers for 15 KB frames -- and leaves the detector's
// 250 ms cadence and the SD recorder plenty of turns.
constexpr uint32_t kStreamYieldMs = 40;

uint32_t gLastStreamFrameMs = 0;
uint32_t gStreamFrameCount = 0;

// Live progress of the active replay, published by handlePlayback and read by
// /api/v1/playback/status. Most fields are written by the single stream task
// and read by an API task, so make them volatile. gPlaybackToken is ALSO
// written by the port-80 /playback/cancel handler: bumping it mid-replay is how
// the browser tells an in-flight stream to stop so a newly selected clip can
// claim the single-task port-81 server immediately. token ties a reading to
// one particular playClip() request so a late client cannot observe a new
// replay's progress under an old request's identity.
volatile uint32_t gPlaybackToken = 0;
volatile bool gPlaybackActive = false;
// Set by /playback/cancel when it bumps the token: the UI has asked for a new
// clip but that clip's handler has not published a snapshot yet. Without this
// flag the status endpoint reports the freshly bumped token with active=false,
// which the browser cannot tell apart from "the clip finished", so it tore the
// progress bar down in the gap between the cancel POST and the new handler.
volatile bool gPlaybackPending = false;
volatile uint32_t gPlaybackFramesDone = 0;
volatile uint32_t gPlaybackFramesTotal = 0;
volatile uint32_t gPlaybackStartMs = 0;
volatile uint32_t gPlaybackDurationMs = 0;

const char* kIndexHtml = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CAM-AI 监控</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body {
    margin: 0; font-family: system-ui, -apple-system, "Segoe UI", sans-serif;
    background: #0d1117; color: #e6edf3;
    display: flex; flex-direction: column; min-height: 100vh;
  }
  header {
    padding: 14px 18px; background: #161b22; border-bottom: 1px solid #30363d;
    display: flex; align-items: center; gap: 10px; flex-wrap: wrap;
  }
  header h1 { font-size: 16px; margin: 0; font-weight: 600; }
  .badge {
    font-size: 12px; padding: 3px 9px; border-radius: 999px;
    background: #21262d; border: 1px solid #30363d; color: #8b949e;
  }
  .badge.ok { background: #1a3a24; border-color: #2ea043; color: #7ee787; }
  .badge.warn { background: #3a2d1a; border-color: #d29922; color: #e3b341; }
  .badge.rec { background: #4a1d1d; border-color: #f85149; color: #ff7b72; }
  main { flex: 1; padding: 16px; display: flex; flex-direction: column; gap: 14px; }
  .viewer {
    position: relative; background: #000; border: 1px solid #30363d;
    border-radius: 10px; overflow: hidden; aspect-ratio: 4 / 3; max-width: 960px;
    width: 100%; margin: 0 auto;
  }
  .viewer img { width: 100%; height: 100%; object-fit: contain; display: block; }
  .overlay {
    position: absolute; inset: 0; display: flex; align-items: center;
    justify-content: center; flex-direction: column; gap: 10px;
    background: rgba(13,17,23,.92); text-align: center; padding: 20px;
  }
  .overlay.hidden { display: none; }
  .pbtrack {
    flex: 1; min-width: 90px; height: 8px; background: #21262d;
    border: 1px solid #30363d; border-radius: 5px; overflow: hidden;
  }
  .pbfill { height: 100%; width: 0%; background: #d29922; transition: width .2s linear; }
  .btn {
    background: #238636; border: 1px solid #2ea043; color: #fff;
    padding: 9px 18px; border-radius: 7px; font-size: 14px; cursor: pointer;
  }
  .btn:hover { background: #2ea043; }
  .btn.danger { background: #6e1f1f; border-color: #a33; }
  .btn.danger:hover { background: #8a2626; }
  .stats {
    display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr));
    gap: 10px; max-width: 960px; width: 100%; margin: 0 auto;
  }
  .stat {
    background: #161b22; border: 1px solid #30363d; border-radius: 8px;
    padding: 10px 12px;
  }
  .stat .k { font-size: 11px; color: #8b949e; text-transform: uppercase; letter-spacing: .04em; }
  .stat .v { font-size: 17px; margin-top: 3px; font-variant-numeric: tabular-nums; }
  h2 { font-size: 14px; margin: 6px auto 0; max-width: 960px; width: 100%; color: #8b949e; font-weight: 600; }
  .events { max-width: 960px; width: 100%; margin: 0 auto; display: flex; flex-direction: column; gap: 8px; }
  .event {
    display: flex; align-items: center; gap: 10px; flex-wrap: wrap;
    background: #161b22; border: 1px solid #30363d; border-radius: 8px;
    padding: 10px 12px; font-size: 13px;
  }
  .event .name { flex: 1; min-width: 180px; font-variant-numeric: tabular-nums; }
  .event .meta { color: #8b949e; font-size: 12px; }
  .event a {
    color: #58a6ff; text-decoration: none; border: 1px solid #30363d;
    padding: 4px 10px; border-radius: 6px; font-size: 12px;
  }
  .event a:hover { background: #21262d; }
  .empty { color: #6e7681; font-size: 13px; padding: 12px; text-align: center; }
  .bulkbar {
    max-width: 960px; width: 100%; margin: 0 auto;
    display: flex; align-items: center; gap: 12px;
    background: #161b22; border: 1px solid #30363d; border-radius: 8px;
    padding: 8px 12px; font-size: 13px;
  }
  .bulkbar .chk { display: flex; align-items: center; gap: 6px; cursor: pointer; }
  .bulkbar input[type=checkbox] { width: 15px; height: 15px; accent-color: #238636; }
  .event input[type=checkbox] { width: 15px; height: 15px; accent-color: #238636; flex-shrink: 0; }
  .btn:disabled { opacity: .45; cursor: not-allowed; }
  .panel {
    max-width: 960px; width: 100%; margin: 0 auto;
    background: #161b22; border: 1px solid #30363d; border-radius: 8px;
    padding: 14px 16px; display: flex; flex-direction: column; gap: 10px;
  }
  .panel .row {
    display: flex; align-items: center; gap: 12px; font-size: 13px;
  }
  .panel .row label { width: 150px; flex-shrink: 0; color: #c9d1d9; }
  .panel .hint { color: #6e7681; font-size: 11px; font-weight: 400; }
  .panel .row input[type=range] { flex: 1; accent-color: #238636; }
  .panel .row input[type=checkbox] { width: 16px; height: 16px; accent-color: #238636; }
  .panel .val {
    width: 52px; text-align: right; font-variant-numeric: tabular-nums;
    color: #7ee787;
  }
  .panel .actions {
    display: flex; gap: 10px; align-items: center; flex-wrap: wrap;
    padding-top: 6px; border-top: 1px solid #21262d;
  }
  footer { padding: 10px 18px; font-size: 12px; color: #6e7681; text-align: center; }
  @media (max-width: 520px) { main { padding: 10px; } .stat .v { font-size: 15px; } }
</style>
</head>
<body>
<header>
  <h1>CAM-AI 网络监控</h1>
  <span class="badge" id="netbadge">连接中</span>
  <span class="badge" id="fpsbadge">-- fps</span>
  <span class="badge" id="recbadge" style="display:none">录像中</span>
  <!-- Always visible. Previously this appeared only while recording, and since
       clips are short and the status poll is 2 s, it flickered in and out and
       often could not be clicked at all. A disabled button states "not
       recording" without the control vanishing. -->
  <button class="btn danger" id="stopBtn" disabled
          style="font-size:12px;padding:4px 12px">停止录像</button>
</header>
<main>
  <div class="viewer">
    <img id="cam" alt="实时画面">
    <div class="overlay" id="overlay">
      <div id="overlayText" style="color:#8b949e">点击开始观看</div>
      <button class="btn" id="startBtn">开始直播</button>
    </div>
  </div>
  <div id="playbackBar"
       style="display:none;max-width:960px;width:100%;margin:6px auto 0;align-items:center;gap:10px;color:#d29922;font-size:13px">
    回放中：<span id="playbackName"></span>
    <div class="pbtrack"><div class="pbfill" id="pbFill"></div></div>
    <span id="pbTime" style="font-variant-numeric:tabular-nums;min-width:86px;text-align:right"></span>
    <button class="btn" id="backLiveBtn" style="font-size:12px;padding:3px 10px">返回直播</button>
  </div>
  <div id="liveBar"
       style="display:none;max-width:960px;width:100%;margin:6px auto 0;align-items:center;gap:10px;color:#3fb950;font-size:13px">
    直播中
    <button class="btn" id="stopStreamBtn" style="font-size:12px;padding:3px 10px">停止直播</button>
  </div>

  <div class="stats">
    <div class="stat"><div class="k">分辨率</div><div class="v" id="res">--</div></div>
    <div class="stat"><div class="k">实测帧率</div><div class="v" id="fps">--</div></div>
    <div class="stat"><div class="k">信号强度</div><div class="v" id="rssi">--</div></div>
    <div class="stat"><div class="k">运行时间</div><div class="v" id="uptime">--</div></div>
    <div class="stat"><div class="k">空闲内存</div><div class="v" id="heap">--</div></div>
    <div class="stat"><div class="k">移动检测</div><div class="v" id="motionv">--</div></div>
    <div class="stat"><div class="k">录像文件</div><div class="v" id="clips">--</div></div>
    <div class="stat"><div class="k">SD 剩余</div><div class="v" id="sd">--</div></div>
  </div>

  <h2>检测器视角（80×60 灰度，用于调参）</h2>
  <div class="stats">
    <div class="stat">
      <img id="motionimg" alt="detector view"
           style="width:100%;image-rendering:pixelated;border-radius:6px">
    </div>
  </div>

  <h2>录像事件 <span id="evcount" class="meta" style="font-weight:400"></span></h2>
  <div class="bulkbar">
    <label class="chk">
      <input type="checkbox" id="selAll"> 全选本页
    </label>
    <span class="meta" id="selInfo" style="font-size:12px;color:#8b949e"></span>
    <button class="btn danger" id="delSelBtn" disabled>删除所选</button>
  </div>
  <div class="events" id="events">
    <div class="empty">暂无录像</div>
  </div>
  <div class="actions" style="max-width:960px;width:100%;margin:0 auto;display:flex;gap:10px;align-items:center">
    <button class="btn" id="prevBtn">上一页</button>
    <span class="meta" id="pageInfo" style="color:#8b949e;font-size:12px"></span>
    <button class="btn" id="nextBtn">下一页</button>
    <span id="eventMsg" class="meta" style="font-size:12px"></span>
  </div>

  <h2>设置</h2>
  <div class="panel">
    <div class="row"><label for="s_sens">灵敏度</label>
      <input type="range" id="s_sens" min="1" max="100" value="65">
      <span class="val" id="v_sens">65</span></div>
    <div class="row"><label for="s_minblk">最小变化区块<br><small class="hint">共 70 块，建议 3–8</small></label>
      <input type="range" id="s_minblk" min="1" max="20" value="3">
      <span class="val" id="v_minblk">3</span></div>
    <div class="row"><label for="s_confirm">连续确认次数</label>
      <input type="range" id="s_confirm" min="1" max="5" value="2">
      <span class="val" id="v_confirm">2</span></div>
    <div class="row"><label for="s_cool">冷却时间 (秒)</label>
      <input type="range" id="s_cool" min="1" max="600" value="30">
      <span class="val" id="v_cool">30</span></div>
    <div class="row"><label for="s_clip">单段录像 (秒)</label>
      <input type="range" id="s_clip" min="1" max="120" value="10">
      <span class="val" id="v_clip">10</span></div>
    <div class="row"><label for="s_jpegq">JPEG 质量</label>
      <input type="range" id="s_jpegq" min="6" max="30" value="12">
      <span class="val" id="v_jpegq">12</span></div>
    <div class="row"><label for="s_minfree">最小剩余 (MB)</label>
      <input type="range" id="s_minfree" min="10" max="2000" value="200" step="10">
      <span class="val" id="v_minfree">200</span></div>
    <div class="row"><label for="s_recen">检测到移动时保存录像<br>
      <small class="hint">关闭后只检测移动，不录像</small></label>
      <input type="checkbox" id="s_recen" checked></div>
    <div class="row"><label for="s_extend">运动时延长录像</label>
      <input type="checkbox" id="s_extend"></div>
    <div class="row"><label for="s_autoclean">空间不足时自动清理</label>
      <input type="checkbox" id="s_autoclean"></div>
    <div class="row"><label for="s_vflip">画面上下翻转</label>
      <input type="checkbox" id="s_vflip" checked></div>
    <div class="row"><label for="s_hflip">画面左右翻转 (镜像)</label>
      <small class="hint">默认开启：校正传感器镜像；文字应为正向可读</small>
      <input type="checkbox" id="s_hflip" checked></div>
    <div class="actions">
      <button class="btn" id="saveBtn">保存设置</button>
      <button class="btn danger" id="cleanBtn">立即清理空间</button>
      <span id="saveMsg" class="meta"></span>
    </div>
  </div>

  <h2>WiFi 网络 <span id="wifistate" class="meta" style="font-weight:400"></span></h2>
  <div class="panel">
    <div class="row"><label>当前网络</label>
      <span class="val" id="wifissid" style="width:auto;text-align:left">--</span></div>
    <div class="row" id="apRow" style="display:none"><label>配网热点</label>
      <span class="val" id="apinfo" style="width:auto;text-align:left;color:#e3b341"></span></div>
    <div class="row"><label for="w_ssid">WiFi 名称 (SSID)</label>
      <input type="text" id="w_ssid" placeholder="路由器名称"
             style="flex:1;min-width:160px;background:#0d1117;border:1px solid #30363d;
                    border-radius:6px;color:#e6edf3;padding:6px 9px;font-size:13px"></div>
    <div class="row"><label for="w_pass">WiFi 密码</label>
      <input type="password" id="w_pass" placeholder="留空表示不修改"
             style="flex:1;min-width:160px;background:#0d1117;border:1px solid #30363d;
                    border-radius:6px;color:#e6edf3;padding:6px 9px;font-size:13px"></div>
    <div class="row"><label>如何配网<br>
      <small class="hint">长按配网键 3 秒开热点，手机连 CAM-AI-XXXX 后打开 192.168.4.1</small></label>
      <span class="meta" id="provTip" style="font-size:11px;color:#6e7681">
        当前密码已保存：<span id="haspass">--</span></span></div>
    <div class="actions">
      <button class="btn" id="wifiSaveBtn">保存并重连</button>
      <span id="wifiMsg" class="meta"></span>
    </div>
  </div>
</main>
<footer id="footer">CAM-AI · ESP32-S3</footer>

<script>
const cam = document.getElementById('cam');
const overlay = document.getElementById('overlay');
const startBtn = document.getElementById('startBtn');
const overlayText = document.getElementById('overlayText');
let streaming = false;
let streamStaleCount = 0;  // consecutive polls with a stalled stream frame age

// 1x1 transparent GIF used to park the <img> when no video is wanted.
// Assigning src='' instead would make the browser re-request the page itself
// as an image; the resulting error event then popped the "画面中断" overlay
// over a perfectly good stream that was opened one line later.
const BLANK_PIXEL =
  'data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7';

// Video and replay live on the second httpd (port 81) so that an endless MJPEG
// response can never starve the API task on port 80. On the LAN we therefore
// point straight at :81. When this page is served through the cloud relay the
// tunnel exposes that port under /dev/, and mixed-content rules forbid
// http://... from an https:// page, so the base has to be injectable.
const STREAM_BASE = window.__CAMAI_STREAM_BASE ||
  ('http://' + location.hostname + ':81');

function detachCam() {
  cam.src = BLANK_PIXEL;
}

// Asks the device for the token to use for the next replay, and returns a
// promise for it. The token MUST come from the device: it is the device that
// compares tokens to decide whether a queued /playback request is stale, so
// only a device-issued number is guaranteed to be newer than the last one it
// saw. A page-minted counter restarts at 0 on reload and gets rejected.
//
// The POST also bumps the device token, which is exactly what frees a busy
// port-81 task for the request that follows.
function requestPlayToken() {
  return fetch('/api/v1/playback/cancel', { method: 'POST', cache: 'no-store' })
    .then(r => r.json())
    .then(d => (d && d.token) ? d.token : 0)
    .catch(() => 0);  // 0 means "tokenless": the device still serves the request
}

async function startStream() {
  // The MJPEG stream is served by a dedicated httpd instance on port 81 so
  // that the page and API on port 80 stay responsive while it runs.
  // streaming flips first so a late error event from the connection being
  // replaced cannot be mistaken for a failure of the new one.
  // Leaving any replay behind: invalidate its token and polling so a late
  // playback status response cannot put the idle overlay back over the stream.
  stopPlaybackPolling();
  const gen = ++playbackToken;
  // Wait for the reservation before touching cam.src. Requests this page fires
  // at the same moment are otherwise ordered by the network, and a cancel that
  // landed after the stream got one would bump the device token while the
  // stream was running.
  const token = await requestPlayToken();
  if (gen !== playbackToken) return;  // a replay was picked while waiting
  setProgressBar(0, 0, 0);
  streaming = true;
  document.getElementById('playbackBar').style.display = 'none';
  document.getElementById('liveBar').style.display = 'flex';
  cam.src = STREAM_BASE + '/stream?t=' + Date.now();
  overlay.classList.add('hidden');
}

function stopStream() {
  streaming = false;
  detachCam();  // closes the /stream connection on port 81
  document.getElementById('liveBar').style.display = 'none';
  overlayText.textContent = '点击开始观看';
  overlay.classList.remove('hidden');
}

// ---- playback state -------------------------------------------------------
// The device serves an AVI clip as an MJPEG stream on port 81. The browser
// cannot know how many frames an MJPEG <img> has decoded, so progress comes
// from a lightweight /playback/status poll on port 80. Each replay gets a
// unique token; a stale poll response for an old token is ignored, which is
// what makes switching clips mid-playback take effect immediately instead of
// being overwritten by an in-flight response from the previous clip.
// Page-local generation guard. Only decides which pending continuation in
// this tab is still current; it is NOT sent to the device. The device issues
// its own playback token (see requestPlayToken) because a page-local counter
// restarted at 0 on every reload while the device kept counting, so a fresh
// page's tokens were always OLDER than the device's and handlePlayback()
// rejected every replay as "superseded" (409) — playback stopped working.
let playbackToken = 0;
let playbackTimer = null;
let pbPollCount = 0;

function setProgressBar(pct, elapsedMs, durationMs) {
  const fill = document.getElementById('pbFill');
  const time = document.getElementById('pbTime');
  if (!fill || !time) return;
  if (durationMs > 0) {
    const capped = Math.max(0, Math.min(100, pct));
    fill.style.width = capped + '%';
    time.textContent = fmtTime(elapsedMs) + ' / ' + fmtTime(durationMs);
  } else {
    fill.style.width = '0%';
    time.textContent = '';
  }
}

function fmtTime(ms) {
  const s = Math.max(0, Math.floor(ms / 1000));
  return String(Math.floor(s / 60)).padStart(2, '0') + ':' +
         String(s % 60).padStart(2, '0');
}

// Called when a replay finishes naturally: tear down the stream and return
// the view to the idle "开始直播" overlay.
function playbackFinished() {
  stopPlaybackPolling();
  playbackToken++;
  streaming = false;
  detachCam();
  document.getElementById('playbackBar').style.display = 'none';
  setProgressBar(0, 0, 0);
  overlayText.textContent = '点击开始观看';
  overlay.classList.remove('hidden');
}

async function pollPlayback(token, gen) {
  if (gen !== playbackToken) return;  // a newer clip / live view took over
  try {
    const r = await fetch('/api/v1/playback/status?t=' + Date.now(),
                          { cache: 'no-store' });
    const d = await r.json();
    if (gen !== playbackToken) return;  // switched while the request was out
    // A response for a different token belongs to a replay we have already
    // left (the new /playback request has not reached the device yet). Ignore
    // it and let the next tick pick up the new token.
    if (d.token !== token) return;
    // The cancel POST bumps the token the moment it frees port 81, but the new
    // clip's handler has not published its snapshot yet. That window reports
    // this token with active=false; treating it as end-of-clip tore the
    // progress bar down just as the new clip was about to start, which is why
    // a switched clip showed no time/duration. Wait for the handler instead.
    if (d.pending) return;
    // The device flips active=false when the frame loop ends: that is a
    // natural end-of-clip, so return the view to the idle screen.
    if (!d.active) { playbackFinished(); return; }
    setProgressBar(d.progress || 0, d.elapsed_ms || 0, d.duration_ms || 0);
  } catch (e) { /* device offline; next tick retries */ }
}

function stopPlaybackPolling() {
  if (playbackTimer !== null) {
    clearInterval(playbackTimer);
    playbackTimer = null;
  }
}

// Replays a recorded clip in the same <img> view: the device re-serves the
// AVI's JPEG frames as an MJPEG stream on the port-81 instance, so no
// browser-side video decoding is needed.
async function playClip(dir, name) {
  // Invalidate any replay still in flight, then hard-reset its connection:
  // assigning the new URL immediately aborts the old MJPEG fetch on port 81,
  // so the new clip starts at once instead of waiting for the old one to end.
  stopPlaybackPolling();
  const gen = ++playbackToken;
  streaming = false;
  document.getElementById('liveBar').style.display = 'none';
  document.getElementById('playbackName').textContent =
    (dir ? dir + '  ' : '') + name.replace('.avi', '');
  document.getElementById('playbackBar').style.display = 'flex';
  setProgressBar(0, 0, 0);
  overlay.classList.add('hidden');
  window.scrollTo({ top: 0, behavior: 'smooth' });

  // Take the token from the device (it also frees the busy port-81 task), so
  // the number this request carries is always newer than anything the device
  // has seen. Minting one here broke playback after a page reload.
  const token = await requestPlayToken();
  if (gen !== playbackToken) return;  // another clip was picked while waiting
  cam.src = STREAM_BASE + '/playback?dir=' +
            encodeURIComponent(dir) + '&name=' + encodeURIComponent(name) +
            '&token=' + token;

  pbPollCount = 0;
  pollPlayback(token, gen);  // fast first update
  // The cancel POST and the new /playback GET race across two HTTP servers;
  // the very first poll usually still sees the old token. Retry quickly so the
  // new clip's time/duration label appears promptly instead of after 500 ms.
  setTimeout(() => pollPlayback(token, gen), 120);
  playbackTimer = setInterval(() => {
    pollPlayback(token, gen);
    // Absolute safety net: clips can be up to 120 s and "extend on motion"
    // can make them longer, so give the device ample margin before giving up
    // on a replay that never reported "done" (e.g. a truncated AVI).
    if (++pbPollCount > 400) playbackFinished();
  }, 500);
}

document.getElementById('backLiveBtn').addEventListener('click', () => {
  stopPlaybackPolling();
  playbackToken++;  // abandon any in-flight replay
  detachCam();      // drop the playback connection before reopening the stream
  setProgressBar(0, 0, 0);
  startStream();
});

startBtn.addEventListener('click', startStream);
document.getElementById('stopStreamBtn').addEventListener('click', stopStream);
cam.addEventListener('error', () => {
  if (!streaming) return;
  streaming = false;
  document.getElementById('liveBar').style.display = 'none';
  overlayText.textContent = '画面中断，点击重连';
  overlay.classList.remove('hidden');
});

function fmtBytes(n) {
  if (n > 1048576) return (n / 1048576).toFixed(1) + ' MB';
  if (n > 1024) return (n / 1024).toFixed(0) + ' KB';
  return n + ' B';
}

async function poll() {
  try {
    const r = await fetch('/api/v1/status', { cache: 'no-store' });
    const s = await r.json();

    const net = document.getElementById('netbadge');
    net.textContent = s.wifi_connected
      ? ('WiFi ' + s.rssi + ' dBm')
      : ('离线 · ' + s.wifi_status);
    net.className = 'badge ' + (s.wifi_connected ? 'ok' : 'warn');

    document.getElementById('fpsbadge').textContent =
      (s.fps || 0).toFixed(1) + ' fps';
    document.getElementById('res').textContent = s.width + '×' + s.height;
    document.getElementById('fps').textContent = (s.fps || 0).toFixed(1) + ' fps';
    document.getElementById('rssi').textContent = s.wifi_connected ? s.rssi + ' dBm' : '--';
    document.getElementById('uptime').textContent = s.uptime_s + ' s';
    document.getElementById('heap').textContent = Math.round(s.heap_free / 1024) + ' KB';

    const m = s.motion || {};
    const mv = document.getElementById('motionv');
    if (!m.ready) {
      mv.textContent = '未启用';
    } else if (m.active) {
      mv.textContent = '检测到移动 ' + m.change_percent + '%';
      mv.style.color = '#f85149';
    } else {
      // No advisory text here on purpose: a few noise blocks below the
      // threshold is normal, and telling the user to "lower the threshold" in
      // that state caused exactly the constant-recording problem it pretended
      // to diagnose. The numbers speak for themselves.
      mv.textContent = '静止 ' + m.change_percent + '%';
      mv.style.color = '#3fb950';
    }

    const st = s.storage || {};
    const rec = document.getElementById('recbadge');
    const stopBtn = document.getElementById('stopBtn');
    rec.style.display = st.recording ? '' : 'none';

    // The button never disappears; it is disabled when there is nothing to
    // stop. Also reflect the SD state, because stopping is pointless if the
    // card is missing.
    stopBtn.disabled = !(st.recording && st.mounted);
    stopBtn.textContent = st.recording ? '停止录像' : '未在录像';
    stopBtn.title = st.recording
      ? '立即结束当前录像并保存'
      : (st.mounted ? '当前没有正在进行的录像' : 'SD 卡未挂载');

    document.getElementById('clips').textContent =
      st.mounted ? (st.clips + ' 段') : '无 SD';
    document.getElementById('sd').textContent =
      st.mounted ? (st.free_mb + ' MB') : '--';

    // ---- live-stream watchdog ------------------------------------------------
    // The MJPEG <img> gives no error event when the device side stops sending
    // (the TCP connection just goes quiet), so a frozen stream must be detected
    // from the device's own status. Two conditions mean "stuck":
    //   (a) device reports no stream client although we think we're watching,
    //   (b) the newest streamed frame is older than 3 s.
    // Two consecutive polls (~2 s) trigger one automatic reconnect; the guard
    // avoids reloading src on every poll while the reconnect is pending.
    const ageEl = s.stream_frame_age_ms || 0;
    const clientsEl = s.stream_clients || 0;
    const stuck = (streaming && (clientsEl === 0 || ageEl > 3000));
    if (stuck) {
      streamStaleCount++;
      if (streamStaleCount >= 2) {
        streamStaleCount = 0;
        const ov = document.getElementById('overlayText');
        if (ov) ov.textContent = '直播中断，正在重连…';
        // A cache-busting t forces a fresh request; reassigning src also tears
        // down the dead connection server-side via the socket close.
        cam.src = BLANK_PIXEL;
        setTimeout(() => {
          if (streaming) {
            cam.src = STREAM_BASE + '/stream?t=' + Date.now();
          }
        }, 300);
      }
    } else {
      streamStaleCount = 0;
    }

    document.getElementById('footer').textContent =
      'CAM-AI · ' + s.ip + ' · ' + s.firmware +
      (st.error ? (' · ' + st.error) : '');
  } catch (e) {
    const net = document.getElementById('netbadge');
    net.textContent = '设备无响应';
    net.className = 'badge warn';
  }
}

function pollMotion() {
  document.getElementById('motionimg').src = '/motion?t=' + Date.now();
}

let eventPage = 0;
const EVENT_PAGE_SIZE = 30;
let lastEventKey = '';

function eventMsg(text, color) {
  const el = document.getElementById('eventMsg');
  el.textContent = text || '';
  el.style.color = color || '#8b949e';
}

// ---- selection + batch delete ---------------------------------------------
// Selection is a Set of "dir/name" keys. It intentionally survives list
// re-renders: the checkbox state is re-applied from the set after each render,
// so a poll that happens mid-selection cannot wipe what the user picked.
const selected = new Set();

function selectionKey(dir, name) { return dir + '/' + name; }

function updateSelectionUi() {
  const n = selected.size;
  const btn = document.getElementById('delSelBtn');
  btn.disabled = n === 0;
  btn.textContent = n === 0 ? '删除所选' : '删除所选 (' + n + ')';
  document.getElementById('selInfo').textContent =
    n === 0 ? '勾选多个录像后可一次性删除' : ('已选 ' + n + ' 项');

  const boxes = document.querySelectorAll('#events input[type=checkbox]');
  let allChecked = boxes.length > 0;
  boxes.forEach(cb => {
    if (!cb.checked) allChecked = false;
  });
  document.getElementById('selAll').checked = allChecked;
}

async function deleteBatch(keys) {
  eventMsg('正在删除 ' + keys.length + ' 个文件…', '#8b949e');
  const btn = document.getElementById('delSelBtn');
  btn.disabled = true;
  try {
    // Payload is "|" separated "dir/name" pairs.
    const payload = keys.join('|');
    const r = await fetch('/api/v1/events/delete', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'items=' + encodeURIComponent(payload)
    });
    const d = await r.json();
    if (d.ok) {
      let text = '已删除 ' + d.deleted + ' 个';
      if (d.missing > 0) text += '，' + d.missing + ' 个不存在';
      if (d.rejected > 0) text += '，' + d.rejected + ' 个失败';
      eventMsg(text + '，剩余 ' + d.remaining + ' 段', '#3fb950');
      selected.clear();
      lastEventKey = '';
      const pages = Math.max(1, Math.ceil(d.remaining / EVENT_PAGE_SIZE));
      if (eventPage >= pages) eventPage = pages - 1;
      pollEvents();
    } else {
      eventMsg('删除失败', '#f85149');
    }
  } catch (e) {
    eventMsg('删除失败，设备无响应', '#f85149');
  }
  updateSelectionUi();
}

async function pollEvents() {
  try {
    const url = '/api/v1/events?offset=' + (eventPage * EVENT_PAGE_SIZE) +
                '&limit=' + EVENT_PAGE_SIZE;
    const r = await fetch(url, { cache: 'no-store' });
    const d = await r.json();
    const box = document.getElementById('events');
    const list = d.events || [];
    const total = d.total || 0;

    document.getElementById('evcount').textContent =
      d.mounted ? ('共 ' + total + ' 段') : '（未挂载 SD 卡）';

    const pages = Math.max(1, Math.ceil(total / EVENT_PAGE_SIZE));
    document.getElementById('pageInfo').textContent =
      d.mounted ? ('第 ' + (eventPage + 1) + ' / ' + pages + ' 页') : '';
    document.getElementById('prevBtn').disabled = eventPage <= 0;
    document.getElementById('nextBtn').disabled = eventPage >= pages - 1;

    // One render key covering EVERY state, including the placeholder ones.
    // Previously the "暂无录像"/"SD 卡未挂载" branches never touched
    // lastEventKey: a single transient failure (e.g. the SD busy while a
    // playback stream reads it) painted the placeholder, and the next poll
    // then saw an unchanged key and skipped the re-render -- the list stayed
    // empty until a full page refresh.
    const key = !d.mounted ? 'unmounted'
              : (list.length === 0 ? 'empty'
              : eventPage + ':' + list.map(e => e.name).join(','));
    if (key === lastEventKey) {
      updateSelectionUi();
      return;
    }
    lastEventKey = key;

    if (!d.mounted) {
      box.innerHTML = '<div class="empty">SD 卡未挂载</div>';
      updateSelectionUi();
      return;
    }
    if (list.length === 0) {
      box.innerHTML = '<div class="empty">暂无录像</div>';
      updateSelectionUi();
      return;
    }

    // Avoid re-rendering an unchanged list every poll: it would fight with
    // clicks in progress. Selection changes alone do not need a re-render.
    {
      box.innerHTML = '';
      list.forEach(ev => {
        const row = document.createElement('div');
        row.className = 'event';

        const label = ev.name.replace('.avi', '').replace('EVT_', '');
        const date = ev.dir || '';
        const keyId = selectionKey(date, ev.name);
        row.dataset.key = keyId;
        const url = '/api/v1/events/file?dir=' + encodeURIComponent(date) +
                    '&name=' + encodeURIComponent(ev.name);

        const cb = document.createElement('input');
        cb.type = 'checkbox';
        cb.checked = selected.has(keyId);
        cb.addEventListener('change', () => {
          if (cb.checked) selected.add(keyId);
          else selected.delete(keyId);
          updateSelectionUi();
        });

        const nameEl = document.createElement('span');
        nameEl.className = 'name';
        nameEl.textContent = (date ? date + '  ' : '') + label;

        const metaEl = document.createElement('span');
        metaEl.className = 'meta';
        metaEl.textContent = fmtBytes(ev.bytes);

        const dl = document.createElement('a');
        dl.href = url;
        dl.textContent = '下载';

        const play = document.createElement('a');
        play.href = '#';
        play.textContent = '回放';
        play.onclick = (e) => {
          e.preventDefault();
          playClip(date, ev.name);
        };

        const del = document.createElement('a');
        del.href = '#';
        del.textContent = '删除';
        del.onclick = (e) => {
          e.preventDefault();
          if (!confirm('删除这段录像？')) return;
          deleteBatch([keyId]);
        };

        row.appendChild(cb);
        row.appendChild(nameEl);
        row.appendChild(metaEl);
        row.appendChild(play);
        row.appendChild(dl);
        row.appendChild(del);
        box.appendChild(row);
      });
    }
    updateSelectionUi();
  } catch (e) { /* device offline; next poll retries */ }
}

document.getElementById('prevBtn').addEventListener('click', () => {
  if (eventPage > 0) { eventPage--; lastEventKey = ''; pollEvents(); }
});
document.getElementById('nextBtn').addEventListener('click', () => {
  eventPage++; lastEventKey = ''; pollEvents();
});

document.getElementById('selAll').addEventListener('change', (e) => {
  const checked = e.target.checked;
  document.querySelectorAll('#events .event').forEach(row => {
    const cb = row.querySelector('input[type=checkbox]');
    if (!cb) return;
    cb.checked = checked;
    // Recover the key from the row's delete link target? No: build it from the
    // dataset we store on the row when rendering.
    const keyId = row.dataset.key;
    if (!keyId) return;
    if (checked) selected.add(keyId);
    else selected.delete(keyId);
  });
  updateSelectionUi();
});

document.getElementById('delSelBtn').addEventListener('click', () => {
  if (selected.size === 0) return;
  if (!confirm('删除所选 ' + selected.size + ' 段录像？此操作不可恢复。')) return;
  deleteBatch([...selected]);
});

// ---- wifi -----------------------------------------------------------------
// Credentials are edited here but stored server-side in their own NVS
// namespace; the password is never sent back to the browser.
async function pollWifi() {
  try {
    const r = await fetch('/api/v1/wifi', { cache: 'no-store' });
    if (!r.ok) return;
    const w = await r.json();

    const state = document.getElementById('wifistate');
    state.textContent = w.connected ? ('已连接 · ' + w.ip)
                      : (w.provisioning ? '配网模式' : '未连接');
    state.style.color = w.connected ? '#3fb950'
                      : (w.provisioning ? '#d29922' : '#f85149');

    document.getElementById('wifissid').textContent = w.ssid || '（未设置）';
    document.getElementById('haspass').textContent = w.has_password ? '是' : '否';

    const apRow = document.getElementById('apRow');
    if (w.provisioning) {
      apRow.style.display = 'flex';
      document.getElementById('apinfo').textContent =
        w.ap_ssid + ' → http://' + w.ap_ip + '/';
    } else {
      apRow.style.display = 'none';
    }

    // Prefill the SSID box only while the user has not typed anything.
    const ssidBox = document.getElementById('w_ssid');
    if (!ssidBox.dataset.touched && w.ssid) ssidBox.value = w.ssid;
  } catch (e) { /* offline */ }
}

document.getElementById('w_ssid').addEventListener('input', (e) => {
  e.target.dataset.touched = '1';
});

document.getElementById('wifiSaveBtn').addEventListener('click', async () => {
  const msg = document.getElementById('wifiMsg');
  const ssid = document.getElementById('w_ssid').value.trim();
  const pass = document.getElementById('w_pass').value;

  if (!ssid) {
    msg.textContent = '请填写 WiFi 名称';
    msg.style.color = '#f85149';
    return;
  }
  if (pass && pass.length < 8) {
    msg.textContent = '密码至少 8 位';
    msg.style.color = '#f85149';
    return;
  }
  if (!pass && !confirm('不修改密码，只切换网络名称？')) return;

  msg.textContent = '保存中…';
  msg.style.color = '#8b949e';
  try {
    const body = new URLSearchParams();
    body.append('ssid', ssid);
    if (pass) body.append('password', pass);
    const r = await fetch('/api/v1/wifi', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: body.toString()
    });
    const d = await r.json();
    if (d.ok) {
      msg.textContent = '已保存，正在重连到 ' + ssid +
                        '（若本机 IP 变化，请改用新地址访问）';
      msg.style.color = '#3fb950';
      document.getElementById('w_pass').value = '';
      pollWifi();
    } else {
      msg.textContent = '失败: ' + (d.error || 'unknown');
      msg.style.color = '#f85149';
    }
  } catch (e) {
    // The device may drop the connection mid-request; that is expected.
    msg.textContent = '已提交，设备正在切换网络';
    msg.style.color = '#d29922';
  }
});

poll();
pollMotion();
pollEvents();
pollConfig();
pollWifi();
// 1 s: clips are only 10 s long, so a 2 s poll left the stop button in the
// wrong state for up to a fifth of each recording.
setInterval(poll, 1000);
setInterval(pollMotion, 3000);
setInterval(pollEvents, 5000);
setInterval(pollWifi, 5000);

// ---- settings -------------------------------------------------------------
const CFG_FIELDS = [
  ['s_sens','v_sens','sensitivity'],
  ['s_minblk','v_minblk','min_changed'],
  ['s_confirm','v_confirm','confirm_samples'],
  ['s_cool','v_cool','cooldown_s'],
  ['s_clip','v_clip','clip_duration_s'],
  ['s_jpegq','v_jpegq','jpeg_quality'],
  ['s_minfree','v_minfree','min_free_mb'],
];
const CFG_CHECKS = [
  ['s_recen','recording_enabled'],
  ['s_extend','extend_on_motion'],
  ['s_autoclean','auto_clean'],
  ['s_vflip','vflip'],
  ['s_hflip','hflip'],
];

// Keep the numeric label in sync while dragging.
CFG_FIELDS.forEach(([id, vid]) => {
  const el = document.getElementById(id);
  el.addEventListener('input', () => {
    document.getElementById(vid).textContent = el.value;
  });
});

function collectConfig() {
  const out = {};
  CFG_FIELDS.forEach(([id,, key]) => {
    out[key] = parseInt(document.getElementById(id).value, 10);
  });
  CFG_CHECKS.forEach(([id, key]) => {
    out[key] = document.getElementById(id).checked;
  });
  return out;
}

function applyConfigToForm(c) {
  CFG_FIELDS.forEach(([id, vid, key]) => {
    if (c[key] === undefined) return;
    document.getElementById(id).value = c[key];
    document.getElementById(vid).textContent = c[key];
  });
  CFG_CHECKS.forEach(([id, key]) => {
    if (c[key] === undefined) return;
    document.getElementById(id).checked = !!c[key];
  });
}

async function pollConfig() {
  try {
    const r = await fetch('/api/v1/config', { cache: 'no-store' });
    applyConfigToForm(await r.json());
  } catch (e) { /* offline; keep current form state */ }
}

document.getElementById('saveBtn').addEventListener('click', async () => {
  const msg = document.getElementById('saveMsg');
  msg.textContent = '保存中…';
  msg.style.color = '#8b949e';
  try {
    const r = await fetch('/api/v1/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(collectConfig())
    });
    const d = await r.json();
    if (d.ok) {
      msg.textContent = d.saved ? '已保存' : '已应用（NVS 写入失败）';
      msg.style.color = d.saved ? '#3fb950' : '#d29922';
    } else {
      msg.textContent = '失败: ' + (d.error || 'unknown');
      msg.style.color = '#f85149';
    }
  } catch (e) {
    msg.textContent = '保存失败，设备无响应';
    msg.style.color = '#f85149';
  }
});

document.getElementById('cleanBtn').addEventListener('click', async () => {
  const msg = document.getElementById('saveMsg');
  if (!confirm('删除最旧的录像以释放空间？')) return;
  msg.textContent = '清理中…';
  msg.style.color = '#8b949e';
  try {
    const r = await fetch('/api/v1/cleanup', { method: 'POST' });
    const d = await r.json();
    if (d.ok) {
      msg.textContent = '已删除 ' + d.removed + ' 个文件，剩余 ' + d.free_mb + ' MB';
      msg.style.color = '#3fb950';
      lastEventKey = '';
      pollEvents();
    } else {
      msg.textContent = '清理失败: ' + (d.error || 'unknown');
      msg.style.color = '#f85149';
    }
  } catch (e) {
    msg.textContent = '清理失败';
    msg.style.color = '#f85149';
  }
});

document.getElementById('stopBtn').addEventListener('click', async () => {
  const btn = document.getElementById('stopBtn');
  // The handler can still fire in the window between polls; bail out early
  // rather than issuing a pointless request.
  if (btn.disabled) return;
  btn.disabled = true;
  try {
    const r = await fetch('/api/v1/recording/stop', { method: 'POST' });
    const d = await r.json();
    if (d.stopped) {
      eventMsg('已停止并保存当前录像', '#3fb950');
    } else if (!d.was_recording) {
      eventMsg('当前没有在录像', '#8b949e');
    }
    lastEventKey = '';
    pollEvents();
    poll();
  } catch (e) {
    eventMsg('停止失败，设备无响应', '#f85149');
  }
});
</script>
</body>
</html>)HTML";

}  // namespace

WebServer* WebServer::instance_ = nullptr;
uint32_t WebServer::streamClients_ = 0;
uint32_t WebServer::playbackClients_ = 0;

bool WebServer::begin(CameraService* camera, NetworkService* network,
                      MotionDetector* motion, EventRecorder* recorder,
                      ConfigService* config, volatile bool* settingsDirty,
                      WifiStore* wifi, RelayClient* relay) {
  camera_ = camera;
  network_ = network;
  motion_ = motion;
  recorder_ = recorder;
  config_ = config;
  settingsDirty_ = settingsDirty;
  wifi_ = wifi;
  relay_ = relay;
  instance_ = this;

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.ctrl_port = 32768;
  cfg.max_uri_handlers = 16;
  // Each in-flight tunnel request occupies one socket on this server, and the
  // cloud page arrives as several parallel fetches. 8 covers the page's own
  // four pollers plus a second tab without starving a LAN client.
  cfg.max_open_sockets = 8;
  cfg.lru_purge_enable = true;  // reclaim sockets from dead browser tabs
  cfg.stack_size = 8192;

  if (httpd_start(&server_, &cfg) != ESP_OK) {
    Serial.println("[web] httpd_start failed");
    server_ = nullptr;
    return false;
  }

  httpd_uri_t uriIndex = {.uri = "/", .method = HTTP_GET,
                          .handler = handleIndex, .user_ctx = nullptr};
  httpd_uri_t uriStatus = {.uri = "/api/v1/status", .method = HTTP_GET,
                           .handler = handleStatus, .user_ctx = nullptr};
  httpd_uri_t uriSnap = {.uri = "/snapshot", .method = HTTP_GET,
                         .handler = handleSnapshot, .user_ctx = nullptr};
  httpd_uri_t uriMotion = {.uri = "/motion", .method = HTTP_GET,
                           .handler = handleMotionPreview, .user_ctx = nullptr};
  httpd_uri_t uriEvents = {.uri = "/api/v1/events", .method = HTTP_GET,
                           .handler = handleEvents, .user_ctx = nullptr};
  httpd_uri_t uriPbStatus = {.uri = "/api/v1/playback/status", .method = HTTP_GET,
                             .handler = handlePlaybackStatus, .user_ctx = nullptr};
  httpd_uri_t uriPbCancel = {.uri = "/api/v1/playback/cancel", .method = HTTP_POST,
                             .handler = handlePlaybackCancel, .user_ctx = nullptr};
  httpd_uri_t uriEventFile = {.uri = "/api/v1/events/file", .method = HTTP_GET,
                              .handler = handleEventFile, .user_ctx = nullptr};
  httpd_uri_t uriEventDel = {.uri = "/api/v1/events/delete", .method = HTTP_POST,
                             .handler = handleEventDelete, .user_ctx = nullptr};
  httpd_uri_t uriGetCfg = {.uri = "/api/v1/config", .method = HTTP_GET,
                           .handler = handleGetConfig, .user_ctx = nullptr};
  httpd_uri_t uriSetCfg = {.uri = "/api/v1/config", .method = HTTP_POST,
                           .handler = handleSetConfig, .user_ctx = nullptr};
  httpd_uri_t uriClean = {.uri = "/api/v1/cleanup", .method = HTTP_POST,
                          .handler = handleCleanup, .user_ctx = nullptr};
  httpd_uri_t uriStop = {.uri = "/api/v1/recording/stop", .method = HTTP_POST,
                         .handler = handleStopRecording, .user_ctx = nullptr};
  // WiFi credentials. Separate from /api/v1/config on purpose: these are
  // secrets and must never round-trip through the settings JSON.
  httpd_uri_t uriWifiGet = {.uri = "/api/v1/wifi", .method = HTTP_GET,
                            .handler = handleWifiGet, .user_ctx = nullptr};
  httpd_uri_t uriWifiSet = {.uri = "/api/v1/wifi", .method = HTTP_POST,
                            .handler = handleWifiSet, .user_ctx = nullptr};

  httpd_register_uri_handler(server_, &uriIndex);
  httpd_register_uri_handler(server_, &uriStatus);
  httpd_register_uri_handler(server_, &uriSnap);
  httpd_register_uri_handler(server_, &uriMotion);
  httpd_register_uri_handler(server_, &uriEvents);
  httpd_register_uri_handler(server_, &uriPbStatus);
  httpd_register_uri_handler(server_, &uriPbCancel);
  httpd_register_uri_handler(server_, &uriEventFile);
  httpd_register_uri_handler(server_, &uriEventDel);
  httpd_register_uri_handler(server_, &uriGetCfg);
  httpd_register_uri_handler(server_, &uriSetCfg);
  httpd_register_uri_handler(server_, &uriClean);
  httpd_register_uri_handler(server_, &uriStop);
  httpd_register_uri_handler(server_, &uriWifiGet);
  httpd_register_uri_handler(server_, &uriWifiSet);

  // Dedicated stream instance: its handler loops forever per viewer, so it
  // must not share the task that serves the API (see web_server.h).
  httpd_config_t scfg = HTTPD_DEFAULT_CONFIG();
  scfg.server_port = kStreamPort;
  scfg.ctrl_port = 32769;  // control ports must differ between instances
  scfg.max_uri_handlers = 4;
  scfg.max_open_sockets = 4;
  scfg.lru_purge_enable = true;
  scfg.stack_size = 8192;

  if (httpd_start(&streamServer_, &scfg) != ESP_OK) {
    Serial.println("[web] stream httpd_start failed");
    httpd_stop(server_);
    server_ = nullptr;
    return false;
  }
  httpd_uri_t uriStream = {.uri = "/stream", .method = HTTP_GET,
                           .handler = handleStream, .user_ctx = nullptr};
  httpd_uri_t uriPlayback = {.uri = "/playback", .method = HTTP_GET,
                             .handler = handlePlayback, .user_ctx = nullptr};
  httpd_register_uri_handler(streamServer_, &uriStream);
  httpd_register_uri_handler(streamServer_, &uriPlayback);

  Serial.printf("[web] listening on port 80, stream on port %u\n",
                (unsigned)kStreamPort);
  return true;
}

void WebServer::end() {
  if (streamServer_) {
    httpd_stop(streamServer_);
    streamServer_ = nullptr;
  }
  if (server_) {
    httpd_stop(server_);
    server_ = nullptr;
  }
}

esp_err_t WebServer::handleIndex(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebServer::handleStatus(httpd_req_t* req) {
  WebServer* self = instance_;
  if (!self || !self->camera_) return ESP_FAIL;

  CameraService& cam = *self->camera_;
  NetworkService* net = self->network_;
  MotionDetector* mo = self->motion_;
  EventRecorder* rec = self->recorder_;
  const EventRecorder::Status* rs = rec ? &rec->status() : nullptr;
  const RelayClient::Status* rly = self->relay_ ? &self->relay_->status() : nullptr;

  char json[1280];
  snprintf(json, sizeof(json),
           "{"
           "\"wifi_connected\":%s,"
           "\"wifi_status\":\"%s\","
           "\"ip\":\"%s\","
           "\"rssi\":%d,"
           "\"width\":%u,"
           "\"height\":%u,"
           "\"fps\":%.1f,"
           "\"sensor\":\"%s\","
           "\"uptime_s\":%lu,"
           "\"heap_free\":%u,"
           "\"psram_free\":%u,"
           "\"stream_clients\":%lu,"
           "\"stream_frame_age_ms\":%lu,"
           "\"provisioning\":%s,"
           "\"relay\":{"
             "\"enabled\":%s,"
             "\"connected\":%s,"
             "\"viewer\":%s,"
             "\"frames_sent\":%lu,"
             "\"bytes_sent\":%lu,"
             "\"reconnects\":%lu,"
             "\"last_error\":\"%s\""
           "},"
           "\"motion\":{"
             "\"ready\":%s,"
             "\"active\":%s,"
             "\"changed_blocks\":%u,"
             "\"change_percent\":%u,"
             "\"samples\":%lu,"
             "\"sensitivity\":%u,"
             "\"triggers\":%lu,"
             "\"decode_fails\":%lu,"
             "\"acquire_fails\":%lu,"
             "\"confirm\":%u,"
             "\"bg_seeded\":%s,"
             "\"luma_sum\":%lu"
           "},"
           "\"storage\":{"
             "\"mounted\":%s,"
             "\"recording\":%s,"
             "\"clips\":%lu,"
             "\"failed\":%lu,"
             "\"last_clip\":\"%s\","
             "\"last_bytes\":%lu,"
             "\"last_frames\":%lu,"
             "\"free_mb\":%lu,"
             "\"total_mb\":%lu,"
             "\"drops\":%lu,"
             "\"error\":\"%s\""
           "},"
           "\"firmware\":\"%s\""
           "}",
           (net && net->isConnected()) ? "true" : "false",
           net ? net->statusText() : "unavailable",
           (net && net->isConnected()) ? net->localIp().toString().c_str() : "0.0.0.0",
           (net && net->isConnected()) ? (int)net->rssi() : 0,
           cam.width(), cam.height(),
           cam.measuredFps(),
           cam.sensorName(),
           (unsigned long)(millis() / 1000),
           ESP.getFreeHeap(),
           ESP.getFreePsram(),
           (unsigned long)self->streamClients(),
           // Age of the newest streamed frame. Do NOT gate on streamClients():
           // when the stream handler dies the client count drops to 0, and the
           // browser (whose <img> is frozen) needs to see a LARGE age to trigger
           // the reconnect watchdog. Gating here used to force age=0 and the
           // watchdog never fired.
           (gLastStreamFrameMs != 0)
               ? (unsigned long)(millis() - gLastStreamFrameMs)
               : 0UL,
           (net && net->provisioningActive()) ? "true" : "false",
           rly && rly->enabled ? "true" : "false",
           rly && rly->connected ? "true" : "false",
           rly && rly->viewerActive ? "true" : "false",
           rly ? (unsigned long)rly->framesSent : 0,
           rly ? (unsigned long)rly->bytesSent : 0,
           rly ? (unsigned long)rly->reconnects : 0,
           rly ? rly->lastError : "",
           (mo && mo->lastAnalysisMs() > 0) ? "true" : "false",
           (mo && mo->isActive()) ? "true" : "false",
           mo ? mo->lastChangedBlocks() : 0,
           mo ? mo->lastChangePercent() : 0,
           mo ? (unsigned long)mo->analysedFrames() : 0,
           mo ? mo->config().sensitivity : 0,
           mo ? (unsigned long)mo->triggerCount() : 0,
           mo ? (unsigned long)mo->decodeFails() : 0,
           mo ? (unsigned long)mo->acquireFails() : 0,
           mo ? (unsigned)mo->confirmCount() : 0,
           (mo && mo->backgroundSeeded()) ? "true" : "false",
           mo ? (unsigned long)mo->lumaSum() : 0,
           (rs && rs->mounted) ? "true" : "false",
           (rs && rs->recording) ? "true" : "false",
           rs ? (unsigned long)rs->clipsWritten : 0,
           rs ? (unsigned long)rs->clipsFailed : 0,
           rs ? rs->lastClip : "",
           rs ? (unsigned long)rs->lastClipBytes : 0,
           rs ? (unsigned long)rs->lastClipFrames : 0,
           rs ? (unsigned long)rs->freeMb : 0,
           rs ? (unsigned long)rs->totalMb : 0,
           rs ? (unsigned long)rs->framesDropped : 0,
           rs ? rs->lastError : "",
           "0.8.0");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebServer::handleSnapshot(httpd_req_t* req) {
  WebServer* self = instance_;
  if (!self || !self->camera_) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  camera_fb_t* fb = self->camera_->acquire(2000);
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);

  self->camera_->release(fb);
  return res;
}

esp_err_t WebServer::handleMotionPreview(httpd_req_t* req) {
  WebServer* self = instance_;
  if (!self || !self->motion_) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  // Serve the detector's own view as a tiny grayscale BMP. Being able to see
  // exactly what the algorithm sees is what makes threshold tuning possible
  // instead of guesswork.
  const int w = MotionDetector::kImgW;
  const int h = MotionDetector::kImgH;
  const int rowBytes = (w + 3) & ~3;          // BMP rows are 4-byte aligned
  const int paletteSize = 256 * 4;
  const int pixelBytes = rowBytes * h;
  const int fileSize = 14 + 40 + paletteSize + pixelBytes;

  uint8_t header[14 + 40 + 256 * 4] = {0};

  // BITMAPFILEHEADER
  header[0] = 'B'; header[1] = 'M';
  header[2] = fileSize & 0xFF; header[3] = (fileSize >> 8) & 0xFF;
  header[4] = (fileSize >> 16) & 0xFF; header[5] = (fileSize >> 24) & 0xFF;
  header[10] = (14 + 40 + paletteSize) & 0xFF;  // pixel data offset

  // BITMAPINFOHEADER
  header[14] = 40;
  header[18] = w & 0xFF; header[19] = (w >> 8) & 0xFF;
  header[22] = h & 0xFF; header[23] = (h >> 8) & 0xFF;
  header[26] = 1;                        // planes
  header[28] = 8;                        // 8 bpp
  header[34] = pixelBytes & 0xFF;        // image size
  header[35] = (pixelBytes >> 8) & 0xFF;
  header[36] = (pixelBytes >> 16) & 0xFF;
  header[37] = (pixelBytes >> 24) & 0xFF;

  // Grayscale palette
  for (int i = 0; i < 256; i++) {
    header[54 + i * 4 + 0] = i;
    header[54 + i * 4 + 1] = i;
    header[54 + i * 4 + 2] = i;
  }

  httpd_resp_set_type(req, "image/bmp");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  esp_err_t res = httpd_resp_send_chunk(req, (const char*)header, sizeof(header));
  if (res != ESP_OK) return res;

  const uint8_t* luma = self->motion_->lumaBuffer();
  uint8_t row[128];
  memset(row, 0, sizeof(row));

  // BMP stores rows bottom-up.
  for (int y = h - 1; y >= 0 && res == ESP_OK; y--) {
    memcpy(row, luma + y * w, w);
    res = httpd_resp_send_chunk(req, (const char*)row, rowBytes);
  }

  httpd_resp_send_chunk(req, nullptr, 0);
  return res;
}

// Walks /events/<date>/ and emits a JSON array of clips, newest first.
// The listing is capped: an unbounded directory walk on a full card would
// block the HTTP task for seconds.
esp_err_t WebServer::handleEvents(httpd_req_t* req) {
  WebServer* self = instance_;
  if (!self || !self->recorder_) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  if (!self->recorder_->status().mounted) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"mounted\":false,\"events\":[]}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Pagination. The previous fixed cap of 40 meant everything beyond the newest
  // 40 clips was invisible in the UI and therefore impossible to delete.
  int offset = 0;
  int limit = 30;
  char query[96];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char buf[12];
    if (httpd_query_key_value(query, "offset", buf, sizeof(buf)) == ESP_OK) {
      offset = atoi(buf);
      if (offset < 0) offset = 0;
    }
    if (httpd_query_key_value(query, "limit", buf, sizeof(buf)) == ESP_OK) {
      limit = atoi(buf);
      if (limit < 1) limit = 1;
      if (limit > 60) limit = 60;
    }
  }

  constexpr int kMaxDirs = 14;

  String json = "{\"mounted\":true,\"events\":[";
  int matched = 0;   // total clips found across all directories
  int emitted = 0;   // clips placed in this page
  bool first = true;

  File eventsRoot = SD_MMC.open("/events");
  if (!eventsRoot || !eventsRoot.isDirectory()) {
    json += "],\"total\":0,\"offset\":0}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), json.length());
  }

  // Collect date directories first, then read newest first.
  String dirs[kMaxDirs];
  int dirCount = 0;
  File d = eventsRoot.openNextFile();
  while (d && dirCount < kMaxDirs) {
    if (d.isDirectory()) {
      String name = d.name();
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      dirs[dirCount++] = name;
    }
    d = eventsRoot.openNextFile();
  }
  eventsRoot.close();

  for (int i = dirCount - 1; i >= 0 && emitted < limit; i--) {
    String dirPath = String("/events/") + dirs[i];
    File dir = SD_MMC.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) continue;

    File f = dir.openNextFile();
    while (f) {
      String fname = f.name();
      int slash = fname.lastIndexOf('/');
      if (slash >= 0) fname = fname.substring(slash + 1);

      if (fname.endsWith(".avi")) {
        // Count every clip so the UI can show a true total, but only emit the
        // slice belonging to this page.
        if (matched >= offset && emitted < limit) {
          if (!first) json += ",";
          first = false;
          json += "{\"name\":\"" + fname + "\"";
          json += ",\"dir\":\"" + dirs[i] + "\"";
          json += ",\"bytes\":" + String((uint32_t)f.size());
          json += "}";
          emitted++;
        }
        matched++;
      }
      f = dir.openNextFile();
    }
    dir.close();
  }

  json += "],\"count\":" + String(emitted) +
          ",\"total\":" + String(matched) +
          ",\"offset\":" + String(offset) + "}";

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json.c_str(), json.length());
}

// Streams one clip. The name is validated against directory traversal: this
// endpoint accepts user input, so ".." must never reach the filesystem.
esp_err_t WebServer::handleEventFile(httpd_req_t* req) {
  char query[128];
  char dir[40] = {0};
  char name[64] = {0};

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    httpd_resp_set_status(req, "400 Bad Request"); httpd_resp_send(req, "bad request", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  httpd_query_key_value(query, "dir", dir, sizeof(dir));
  httpd_query_key_value(query, "name", name, sizeof(name));

  // Reject anything that is not a plain filename.
  auto unsafe = [](const char* s) {
    return !s || !*s || strstr(s, "..") || strchr(s, '/') || strchr(s, '\\');
  };
  if (unsafe(dir) || unsafe(name) || !strstr(name, ".avi")) {
    Serial.printf("[web] rejected path dir=%s name=%s\n", dir, name);
    httpd_resp_set_status(req, "400 Bad Request"); httpd_resp_send(req, "bad request", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  char path[128];
  snprintf(path, sizeof(path), "/events/%s/%s", dir, name);

  File f = SD_MMC.open(path);
  if (!f || f.isDirectory()) {
    httpd_resp_send_404(req);
    return ESP_OK;
  }

  httpd_resp_set_type(req, "video/x-msvideo");
  httpd_resp_set_hdr(req, "Content-Disposition",
                     "attachment; filename=\"clip.avi\"");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  // Chunked transfer so a multi-megabyte clip never needs to fit in RAM.
  uint8_t buf[2048];
  esp_err_t res = ESP_OK;
  while (f.available() && res == ESP_OK) {
    size_t n = f.read(buf, sizeof(buf));
    if (n == 0) break;
    res = httpd_resp_send_chunk(req, (const char*)buf, n);
  }
  f.close();
  httpd_resp_send_chunk(req, nullptr, 0);

  if (res == ESP_OK) Serial.printf("[web] served %s\n", path);
  return res;
}

esp_err_t WebServer::handleEventDelete(httpd_req_t* req) {
  // Accepts a payload of "|" separated "dir/name" pairs:
  //   items=dir1/name1.avi|dir2/name2.avi
  //
  // The client URL-encodes the WHOLE list, so separators arrive as %7C and %2F.
  // The body is therefore URL-decoded BEFORE splitting -- splitting first is
  // impossible because the encoded text contains no literal '|' at all.
  constexpr int kMaxBody = 4096;
  char body[kMaxBody + 1] = {0};

  if (req->content_len <= 0 || req->content_len > kMaxBody) {
    httpd_resp_set_status(req, "413 Payload Too Large");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"body too large\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  int received = 0;
  while (received < req->content_len) {
    const int n = httpd_req_recv(req, body + received,
                                 req->content_len - received);
    if (n <= 0) {
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_send(req, "{\"ok\":false,\"error\":\"recv failed\"}",
                      HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    received += n;
  }
  body[received] = 0;

  // Minimal percent-decoding in place. '+' means a literal space in
  // form-encoded data, but our filenames contain none, so it is left alone.
  auto urlDecodeInPlace = [&](char* s) {
    char* w = s;
    for (char* r = s; *r; ) {
      if (*r == '%' && r[1] && r[2]) {
        auto nib = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          return -1;
        };
        const int hi = nib(r[1]);
        const int lo = nib(r[2]);
        if (hi >= 0 && lo >= 0) {
          *w++ = (char)((hi << 4) | lo);
          r += 3;
          continue;
        }
      }
      *w++ = *r++;
    }
    *w = 0;
  };

  // Skip a leading "items=" if present (either query or bare form).
  char* list = body;
  char* eq = strchr(list, '=');
  if (eq && strncmp(list, "items", 5) == 0) list = eq + 1;
  urlDecodeInPlace(list);

  auto unsafe = [](const char* s) {
    return !s || !*s || strstr(s, "..") || strchr(s, '\\');
  };

  int deleted = 0;
  int missing = 0;
  int rejected = 0;

  char* save = nullptr;
  for (char* item = strtok_r(list, "|", &save); item;
       item = strtok_r(nullptr, "|", &save)) {
    if (!*item) continue;

    char* slash = strchr(item, '/');
    if (!slash) { rejected++; continue; }
    *slash = 0;
    const char* dir = item;
    const char* name = slash + 1;

    if (unsafe(dir) || unsafe(name) || strchr(name, '/')) {
      rejected++;
      continue;
    }

    char path[128];
    snprintf(path, sizeof(path), "/events/%s/%s", dir, name);

    // SD_MMC.remove() reports success even for a nonexistent path, so existence
    // is checked explicitly; otherwise the UI would claim deletions that never
    // happened.
    if (!SD_MMC.exists(path)) {
      missing++;
      continue;
    }

    if (SD_MMC.remove(path)) {
      deleted++;
      // Remove the sidecar metadata alongside the clip.
      char jsonPath[128];
      snprintf(jsonPath, sizeof(jsonPath), "/events/%s/%s", dir, name);
      char* dot = strrchr(jsonPath, '.');
      if (dot) {
        strcpy(dot, ".json");
        if (SD_MMC.exists(jsonPath)) SD_MMC.remove(jsonPath);
      }
    } else {
      rejected++;
    }

    // Yield so the HTTP task and streaming keep running during a long batch.
    delay(1);
  }

  // Report per-item results only. Counting every remaining clip here used to
  // walk the whole SD tree on each request; with the FAT lock contended by an
  // active recording that walk took seconds and, because esp_http_server is
  // single-tasked, it queued every other request (stop, save, status) behind
  // it. The UI re-fetches the events list to learn the new total anyway.
  char out[160];
  snprintf(out, sizeof(out),
           "{\"ok\":%s,\"deleted\":%d,\"missing\":%d,\"rejected\":%d}",
           (deleted > 0) ? "true" : "false", deleted, missing, rejected);
  Serial.printf("[web] batch delete: %s\n", out);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t WebServer::handleGetConfig(httpd_req_t* req) {
  WebServer* self = instance_;
  if (!self || !self->config_) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  const String json = self->config_->toJson();
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json.c_str(), json.length());
}

esp_err_t WebServer::handleSetConfig(httpd_req_t* req) {
  WebServer* self = instance_;
  if (!self || !self->config_) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  // Bound the body so a malformed or hostile request cannot exhaust RAM.
  constexpr int kMaxBody = 1024;
  if (req->content_len <= 0 || req->content_len > kMaxBody) {
    httpd_resp_set_status(req, "413 Payload Too Large");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"body too large\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  char body[kMaxBody + 1];
  int received = 0;
  while (received < req->content_len) {
    const int n = httpd_req_recv(req, body + received, req->content_len - received);
    if (n <= 0) {
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_send(req, "{\"ok\":false,\"error\":\"recv failed\"}",
                      HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    received += n;
  }
  body[received] = 0;

  String error;
  if (!self->config_->updateFromJson(body, (size_t)received, error)) {
    // Reject the whole payload: nothing is applied on validation failure.
    String out = "{\"ok\":false,\"error\":\"" + error + "\"}";
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out.c_str(), out.length());
    return ESP_OK;
  }

  const bool saved = self->config_->save();

  // Hand off to main: module configs are rebuilt there, outside any request.
  if (self->settingsDirty_) *self->settingsDirty_ = true;

  const String out = saved
      ? String("{\"ok\":true,\"saved\":true}")
      : String("{\"ok\":true,\"saved\":false,\"error\":\"NVS write failed\"}");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, out.c_str(), out.length());
  return ESP_OK;
}

esp_err_t WebServer::handleCleanup(httpd_req_t* req) {
  WebServer* self = instance_;
  if (!self || !self->recorder_) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  if (!self->recorder_->status().mounted) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"SD not mounted\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Refuse while recording: deleting files during an active write can corrupt
  // the FAT directory the writer is relying on.
  if (self->recorder_->isRecording()) {
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"recording in progress\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  const int target = self->config_
                         ? (int)self->config_->get().minFreeMb + 500
                         : 700;
  const int removed = self->recorder_->cleanOldest(target, 30);

  char out[128];
  snprintf(out, sizeof(out),
           "{\"ok\":true,\"removed\":%d,\"free_mb\":%lu}", removed,
           (unsigned long)self->recorder_->status().freeMb);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t WebServer::handleStopRecording(httpd_req_t* req) {
  WebServer* self = instance_;
  if (!self || !self->recorder_) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  const bool wasRecording = self->recorder_->isRecording();
  const bool stopped = self->recorder_->stopRecording();

  char out[128];
  snprintf(out, sizeof(out),
           "{\"ok\":true,\"was_recording\":%s,\"stopped\":%s}",
           wasRecording ? "true" : "false", stopped ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t WebServer::handleWifiGet(httpd_req_t* req) {
  WebServer* self = instance_;
  if (!self || !self->wifi_) {
    // This esp_http_server build has no httpd_resp_send_503 helper.
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"wifi store unavailable\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // The password is never returned: only whether one is stored.
  NetworkService* net = self->network_;
  const bool up = net && net->isConnected();
  const bool prov = net && net->provisioningActive();

  char apSsid[40] = {0};
  char apIp[20] = {0};
  if (prov) {
    snprintf(apSsid, sizeof(apSsid), "%s", net->apSsid().c_str());
    snprintf(apIp, sizeof(apIp), "%s", WiFi.softAPIP().toString().c_str());
  }

  char json[320];
  snprintf(json, sizeof(json),
           "{"
           "\"ssid\":\"%s\","
           "\"has_password\":%s,"
           "\"connected\":%s,"
           "\"ip\":\"%s\","
           "\"provisioning\":%s,"
           "\"ap_ssid\":\"%s\","
           "\"ap_ip\":\"%s\""
           "}",
           self->wifi_->ssid(),
           (self->wifi_->password() && *self->wifi_->password()) ? "true" : "false",
           up ? "true" : "false",
           up ? net->localIp().toString().c_str() : "0.0.0.0",
           prov ? "true" : "false",
           apSsid, apIp);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// Accepts form-encoded "ssid=...&password=..." or the same fields as JSON.
// Validation happens in WifiStore; on success the station is re-pointed at the
// new network immediately, so the user can watch the page reconnect.
esp_err_t WebServer::handleWifiSet(httpd_req_t* req) {
  WebServer* self = instance_;
  if (!self || !self->wifi_) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"wifi store unavailable\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  constexpr int kMaxBody = 256;
  if (req->content_len <= 0 || req->content_len > kMaxBody) {
    httpd_resp_set_status(req, "413 Payload Too Large");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"body too large\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  char body[kMaxBody + 1];
  int received = 0;
  while (received < req->content_len) {
    const int n = httpd_req_recv(req, body + received, req->content_len - received);
    if (n <= 0) {
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_send(req, "{\"ok\":false,\"error\":\"recv failed\"}",
                      HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    received += n;
  }
  body[received] = 0;

  // Field extraction that works for both encodings without pulling in a parser:
  // find "ssid" and "password", then read the quoted or form value after ':'
  // or '='. '+' is not decoded here because WiFi passwords may legitimately
  // contain it, and the form is sent by our own page which encodes spaces as %20.
  auto extract = [&](const char* key, char* out, size_t outLen) -> bool {
    const char* p = strstr(body, key);
    if (!p) return false;
    p += strlen(key);
    while (*p && *p != ':' && *p != '=') p++;
    if (!*p) return false;
    p++;
    while (*p == ' ' || *p == '"') p++;

    size_t o = 0;
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    while (*p && o + 1 < outLen) {
      if (*p == '"' || *p == '&' || *p == '}') break;
      if (*p == '%' && p[1] && p[2]) {
        const int hi = nib(p[1]);
        const int lo = nib(p[2]);
        if (hi >= 0 && lo >= 0) {
          out[o++] = (char)((hi << 4) | lo);
          p += 3;
          continue;
        }
      }
      out[o++] = *p++;
    }
    out[o] = 0;
    return true;
  };

  char ssid[40] = {0};
  char pass[70] = {0};
  const bool haveSsid = extract("\"ssid\"", ssid, sizeof(ssid)) ||
                        extract("ssid", ssid, sizeof(ssid));
  const bool havePass = extract("\"password\"", pass, sizeof(pass)) ||
                        extract("password", pass, sizeof(pass));

  if (!haveSsid || !*ssid) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"ssid required\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // An omitted/empty password means "keep the stored one", matching the UI
  // placeholder ("留空表示不修改"). Only an explicit new password replaces it.
  const char* effectivePass = pass;
  if (!havePass || !*pass) {
    effectivePass = self->wifi_->password();
  }

  if (!self->wifi_->save(ssid, effectivePass)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid credentials\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Point the station at the new network. The AP stays up for a few more
  // seconds so this response can reach the phone that sent it.
  if (self->network_) {
    self->network_->setCredentials(self->wifi_->ssid(), self->wifi_->password());
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"ok\":true,\"saved\":true,\"reconnecting\":true}",
                  HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t WebServer::handleStream(httpd_req_t* req) {
  WebServer* self = instance_;
  if (!self || !self->camera_) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  if (streamClients_ >= kMaxStreamClients) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "stream busy: another viewer is connected", HTTPD_RESP_USE_STRLEN);
    Serial.println("[web] rejected extra stream client");
    return ESP_OK;
  }

  streamClients_++;

  esp_err_t res = httpd_resp_set_type(req, kStreamContentType);
  if (res == ESP_OK) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // Discourage proxies from buffering an endless response.
    httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");
  }

  char partHeader[96];
  uint32_t frames = 0;
  uint32_t startMs = millis();
  int sendFails = 0;  // consecutive send failures; tolerate transient timeouts

  // This handler owns its own httpd task (port 81), so it no longer competes
  // with the API task on port 80 -- a generous send timeout is safe here and
  // avoids killing the whole stream on one transient TCP-window stall.
  int sockfd = httpd_req_to_sockfd(req);
  if (sockfd >= 0) {
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;  // 1000 ms (was 200 ms: too eager, caused frozen streams)
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }

  // A fresh stream starts with a clean frame-age budget: a stale timestamp
  // from a previous (dead) stream must not fool the browser watchdog.
  gLastStreamFrameMs = millis();

  // Reset whenever a frame DOES arrive; grown while none does. Scoped to this
  // handler call so each stream starts with a clean 5 s budget.
  uint32_t noFrameSinceMs = 0;

  while (res == ESP_OK) {
    camera_fb_t* fb = self->camera_->acquire(1000);
    if (!fb) {
      // Sensor hiccup: brief pause, then try again rather than killing the
      // stream. But if no frame arrives for 5 s the sensor is wedged for real
      // (or the camera was reconfigured mid-stream); end the stream so the
      // socket closes and the browser's watchdog can reconnect, instead of
      // spinning here forever holding streamClients_ at 1.
      if (noFrameSinceMs == 0) noFrameSinceMs = millis();
      if (millis() - noFrameSinceMs >= 5000) {
        Serial.println("[web] stream: no frames for 5s, closing");
        break;
      }
      delay(20);
      continue;
    }
    noFrameSinceMs = 0;

    if (fb->format != PIXFORMAT_JPEG) {
      self->camera_->release(fb);
      res = ESP_FAIL;
      break;
    }

    int len = snprintf(partHeader, sizeof(partHeader),
                       "\r\n--%s\r\nContent-Type: image/jpeg\r\n"
                       "Content-Length: %u\r\n\r\n",
                       kBoundary, (unsigned)fb->len);

    res = httpd_resp_send_chunk(req, partHeader, len);
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    }
    self->camera_->release(fb);

    if (res != ESP_OK) {
      // A single timeout (SO_SNDTIMEO) is usually a transient TCP-window stall,
      // NOT a disconnect. Breaking here used to freeze the MJPEG <img> forever
      // because the browser gets no error event for a silently closed stream.
      // Tolerate a few consecutive failures; only treat it as a real goodbye
      // after several, and let the frontend watchdog cover the rest.
      if (++sendFails >= 5) {
        Serial.printf("[web] stream: %d consecutive send fails, closing\n",
                      sendFails);
        break;
      }
      delay(50);
      continue;
    }
    sendFails = 0;

    frames++;
    gLastStreamFrameMs = millis();
    gStreamFrameCount++;

    // Yield between frames. Without this the loop re-acquired immediately and
    // held both frame buffers continuously; the motion detector, which also
    // calls acquire() from the main loop, never won a buffer and its readings
    // froze for as long as a viewer was connected. A short sleep is enough to
    // let the detector (250 ms cadence) and the SD recorder get their turns,
    // while stream throughput stays bounded by the network anyway.
    delay(kStreamYieldMs);
  }

  // Terminating chunk. Harmless if the socket is already gone.
  httpd_resp_send_chunk(req, nullptr, 0);

  streamClients_--;
  uint32_t elapsed = millis() - startMs;
  Serial.printf("[web] stream closed: %lu frames in %lu ms (%.1f fps)\n",
                (unsigned long)frames, (unsigned long)elapsed,
                elapsed ? frames * 1000.0f / elapsed : 0.0f);
  return ESP_OK;
}

// Replays a recorded clip as an MJPEG stream.
//
// The AVI files written by AviWriter are RIFF containers of raw JPEG frames
// (00dc chunks inside the movi LIST). No browser decodes AVI natively, so
// instead we walk the container here and re-serve the JPEGs in exactly the
// multipart format the live view already renders, paced at the frame rate
// recorded in the avih header. Like the live stream this runs on the
// dedicated port-81 instance, so a slow viewer never stalls the API.
//
// Frames are relayed in 1.4 KB blocks straight from the SD file: only the
// chunk header is needed up front, so even a 40 KB frame never has to be
// held in RAM.
esp_err_t WebServer::handlePlayback(httpd_req_t* req) {
  WebServer* self = instance_;
  if (!self || !self->recorder_ || !self->recorder_->status().mounted) {
    httpd_resp_send_404(req);
    return ESP_OK;
  }

  char query[256];
  char dir[40] = {0};
  char name[64] = {0};
  char tokenStr[16] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "bad request", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  httpd_query_key_value(query, "dir", dir, sizeof(dir));
  httpd_query_key_value(query, "name", name, sizeof(name));
  httpd_query_key_value(query, "token", tokenStr, sizeof(tokenStr));
  const uint32_t token = (uint32_t)strtoul(tokenStr, nullptr, 10);

  // Same traversal guard as the download endpoint: only plain filenames.
  auto unsafe = [](const char* s) {
    return !s || !*s || strstr(s, "..") || strchr(s, '/') || strchr(s, '\\');
  };
  if (unsafe(dir) || unsafe(name) || !strstr(name, ".avi")) {
    Serial.printf("[web] playback rejected path dir=%s name=%s\n", dir, name);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "bad request", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  if (playbackClients_ >= kMaxPlaybackClients) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "playback busy: another replay is running",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  char path[128];
  snprintf(path, sizeof(path), "/events/%s/%s", dir, name);
  File f = SD_MMC.open(path);
  if (!f || f.isDirectory()) {
    httpd_resp_send_404(req);
    return ESP_OK;
  }

  // Chunk header reader: 4-byte id + little-endian 32-bit size.
  auto readChunkHeader = [&](char id[5], uint32_t* size) -> bool {
    uint8_t hdr[8];
    if (f.read(hdr, 8) != 8) return false;
    memcpy(id, hdr, 4);
    id[4] = 0;
    *size = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
            ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    return true;
  };

  uint8_t magic[12];
  if (f.read(magic, 12) != 12 || memcmp(magic, "RIFF", 4) != 0 ||
      memcmp(magic + 8, "AVI ", 4) != 0) {
    f.close();
    httpd_resp_set_status(req, "422 Unprocessable Entity");
    httpd_resp_send(req, "not an AVI file", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // ---- locate the frame data ----
  // The container header is only ~220 bytes, so read the first 512 bytes and
  // pattern-match 'LIST <size> movi' directly. A strict RIFF walk would rely
  // on every chunk-size field being exact; the pattern match does not care
  // (the 12-byte 'LIST????movi' sequence cannot appear in JPEG data either).
  uint8_t head[512];
  const size_t hn = f.read(head, sizeof(head));

  uint32_t frameMs = 100;         // fallback 10 fps; avih overrides
  uint32_t totalFrames = 0;       // dwTotalFrames from avih, if present
  for (size_t i = 0; i + 12 <= hn; i++) {
    if (memcmp(head + i, "avih", 4) == 0) {
      uint32_t usec = (uint32_t)head[i + 8] | ((uint32_t)head[i + 9] << 8) |
                      ((uint32_t)head[i + 10] << 16) |
                      ((uint32_t)head[i + 11] << 24);
      if (usec > 0) {
        frameMs = usec / 1000;
        if (frameMs < 20) frameMs = 20;     // do not outrun the network
        if (frameMs > 1000) frameMs = 1000;
      }
      // dwTotalFrames is the 5th field of the avih payload. Relative to the
      // 'avih' FourCC at i: +4 size, +8 usec, +12 maxBps, +16 padding,
      // +20 flags, then +24..27 = dwTotalFrames. Reading +20 mistakenly
      // returns dwFlags (0x10 = 16), which made every clip look ~1.6 s long.
      if (i + 28 <= hn) {
        totalFrames = (uint32_t)head[i + 24] | ((uint32_t)head[i + 25] << 8) |
                      ((uint32_t)head[i + 26] << 16) |
                      ((uint32_t)head[i + 27] << 24);
      }
      break;
    }
  }

  uint32_t moviEnd = 0;
  uint32_t moviStart = 0;
  bool foundMovi = false;
  for (size_t i = 8; i + 4 <= hn; i++) {
    if (memcmp(head + i, "movi", 4) == 0 &&
        memcmp(head + i - 8, "LIST", 4) == 0) {
      uint32_t lsize = (uint32_t)head[i - 4] | ((uint32_t)head[i - 3] << 8) |
                       ((uint32_t)head[i - 2] << 16) |
                       ((uint32_t)head[i - 1] << 24);
      // 'head' starts at absolute offset 12 (the RIFF magic was consumed).
      moviStart = 12 + (uint32_t)i + 4;
      moviEnd = moviStart + (lsize > 4 ? lsize - 4 : 0);
      f.seek(moviStart);
      foundMovi = true;
      break;
    }
  }

  if (!foundMovi) {
    f.close();
    httpd_resp_set_status(req, "422 Unprocessable Entity");
    httpd_resp_send(req, "AVI has no movi list", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // Old clips can carry an unpatched dwTotalFrames (0). If so, recover the
  // frame count from the idx1 index at the end of the file: every entry is
  // 16 bytes and the index declares its own byte size. This is a bounded read
  // of the tail, independent of the number of frames.
  if (totalFrames == 0) {
    const uint32_t fileLen = f.size();
    if (fileLen >= 8) {
      f.seek(fileLen - 8);
      uint8_t tail[8];
      if (f.read(tail, 8) == 8 && memcmp(tail, "idx1", 4) == 0) {
        uint32_t idxSize = (uint32_t)tail[4] | ((uint32_t)tail[5] << 8) |
                           ((uint32_t)tail[6] << 16) | ((uint32_t)tail[7] << 24);
        // The idx1 chunk sits immediately before its 8-byte header at EOF.
        if (idxSize >= 16 && idxSize <= fileLen - 8) {
          totalFrames = idxSize / 16;
        }
      }
    }
    f.seek(moviStart);  // restore the frame-reading position
  }

  // A newer replay may already have claimed playback while this request was
  // queued behind the previous one on the single-task port-81 server. The
  // browser bumps the token through /playback/cancel before every switch, so
  // a request that arrives with a token older than the current one belongs to
  // a clip the user has already left. Publishing it here would drag the token
  // backwards and flip active back to true, resurrecting a replay the UI has
  // already torn down — that is what made a finished clip appear to play
  // itself a second time. Drop the request instead. A request with no token at
  // all (a hand-typed/bookmarked URL) is exempt: it carries no ordering
  // information, so it is served and adopts the current token order.
  if (token != 0 && token < gPlaybackToken) {
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_send(req, "superseded", HTTPD_RESP_USE_STRLEN);
    f.close();
    return ESP_OK;
  }

  playbackClients_++;

  esp_err_t res = httpd_resp_set_type(req, kStreamContentType);
  if (res == ESP_OK) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");
  }

  // Bound how long a single send may block, so a dead viewer cannot park this
  // task forever. Playback gets a much more generous bound than the live
  // stream: its sends are large whole frames and the frame pacing deliberately
  // idles the socket between them, so a 200 ms cap — fine for the live path,
  // where a dropped frame just means the browser reconnects — aborted the
  // whole replay instead. Measured 86-frame run: 7 sends exceeded 300 ms and
  // one tripped the cap mid-clip, which the UI then showed as "finished".
  int sockfd = httpd_req_to_sockfd(req);
  if (sockfd >= 0) {
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 500000;  // 1500 ms
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }

  char partHeader[96];
  uint8_t buf[1408];
  uint32_t frames = 0;
  uint32_t startMs = millis();
  // Why the frame loop stopped. An unexplained early stop is much harder to
  // chase from the outside, so record the exact exit path in the log.
  const char* stopReason = "eof";

  // Whole-frame staging buffer so one JPEG goes out in a single send_chunk()
  // like the live stream, instead of ~16 calls through the small buffer below.
  // 48 KB covers the recorder's usual frame size; anything larger falls back to
  // the incremental loop. Allocated once per replay and freed on every exit.
  static const size_t kFrameBufMax = 48 * 1024;
  uint8_t* frameBuf = (uint8_t*)malloc(kFrameBufMax);
  if (frameBuf == nullptr) {
    Serial.println("[web] playback: frame buffer alloc failed, using small buffer");
  }
  // Timing split: SD read vs socket send. A slow replay could be either, and
  // the two need very different fixes, so measure instead of guessing.
  uint32_t dbgReadMs = 0, dbgSendMs = 0, dbgSlowSends = 0;

  // Publish the initial progress snapshot. duration_ms uses the avih frame
  // period when the frame count is known; otherwise it stays 0 and the UI
  // shows the bar without a time label until more data is available.
  // A tokenless request (hand-opened URL) keeps the current token order rather
  // than resetting it to 0, which would let a later stale request through the
  // superseded check above.
  if (token != 0) gPlaybackToken = token;
  // Identity this stream uses for every ownership check below. A tokenless
  // request adopts the order it just published instead of comparing against 0,
  // which would make its own loop exit on the very first frame.
  const uint32_t myToken = gPlaybackToken;
  gPlaybackFramesDone = 0;
  gPlaybackFramesTotal = totalFrames;
  gPlaybackStartMs = startMs;
  gPlaybackDurationMs = (totalFrames > 0 && frameMs > 0)
                          ? totalFrames * frameMs : 0;
  gPlaybackActive = true;
  gPlaybackPending = false;  // this handler owns the token now

  while (res == ESP_OK && f.position() + 8 <= moviEnd) {
    char id[5];
    uint32_t len;
    if (!readChunkHeader(id, &len)) { stopReason = "chunk-header-eof"; break; }
    if (memcmp(id, "idx1", 4) == 0) { stopReason = "idx1"; break; }

    if (memcmp(id, "00dc", 4) != 0 && memcmp(id, "00db", 4) != 0) {
      // Unknown chunk inside movi: skip rather than abort.
      f.seek(f.position() + len + (len & 1));
      continue;
    }

    int hlen = snprintf(partHeader, sizeof(partHeader),
                        "\r\n--%s\r\nContent-Type: image/jpeg\r\n"
                        "Content-Length: %u\r\n\r\n",
                        kBoundary, (unsigned)len);
    res = httpd_resp_send_chunk(req, partHeader, hlen);

    // Send the JPEG with as few send_chunk() calls as possible.
    //
    // The live stream hands the whole frame to one call; replay used to stage
    // it through a 1408-byte buffer, turning a ~20 KB frame into ~16 separate
    // chunked sends. Each call pays chunk-header and socket overhead, and the
    // per-send SO_SNDTIMEO applies to every one of them, so replay measured
    // ~28 KB/s against ~111 KB/s on the live path and could time out mid-clip
    // (which looked like the replay ending early). A reusable frame buffer
    // restores one send per frame; frames larger than the buffer still fall
    // back to the streaming loop.
    if (len <= kFrameBufMax && frameBuf != nullptr) {
      uint32_t tR0 = millis();
      size_t got = f.read(frameBuf, len);
      uint32_t tR1 = millis();
      if (got != len) {
        res = ESP_FAIL;
        stopReason = "file-truncated";
        break;
      }
      res = httpd_resp_send_chunk(req, (const char*)frameBuf, len);
      uint32_t tR2 = millis();
      dbgReadMs += (tR1 - tR0);
      dbgSendMs += (tR2 - tR1);
      if (tR2 - tR1 > 300) dbgSlowSends++;
    } else {
      uint32_t remaining = len;
      while (res == ESP_OK && remaining > 0) {
        size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
        size_t n = f.read(buf, want);
        if (n == 0) {
          res = ESP_FAIL;  // truncated file
          stopReason = "file-truncated";
          break;
        }
        res = httpd_resp_send_chunk(req, (const char*)buf, n);
        remaining -= n;
      }
    }
    if (len & 1) f.seek(f.position() + 1);  // RIFF word alignment
    if (res != ESP_OK) { stopReason = "send-failed"; break; }  // client gone

    frames++;
    // Only the replay that still owns the token may publish its progress. A
    // stale connection unwinding after a clip switch must not overwrite the
    // new clip's counter (otherwise the progress bar jumps back to zero).
    if (myToken == gPlaybackToken) gPlaybackFramesDone = frames;
    // If a newer clip has claimed playback, stop pushing frames now. In the
    // single-task stream server a new handler cannot start until we return,
    // but this also covers builds that run URI handlers concurrently.
    if (myToken != gPlaybackToken) { stopReason = "superseded"; res = ESP_FAIL; break; }
    // Pace against an absolute deadline instead of sleeping a fixed frame time.
    //
    // delay(frameMs) assumed the send was free. It is not: measured over an
    // 86-frame replay, sends cost 13516 ms (~157 ms/frame) on their own, so
    // "100 ms per frame" became ~257 ms and an 11.8 s clip took ~30 s to watch
    // -- while the progress bar, which is frame-based, raced ahead of the
    // picture. Waiting only for the remainder keeps replay at the recorded
    // frame rate, and skips the wait entirely when sending already ran long.
    uint32_t due = startMs + (uint32_t)(frames * frameMs);
    int32_t waitMs = (int32_t)(due - millis());
    if (waitMs > 0) delay((uint32_t)waitMs);
  }

  // Mark this replay finished before tearing down the socket, but ONLY if it
  // is still the current replay. A superseded connection (a newer clip was
  // selected mid-playback) must not clear the new clip's active flag — doing
  // so made the freshly switched clip look instantly finished, so its time
  // and duration never appeared on the progress bar.
  if (myToken == gPlaybackToken) gPlaybackActive = false;
  httpd_resp_send_chunk(req, nullptr, 0);
  f.close();
  playbackClients_--;
  free(frameBuf);

  Serial.printf("[web] playback closed: %s (%lu/%lu frames in %lu ms, stop=%s)\n",
                path, (unsigned long)frames, (unsigned long)totalFrames,
                (unsigned long)(millis() - startMs), stopReason);
  Serial.printf("[web] playback timing: read=%lu ms send=%lu ms slowSends=%lu\n",
                (unsigned long)dbgReadMs, (unsigned long)dbgSendMs,
                (unsigned long)dbgSlowSends);
  return ESP_OK;
}

// Lightweight progress endpoint polled by the browser during a replay. Runs
// on the main (port-80) server so it stays responsive while the port-81 task
// is busy streaming frames. All state is published by handlePlayback.
esp_err_t WebServer::handlePlaybackStatus(httpd_req_t* req) {
  const uint32_t token = gPlaybackToken;
  const bool active = gPlaybackActive;
  const bool pending = gPlaybackPending;
  const uint32_t done = gPlaybackFramesDone;
  const uint32_t total = gPlaybackFramesTotal;
  uint32_t duration = gPlaybackDurationMs;

  // Report elapsed in MEDIA time, derived from frames delivered, not wall clock.
  // The two diverge whenever the network is slower than the recorded frame
  // rate (measured: an 11.8 s clip took ~30 s to transfer). Wall-clock elapsed
  // then ran past duration and the label read like "00:25 / 00:11" while the
  // bar, being frame-based, disagreed with it. Frame count drives both the bar
  // and the label, so they can never contradict each other.
  uint32_t elapsed;
  if (total > 0 && duration > 0) {
    elapsed = (uint32_t)((uint64_t)done * duration / total);
  } else {
    elapsed = millis() - gPlaybackStartMs;
  }

  // Prefer frame-based progress; it is exact even if the network stalls and
  // the client buffers. Fall back to wall-clock when no frame count exists.
  uint32_t progress = 0;
  if (total > 0) {
    progress = (uint32_t)((uint64_t)done * 100 / total);
    if (progress > 100) progress = 100;
  } else if (duration > 0) {
    progress = elapsed * 100 / duration;
    if (progress > 100) progress = 100;
  }

  char json[192];
  snprintf(json, sizeof(json),
           "{\"active\":%s,\"pending\":%s,\"token\":%lu,\"done\":%lu,\"total\":%lu,"
           "\"progress\":%lu,\"elapsed_ms\":%lu,\"duration_ms\":%lu}",
           active ? "true" : "false", pending ? "true" : "false",
           (unsigned long)token,
           (unsigned long)done, (unsigned long)total,
           (unsigned long)progress, (unsigned long)elapsed,
           (unsigned long)duration);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// Out-of-band cancellation, called by the browser (on port 80) right before it
// switches clips or goes back to live. Port 81 runs all its URI handlers in a
// single task and handlePlayback() holds that task for the whole clip, so just
// aborting the old <img> socket is not enough: the new request would queue
// behind the still-running old loop. Bumping the token here makes the old loop
// see `token != gPlaybackToken` on its next frame and return, releasing port 81
// for the new request. This handler never touches the stream task's other
// state, so the new handler is free to publish a clean snapshot when it starts.
esp_err_t WebServer::handlePlaybackCancel(httpd_req_t* req) {
  // The device is the only authority that mints playback tokens, and it hands
  // the new one back in the reply. The page must not invent its own: a
  // page-local counter restarts at 0 on every reload while this device keeps
  // counting, so a fresh page's tokens were always older than ours and
  // handlePlayback() rejected every replay as superseded (409), which looked
  // like playback breaking entirely after a few switches.
  //
  // Bumping here also serves the original purpose: an in-flight replay holding
  // an older token sees the mismatch on its next frame, exits, and releases the
  // single-task port 81 for the request that is about to arrive.
  const uint32_t next = gPlaybackToken + 1;
  gPlaybackToken = next;
  gPlaybackActive = false;
  gPlaybackPending = true;  // the clip that owns this token has not started

  char json[48];
  snprintf(json, sizeof(json), "{\"ok\":true,\"token\":%lu}",
           (unsigned long)next);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}
