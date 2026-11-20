# 实现进度记录

> 本文件记录每个里程碑的**实际实现状态、实测数据与踩过的坑**。
> 与 `PROJECT_PLAN.md`（设计与方案）和 `docs/hardware.md`（硬件实测）配套阅读。
>
> 最后更新：2026-11-20

## 里程碑状态

| 阶段 | 状态 | 说明 |
|---|---|---|
| M0 硬件基线 | ✅ 完成 | 芯片、相机、SD、引脚、容量全部实测确认 |
| M0.5 GPIO38 冲突 | ✅ 完成 | BL 飞线至 GPIO42，相机+SD+背光并行通过 |
| M1 局域网直播 | ✅ 完成 | MJPEG 8.6fps / 1.06Mbps |
| M2 移动检测 | ✅ 完成 | 80×60 灰度帧差，静止零误报 |
| M3 事件录像 | ✅ 完成 | 触发后 10 秒 AVI，已验证可播放 |
| M4 管理与存储 | ✅ 完成 | NVS 配置持久化、热更新、容量清理、分页与手动停止 |
| M5 屏幕显示 | ✅ 完成 | 2026-11-13 TFT 点亮（详见 hardware.md 5.4），仅显示 5.3fps / 显示+直播 11.6fps |
| M6 本地交互 | ✅ 完成 | 按键与菜单已集成（ui_controller / button），随固件 0.5.0 运行 |
| M7 公网远程 | ✅ 代码完成 | 设备端 relay_client + 云端 Node 中继 + 冒烟测试 11/11 通过；待真机部署验证 |

## 当前运行参数（firmware 0.6.0）

```c
相机    : VGA 640x480, JPEG 质量 12, fb_count=2 (PSRAM)
检测    : 80x60 luma, 采样间隔 250ms
          灵敏度 65 -> 阈值 17
          minChangedBlocks = 3 (共 70 块)
          confirmSamples = 2
          cooldown = 30000ms
录像    : 触发后严格 10 秒, extendOnMotion = false
          单段实测约 2.6-2.9 MB, 110-125 帧
SD      : CLK=39 CMD=38 D0=40 (1-bit SDMMC)
显示    : ST7789 240x320, SCL=41 SDA=2 RST=21 BL=42 CS=45 DC=47
配置    : 全部存 NVS (namespace "camai"), 支持运行时热更新
中继    : relayEnabled 默认关, relayFps 1-15 默认 8
          RELAY_HOST 为空时中继休眠不连接
```

以上参数均可通过网页设置面板修改，无需重新编译。

## M1 实测数据

```
启动  : flash 16MB, psram 8MB, heap 321KB
WiFi  : Gearline, RSSI -28 dBm, IP 192.168.1.142
直播  : 8.6 fps, 129 KB/s (1.06 Mbps)
截图  : 16 KB JPEG
内存  : heap 250KB / psram 8057KB 稳定
```

## M2 实测数据

```
静止 25 秒 (43 次采样) : 峰值 5%, 误触发 0
挥手                   : 峰值 24%, 触发成功
检测器视角             : 80x60 灰度, luma 8-241
```

## M3 实测数据

```
触发 -> 录像 -> 落盘 : 端到端通过
单段               : 113-125 帧, 2.58-2.85 MB
AVI 结构           : RIFF size 与实际文件完全匹配
JPEG 帧校验        : 159/159 帧头尾完整 (SOI+EOI)
播放               : 用户实测可正常播放
删除接口           : 11 个文件全部删除成功, 空间正确回收
```

## M4 实测数据

```
GET  /api/v1/config   : 返回 15 项设置, 格式正确
POST /api/v1/config   : 保存成功, NVS 写入确认
越界校验              : sensitivity=999 -> HTTP 400, 原值不变
重启持久化            : 设置完整保留 (sens=80 cooldown=20 clip=8)
运行时热更新          : cooldown 5s + clip 3s -> 40 秒内 7 段录像
                        片段从 109 帧降到 18-42 帧, 配置立即生效
清理保护              : 录像中调用清理 -> 409 "recording in progress"
清理执行              : min_free=30000 时 removed=30, 空间正确回收
```

