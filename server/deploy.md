# 云端部署说明（relay + nginx）

本文是把 `server/` 部署到一台公网 Linux 服务器（本项目实测为阿里云 ECS / Alibaba
Cloud Linux 3）的完整步骤。做完之后，外网用浏览器登录即可获得与局域网页面完全等
同的功能。

部署形态一共三层：

```
手机/电脑 ──HTTPS 443──> nginx ──127.0.0.1:9000──> relay.js ──出站 WSS──> 设备
              (TLS + 登录鉴权)         (隧道 + 帧泵)      (443, 设备主动外连)
```

设备只**向外**拨号，家里网络不需要开任何端口。

---

## 0. 前置条件

| 项 | 要求 | 本项目实测值 |
|---|---|---|
| 服务器 | 1 核 2G 起，公网 IP 可达 | 阿里云 ECS，47.103.200.7 |
| 系统 | systemd + nginx + dnf/apt | Alibaba Cloud Linux 3 (Anolis)，kernel 5.10 |
| Node.js | ≥ 18（用到 `crypto.timingSafeEqual`、`base64url`） | v20.19.5，装在 `/opt/nodejs` |
| nginx | 需要 `http_ssl_module`、`http_v2_module`、**`http_sub_module`** | 1.28.3（宝塔面板自带，模块齐全） |
| 端口 | 只需对外开 **443** | relay 绑 `127.0.0.1:9000`，安全组不要放行 |

> ⚠️ **9000 必须只对本机开放**。relay 的隧道路由没有任何鉴权，认证全在 nginx。
> 代码里 `RELAY_BIND` 默认就是 `127.0.0.1`，这是第二道保险——安全组规则可能被改，
> 但绑定地址不会；两层都生效才算稳妥。若把 9000 暴露到公网，等于绕过登录页直连设备。

检查 nginx 模块：

```bash
nginx -V 2>&1 | tr ' ' '\n' | grep -E 'sub_module|auth_request|http_v2'
# 三个都要出现。sub_module 用来把 __CAMAI_STREAM_BASE 注入设备页面。
```

---

## 1. 安装 Node.js

系统自带的 Node 版本通常太旧（Anolis 的 dnf 模块最高到 16），直接从官方装二进制：

```bash
cd /tmp
curl -fsSLO https://nodejs.org/dist/v20.19.5/node-v20.19.5-linux-x64.tar.xz
tar -xJf node-v20.19.5-linux-x64.tar.xz
mv node-v20.19.5-linux-x64 /opt/nodejs
ln -sf /opt/nodejs/bin/node /usr/local/bin/node
ln -sf /opt/nodejs/bin/npm  /usr/local/bin/npm
ln -sf /opt/nodejs/bin/npx  /usr/local/bin/npx
node -v    # v20.19.5
```

relay 零第三方依赖，**不需要 `npm install`**，也不需要 `package.json`。

---

## 2. 建服务用户与目录

```bash
useradd -r -s /sbin/nologin -d /opt/cam-ai camai
mkdir -p /opt/cam-ai /etc/cam-ai/tls /var/log/cam-ai /var/www/cam-ai
```

放到服务器上的文件：

| 路径 | 来源 | 属主/权限 |
|---|---|---|
| `/opt/cam-ai/relay.js` | `server/relay.js` | `camai:camai` 644 |
| `/opt/cam-ai/smoke_test.js` | `server/smoke_test.js` | `camai:camai` 644 |
| `/etc/cam-ai/tls/ca.crt` | 本机生成 | `root:root` 644 |
| `/etc/cam-ai/tls/ca.key` | 本机生成 | `root:root` **600** |
| `/etc/cam-ai/tls/server.crt` | 本机生成 | `root:root` 644 |
| `/etc/cam-ai/tls/server.key` | 本机生成 | `root:root` **600** |
| `/etc/cam-ai/relay.env` | 手写 | `root:root` **600** |
| `/var/www/cam-ai/camai-root-ca.crt` | `ca.crt` 的副本 | `root:root` 644 |
| `/www/server/panel/vhost/nginx/cam-ai-relay.conf` | `server/nginx-cam-ai-relay.conf` | 644 |

上传示例（从开发机执行）：

```bash
scp -i ~/.ssh/id_ed25519_aliyun server/relay.js server/smoke_test.js root@<IP>:/tmp/
ssh -i ~/.ssh/id_ed25519_aliyun root@<IP>
install -o camai -g camai -m 644 /tmp/relay.js /tmp/smoke_test.js /opt/cam-ai/
```

> 上传前先 `node --check server/relay.js` 自检语法——服务器上才发现在第 500 行有
> 语法错误的话，systemd 会一直重启同一个崩溃进程。

