# 车牌识别系统部署和使用

## 1. 文档目的与适用范围

本文面向在本地 Windows 主机和 Ubuntu 虚拟机中搭建本项目，说明当前版本的实际部署方式、日常操作、课堂演示、源码重建和交付方法。

本文以 2026-08-26 的当前工程状态为准，推荐环境如下：

| 位置 | 推荐环境 | 作用 |
|---|---|---|
| Windows 主机 | Windows 10/11 x64 | 运行 Qt Creator 和 Qt 管理客户端 |
| Ubuntu 虚拟机 | Ubuntu 22.04 LTS、Linux `amd64` | 运行 Docker Compose、Linux 服务端、MySQL 和 Mosquitto |
| 网络 | 仅主机、桥接或可互通的 NAT 网络 | Windows 必须能访问 Ubuntu 的 8080 和 1883 端口 |

当前已验收环境使用 Ubuntu 22.04.5 LTS、Docker 29.1.3、Docker Compose 2.40.3；Windows 客户端使用 Qt 6.11.1、MinGW-w64 GCC 13.1.0、CMake 3.30.5 和 Ninja 1.12.1。

## 2. 系统组成与推荐部署拓扑

```text
Windows 主机
  Qt 管理客户端
      | HTTP 8080：登录、心跳、历史、图片、CSV、名单
      | MQTT 1883：管理端实时识别事件
      v
Ubuntu 虚拟机
  Docker Compose
      +-- ocrservice-app    Linux C++ 服务端、模型推理
      +-- ocrservice-mysql  MySQL 8.4
      +-- ocrservice-mqtt   Mosquitto 2.0.20
      +-- 持久卷：数据库、MQTT、识别图片、日志
      ^
      | HTTP 8080：上传图片
      | MQTT 1883：订阅设备最终结果
  C++ embedded_device_simulator（模拟嵌入式设备）
```

MySQL、Mosquitto 和应用是三个独立容器，不是三个进程塞进同一个容器。`docker-compose.yml` 负责统一创建、配置和启动它们：

| Compose 服务 | 默认镜像 | 是否映射到宿主机 | 持久数据 |
|---|---|---|---|
| `app` | `ocrservice:teaching` | `8080:8080` | `app-images`、`app-logs` |
| `mysql` | `mysql:8.4` | 不映射 3306 | `mysql-data` |
| `mqtt` | `ocrservice-mosquitto:teaching` | `1883:1883` | `mosquitto-data` |

因此，正常的 Compose 部署不需要在 Ubuntu 宿主机单独安装 MySQL Server 或 Mosquitto。只需要安装 Docker Engine 和 Docker Compose 插件。MySQL 不开放给 Windows，Qt 和嵌入式端都不得直连数据库。

## 3. 课前资源与虚拟机准备

### 3.1 建议的虚拟机资源

- CPU：4 个虚拟 CPU 或更多。
- 内存：8 GiB 或更多。
- 磁盘：至少 40 GiB，总可用空间建议不少于 15 GiB。
- 架构：必须是 `x86_64/amd64`，当前镜像不支持 ARM64。
- 时间：Ubuntu 和 Windows 的时钟、时区应正确；协议对外使用 `+08:00` 时间。

检查 Ubuntu 环境：

```bash
uname -m
cat /etc/os-release
df -h /
timedatectl
```

`uname -m` 应输出 `x86_64`。

### 3.2 配置 Windows 与 Ubuntu 互通

在 Ubuntu 中查看地址：

```bash
ip -br address
ip route
hostname -I
```

以下示例假设 Ubuntu 地址是 `192.168.137.128`。学生环境地址不同时，必须同时修改服务端 `.env` 的 `MQTT_PUBLIC_HOST` 和 Qt 的 `config/client.json`。

在 Windows PowerShell 中测试：

```powershell
ping 192.168.137.128
Test-NetConnection 192.168.137.128 -Port 8080
Test-NetConnection 192.168.137.128 -Port 1883
```

服务尚未启动时端口测试失败是正常的，但 `ping` 或基本路由应先可用。若 Ubuntu 开启了 UFW，只在隔离教学网段内开放端口：

```bash
sudo ufw allow from 192.168.137.0/24 to any port 8080 proto tcp
sudo ufw allow from 192.168.137.0/24 to any port 1883 proto tcp
sudo ufw status
```

应根据实际教学网段替换 `192.168.137.0/24`，不要直接把端口开放到互联网。

## 4. Ubuntu 必装软件

### 4.1 推荐运行环境所需软件

推荐 Compose 部署只要求：

- Docker Engine；
- Docker Compose v2 插件；
- Git，用于取得源码；
- `curl`，用于健康检查；
- `openssl`，用于生成教学环境凭据。

以下是 Docker 官方 APT 仓库的一种安装方式。执行前应确认虚拟机能够访问 `download.docker.com`：

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates curl git openssl
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
  -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc

. /etc/os-release
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu ${VERSION_CODENAME} stable" \
  | sudo tee /etc/apt/sources.list.d/docker.list >/dev/null

sudo apt-get update
sudo apt-get install -y \
  docker-ce docker-ce-cli containerd.io \
  docker-buildx-plugin docker-compose-plugin

sudo systemctl enable --now docker
sudo usermod -aG docker "$USER"
```

完成用户组修改后，注销并重新登录 Ubuntu，再验证：

```bash
docker version
docker compose version
docker run --rm hello-world
```

不希望把用户加入 `docker` 组时，可以在后续 Docker 命令前使用 `sudo`。需要注意：`docker` 组等价于较高的宿主机权限，只能授予可信用户。

### 4.2 仅源码原生构建需要的开发包

只加载现成镜像时不需要本节的软件。修改服务端代码并采用 Docker 重建时，编译工具也会在 Docker 构建阶段自动安装。

只有需要在 Ubuntu 宿主机原生编译服务端、测试或嵌入式模拟器时，才安装：

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config curl ca-certificates git \
  libopencv-dev libssl-dev libcrypt-dev libmysqlclient-dev
```