## M4 设计要点

**配置与模块解耦。** 网页层只负责校验和写入 NVS，然后置一个 `gConfigDirty` 标志；
主循环在安全时机重建各模块的 Config 并应用。这样：
- HTTP 任务不会在帧关键路径上改参数
- 所有设置翻译逻辑集中在 `main.cpp` 的一处

**录像时长在开始时冻结。** `clipTargetMs_` 在片段启动时确定，
之后改设置不会截断正在写的文件——否则用户在录像中途点保存会得到半截视频。

**清理只在录像之间执行。** 录像期间删除文件会破坏 FAT 目录表，
因此清理接口在录像中直接返回 409，主循环的自动清理也有 `!isRecording()` 保护。

**清理分批次。** 单次最多删 30 个文件，避免长时间占住主循环影响推流。

## M5 实现要点（详见 hardware.md 5.4）

三个坑均已修复：
- JPEGDEC 在栈上声明导致栈溢出 → 堆分配 `new JPEGDEC`
- 背光 LEDC 通道与相机 XCLK 冲突 → 背光改 `LEDC_CHANNEL_2 / TIMER_2`
- ST7789 8 位寄存器被误发成 16 位 → TFT_eSPI 新增 `spiWriteData8`

实测：仅显示 5.3fps；显示+直播并行 11.6fps（直播不受影响）。

## M6 实现要点

按键（GPIO0 配网 / GPIO46 预留）与 UI 菜单（ui_controller）已集成进
main.cpp 主循环，随固件 0.5.0 一起运行。无新增缺陷记录。

### 面板 UI 改版（2026-11-20，自绘精致风格）

用户反馈原界面"太简陋"。选择**在现有 ST7789 裸驱动上自绘精致 UI**，
不引入 LVGL（评估后放弃：需 ~200KB PSRAM 渲染缓冲 + 渲染回调适配 +
重写 ui_controller 交互层，风险高收益小）。

改动全部在 `src/display/display_service.cpp/.h`，零新依赖：

- **主题系统**（`uitheme` 命名空间）：暗色背景 + 绿色 accent + 青色 info +
  黄色 warning + 红色 error 的统一配色
- **新绘制原语**：`drawCard`（圆角卡片，内部 1 次 fillRect + 角部逐行）、
  `drawBar`（圆头进度条）、`fillCircle`（中点扫描线圆）、
  `drawWifiIcon`（WiFi 弧线/红叉）、`drawRecIcon`（圆环+红点）、
  `drawSdIcon`（SD 卡形+余量条）、`drawRelayIcon`（信号碟）、
  `drawBigNum`（3x5 大号数字，用于计数读数）
- **Preview 页**：状态栏重绘为图标条——WiFi 图标（弧线/断线红叉）、
  录像圆环（闪烁）、中继碟、右侧 SD 卡形+圆头余量条（按 freeMb/totalMb 比例）
- **Events 页**：存储卡片（SD 图标 + free/total + 余量条）+ 剪辑卡片
  （大号数字显示写入数 + FAIL 红字 + 最近文件名）
- **Network 页**：连接卡片（WiFi 图标 + 状态 + RSSI）+ IP 卡片（scale2 大字 IP）+
  系统卡片（uptime/heap/FW + 堆健康条，>30% 绿色否则红色）
- **Settings 页**：菜单行收进一张圆角卡片，选中行变为内嵌高亮圆角条
  （浏览=暗绿、编辑=暗橙），编辑值后缀 "<" 光标并变黄
- **标题栏**：左侧 accent 圆角竖条（各页不同色）+ scale2 页名 + 右上角页码

实测编译通过，RAM 23.0% (+4 bytes)，Flash 36.5% (+2500 bytes)。
暂无真机截图对比；点亮效果待烧录确认（特别是圆角弧线的 40MHz SPI 下逐行
fillRect 的耗时——文本页只在重绘时画一次，预览帧不受影响）。

### 面板 UI 二轮调整（2026-11-20，烧录后用户反馈）