---

## 3. 生成自签 TLS 证书

没有域名时用 IP + 自签 CA。**关键点：证书的 SAN 必须同时写 `DNS:` 和 `IP:` 两种
形式**，原因见下面的「排障」一节。

```bash
cd /etc/cam-ai/tls

# --- 根 CA（10 年）---
openssl ecparam -name prime256v1 -genkey -noout -out ca.key
openssl req -x509 -new -key ca.key -sha256 -days 3650 \
  -subj "/C=CN/O=CAM-AI/CN=CAM-AI Relay Root CA" \
  -addext "basicConstraints=critical,CA:TRUE" \
  -addext "keyUsage=critical,keyCertSign,cRLSign" \
  -out ca.crt

# --- 服务器证书（10 年，SAN 双写）---
openssl ecparam -name prime256v1 -genkey -noout -out server.key

cat > san.cnf <<'EOF'
[v3_server]
subjectAltName=DNS:47.103.200.7,IP:47.103.200.7
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
EOF

openssl req -new -key server.key -subj "/C=CN/O=CAM-AI/CN=47.103.200.7" -out server.csr
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out server.crt -days 3650 -sha256 -extfile san.cnf -extensions v3_server

# 确认 SAN 真的写进去了（这一步别省，写错形式的话设备端只会给你一个 -9984）
openssl x509 -in server.crt -noout -text | grep -A1 'Subject Alternative Name'
# 期望：DNS:47.103.200.7, IP Address:47.103.200.7

chmod 600 ca.key server.key
chmod 644 ca.crt server.crt

# 校验链
openssl verify -CAfile ca.crt server.crt
```

把 `ca.crt` 复制一份供下载：

```bash
cp ca.crt /var/www/cam-ai/camai-root-ca.crt
chmod 644 /var/www/cam-ai/camai-root-ca.crt
```

设备端 `include/secrets.h` 的 `RELAY_CA_CERT` 就是这段 PEM（逐字粘贴，换行用
`"...\n"` 拼接）。**CA 变了就要重新烧录固件**；只重签服务器证书则不用。

换 IP 或加域名时重跑 server 证书部分即可，`ca.crt` 保持不变。

---

## 4. 写 relay.env

```bash
cat > /etc/cam-ai/relay.env <<'EOF'
RELAY_PORT=9000
RELAY_BIND=127.0.0.1

# 设备 hello 帧的预共享令牌，必须与 secrets.h 的 RELAY_TOKEN 一致
RELAY_TOKEN=<32 字节 hex 随机串>

RELAY_DEVICE_ID=cam-01

# 旧版 Basic Auth（/stream.mjpeg 与 /view 的兜底，配置为空则关闭）
RELAY_VIEW_AUTH=<user>:<pass>

# M9 登录（三项任一为空时 /login 与 /dev* 会拒绝一切请求）
RELAY_LOGIN_USER=<登录用户名>
RELAY_LOGIN_PASS=<登录密码>
RELAY_SESSION_SECRET=<32 字节 hex 随机串>
EOF

chmod 600 /etc/cam-ai/relay.env
```

生成随机串：

```bash
openssl rand -hex 32
```

| 变量 | 作用 |
|---|---|
| `RELAY_PORT` | 监听端口，默认 9000 |
| `RELAY_BIND` | **监听地址，默认 `127.0.0.1`**。relay 自身的隧道路由没有鉴权（鉴权全在 nginx），所以默认只绑回环；`ss -lntp \| grep 9000` 应显示 `127.0.0.1:9000` 而不是 `*:9000` |
| `RELAY_TOKEN` | 设备认证。错了设备连不上，但登录页仍可用 |
| `RELAY_DEVICE_ID` | 多设备时的设备标识，需与固件 `Config.deviceId` 一致 |
| `RELAY_VIEW_AUTH` | M7 时代的 Basic Auth，M9 后仅作旧客户端兜底 |
| `RELAY_LOGIN_USER/PASS` | 登录页账号密码。**改完必须 `systemctl restart`** |
| `RELAY_SESSION_SECRET` | 会话 Cookie 的 HMAC 密钥。换掉即让所有已登录会话失效 |

---

## 5. systemd 单元

写到 `/etc/systemd/system/cam-ai-relay.service`：