原生 CMake 还要求精确的 MySQL Connector/C++ 8.4.0 和 ONNX Runtime 1.20.1。第 12 节给出说明。课堂部署优先使用 Docker，能避免每台虚拟机重复配置这些开发依赖。

## 5. 使用源码和 Docker Compose 部署

### 5.1 准备项目目录

将完整服务端工程放到 Ubuntu 本地目录，例如：

```bash
cd /home/student
git clone <SERVER_REPOSITORY_URL> ocrservice
cd /home/student/ocrservice
```

教师通过压缩包或 Samba 分发源码时，只需保证进入的目录中存在：

```text
docker-compose.yml
Dockerfile
.env.example
config/server.json.example
models/yolov8_plate.onnx
models/lprnet.onnx
```

构建目录和 Docker 数据卷不要放入 Git。

### 5.2 创建教学环境配置

```bash
cd /home/student/ocrservice
cp .env.example .env
chmod 600 .env
vim .env
```

`.env` 的字段如下：

```dotenv
MYSQL_ROOT_PASSWORD=<REPLACE_WITH_RANDOM_VALUE>
MYSQL_PASSWORD=<REPLACE_WITH_RANDOM_VALUE>
MQTT_SERVER_PASSWORD=<REPLACE_WITH_RANDOM_VALUE>
MQTT_MANAGEMENT_PASSWORD=<REPLACE_WITH_RANDOM_VALUE>
MQTT_PUBLIC_HOST=192.168.137.128
MYSQL_IMAGE=mysql:8.4
MQTT_IMAGE=ocrservice-mosquitto:teaching
APP_IMAGE=ocrservice:teaching
```

必须替换四个密码。为减少 shell 和 `.env` 转义问题，课堂环境可使用 48 位十六进制随机值。生成命令会把密码显示在当前终端，生成后不要截图、粘贴到聊天或提交到 Git：

```bash
openssl rand -hex 24
```

`MQTT_PUBLIC_HOST` 必须是 Windows 能访问的 Ubuntu 地址。它不仅供服务端连接 Broker，还会在 Qt 登录响应中下发。只修改 Qt 的 HTTP 地址而不修改该字段，会出现“HTTP 登录成功但 MQTT 未连接”。

`.env` 不得提交到仓库：

```bash
git status --short
```

四个静态密码应在第一次创建数据卷前确定。MySQL 和 Mosquitto 初始化后，直接修改 `.env` 不会自动、原子地轮换持久卷中的全部账号；需要换密码时，应执行相应运维流程，或在确认不再需要旧数据后用第 5.5 节的空卷方式重新初始化。

### 5.3 首次在线构建并启动

```bash
docker compose --env-file .env config --quiet
docker compose --env-file .env up -d --build
docker compose --env-file .env ps

# sudo systemctl disable --now mosquitto 如果本机有安装mosquitto，先卸载服务
# docker compose --env-file .env up -d 启动docker服务
# docker compose --env-file .env up -d --force-recreate mqtt app  重建服务
```

首次构建会下载 Ubuntu、MySQL、Mosquitto、ONNX Runtime 和 C++ 依赖，耗时取决于网络与 CPU。应用镜像包含两个固定 ONNX 模型，构建和启动时都会校验模型 SHA-256。

Compose 会等待 MySQL 健康后再启动应用。Mosquitto 暂时不可用时应用允许启动并后台重连，这是当前版本的既定行为。

### 5.4 检查服务状态

```bash
docker compose --env-file .env ps
curl --fail --silent http://127.0.0.1:8080/health
docker compose --env-file .env logs --tail=200 app
```

完全健康时，`/health` 的 `data` 应包含：

```json
{
  "status": "UP",
  "model": "UP",
  "mysql": "UP",
  "mqtt": "UP",
  "queueDepth": 0,
  "queueCapacity": 20
}
```

外层实际响应是项目统一的 `success/code/message/requestId/data` 五字段 JSON 信封。仅 MQTT 断开时可返回 HTTP 200 且 `status=DEGRADED`；模型或 MySQL 不可用时返回 HTTP 503。

从 Windows 再检查：

```powershell
Invoke-RestMethod http://192.168.137.128:8080/health
Test-NetConnection 192.168.137.128 -Port 1883
```

### 5.5 启停、重启和日志

查看日志：

```bash
docker compose --env-file .env logs --tail=200 app
docker compose --env-file .env logs --tail=200 mysql
docker compose --env-file .env logs --tail=200 mqtt
docker compose --env-file .env logs -f app
```

启动、停止和重启：

```bash
docker compose --env-file .env start
docker compose --env-file .env stop
docker compose --env-file .env restart app
```

停止并删除容器和网络，但保留四个持久卷：

```bash
docker compose --env-file .env down
```

下次执行 `up -d` 后，数据库、Broker 状态、图片和日志仍存在。

彻底清空课堂数据：

```bash
docker compose --env-file .env down --volumes
```

**警告：`down --volumes` 会永久删除本 Compose 项目的 MySQL 数据、Mosquitto 密码与 ACL、识别图片和应用日志。只有明确需要恢复空环境时才执行。**

## 6. 登录 MySQL 并查看表数据

MySQL 的 3306 端口没有映射到 Ubuntu 宿主机，更没有开放给 Windows。应从 Ubuntu 通过 Compose 进入 MySQL 容器：

```bash
cd /home/student/ocrservice
docker compose --env-file .env exec mysql \
  mysql --protocol=TCP -h 127.0.0.1 -u ocrservice -p ocrservice
```