用户反馈两点：Settings 菜单字体太小；Events 页想直接看到已保存的录像列表。

- **Settings 字体加大**：菜单行从 5x7 scale1（rowH 16）改为 scale2 12x14
  字形（rowH 26），标签/值同尺寸右对齐；高亮条与编辑橙色底同步放大。
- **Events 页新增"最近录像"列表**：`DisplayService::scanClips()`
  （display_service.cpp）扫描 `/events/<日期>/EVT_*.avi`，FAT 目录序近似
  新到旧，取最新 4 条（`kMaxClipList`）；每行画播放小圆钮 + 短文件名
  （去 `EVT_`/`.avi` 前后缀）+ KB 大小。**只在页面重绘时扫描一次**
  （SD 目录遍历几十 ms/夹，禁止逐帧跑）；存储卡与剪辑统计卡压缩为
  紧凑两行给列表腾位。当前为只读展示，播放/删除仍走网页端。

编译 RAM 23.0% (75420)，Flash 36.6% (1151553)，已烧录真机。

### 竖屏预览下部"雪花"根因修复（2026-11-20，缺陷 #20）

现象：方向 2（竖屏）预览时下部图像带持续雪花噪点，第一次只把 letterbox
留白改白色未解决（噪点在图像区内部，不是留白）。根因在
`composeLetterboxPortrait()`（display_service.cpp:1181）：
`imgH = kDispW * 3 / 4` 误用了宽 320（得 240 行），而源帧只有 240 行高，
`srcRow = frameBuf_ + (gy*4/3)*kDispW` 在 gy>179 后越界读到帧缓冲之后约
50 KB 的 PSRAM，即雪花。修正为 `imgH = kDispH * 3 / 4`（180），`imgW =
kDispW * 3 / 4`（240），yOff=70 不变，与注释描述的 240×180 letterbox 一致。
编译 RAM 23.1%(75564) / Flash 36.8%(1156525)，已烧录。

### Events 屏幕选中 + 回放（2026-11-20）

用户在只读列表基础上要求"能选中和回放"。新增屏幕端录像回放：

- **新建 `src/storage/avi_reader.h`**（header-only）：顺序 MJPEG-AVI 读取器。
  校验 RIFF/AVI 头后向前扫描 `00dc` 帧块（不依赖 idx1 索引，兼容历史
  hdrl 尺寸超标的旧录像），跳过非帧块与超尺寸损坏块；`progressPercent()`
  按 file position/file size 估算播放进度。单帧 JPEG 暂存缓冲 96KB（PSRAM）。
- **display_service**：新增回放状态机（Idle/Playing），完全运行在显示任务
  上（SPI/JPEGDEC 单一持有者，无需互斥）。`startPendingPlayback()` 由
  `requestPlayback()` 触发（UI 任务只置请求位，SD 打开耗时几十 ms 不占
  UI）；`playTaskLoop()` 按 10fps（录制标称帧率）逐帧解码推屏，复用
  `decodeAndShow` 的 JPEGDEC 路径与竖屏 letterbox 合成；底部叠加进度条
  pill + 暂停双竖条图标。结束（EOF/停止/错误）后自动回 Events 页并重绘。
- **选中条**：Events 列表选中行画内嵌高亮 pill + 播放钮反色，与 Settings
  菜单交互语言一致。
- **按键语义**（仅 Events 页生效，其他页不变）：
  - GPIO46 短按：移动选中条（在列表内循环）
  - GPIO46 长按：开始回放选中录像
  - 回放中 GPIO46 短按：暂停/继续；GPIO46 长按或 GPIO0 短按：退出回放
- 编译 RAM 23.1% (75540)，Flash 36.7% (1153909)，已烧录真机。

### 回放卡死修复（2026-11-20，缺陷 #15）

用户反馈：长按回放后不播放、停在 Events 页，GPIO46/GPIO0 均无响应。

