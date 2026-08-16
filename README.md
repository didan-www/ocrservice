# 车牌识别系统 Linux 服务端

本项目是三端车牌识别教学系统中的 Linux 服务端。服务端接收嵌入式设备上传的车辆图片，异步调用 YOLOv8 和 LPRNet 完成车牌识别，将业务记录写入 MySQL，并通过 MQTT 分别通知 Qt 管理客户端和嵌入式设备。

> 当前状态：需求基线已建立，服务端代码尚未开始实现。所有开发必须先阅读 [REQ-001](docs/REQ-001.md) 和 [AGENTS.md](AGENTS.md)。README 中暂不提供未经实际验证的构建或运行命令。

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

## 教学部署基线

Compose 计划包含三个独立容器：

```text
ocrservice-app       Linux 服务程序、模型和运行依赖
ocrservice-mysql     MySQL 8
ocrservice-mqtt      Mosquitto
```

计划暴露：

- HTTP：`8080`
- MQTT：`1883`

计划持久化：

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

## 计划目录

```text
ocrservice/
├── AGENTS.md
├── README.md
├── CMakeLists.txt
├── Dockerfile
├── docker-compose.yml
├── config/
│   ├── server.json.example
│   └── mosquitto/
├── docs/
│   └── REQ-001.md
├── models/
│   ├── yolov8_plate.onnx
│   └── lprnet.onnx
├── scripts/
│   └── provision-device.sh
├── src/
│   ├── app/
│   ├── domain/
│   ├── http/
│   ├── services/
│   ├── repositories/
│   ├── model/
│   ├── mqtt/
│   ├── storage/
│   └── logging/
├── migrations/
└── tests/
```

目录将在实施阶段按任务逐步创建。当前文档基线不意味着上述代码、脚本或命令已经存在。

## 建议开发顺序

1. CMake、配置、日志和 Docker 构建基线。
2. 领域 DTO、严格 JSON 编解码和 MySQL migration。
3. MySQL Repository、固定演示数据和设备配置脚本。
4. Qt 登录、注销、心跳和会话。
5. 设备上传、图片存储、幂等和有界队列。
6. YOLOv8/LPRNet 模型适配和后处理。
7. MQTT 连接、ACL、管理事件和设备结果。
8. 历史、图片、CSV 和黑白名单 API。
9. Docker Compose 三端集成和实际 Qt/设备模拟器验收。

每一步必须同时提交对应测试，不得先建立大量空接口。

## 文档

- [REQ-001 服务端需求规格](docs/REQ-001.md)：业务、接口、数据库、部署和验收标准的唯一来源。
- [AGENTS.md](AGENTS.md)：后续 AI 代理和开发者必须遵守的工程与协议约束。

当实现状态、可用命令或已知限制发生变化时，更新 README；当业务或公开契约变化时，先更新 REQ-001。