```ini
[Unit]
Description=CAM-AI cloud relay (WebSocket + MJPEG gateway)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=camai
Group=camai
WorkingDirectory=/opt/cam-ai
EnvironmentFile=/etc/cam-ai/relay.env
ExecStart=/usr/local/bin/node /opt/cam-ai/relay.js
Restart=always
RestartSec=3
KillSignal=SIGTERM
TimeoutStopSec=10

# 加固：relay 不需要文件系统、不需要特权、不派生进程
NoNewPrivileges=true
PrivateTmp=true
PrivateDevices=true
ProtectSystem=strict
ProtectHome=true
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectControlGroups=true
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
RestrictNamespaces=true
LockPersonality=true
# Node 的 JIT 需要可写可执行内存，这一项必须保持 false
MemoryDenyWriteExecute=false

StandardOutput=journal
StandardError=journal
SyslogIdentifier=cam-ai-relay

[Install]
WantedBy=multi-user.target
```

```bash
systemctl daemon-reload
systemctl enable --now cam-ai-relay
systemctl status cam-ai-relay --no-pager
journalctl -u cam-ai-relay -n 20 --no-pager
```

正常日志：

```
[relay] listening on 127.0.0.1:9000 (device token set)
[device] authenticated dev=cam-01           # 设备连上后
```

缺登录配置时会有告警：

```
[relay] WARNING: login not configured (RELAY_LOGIN_USER/PASS/SESSION_SECRET)
        - /login and /dev* will refuse every request
```

`MemoryDenyWriteExecute=true` 会让 Node 直接启动失败（`Fatal error ... v8`）。
这是唯一必须留 `false` 的加固项。

---

## 6. nginx 配置

把 `server/nginx-cam-ai-relay.conf` 放到宝塔的 vhost 目录（其它发行版则是
`/etc/nginx/conf.d/`）：

```bash
cp /tmp/nginx-cam-ai-relay.conf /www/server/panel/vhost/nginx/cam-ai-relay.conf
# 宝塔的 nginx.conf 会用通配符 include 这个目录，无需再改主配置
nginx -t
nginx -s reload
```

这份配置做了五件事，改动时别破坏：

1. **`auth_request /_auth`** — 每次请求先去 relay 的 `/auth` 问一句"这个 Cookie
   有效吗"，401 就 `error_page 401 = @need_login` 跳到登录页。
2. **`sub_filter '</head>' '<script>window.__CAMAI_STREAM_BASE="/dev81";</script></head>'`**
   — 把设备页面里写死的 `:81` 视频地址改写成同源路径，否则浏览器会因混合内容
   拦截 `https://IP:81/...`。**前提是 `proxy_set_header Accept-Encoding ""`**，
   经过 gzip 的响应 sub_filter 匹配不到。
3. **`location /dev80/` 与 `/dev81/`** — 两条隧道，分别指向设备的 80（API + 页面）
   和 81（MJPEG + 回放）。
4. **`location /devices/` 不加 auth** — 设备用 hello 帧里的 `RELAY_TOKEN` 自鉴权，
   这里加 auth_request 会把设备挡在门外。
5. **`location / { return 404; }`** — 白名单式，没列出的路径一律 404，不给扫描器
   任何探测面。

---

## 7. 验证

### 7.1 服务活着

```bash
curl -s http://127.0.0.1:9000/health
curl -sk https://127.0.0.1/health
# {"ok":true,"device":false,"viewers":0,"push":false,"uptime_s":...,"tunnels":0}

# 监听地址必须是回环
ss -lntp | grep 9000
# LISTEN ... 127.0.0.1:9000 ... users:(("node",...))       ← 正确
# LISTEN ...        *:9000 ...                             ← 绑宽了，检查 RELAY_BIND
```

### 7.2 登录链路

```bash
# 无 Cookie 访问受保护资源 → API 401
curl -s -o /dev/null -w '%{http_code}\n' -k https://127.0.0.1/api/v1/status   # 401
# 页面无 Cookie → 302 到 /login
curl -s -o /dev/null -w '%{http_code} %{redirect_url}\n' -k https://127.0.0.1/  # 302 .../login

# 正确密码 → 302 + Set-Cookie
curl -s -k -D - -o /dev/null -X POST https://127.0.0.1/login \
  -d 'user=<user>&pass=<pass>' | grep -iE '^(HTTP/|set-cookie|location)'
# Set-Cookie: camai_sess=<exp>.<hmac>; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=43200

# 带上 Cookie 再用
CK=<上面 cookie 的值>
curl -s -o /dev/null -w '%{http_code}\n' \
  -k -H "Cookie: camai_sess=$CK" https://127.0.0.1/api/v1/status   # 200
```

`/auth` 是给 nginx `auth_request` 用的**内部端点**（配置里是 `location = /_auth`,
`internal`），所以从 443 直接访问只会命中白名单兜底拿 404，这是对的、不是故障。
想单独验它就走 relay 自己的 9000 端口：