出现 `Enter password:` 后，交互输入 `.env` 中的 `MYSQL_PASSWORD`。不要把密码写在 `-pPASSWORD` 参数里，否则可能进入 shell 历史或进程列表。

进入 `mysql>` 后执行：

```sql
SHOW DATABASES;
USE ocrservice;
SHOW TABLES;

DESCRIBE admin_users;
DESCRIBE devices;
DESCRIBE recognition_logs;
DESCRIBE access_lists;
```

查看非敏感业务信息：

```sql
SELECT id, username, display_name, enabled
FROM admin_users;

SELECT device_id, device_name, mqtt_username, enabled
FROM devices
ORDER BY device_id;

SELECT recognition_id, device_id, status, plate_number, error_code,
       captured_at, completed_at
FROM recognition_logs
ORDER BY captured_at DESC, recognition_id DESC
LIMIT 20;

SELECT id, list_type, plate_number, remark,
       created_by_display_name, created_at
FROM access_lists
ORDER BY created_at DESC, id DESC;
```

退出：

```sql
exit;
```

需要数据库管理权限时，将命令中的用户改为 `root`，并输入 `.env` 的 `MYSQL_ROOT_PASSWORD`。课堂展示不要查询或截图 `password_hash`、`http_token_hash` 等安全摘要。数据库时间按 UTC 保存，HTTP/MQTT 对外才转换为 `+08:00`。

## 7. Windows Qt 客户端配置与运行

### 7.1 安装工具链

当前工程固定使用：

- Qt 6.11.1 MinGW 64-bit；
- MinGW-w64 GCC 13.1.0；
- CMake 3.30.5；
- Ninja 1.12.1；
- Qt Creator；
- 项目本地构建的 Qt MQTT v6.11.1。

教师当前客户端仓库位于 `D:\code\ocr_project\plate_client`。学生可以使用其他本地目录，后续 PowerShell 命令均应在各自的客户端仓库根目录执行。

以下示例路径与当前已验收环境一致：

```text
D:\Qt\6.11.1\mingw_64
D:\Qt\Tools\mingw1310_64
D:\Qt\Tools\CMake_64
D:\Qt\Tools\Ninja
```

Qt MQTT 不安装到全局 Qt Kit。首次构建客户端前，在客户端仓库根目录运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\bootstrap-qtmqtt.ps1
```

该脚本把 Qt MQTT 安装到 `.deps\install\qtmqtt`。Qt MQTT 主要源码采用 `LicenseRef-Qt-Commercial OR GPL-3.0-only`；分发客户端前必须具有有效 Qt 商业许可或遵守 GPL-3.0-only。

### 7.2 配置服务器 IP

客户端从可执行文件旁读取：

```text
<plate_client.exe 所在目录>\config\client.json
```

示例：

```json
{
  "clientId": "28ab84b4-07c9-4978-a7b3-2f79e79957f2",
  "loginBaseUrl": "http://192.168.137.128:8080",
  "allowInsecureTransport": true
}
```

规则：

- `loginBaseUrl` 中的地址改为学生自己的 Ubuntu 地址。
- 当前教学部署是明文 HTTP/MQTT，因此 `allowInsecureTransport` 必须显式为 `true`。
- 每个客户端安装必须使用不同的非零 UUID；不要复制同一个 `clientId` 给全班。
- 配置文件只能包含这三个字段，不得写入用户名、登录密码、HTTP Token 或 MQTT 密码。

在 PowerShell 中生成 UUID：

```powershell
[guid]::NewGuid().ToString()
```

服务端 `.env` 中的 `MQTT_PUBLIC_HOST` 也必须是相同的可达 Ubuntu 地址。Qt 的 `loginBaseUrl` 只决定登录 HTTP 地址，登录成功后 MQTT 地址由服务端响应提供。

### 7.3 登录和功能演示

当前固定教学管理账号：

```text
用户名：admin
密码：plate-demo-2026
```

该账号会随迁移创建，只能用于隔离教学网络。登录后建议按以下顺序演示：

1. 确认导航区 HTTP 和 MQTT 都显示已连接。
2. 打开实时监控页。
3. 运行第 9 节的嵌入式模拟器上传车辆图片。
4. 观察 `PROCESSING` 事件和最终 `SUCCEEDED` 或 `FAILED` 事件。
5. 打开详情和原始图片。
6. 在历史页按时间、设备或车牌查询并导出 CSV。
7. 在黑白名单页新增、查询和删除记录。
8. 返回 MySQL 命令行核对 `recognition_logs` 和 `access_lists`。

同一 `clientId` 再次登录会使旧 Token 失效。客户端关闭、注销和会话过期时会清理内存中的 HTTP/MQTT 凭据。

## 8. 当前 HTTPS 与 MQTT TLS 边界

当前交付版本只监听：

```text
HTTP  8080，明文
MQTT  1883，明文，tls=false
```

服务端应用没有 HTTPS 监听器，Mosquitto 配置没有 TLS listener，设备配置生成器也固定输出 `tls=false`。因此，把证书文件复制进应用容器或在 `.env` 中增加自定义字段，不会自动启用 HTTPS/TLS。

课堂推荐做法是保持 Windows 与 Ubuntu 位于隔离的仅主机或实验网段，继续使用当前已验收的明文部署。

如果后续必须使用证书，正确架构应包括：

1. 在应用前增加 Nginx、Caddy 或 Traefik，使用证书和私钥监听 HTTPS 443，再反向代理到仅本机可达的 `http://127.0.0.1:8080`。
2. 证书的 SAN 必须包含 Qt 实际访问的 DNS 名称或 IP；私钥只保存在 Ubuntu，权限限制为服务账号可读。
3. 使用教学自签 CA 时，应把 CA 证书导入 Windows“受信任的根证书颁发机构”，不能让 Qt 静默忽略证书错误。
4. Qt 的 `loginBaseUrl` 改为 `https://<HOSTNAME>`，完整 TLS 环境应把 `allowInsecureTransport` 设为 `false`。
5. Mosquitto 需要另行增加 TLS listener、CA、服务器证书、私钥和相应端口，并同步修改登录响应和设备摘要中的 MQTT `host/port/tls`。