**根因**：`AviReader::begin()` 原来从头按 RIFF chunk 结构走——遇到
`LIST....movi` 时按声明 size 整体跳过，直接落到 `idx1` 索引区；idx1 每个
16 字节条目开头同样是 `"00dc"`，被误判为帧，读出 flags/offset 垃圾字节，
JPEG 解码必失败 → 屏幕从不更新（停留在上次的 Events 画面）。此时按键
已被回放状态机接管（GPIO46 短按=暂停无视觉变化、GPIO0=停止后重绘同一
列表），且要扫完整个 idx1 才 EOF——表现为"全部按键失灵的假死"。

**修复**（`avi_reader.h` + `display_service`）：
1. `begin()` 头部定位改为 `findFirstFrame()`：64KB 上限内**原始字节扫描**
   第一个 `"00dc"`（不按 LIST size 跳块；窗口 256B、跨读重叠 3 字节防
   边界漏配；命中后读 8B 头校验 0<size≤512KB，停在 payload）。
2. `seekNextFrame()` 跳块加 `remain = file size - position` 边界检查，
   损坏尾块直接判 EOF 而非跑飞。
3. 防御性兜底：`renderPlaybackFrame()` 返回成功与否，
   `playTaskLoop()` 连续 30 帧（约 3s）解码失败自动 `endPlayback("BAD FILE")`
   退出——任何坏文件最多"卡"3 秒后回列表，不再假死。

编译 RAM 23.1% (75540)，Flash 36.7% (1154289)，已烧录真机待复测。

**补充（同日）**：用户指出 Events 页是"单向死角"——GPIO46 短/长按都被
选中/回放占用，GPIO0 短按原来只弹提示不翻页。修复：Events 页 **GPIO0
短按 = 翻到下一页**（cyclePage），其他页 GPIO0 行为不变（长按仍=配网）。

**补充 2（同日，缺陷 #16）**：Settings 编辑态数值只能加不能减。
修复：编辑态**按住 GPIO46 ≥1s 开始递减，之后每 400ms 连减**（短按仍 +1）；
进入编辑/每次短按后重置重复计时器；菜单提示语更新为
"EDIT: TAP +1, HOLD -1, GPIO0 EXIT"。已烧录真机。

**补充 3（同日，缺陷 #17）**：网页画面左右镜像（用户拍键盘照片证实
字符反向）。根因：本模块 OV2640 默认输出镜像，而 `hflip` 默认 false 且
网页设置页未暴露该开关。修复：`Settings::hflip` 默认改 true（启动时
`applyImageSettings` 对传感器 `set_hmirror` 校正）；网页设置页新增
"画面左右翻转 (镜像)" 复选框（s_hflip→hflip），可随时改。已烧录真机。
（后确认：镜像本身已生效，用户当时没点保存——保留开关与默认值。）

**补充 4（同日，缺陷 #18）**：网页直播容易卡死，须手动停止重开。
根因组合：(a) 设备端 `handleStream` 在 acquire 失败时 delay(20) 无限
重试，传感器卡死时连接永不结束也不报错；(b) MJPEG `<img>` 在设备侧
停止发帧时**不触发 onerror**（TCP 静默），浏览器无从感知。修复两层：
- 设备端：连续 5s 无帧主动关闭流（释放 socket + streamClients_）；
  /api/v1/status 新增 `stream_frame_age_ms`（当前流最新帧龄）。
- 前端：1s 状态轮询里做看门狗——`streaming && age>3000` 连续 2 次
  （约 2s）自动重连（先置 BLANK_PIXEL 再 300ms 后重设带新时间戳的
  /stream URL），覆盖层提示"直播中断，正在重连…"。已烧录真机。

**补充 5（同日，缺陷 #19，看门狗不生效）**：实测卡死时 `/api/v1/status`
返回 `stream_clients:0, stream_frame_age_ms:0`（但 fps=5、运动检测正常），
说明设备端流 handler 早已退出、浏览器 `<img>` 冻在末帧，而看门狗没触发。
根因两个：
1. `stream_frame_age_ms` 被 `streamClients()>0` 限制——流一断 clients=0，
   age 被强制回 0，`age>3000` 永远不成立，前端不会重连。