```bash
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:9000/auth   # 401 无 Cookie
curl -s -o /dev/null -w '%{http_code}\n' \
  -H "Cookie: camai_sess=$CK" http://127.0.0.1:9000/auth              # 204 Cookie 有效
```

> 部分环境下 `curl -c/-b` 的 cookie jar 不可靠，用 `-H "Cookie: camai_sess=..."`
> 手工传更稳。

### 7.3 隧道与页面

```bash
CK=...
curl -s -k -H "Cookie: camai_sess=$CK" https://127.0.0.1/ | grep -o '__CAMAI_STREAM_BASE="[^"]*"'
# __CAMAI_STREAM_BASE="/dev81"          ← 注入成功的唯一证据

curl -s -k -o /dev/null -w '%{http_code} %{size_download}\n' \
  -H "Cookie: camai_sess=$CK" https://127.0.0.1/motion        # 200 约 5.8 KB image/bmp
curl -s -k -o /dev/null -w '%{http_code} %{size_download}\n' \
  -H "Cookie: camai_sess=$CK" https://127.0.0.1/snapshot      # 200 约 24 KB image/jpeg
```

### 7.4 冒烟测试（11 项）

`smoke_test.js` 自带一个模拟设备和模拟观看者，覆盖握手、令牌校验、`start_stream`、
二进制转发、`stop_stream`、MJPEG 头与载荷。

**它在 9123 端口自己拉起一个 relay 实例**，跑完自动关掉，所以只要一条命令：

```bash
cd /opt/cam-ai
node smoke_test.js
# 期望：最后一行 11 passed, 0 failed，退出码 0
```

它注入的环境是 `RELAY_PORT=9123 RELAY_TOKEN=test-token`，且把 `RELAY_VIEW_AUTH`、
`RELAY_LOGIN_*`、`RELAY_SESSION_SECRET` 全部清空——因此登录与 `/dev*` 隧道在本次
测试里是关闭的，脚本里依赖 Basic Auth 的用例会跳过。**这不会连到生产实例**：端口不同、
环境变量不同，`/opt/cam-ai/relay.env` 也不参与。

> 如果 9123 被占用，脚本会在等待健康检查 5 秒后报
> `SMOKE ERROR relay did not come up on port 9123` 并以退出码 2 结束。

---

## 8. 设备端配置

`include/secrets.h`（已在 `.gitignore`，只把 `secrets.h.example` 入库）：

```cpp
#define RELAY_HOST "47.103.200.7"
#define RELAY_PORT 443          // 对外 TLS 端口，不是 relay 内部的 9000
#define RELAY_TOKEN "<与 relay.env 的 RELAY_TOKEN 一致>"
#define RELAY_CA_CERT \
  "-----BEGIN CERTIFICATE-----\n" \
  "...\n" \
  "-----END CERTIFICATE-----\n"
```

`RELAY_CA_CERT` 留空会让 `relay_client.cpp` 直接以 `no relay CA cert pinned`
拒绝连接——这是**有意设计**，不允许"跳过校验"的生产路径。

烧录后还要把中继开关打开（`relayEnabled` 默认 false）：

```bash
curl -s -X POST http://<设备局域网IP>/api/v1/config \
  -d '{"relay_enabled":true,"relay_fps":8}'
# {"ok":true,"saved":true}
```

设备串口应出现：

```
[net] clock synced: 2026-11-27 07:53:03 (UTC)
[relay] connecting to 47.103.200.7:443
[relay] authenticated, idle (look-to-stream)
```

---

## 9. 排障

### `start_ssl_client: -9984`（`MBEDTLS_ERR_X509_CERT_VERIFY_FAILED`）

两个都在本项目真实踩过，按顺序排查：

1. **设备时钟没同步**。ESP32 上电是 1970 年，早于证书 `notBefore`，必然验证失败。
   固件自 M9 起在 `NetworkService` 里跑 SNTP（`ntp.aliyun.com` + `pool.ntp.org`），
   且 `RelayClient` 在时钟未就绪时发 `waiting for clock sync` 而不去连。若串口没有
   `[net] clock synced`，检查路由器是否放通 UDP 123。
2. **证书 SAN 只有 `IP:` 一种形式**。ESP32 的 mbedtls 是 2.28，其
   `mbedtls_x509_subject_alternative_name` 头文件注释明确写着
   *"Only dnsName currently supported"*，主机名校验只遍历 dNSName，`iPAddress`
   分支不参与比对。所以按 IP 字面量连接会判 `CN_MISMATCH`。
   **修法：证书 SAN 同时写 `DNS:47.103.200.7` 和 `IP:47.103.200.7`**（见第 3 节）。
   浏览器仍走 iPAddress 分支通过，两边都满足。

