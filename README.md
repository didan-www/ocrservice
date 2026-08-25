# 车牌识别系统 Linux 服务端

本项目是三端车牌识别教学系统中的 Linux 服务端。服务端接收嵌入式设备上传的车辆图片，异步调用 YOLOv8 和 LPRNet 完成车牌识别，将业务记录写入 MySQL，并通过 MQTT 分别通知 Qt 管理客户端和嵌入式设备。

> 当前状态：TASK-001 至 TASK-024 已全部实现并完成开发者验收及主 Agent 独立复验。所有开发必须先阅读 [REQ-001](docs/REQ-001.md) 和 [AGENTS.md](AGENTS.md)。本文件只记录已经实际验证成功的构建、测试和运行方法。

## 系统组成

| 组件 | 职责 |
|---|---|
| 嵌入式端 | 拍照、HTTP 上传、订阅本设备最终结果、执行抬杆或保持关闭 |
| Linux 服务端 | 鉴权、保存图片、异步排队、模型识别、MySQL 业务、HTTP API、MQTT 发布 |
| MySQL 8 | 管理用户、设备、识别历史和黑白名单数据 |
| Mosquitto | 管理事件和设备识别结果的消息 Broker |
| Qt 管理端 | 登录、心跳、实时监控、历史查询、图片查看、CSV 下载和名单管理 |

Qt 客户端不调用模型、不上传车辆图片、不连接 MySQL，也不向设备发布抬杆结果。

## 核心流程

```text
嵌入式设备先连接 MQTT 并订阅本设备结果主题
    -> POST multipart 上传 JPEG/PNG、captureId、capturedAt
    -> 服务端鉴权、幂等校验并预留队列槽位
    -> 图片落盘，MySQL 写入 PROCESSING/revision=1
    -> HTTP 202 返回 recognitionId
    -> 后台工作线程执行 YOLOv8 + LPRNet
    -> MySQL 更新 SUCCEEDED 或 FAILED
    -> 管理主题发布完整 RecognitionSnapshot
    -> 设备主题发布最终快照和 OPEN/KEEP_CLOSED
```

Qt 管理客户端收到 `PROCESSING` 后按 `recognitionId` 从 HTTP 下载图片；收到最终快照后更新实时结果。历史页面始终从 MySQL 对应的 HTTP API 查询，不依赖 MQTT 补齐历史。

## 已确认的首版功能

- Qt 用户名密码登录、注销、8 小时单访问令牌和 30 秒心跳。
- 登录响应下发管理端 MQTT 地址、账号、密码、主题和同到期时间。
- 嵌入式设备独立 HTTP Token、MQTT 账号、Client ID 和主题 ACL。
- JPEG/PNG 异步上传，按 `deviceId + captureId` 幂等。
- 有界内存识别队列，默认单工作线程和容量 20。
- YOLOv8 单类别车牌定位和 LPRNet CTC 字符识别。
- MySQL 识别日志、历史分页、图片下载和服务端 CSV 导出。
- 黑白名单查询、新增和删除。
- Qt 管理 MQTT 实时事件和设备最终动作消息。
- Docker 应用镜像及 MySQL/Mosquitto Compose 教学部署。

## 明确不包含

- 黑白名单参与放行决策。
- 设备抬杆执行回执。
- MQTT Outbox 或应用级消息补发。
- 生产级 TLS、密钥管理、角色权限、集群、高可用和负载均衡。
- GPU、CUDA、ARM64、视频流识别、摄像头直连和云端 API。
- 自动删除识别图片。

## 已接受限制

MQTT QoS 1 和设备持久会话只保证 Broker 已经接受消息后的交付。若服务端发布结果时 Broker 连接不可用，首版只记录错误，不执行 Outbox 或业务重发，嵌入式设备可能永久收不到该次结果。设备在未收到最终结果时必须保持闸杆关闭。

这是已确认的教学版限制，不应在后续文档中描述为“消息可靠必达”。

## 技术基线

| 类别 | 选择 |
|---|---|
| 系统与架构 | Linux amd64、单进程模块化单体 |
| 语言与构建 | C++17、GCC、CMake |
| HTTP | Crow |
| 图像 | OpenCV 4 |
| 推理 | ONNX Runtime CPU |
| 模型 | YOLOv8、LPRNet |
| 数据库 | MySQL 8 |
| MQTT | Mosquitto、MQTT 3.1.1、QoS 1 |
| 部署 | Docker 应用镜像、Docker Compose |
| 测试 | CTest 与适合各模块的 C++ 测试框架 |