第 5 步不是当前版本的纯部署配置：当前公共契约和服务端配置固定 MQTT `tls=false`。完整 HTTPS + MQTT TLS 需要先修改 `docs/REQ-001.md`，再修改服务端、设备开通脚本、Compose、Mosquitto、Qt 契约测试和嵌入式模拟测试。未完成这些变更前，不应宣称系统已经支持端到端 HTTPS/TLS。

## 9. 使用嵌入式设备模拟器完成课堂闭环

### 9.1 是否有 Python 模拟脚本

当前仓库没有用于正式课堂闭环的 Python 嵌入式模拟器。已经实现和验收的是 C++17 目标 `embedded_device_simulator`，源码目录为：

```text
tests/simulators/embedded/
```

它会按照真实设备流程执行：

1. 使用设备专属 MQTT 用户连接 Broker；
2. 使用 `cleanSession=false`、QoS 1 订阅本设备结果主题；
3. 收到 SUBACK 后生成 `captureId` 并上传 JPEG/PNG；
4. 接收最终结果；
5. 按 `recognitionId + revision` 去重；
6. 将 `SUCCEEDED` 映射为 `OPEN`，将 `FAILED` 映射为 `KEEP_CLOSED`。

教师可以把已编译的 Linux `amd64` 模拟器和镜像一起发给学生，也可以按第 12.2 节从源码构建。

### 9.2 为什么演示前必须开通设备

不要只向 MySQL 插入设备记录。设备必须同时存在于：

- MySQL `devices` 表；
- Mosquitto 密码文件；
- 本设备主题 ACL。

空数据卷会创建数据库演示行 `device-001`，但不能把该行当成一个已经完成 MQTT 密码和 ACL 开通的课堂模拟设备。正常演示应使用 `scripts/provision-device.sh` 创建一个新设备。

### 9.3 在固定端口 Compose 环境中开通课堂设备

以下命令在 Ubuntu 服务端仓库根目录执行。示例创建 `device-classroom-01`，所有 secret 通过权限为 `0600` 的文件传递，不出现在脚本参数中。

```bash
cd /home/student/ocrservice
mkdir -p demo-device
chmod 700 demo-device
umask 077

openssl rand -hex 24 > demo-device/http-token.txt
openssl rand -hex 24 > demo-device/mqtt-password.txt
chmod 600 demo-device/http-token.txt demo-device/mqtt-password.txt

set -a
. ./.env
set +a

cat > demo-device/provision.env <<EOF
MYSQL_HOST=mysql
MYSQL_PORT=3306
MYSQL_DATABASE=ocrservice
MYSQL_USER=ocrservice
MYSQL_PASSWORD=${MYSQL_PASSWORD}
MQTT_PUBLIC_HOST=${MQTT_PUBLIC_HOST}
MQTT_PORT=1883
MQTT_SERVER_USERNAME=plate-server
MQTT_MANAGEMENT_USERNAME=management-client
EOF
chmod 600 demo-device/provision.env

MQTT_CONTAINER="$(docker compose --env-file .env ps -q mqtt)"
test -n "$MQTT_CONTAINER"

docker compose --env-file .env exec -T -u root mqtt \
  apk add --no-cache mariadb-client mariadb-connector-c coreutils grep

docker cp scripts/provision-device.sh \
  "$MQTT_CONTAINER:/tmp/provision-device.sh"
docker cp demo-device/http-token.txt \
  "$MQTT_CONTAINER:/tmp/device-http-token.txt"
docker cp demo-device/mqtt-password.txt \
  "$MQTT_CONTAINER:/tmp/device-mqtt-password.txt"
docker cp demo-device/provision.env \
  "$MQTT_CONTAINER:/tmp/ocrservice-provision.env"

docker compose --env-file .env exec -T -u root mqtt chmod 0700 \
  /tmp/provision-device.sh
docker compose --env-file .env exec -T -u root mqtt chmod 0600 \
  /tmp/device-http-token.txt \
  /tmp/device-mqtt-password.txt \
  /tmp/ocrservice-provision.env

docker compose --env-file .env exec -T -u root mqtt bash -c \
  'set -a; source /tmp/ocrservice-provision.env; set +a; exec /tmp/provision-device.sh "$@"' \
  provision \
  --device-id device-classroom-01 \
  --device-name '课堂入口设备 01' \
  --mqtt-username mqtt-classroom-01 \
  --http-token-file /tmp/device-http-token.txt \
  --mqtt-password-file /tmp/device-mqtt-password.txt \
  --http-base-url "http://${MQTT_PUBLIC_HOST}:8080" \
  --output /tmp/device-classroom-01.json

docker cp "$MQTT_CONTAINER:/tmp/device-classroom-01.json" \
  demo-device/device-classroom-01.json
chmod 600 demo-device/device-classroom-01.json

docker compose --env-file .env exec -T -u root mqtt rm -f -- \
  /tmp/provision-device.sh \
  /tmp/device-http-token.txt \
  /tmp/device-mqtt-password.txt \
  /tmp/ocrservice-provision.env \
  /tmp/device-classroom-01.json

rm -f -- \
  demo-device/http-token.txt \
  demo-device/mqtt-password.txt \
  demo-device/provision.env
unset MYSQL_ROOT_PASSWORD MYSQL_PASSWORD \
  MQTT_SERVER_PASSWORD MQTT_MANAGEMENT_PASSWORD MQTT_PUBLIC_HOST
```

成功输出应依次包含：

```text
VALIDATED
MYSQL_COMMITTED
PASSWORD_INSTALLED
ACL_INSTALLED
BROKER_RELOADED
SUMMARY_WRITTEN
```