2. 设备端 `handleStream` 首次 `send_chunk` 超时（SO_SNDTIMEO 200ms）就
   `break` 关流——偶发 TCP 窗口停顿就误杀整条流；且那段"单 httpd task
   会拖垮 API"的注释已过时（流早已独立在 port 81 的 httpd task）。
修复：
- status 的 `stream_frame_age_ms` 去掉 clients 限制，流死后 age 持续增大，
  让前端能看见"卡住"。
- 前端看门狗判据改为 `streaming && (clients==0 || age>3000)`，覆盖
  "设备流已断但 age 还未来得及涨"的情形。
- 设备端 SO_SNDTIMEO 200ms→1000ms；send_chunk 失败改为累计 5 次才关流
  （中间 delay(50) 重试），流开始时重置 gLastStreamFrameMs。
设备 IP=192.168.1.144，已烧录。

**补充 6（同日）**：屏幕方向默认改 index 2（config_service.h
`displayRotation` 默认 0→2，注释说明 0/2 竖屏、1/3 横屏，2 为竖屏 180°）。
注意代码默认仅在 NVS 未存该键时生效——设备里已有旧值 0，故另经
POST /api/v1/config `{"display_rotation":2}` 落 NVS，实测回读=2。
已烧录。

**补充 7（同日）**：用户反馈竖屏预览下部"超出显示范围"区域显示雪花噪点。
根因：letterbox 上/下各 70px 留白用 `memset(...,0)` 填黑，而 0x0000 在
反相面板上并非稳定黑，未被图像覆盖的 GRAM 区露出随机噪声。修复：
`composeLetterboxPortrait()` 用 0xFFFF 填满 rotBuf_（留白=干净白），
`initSt7789()` 末尾整屏清屏也从 0x0000 改 0xFFFF，避免上电残留雪花。
编译 RAM 23.1%/75564，Flash 36.8%/1156517，已烧录。

## M7 实现要点（公网远程，代码完成待真机部署）

**架构**（PROJECT_PLAN.md 5.3）：设备主动出站 WSS 连云端中继，
家庭网络零开放端口；浏览器连中继观看。

**设备端**（`src/network/relay_client.h/.cpp`）：
- 手写 RFC6455 WebSocket 客户端，over WiFiClientSecure TLS
- 协议：设备→`{"type":"hello","token","dev","ver"}` + binary=JPEG 帧；
  服务器→`hello_ok / start_stream{fps} / stop_stream / ping / set_quality{fps}`
- 零掩码 key（RFC 允许，省 CPU）；Sec-WebSocket-Accept 用 mbedtls_sha1+base64 校验
- **看才推流**：`viewerActive_` 门控，无观看者时不采帧不发送
- 心跳：服务器 30s ping，设备 40s 超时；断线指数退避 2s→60s 重连
- TLS：`RELAY_CA_CERT` 证书 pin（secrets.h），为空则**拒绝连接**；
  仅 `-DRELAY_INSECURE_SKIP_CERT_VERIFY` 才允许 setInsecure（调试用）
- 独立任务 12KB words 栈，优先级 1

**云端中继**（`server/relay.js`，Node 零依赖，~330 行）：
- 手写 RFC6455 服务端（upgrade/帧编解码/掩码/ping-pong）
- 端点：`/devices/{id}` 设备 WS、`/view` 观看 WS、`/stream.mjpeg`
  （multipart，`<img>` 直接可用）、`/health`
- 设备 hello token 校验（`RELAY_TOKEN`）；观看侧 Basic Auth（`RELAY_VIEW_AUTH`，
  timingSafeEqual 比较）
- 看才推流：首 viewer 连入发 `start_stream`，最后一个离开发 `stop_stream`
- 心跳 30s ping / 40s 超时；binary 帧扇出到 WS viewer + MJPEG 客户端

**配置**：`Settings` 新增 `relayEnabled`（默认 false）/`relayFps`（1-15，默认 8），
NVS 键 `relayen`/`relayfps`，JSON `relay_enabled`/`relay_fps`；
`/api/v1/status` 增加 `relay` 对象（enabled/connected/viewer/frames_sent/
bytes_sent/reconnects/last_error）。