应用镜像包含模型和运行依赖，目标平台固定为 `linux/amd64` CPU。构建不得使用 `-march=native`，以免镜像只能在构建机 CPU 上运行。

## 通信入口

### Qt 管理端

- `POST /api/v1/auth/login`
- `POST /api/v1/auth/logout`
- `POST /api/v1/clients/heartbeat`
- `GET /api/v1/recognitions`
- `GET /api/v1/recognitions/{recognitionId}`
- `GET /api/v1/recognitions/{recognitionId}/image`
- `GET /api/v1/recognitions/export`
- `GET/POST/DELETE /api/v1/access-lists...`
- MQTT：`plate/management/recognition-events`

### 嵌入式端

- `POST /api/v1/devices/{deviceId}/recognitions`
- MQTT：`plate/devices/{deviceId}/recognition-results`

字段、状态、错误码、时限和消息样例必须以 `docs/REQ-001.md` 为准，不能只根据本节摘要实现。

## 教学部署

Compose 包含三个独立容器：

```text
ocrservice-app       Linux 服务程序、模型和运行依赖
ocrservice-mysql     MySQL 8
ocrservice-mqtt      Mosquitto
```

默认暴露：

- HTTP：`8080`
- MQTT：`1883`

持久化内容：

- MySQL 数据。
- Mosquitto 会话和离线 QoS 消息。
- 识别图片。
- 服务端技术日志。

应用镜像也必须能通过环境变量连接已有的外部 MySQL 和 Mosquitto。MySQL 是启动必需依赖；MQTT 暂不可用时应用可以启动并后台重连。

## 固定演示凭据

首版部署包预置一名管理员和一台设备：

| 用途 | 用户名/标识 | 演示密码或 Token |
|---|---|---|
| Qt 登录 | `admin` | `plate-demo-2026` |
| 设备 ID | `device-001` | HTTP Token：`device-http-001` |
| Qt MQTT | `management-client` | `management-mqtt-2026` |
| 服务端 MQTT | `plate-server` | `plate-server-mqtt-2026` |
| 设备 MQTT | `device-001` | `device-mqtt-001` |

这些固定凭据会随教学交付物公开，只能用于隔离演示网络，不得用于生产或暴露到互联网。

新增设备不能只插入 MySQL。后续必须提供统一设备配置脚本，同时更新设备表、Mosquitto 密码文件和 ACL，并输出嵌入式端配置。

## 已验证命令

以下命令是 TASK-024 实际成功命令的脱敏、可复现形式。尖括号参数必须替换为本机路径或凭据文件，不能把秘密写入命令历史、仓库或日志。

全新 Debug/Release 配置显式复用固定版本源码目录，不复用旧二进制、`CMakeCache.txt` 或测试产物：

```bash
PINNED_SOURCE_ROOT="$PWD/build-task021-debug/_deps"
MYSQL_CONCPP_ROOT='<ABSOLUTE_MYSQL_CONNECTOR_CPP_ROOT>'

configure_build() {
  build_type=$1
  build_dir=$2
  cmake -S . -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE="$build_type" \
    -DOCRSERVICE_BUILD_TESTING=ON \
    -DOCRSERVICE_MYSQL_CONCPP_ROOT="$MYSQL_CONCPP_ROOT" \
    -DFETCHCONTENT_SOURCE_DIR_ASIO="$PINNED_SOURCE_ROOT/asio-src" \
    -DFETCHCONTENT_SOURCE_DIR_CROW="$PINNED_SOURCE_ROOT/crow-src" \
    -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$PINNED_SOURCE_ROOT/googletest-src" \
    -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$PINNED_SOURCE_ROOT/nlohmann_json-src" \
    -DFETCHCONTENT_SOURCE_DIR_ONNXRUNTIME="$PINNED_SOURCE_ROOT/onnxruntime-src" \
    -DFETCHCONTENT_SOURCE_DIR_PAHO_MQTT_C="$PINNED_SOURCE_ROOT/paho_mqtt_c-src" \
    -DFETCHCONTENT_SOURCE_DIR_PAHO_MQTT_CPP="$PINNED_SOURCE_ROOT/paho_mqtt_cpp-src" \
    -DFETCHCONTENT_SOURCE_DIR_SPDLOG="$PINNED_SOURCE_ROOT/spdlog-src" \
    -DFETCHCONTENT_SOURCE_DIR_UTF8PROC="$PINNED_SOURCE_ROOT/utf8proc-src"
}

configure_build Debug build-task024-dev-debug
cmake --build build-task024-dev-debug --parallel
ctest --test-dir build-task024-dev-debug --output-on-failure

configure_build Release build-task024-dev-release
cmake --build build-task024-dev-release --parallel
ctest --test-dir build-task024-dev-release --output-on-failure
```