最终保留的 `demo-device/device-classroom-01.json` 含 HTTP Token 和 MQTT 密码，必须保持 `0600`，不得提交、截图或发到公共群聊。

再次使用同一 `deviceId` 但改变设备名、HTTP Token 或 MQTT 用户名会被拒绝。轮换凭据需要明确使用脚本的 `--rotate`，并保持设备 ID、设备名和 MQTT 用户名不变。

### 9.4 上传照片并接收 MQTT 最终结果

先在 Windows 启动 Qt 客户端、登录并等待 HTTP/MQTT 都显示已连接。然后只把一张 JPEG 或 PNG 课堂照片放入 Ubuntu 仓库的 `scripts/` 目录。该目录中不能同时存在第二张 `.jpg`、`.jpeg` 或 `.png` 图片。

图片扩展名大小写均可，压缩体必须非空且不超过 10 MiB，扩展名必须与文件签名一致，并且不能使用符号链接。准备好图片后，在任意目录直接执行：

```bash
/home/student/ocrservice/scripts/run-classroom-device-demo.sh
```

如果仓库实际位于 `/home/share/ocrservice`，对应命令是：

```bash
/home/share/ocrservice/scripts/run-classroom-device-demo.sh
```

脚本不接受命令行参数，并固定只上传 1 张图片、上传间隔 1000 ms、等待最终结果 120 秒。它会自动定位已经构建的 `embedded_device_simulator`，严格确认服务、模型、MySQL 和 MQTT 均为 `UP`，再由模拟器先建立设备 MQTT 持久会话并收到本设备主题的 SUBACK，最后执行 HTTP 上传。

如果 `demo-device/device-classroom-01.json` 尚不存在，脚本会自动复用 `scripts/provision-device.sh` 完成第 9.3 节的一次性设备开通：生成临时 HTTP Token 和 MQTT 密码，同步更新 MySQL、Mosquitto 密码文件与设备 ACL，重载 Broker，并且只保留权限为 `0600` 的设备摘要。后续运行直接复用该摘要，不会重复开通。

首次自动开通要求固定端口 Compose 已正常运行、仓库根目录 `.env` 权限为 `0600`。如果 MQTT 镜像尚未包含运维依赖，脚本会安装 `mariadb-client`、`mariadb-connector-c`、`coreutils` 和 `grep`；下载期间会显示进度，可能耗时数分钟。`mariadb-connector-c` 提供连接 MySQL 8.4 所需的 `caching_sha2_password` 客户端插件。

成功示例：

```text
accepted=1 uniqueResults=1 duplicateResults=0 actionsExecuted=1
```

`uniqueResults=1` 表示收到了一个最终 MQTT 结果，`actionsExecuted=1` 表示执行了对应的闸杆动作。模型识别成功时为 `OPEN`；无法识别或模型失败时为 `KEEP_CLOSED`。课堂照片应是清晰的 JPEG/PNG，压缩体不超过 10 MiB。

### 9.5 完整自动嵌入式联调脚本

仓库还提供：

```bash
EMBEDDED_SIMULATOR_BIN=<ABSOLUTE_SIMULATOR_PATH> \
  bash scripts/run-embedded-e2e.sh
```

该脚本会创建隔离的临时 Compose 项目、随机回环端口、多个设备和测试数据，并在结束后清理。它用于完整系统验收，不是固定 8080/1883 的日常课堂启动器，也不能替代上面的正常 Compose 演示流程。

## 10. 导出、分发和加载 Docker 镜像

### 10.1 构建并导出应用发布镜像

项目提供经过验收的发布脚本。输出目录必须是仓库外已经存在的绝对目录，目标文件不能已存在：

```bash
mkdir -p /home/student/releases
cd /home/student/ocrservice

./scripts/build-release-image.sh \
  --output /home/student/releases/ocrservice-req-001-v1.0.0-linux-amd64.tar
```

脚本会：

- 从当前 Git HEAD 构建 `linux/amd64` 镜像；
- 拒绝未提交的产品镜像输入；
- 校验两个模型 SHA-256；
- 检查应用动态库没有 `not found`；
- 创建标签 `ocrservice:req-001-v1.0.0`；
- 输出归档 SHA-256。

将脚本输出的 SHA-256 与归档一起交给学生，学生先核对：

```bash
sha256sum ocrservice-req-001-v1.0.0-linux-amd64.tar
```

然后加载：

```bash
docker load --input ocrservice-req-001-v1.0.0-linux-amd64.tar
docker image inspect ocrservice:req-001-v1.0.0
```

学生 `.env` 中设置：

```dotenv
APP_IMAGE=ocrservice:req-001-v1.0.0
```

### 10.2 制作离线课堂镜像包

只导出应用镜像仍需要学生在线拉取 MySQL，并构建 Mosquitto 教学镜像。教师准备离线包时，应先保证三个镜像都存在：

```bash
docker compose --env-file .env build mqtt
docker pull mysql:8.4
docker image inspect \
  ocrservice:req-001-v1.0.0 \
  ocrservice-mosquitto:teaching \
  mysql:8.4 >/dev/null

docker tag ocrservice:teaching ocrservice:req-001-v1.0.0
docker save --output ocrservice-teaching-bundle.tar \
  ocrservice:req-001-v1.0.0 \
  ocrservice-mosquitto:teaching \
  mysql:8.4

sha256sum ocrservice-teaching-bundle.tar
```

给学生的离线材料至少包括：

```text
ocrservice-teaching-bundle.tar
docker-compose.yml
.env.example
config/server.json.example
本指南
归档 SHA-256
```

学生加载并启动：