**部署冒烟测试**（`server/smoke_test.js`，11 项全过）：
health 初始态、WS 握手 Accept 头、hello 认证、错误 token 拒绝、
start_stream 下发、binary 帧透传、stop_stream、MJPEG 流。

### M7 期间发现的缺陷

**13. 设备通道被未认证连接抢占（冒烟测试发现）**

**现象**：模拟"错误 token 的连接"后，正常设备再也收不到 start_stream。

**根因**：`deviceSock = ws` 在 hello 认证**之前**赋值——任何连上
`/devices/{id}` 的连接都会先顶掉已认证设备，认证失败销毁后设备槽位为空。

**修复**：只有 hello 认证成功后才 `deviceSock = ws`；替换旧连接前先检查
旧连接是否存活。

**14. Windows/Node 下升级 socket 只发 'end' 不发 'close'（冒烟测试发现）**

**现象**：viewer 断开后 `stop_stream` 永远不发出，服务器认为 viewer 还在。

**根因**：Windows + Node v24 上，客户端 `destroy()` 后升级 socket 只触发
`end`（FIN），不触发 `close`。handler 只监听了 `close`/`error`。

**修复**：设备与 viewer 的 cleanup 同时挂到 `close` + `end` + `error`。

**教训**：不要假设 socket 生命周期事件在所有平台表现一致；连接状态
清理逻辑应挂全部三个事件。

## 已解决的关键缺陷（按发现顺序）

### 1. 背景模型吸收运动（M2/M3 期间）

**现象**：持续挥手时 `change_percent` 恒为 0。

**根因**：背景以 `>>5`（约 8 秒时间常数）每帧更新，持续运动会被背景模型"学会"，
之后就不再判定为变化。

**修复**：
- 更新系数改为 `>>6`（约 16 秒）
- **冻结正在偏离的区块**——运动物体不允许教模型自己的外观

**效果**：峰值变化 1% → 52%。

### 2. 亮度补偿抹平运动（M2/M3 期间）

**现象**：手占画面较大比例时检测失效。

**根因**：用整帧**平均**亮度做全局补偿。手挡住大片区域时平均值被拉偏，
减掉后连运动差异一起抵消了。

**修复**：改用**中位数**。手占少数区块时中位数接近真实光照偏移，
变化区块仍保持偏离。

### 3. 首次触发被冷却吞掉

**现象**：开机后 30 秒内怎么动都不触发。

**根因**：`lastTriggerMs_` 初始为 0，`millis() - 0 >= 30000` 在启动初期永假。

**修复**：加 `triggerCount_ == 0` 短路，保证第一次一定放行。

### 4. 持续运动只触发一次（用户反馈的核心问题）

**现象**：用户报告"不是每次画面有变化都会触发保存"。

**根因**：触发条件是 `nowActive && !active_`（上升沿）。
运动持续期间 `nowActive` 和 `active_` 都为 true，边沿**永远不会再出现**。
所以一次持续运动只产生一个片段，之后无论动多久都不再录。

**修复**：
- 触发条件改为 `cooled`（不再要求上升沿），运动持续时每 30 秒可再次触发
- 运动停止时把 `lastTriggerMs_` 回拨，使下一段运动能立即触发
- 新增 `noteActivity()`，用检测器的 active 状态驱动片段延长

**验证**：75 秒内触发次数 1→2→3→4，按 30 秒周期稳定增长。

### 5. 录像被无限延长（修复 4 引入）

**现象**：运动持续时录像不断延长，最长 60 秒，13 MB 一段，
违背"10 秒事件录像"的定位。

**根因**：`extendOnMotion = true` 且只要 10 秒内有运动就延长。

**修复**：默认 `extendOnMotion = false`，严格录 10 秒。
延长能力保留为可配置项，供将来"要求录完整事件"的场景使用。

**验证**：修复前 13 MB/段，修复后稳定 2.6 MB/段。

### 6. 删除文件假成功（用户反馈）

**现象**：网页点"删除"看似成功，文件却还在。