### 设备连不上但 `-9984` 之外没别的错

```bash
journalctl -u cam-ai-relay -n 50 --no-pager | grep -i 'device\|auth'
```

- 日志里完全没有 `device ... authenticated` → 请求根本没到 relay。查安全组、查
  `RELAY_HOST`、查设备串口是否有 `TLS connect failed`。
- 有 `auth` 失败 → `RELAY_TOKEN` 两侧不一致，或 `RELAY_DEVICE_ID` 与固件
  `Config.deviceId` 不一致。

### 页面能打开但视频不显示

按顺序确认：

1. `/` 响应里有没有 `__CAMAI_STREAM_BASE="/dev81"`。没有 → `Accept-Encoding ""`
   漏了，或 `http_sub_module` 没编进去。
2. 浏览器控制台有没有 `Mixed Content` 报错。有 → 注入没生效，`<img>` 还在打 `:81`。
3. `curl -k -H "Cookie: ..." https://IP/dev81/stream` 能不能出字节。不出 → 隧道问题，
   看 relay 的 `curl -s http://127.0.0.1:9000/health` 里的 `tunnels` 计数与
   `journalctl -u cam-ai-relay` 的隧道日志（设备端 `/api/v1/status` 只报
   `relay.connected/viewer/frames_sent`，没有 tunnels 字段）。

### 隧道 502 `device busy` / `tunnel busy`

设备侧只有 **4 条** 并发隧道通道，上限原因是每通道要吃掉两个 lwIP socket（设备发起
的 + 设备 httpd accept 的），而 `CONFIG_LWIP_MAX_SOCKETS = 16`。relay 侧会排队
（`TUNNEL_QUEUE_MAX = 16`），排满才回 502。多开标签页同时点会看到这个，属预期。

### 登录后立刻被踢回登录页

- `RELAY_SESSION_SECRET` 改过 → 旧 Cookie 全部失效，重新登录即可。
- 服务器时间不对 → HMAC 里带 `exp`，时间跳变会让 Cookie 看起来已过期。
  `timedatectl` 确认 NTP 正常。
- 通过 IP 访问但用了 http → Cookie 带 `Secure`，不会发出去。必须走 https。

---

## 10. 日常运维

```bash
# 更新 relay 代码
scp -i ~/.ssh/id_ed25519_aliyun server/relay.js root@<IP>:/tmp/
ssh -i ~/.ssh/id_ed25519_aliyun root@<IP> \
  'node --check /tmp/relay.js && install -o camai -g camai -m 644 /tmp/relay.js /opt/cam-ai/relay.js && systemctl restart cam-ai-relay'

# 改登录密码（改 relay.env 后必须 restart，代码不热加载）
vi /etc/cam-ai/relay.env && systemctl restart cam-ai-relay

# 换证书
#   只重签 server.crt → nginx -s reload，设备端不用动
#   换了 ca.crt      → 必须重新烧录固件（RELAY_CA_CERT 变了）

# 日志
journalctl -u cam-ai-relay -f
tail -f /var/log/cam-ai/nginx-access.log
```

**轮换登录密码时记得**：`RELAY_LOGIN_PASS` 是明文写在 `relay.env` 里的（600，
root 可读）。改完 `systemctl restart cam-ai-relay` 才生效，nginx 不需要动。

---

## 11. 安全边界

这套部署明确的分工，不要混淆：

| 面 | 谁负责 | 现状 |
|---|---|---|
| 公网入口 | nginx + relay（`/auth`） | 登录表单 + HMAC 签名 Cookie，12 小时有效，`HttpOnly`/`Secure`/`SameSite=Lax` |
| 设备认证 | relay `handleDevice` | hello 帧 `RELAY_TOKEN` 定长比对 |
| **设备局域网 httpd** | **无** | **完全没有鉴权**，信任模型是"内网可信" |
| 设备↔relay 传输 | TLS + CA 固定 | 证书 pinning，不允许跳过校验 |
| 服务器→设备 | relay 不主动请求设备 | 只有设备主动出站，家里零开放端口 |

已知缺口（记录在 `docs/progress.md` 的 M9 章节）：

- 设备 80 端口在局域网内无鉴权。任何能进家庭网络的人都能改配置、删录像。
- 登录无失败次数限制，无验证码。密码强度就是唯一防线。
- 会话是无状态 HMAC，**没有服务端吊销能力**——只能靠换 `SESSION_SECRET`
  让所有会话一起失效。
- 服务器上的 `relay.env`、`tls/*.key` 为明文文件，安全性等同于 root 账号安全性。