```bash
docker load --input ocrservice-teaching-bundle.tar
cp .env.example .env
chmod 600 .env
vim .env
docker compose --env-file .env config --quiet
docker compose --env-file .env up -d --no-build
docker compose --env-file .env ps
curl --fail --silent http://127.0.0.1:8080/health
```

`.env` 中应使用：

```dotenv
MYSQL_IMAGE=mysql:8.4
MQTT_IMAGE=ocrservice-mosquitto:teaching
APP_IMAGE=ocrservice:req-001-v1.0.0
```

`docker save` 导出的是镜像，不是运行中的容器，也不包含 MySQL、Mosquitto、图片和日志卷。不要使用 `docker commit` 制作课堂发布物；它不能形成可审计、可重复的构建，也不会正确交付 Compose 持久卷。每个学生应从空卷启动并单独开通课堂设备。

## 11. 修改服务端代码后重新编译和部署

### 11.1 推荐：在 Docker 中完整重编译应用

修改服务端 C++、CMake、migration、模型适配或镜像输入后，在仓库根目录执行：

```bash
git status --short
docker compose --env-file .env build app
docker compose --env-file .env up -d --no-deps --force-recreate app
docker compose --env-file .env logs --tail=200 app
curl --fail --silent http://127.0.0.1:8080/health
```

`docker compose build app` 会在 Docker 的 Ubuntu 22.04 builder 中重新运行 CMake 和 Ninja，生成服务程序，再复制运行库、migration 和模型到新的运行镜像。宿主机不需要安装 C++ 编译器。

`--no-deps` 只重建和替换应用容器，MySQL、Mosquitto及其持久数据保持不变。若修改了 Mosquitto 镜像或希望重建全部服务：

```bash
docker compose --env-file .env build
docker compose --env-file .env up -d --force-recreate
docker compose --env-file .env ps
```

只修改 `.env` 或 `config/server.json.example` 时，也应重新创建应用容器使配置生效：

```bash
docker compose --env-file .env up -d --no-deps --force-recreate app
```

不要在运行容器内手工替换 `/app/ocrservice`。容器重建后这些临时修改会丢失，也无法通过 Git 和镜像标签追踪。

### 11.2 修改后测试

至少执行：

```bash
docker compose --env-file .env config --quiet
docker compose --env-file .env up -d
curl --fail --silent http://127.0.0.1:8080/health
```

涉及协议、数据库、MQTT、设备上传或模型时，还应运行仓库对应测试。完整 Compose 验收入口会使用独立项目和数据卷：

```bash
bash tests/system/docker_compose/test_compose.sh
```

完整嵌入式验收使用第 9.5 节脚本。测试脚本可能创建和清理自己的容器、卷与端口，运行前先阅读脚本并确认没有与课堂服务冲突。

## 12. Ubuntu 原生编译服务端和模拟器

### 12.1 固定二进制依赖

Docker 是推荐编译方式。确实需要在 Ubuntu 原生编译时，除第 4.2 节系统包外，还必须准备：

- MySQL Connector/C++ 8.4.0 Linux glibc 2.28 x86-64；
- ONNX Runtime 1.20.1 Linux x64；
- 仓库中 SHA-256 精确匹配的两个 ONNX 模型；
- CMake FetchContent 下载的 Crow、Asio、Paho MQTT、nlohmann/json、spdlog、utf8proc 和测试依赖。

MySQL Connector/C++ 固定归档：

```bash
mkdir -p "$HOME/ocr-deps"
cd "$HOME/ocr-deps"
curl --fail --location --retry 4 \
  'https://dev.mysql.com/get/Downloads/Connector-C++/mysql-connector-c%2B%2B-8.4.0-linux-glibc2.28-x86-64bit.tar.gz' \
  -o mysql-connector-cpp-8.4.0.tar.gz

printf '%s  %s\n' \
  '0d0ef94f0e20c152b7af5e8c374e915eef96274c1fb4aa936ea855cc989387d3' \
  mysql-connector-cpp-8.4.0.tar.gz | sha256sum -c -

tar -xzf mysql-connector-cpp-8.4.0.tar.gz
```

### 12.2 配置、构建和测试

回到服务端仓库：

```bash
cd /home/student/ocrservice

MYSQL_CONCPP_ROOT="$HOME/ocr-deps/mysql-connector-c++-8.4.0-linux-glibc2.28-x86-64bit"

cmake -S . -B build/teaching-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DOCRSERVICE_BUILD_TESTING=ON \
  -DOCRSERVICE_MYSQL_CONCPP_ROOT="$MYSQL_CONCPP_ROOT"

cmake --build build/teaching-release --parallel
ctest --test-dir build/teaching-release --output-on-failure
```

首次配置需要联网下载固定第三方依赖和 ONNX Runtime。完整构建后主要产物是：

```text
build/teaching-release/ocrservice
build/teaching-release/tests/tests/simulators_embedded/embedded_device_simulator
```

原生 `ocrservice` 的生产运行路径仍按容器设计为 `/app/config/server.json`、`/app/migrations`、`/app/models` 和 `/app/data`，因此不建议直接在宿主机长期运行该二进制。开发完成后仍用第 11.1 节 Docker 流程部署。

真实 MySQL、Mosquitto、Compose、Qt 和设备联调不是普通 `ctest` 一条命令能够全部替代的；最终发布应执行工程验收报告中列出的组件和系统门禁。

## 13. Qt Creator 编译与 Windows 发布包

### 13.1 在 Qt Creator 中构建

1. 在 Qt Creator 中打开客户端仓库根目录的 `CMakeLists.txt`。
2. 选择 Qt 6.11.1 MinGW 64-bit Kit。
3. 确认编译器是 `D:\Qt\Tools\mingw1310_64\bin\g++.exe`，CMake 和 Ninja 使用第 7.1 节版本。
4. 选择 Release 构建配置。
5. 先执行第 7.1 节 `bootstrap-qtmqtt.ps1`。
6. 执行 Build Project。