空数据卷 Compose 会占用宿主 `8080` 和 `1883`。运行前必须由操作者使用适合其环境的受控方式释放这两个端口，结束后恢复原宿主服务。本轮验收使用受控 host namespace 包装方式并确认宿主 Mosquitto 最终恢复为 active；公开测试入口为：

```bash
bash tests/system/docker_compose/test_compose.sh
```

设备开通必须使用统一脚本同步 MySQL、Mosquitto 密码文件和 ACL。Token 和密码通过模式 `0600` 的文件传入：

```bash
DEVICE_ID='<DEVICE_ID>'
DEVICE_NAME='<DEVICE_NAME>'
MQTT_USERNAME='<MQTT_USERNAME>'
HTTP_TOKEN_FILE='<HTTP_TOKEN_FILE>'
MQTT_PASSWORD_FILE='<MQTT_PASSWORD_FILE>'
HTTP_BASE_URL='<HTTP_BASE_URL>'
DEVICE_CONFIG_JSON='<DEVICE_CONFIG_JSON>'
scripts/provision-device.sh \
  --device-id "$DEVICE_ID" \
  --device-name "$DEVICE_NAME" \
  --mqtt-username "$MQTT_USERNAME" \
  --http-token-file "$HTTP_TOKEN_FILE" \
  --mqtt-password-file "$MQTT_PASSWORD_FILE" \
  --http-base-url "$HTTP_BASE_URL" \
  --output "$DEVICE_CONFIG_JSON"
```

嵌入式 Full 联调使用全新 Release 构建出的模拟器：

```bash
EMBEDDED_SIMULATOR_BIN=build-task024-dev-release/tests/tests/simulators_embedded/embedded_device_simulator \
  bash tests/system/embedded_e2e/test_embedded_e2e.sh
```

真实 Qt Full 联调在 Windows PowerShell 中运行，Qt 可执行文件和运行库均来自已验收的 Qt revision：

```powershell
$env:QT_E2E_USERNAME = '<QT_USERNAME>'
$env:QT_E2E_PASSWORD = '<QT_PASSWORD>'
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File '<SERVER_REPOSITORY>\tests\system\qt_e2e\run-qt-e2e.ps1' `
  -Executable '<QT_REPOSITORY>\build\task023-main-release\src\plate_client.exe' `
  -QtMqttBin '<QT_MQTT_BIN>' `
  -QtBin '<QT_6_11_1_BIN>' `
  -MingwBin '<MINGW_13_1_0_BIN>' `
  -Slice Full
```

发布镜像固定为 `ocrservice:req-001-v1.0.0` 和 `linux/amd64`。输出目录必须存在、位于仓库外且目标文件不能已存在：

```bash
OUTPUT_DIRECTORY='<ABSOLUTE_OUTPUT_DIRECTORY>'
./scripts/build-release-image.sh \
  --output "$OUTPUT_DIRECTORY/ocrservice-req-001-v1.0.0-linux-amd64.tar"
```

隔离网络无法访问 GitHub 时，可通过本地 HTTP 源提供官方 ONNX Runtime 固定归档；脚本仍强制验证 Dockerfile 中固定的 SHA-256：

```bash
OUTPUT_DIRECTORY='<ABSOLUTE_OUTPUT_DIRECTORY>'
OCRSERVICE_RELEASE_ONNXRUNTIME_URL=http://172.17.0.1:38080/onnxruntime-linux-x64-1.20.1.tgz \
  ./scripts/build-release-image.sh \
  --output "$OUTPUT_DIRECTORY/ocrservice-req-001-v1.0.0-linux-amd64.tar"
```

该 URL 必须指向 SHA-256 与 Dockerfile 固定值完全一致的官方归档。TASK-024 验收实际将输出写入了仓库外目录；报告不记录验收机用户私有绝对路径。

发布脚本拒绝覆盖已有归档、拒绝把归档写入仓库，并拒绝使用相对路径、符号链接逃逸或未提交的产品镜像输入。当前发布归档的 SHA-256 和内部负向验收证据见 [REQ-001 最终验收报告](docs/verification/REQ-001-acceptance-report.md)。

## 文档

- [REQ-001 服务端需求规格](docs/REQ-001.md)：业务、接口、数据库、部署和验收标准的唯一来源。
- [AGENTS.md](AGENTS.md)：后续 AI 代理和开发者必须遵守的工程与协议约束。

当实现状态、可用命令或已知限制发生变化时，更新 README；当业务或公开契约变化时，先更新 REQ-001。