**根因**：`SD_MMC.remove()` 对**不存在的路径也返回 true**。
接口直接把该返回值当作删除结果，于是永远报告成功。

**修复**：
- 先用 `SD_MMC.exists()` 确认再删，返回真实的 `ok`
- 响应增加 `remaining` 字段，供前端判断是否删到最后一页

**验证**：连续删除 5 个，总数 57→52，全部成功。

### 7. 事件列表只显示 40 条（用户反馈）

**现象**：删除看不到的文件，且删掉一个总数不变。

**根因**：列表硬编码 `kMaxListed = 40`，超出部分在 UI 中不可见，
自然也无法删除。实际卡里有 50+ 个文件。

**修复**：改为 `offset` / `limit` 分页，响应增加 `total` 字段；
前端加上一页/下一页与页码显示。

**验证**：`total=53 > 40`，翻页正确，全部文件可见可删。

### 8. 缺少手动停止录像（用户反馈）

**现象**：正在录像时无法中止，只能等它自然结束。

**修复**：新增 `POST /api/v1/recording/stop`，
`EventRecorder::stopRecording()` 立即收尾当前片段。
**停止不等于丢弃**——文件正常保存且可播放，因为中断的录像仍是有效证据。

**验证**：录像中（129 帧）调用停止，clips 1→2，得到 124 帧、
SOI/EOI 全完整、RIFF 尺寸正确的有效 AVI。

### 9. 批量删除首次失败：双重 URL 编码

**现象**：批量删除 3 个文件，只处理了 1 个（`rejected=1`）。

**根因**：前端把整个列表 `encodeURIComponent`，`|` 变成 `%7C`、
`/` 变成 `%2F`。服务器按字面 `|` 分割时，收到的是**一个不含任何
`|` 的整体字符串**，自然只能解析出一项。

**修复**：服务器先做 percent-decode，**再**分割。
解码必须在分割之前——顺序不可颠倒，因为编码后的文本里根本没有
字面分隔符。

**验证**：3 个一批 `deleted=3`；10 个一批 `deleted=10`；
不存在的文件计 `missing` 不误报；路径穿越（`../`）被拒绝。

### 10. NVS 里残留了"让检测器失聪"的参数（用户反馈）

**现象**：用户反馈"停止录像按钮不见了、移动镜头不保存画面"。

**排查**：状态 API 显示 `changed=17 (24%)` 却 `active=false`、
`confirm=0`、`triggers=0`；`samples` 和 `luma_sum` 都在涨，
说明解码正常、画面确实在变，但变化没转化成触发。

**根因**：NVS 里存着 `sensitivity=25, min_changed=35, cooldown_s=60`。
`min_changed=35` 要求 70 个区块中有 35 个（50%）变化，
而实际移动镜头只改变 10~17 个区块，**永远达不到门槛**。
这些参数是之前在网页上调整后保存的，重启后从 NVS 恢复并持续生效。

**修复**：
- `min_changed` 合理上界从 70 收紧到 20（约 29%），
  `updateFromJson` 与 `validate()` 同步，越界直接 HTTP 400
- 网页滑块上限改为 20，并标注"共 70 块，建议 3–8"

**验证**：`min_changed=35` 现被 HTTP 400 拒绝。

**教训**：**默认值正确不代表运行时正确**。凡是能持久化的用户设置，
排查异常时必须先读回实际生效值，不能只看源码默认值。

### 11. 背景"冻结"是单向死锁，导致永久录像（用户反馈的核心问题）

**现象**：用户报告三个表面独立的症状——
① 停止录像按钮点了"没反应"；② `min_changed=3` 时画面静止却一直在保存；
③ `min_changed=4` 时 UI 提示"高于阈值？请调低最小变化区块"。

**排查**：状态显示 `confirm=255`（饱和）、`triggers=196`、`clips=70`，
停止接口实测 297ms 返回 `stopped=True`（后端正常）。