也可以使用已验证的命令行构建：

```powershell
D:\Qt\Tools\CMake_64\bin\cmake.exe -S . -B build\teaching-release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER=D:/Qt/Tools/mingw1310_64/bin/g++.exe `
  -DCMAKE_MAKE_PROGRAM=D:/Qt/Tools/Ninja/ninja.exe `
  -DCMAKE_PREFIX_PATH=D:/Qt/6.11.1/mingw_64

D:\Qt\Tools\CMake_64\bin\cmake.exe `
  --build build\teaching-release -j 4
```

可执行文件位于：

```text
build\teaching-release\src\plate_client.exe
```

### 13.2 为什么不能只复制 exe

`plate_client.exe` 动态依赖 Qt Core、GUI、Network、Widgets、Qt MQTT、MinGW 运行库和 Windows 平台插件。只复制 exe 到学生电脑会出现 DLL 缺失或“无法初始化 Qt platform plugin”。

项目的 Qt MQTT 位于 `.deps\install\qtmqtt`，不在全局 Qt `bin`。当前 Qt 6.11.1 `windeployqt` 会尝试从全局 Qt `bin` 查找 `Qt6Mqtt.dll` 并失败。因此本项目使用下面经过独立启动验证的显式发布清单，不修改全局 Qt 安装。

### 13.3 创建发布目录

在客户端仓库根目录打开 PowerShell：

```powershell
$package = Join-Path $PWD 'dist\plate-client'
if (Test-Path -LiteralPath $package) {
    throw "发布目录已存在，请先人工确认并使用新的版本目录：$package"
}

New-Item -ItemType Directory -Path `
  $package, `
  (Join-Path $package 'config'), `
  (Join-Path $package 'platforms'), `
  (Join-Path $package 'imageformats'), `
  (Join-Path $package 'styles'), `
  (Join-Path $package 'tls') | Out-Null

Copy-Item -LiteralPath `
  'build\teaching-release\src\plate_client.exe', `
  '.deps\install\qtmqtt\bin\Qt6Mqtt.dll', `
  'D:\Qt\6.11.1\mingw_64\bin\Qt6Core.dll', `
  'D:\Qt\6.11.1\mingw_64\bin\Qt6Gui.dll', `
  'D:\Qt\6.11.1\mingw_64\bin\Qt6Network.dll', `
  'D:\Qt\6.11.1\mingw_64\bin\Qt6Widgets.dll', `
  'D:\Qt\Tools\mingw1310_64\bin\libgcc_s_seh-1.dll', `
  'D:\Qt\Tools\mingw1310_64\bin\libstdc++-6.dll', `
  'D:\Qt\Tools\mingw1310_64\bin\libwinpthread-1.dll' `
  -Destination $package

Copy-Item -LiteralPath `
  'D:\Qt\6.11.1\mingw_64\plugins\platforms\qwindows.dll' `
  -Destination (Join-Path $package 'platforms')

Copy-Item -LiteralPath `
  'D:\Qt\6.11.1\mingw_64\plugins\imageformats\qgif.dll', `
  'D:\Qt\6.11.1\mingw_64\plugins\imageformats\qico.dll', `
  'D:\Qt\6.11.1\mingw_64\plugins\imageformats\qjpeg.dll' `
  -Destination (Join-Path $package 'imageformats')

Copy-Item -LiteralPath `
  'D:\Qt\6.11.1\mingw_64\plugins\styles\qmodernwindowsstyle.dll' `
  -Destination (Join-Path $package 'styles')

Copy-Item -LiteralPath `
  'D:\Qt\6.11.1\mingw_64\plugins\tls\qschannelbackend.dll' `
  -Destination (Join-Path $package 'tls')

Copy-Item -LiteralPath 'config\client.json.example' `
  -Destination (Join-Path $package 'config\client.json')
```

编辑发布目录的 `config\client.json`，设置 Ubuntu 地址和新的客户端 UUID。

### 13.4 在不借用 Qt 安装 PATH 的情况下验证

```powershell
$oldPath = $env:PATH
$oldPluginPath = $env:QT_PLUGIN_PATH
try {
    $env:PATH = "$package;$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_PLUGIN_PATH = $package
    Push-Location $package
    try {
        .\plate_client.exe --smoke-test
        if ($LASTEXITCODE -ne 0) {
            throw "客户端发布包 smoke test 失败：$LASTEXITCODE"
        }
    }
    finally {
        Pop-Location
    }
}
finally {
    $env:PATH = $oldPath
    $env:QT_PLUGIN_PATH = $oldPluginPath
}
```

当前工程已经用该方式验证过 16 个文件、约 38.9 MB 的 Release 发布目录，`--smoke-test` 通过。正式交付前还应双击启动、登录真实课堂服务并检查实时、历史、图片、CSV 和名单功能。

### 13.5 压缩和交付

```powershell
$zip = Join-Path $PWD 'dist\plate-client-windows-x64.zip'
if (Test-Path -LiteralPath $zip) {
    throw "目标压缩包已存在：$zip"
}

Compress-Archive -Path "$package\*" -DestinationPath $zip
Get-FileHash -Algorithm SHA256 -LiteralPath $zip
```

交付时同时提供：

- ZIP 文件；
- ZIP 的 SHA-256；
- 本指南；
- Qt 和 Qt MQTT 对应许可材料；
- 客户端版本与服务器镜像版本。

每台学生电脑解压后都要重新生成 `clientId`。不要在 ZIP 中预置教师登录密码、HTTP Token 或 MQTT 密码。

## 14. 推荐课堂演示流程

### 14.1 教师课前检查

```bash
docker compose --env-file .env up -d
docker compose --env-file .env ps
curl --fail --silent http://127.0.0.1:8080/health
```

确认：