**根因**：背景更新逻辑中"偏离的区块永久冻结"是**单向死锁**——
区块一旦偏离超阈值就永远冻结，背景不再更新。
用户之前移动过镜头，画面永久改变，7 个区块永远判"变化中" →
`hot` 恒真 → 每 30 秒冷却一到就触发 → 不停录像。
停止按钮其实成功了，但检测器仍永久激活，冷却一到立刻重新开录，
界面上看就像"没反应"。那个"请调低阈值"提示在静态噪声下也误报，
且文案恰好说反（噪声低于阈值是正常，调低反而更糟）。

**修复**：
- 背景更新改为"**超时采纳**"：连续偏离 ≥120 个采样（30 秒）的区块
  直接采纳当前画面为背景（认定为场景真的变了，不是路过的人）；
  普通偏离区块以 >>8（约 64 秒时间常数）极慢适应，不会被路过的人教会
- 手动停止后 **15 秒触发抑制窗口**，让停止操作在界面上可见
- 删除那个误导性的阈值提示文案

**验证**：
- 静置 75 秒：`changed=0`、零误触发、零新增片段
- 高灵敏度制造录像后点停止：`stopped=True`、3 秒后 `recording=False`、
  **15 秒内无重新开录**（抑制窗口生效）
- `min_changed=3` 静置不再常录

### 12. 录像中删除/保存设置卡死（用户反馈）

**现象**：开始录像后，删除文件和保存设置都卡在"进行中"。

**根因**：上次给删除接口加的"统计剩余文件数"是 **O(全库遍历)**——
打开每个目录、每个文件数一遍。录像时 FAT 锁被 10fps 写入占用，
遍历极慢（实测单删 3.2 秒）。ESP 的 HTTP 服务器是**单任务**，
删除处理不返回，保存/停止/状态轮询全部排队卡死。

**修复**：删除接口不再统计剩余数，只返回 `deleted/missing/rejected`；
UI 本来就会重新拉取事件列表获取最新总数。

**验证**（录像中实测）：
- 保存设置：301ms
- 删除单个：122ms（原 3195ms，**快 26 倍**）
- 批量删 5 个：472ms

## 工具链上的坑

### ESP32-S3 板卡定义不匹配 16MB Flash

`board = esp32-s3-devkitc-1` 定义为 **8MB Flash 的 N8 型号**，
用于 16MB 的 N16R8 会导致启动循环。

**必须**在 `platformio.ini` 显式覆盖：

```ini
board_upload.flash_size = 16MB
board_upload.maximum_size = 16777216
board_build.flash_mode = qio
board_build.f_flash = 80000000L
```

另注：该框架版本**只提供 80MHz bootloader**，没有 40MHz 变体，指定 40MHz 会直接编译失败。

### 不要用 PowerShell 做源码文本替换

`Get-Content -Raw` + `Set-Content` 按系统默认 GBK 编码读写，
会把源码里的中文破坏成 `?????`，且不可逆。

**要求**：修改源码一律用编辑工具（edit），不用 shell 文本替换。

### 串口日志采集不可靠

115200 波特率下用 PowerShell `ReadLine` 采集会丢数据。
**调试应走 HTTP 状态 API**，不要依赖串口日志判断行为。

## 待完成事项

- [x] ~~M4：配置持久化到 NVS~~ **已完成**
- [x] ~~M4：容量水位策略与自动清理~~ **已完成**
- [x] ~~M4：网页设置页~~ **已完成**
- [x] ~~M5：ST7789 驱动与本地预览~~ **已完成 2026-11-13**（见 hardware.md）
- [x] ~~M6：按键与菜单~~ **已完成**（固件 0.5.0）
- [x] ~~M7：公网中继（设备端 + 云端）~~ **代码完成 2026-11-20**，冒烟测试 11/11
- [ ] M7 真机部署：阿里云 ECS 部署 relay.js + TLS 终结（Nginx/Caddy）+ 烧录真机联调
- [ ] 安全：网页认证（公网开放前必须完成；中继侧 Basic Auth 已就位，设备 Web 尚无）
- [ ] 复杂场景 / 夜间低照度下的 JPEG 大小实测
- [ ] M8（规划）：按 PROJECT_PLAN 第 12 节推进