- Windows 到 Ubuntu 8080、1883 可达；
- `.env` 的 `MQTT_PUBLIC_HOST` 是课堂可达地址；
- Qt 包具有唯一 `clientId`；
- 课堂设备已经通过统一脚本开通；
- 演示图片可被实际解码且小于 10 MiB；
- 磁盘空间足够；
- 不存在占用 8080/1883 的其他服务。

检查端口：

```bash
sudo ss -ltnp | grep -E ':(8080|1883)\b'
```

### 14.2 课堂演示顺序

1. 展示 `docker compose ps`，说明三容器和四类持久数据。
2. 调用 `/health`，讲解模型、MySQL、MQTT 和队列状态。
3. 启动 Qt，使用固定教学管理账号登录。
4. 展示 Qt HTTP/MQTT 双连接状态。
5. 启动 C++ 嵌入式模拟器上传一张真实车辆图片。
6. 在 Qt 实时页观察 `PROCESSING` 到最终状态，并打开图片。
7. 在历史页查询记录并导出带 UTF-8 BOM 的 CSV。
8. 新增、冲突验证并删除黑白名单数据；说明首版名单不参与抬杆决策。
9. 从 MySQL 查看 `recognition_logs` 和 `access_lists`。
10. 重启应用容器，确认数据与图片仍存在。

重启应用：

```bash
docker compose --env-file .env restart app
docker compose --env-file .env ps
curl --fail --silent http://127.0.0.1:8080/health
```

应用重启时遗留的 `PROCESSING` 记录会变为 `FAILED/SERVER_RESTARTED`，不会自动重新推理。

## 15. 常见问题排查

### 15.1 Compose 启动失败

```bash
docker compose --env-file .env config --quiet
docker compose --env-file .env ps -a
docker compose --env-file .env logs --tail=300 app mysql mqtt
docker system df
df -h /
```

重点检查 `.env` 是否缺字段、模型是否存在、磁盘是否已满，以及 8080/1883 是否被占用。

### 15.2 Qt 无法登录

在 Windows 执行：

```powershell
Get-Content .\config\client.json
Test-NetConnection 192.168.137.128 -Port 8080
Invoke-RestMethod http://192.168.137.128:8080/health
```

确认运行中的 exe 旁边确实有 `config\client.json`，地址没有写成另一台虚拟机的旧 IP。

### 15.3 HTTP 正常但 Qt MQTT 未连接

检查：

```bash
grep '^MQTT_PUBLIC_HOST=' .env
docker compose --env-file .env ps
docker compose --env-file .env logs --tail=200 mqtt app
```

常见原因是 `.env` 的 `MQTT_PUBLIC_HOST` 不可从 Windows 访问、1883 被防火墙阻止、客户端 UUID 重复，或管理端 MQTT 密码与持久卷中的旧值不一致。修改静态 MQTT 密码后若沿用旧 `mosquitto-data`，应先确认是否需要保留数据；不要未经确认直接删除卷。

### 15.4 模拟器认证失败

不要手工只改 MySQL。重新核对第 9.3 节是否完成六个开通阶段，并确认模拟器使用最终生成的 `device-classroom-01.json`。该配置中的 Ubuntu IP 必须可从模拟器运行位置访问。

### 15.5 模拟器上传成功但识别失败

最终 `FAILED/KEEP_CLOSED` 也是合法业务结果。检查图片是否清晰、是否包含单个可见车牌，以及格式是否是真实 JPEG/PNG。服务端会实际解码图片，不信任扩展名或 multipart MIME。

```bash
docker compose --env-file .env logs --tail=200 app
```

日志不会记录完整图片、Token、密码或完整 MQTT Payload。

### 15.6 Windows 提示 DLL 或平台插件缺失

确认发布目录保留第 13.3 节的层次，尤其是：

```text
Qt6Mqtt.dll
libgcc_s_seh-1.dll
libstdc++-6.dll
libwinpthread-1.dll
platforms\qwindows.dll
```

不要依靠开发电脑全局 `PATH` 判断发布包是否完整，必须执行第 13.4 节隔离 smoke test。

## 16. 安全和已接受限制

- 明文 HTTP/MQTT 仅限隔离教学网络。
- `.env`、设备 JSON、Token、MQTT 密码和客户端运行日志不得提交或公开。
- MySQL 不对 Windows 或教学网段映射 3306。
- Qt 不直连 MySQL，不上传图片，不执行模型，也不发布抬杆消息。
- Broker 开启持久化，但当前没有应用级 MQTT Outbox。
- Broker 在服务端发布前断开时，最终消息可能永久丢失；数据库最终状态仍必须正确，服务不能崩溃。这是已接受的教学版限制，不是可靠必达。
- 设备未收到最终消息时必须保持闸杆关闭，并按 `recognitionId + revision` 去重。
- 黑白名单首版只用于管理演示，不参与 `OPEN/KEEP_CLOSED` 决策。
- `docker save` 不包含持久卷；`down --volumes` 会删除持久数据。
- 分发服务端镜像和 Qt 包前，必须核对第三方库、Qt MQTT 和两个模型的许可。模型具体来源与条款见 `models/LICENSES.md`。

## 17. 相关工程文档

- `README.md`：服务端入口、当前状态和已经验证的工程命令。
- `docs/REQ-001.md`：业务、HTTP/MQTT、数据库和验收契约的唯一事实来源。
- `docs/REQ-001-design.md`：服务端架构设计。
- `docs/REQ-001-tasks.md`：服务端任务拆分和完成边界。
- `docs/verification/REQ-001-acceptance-report.md`：最终验收结果和发布镜像证据。
- Qt 客户端仓库 `README.md`：客户端工具链、Qt MQTT 和完整测试记录。

当公共接口、DTO、数据库字段、MQTT 消息或业务流程变化时，必须先更新 `docs/REQ-001.md`，再同步修改实现、测试和本指南。
