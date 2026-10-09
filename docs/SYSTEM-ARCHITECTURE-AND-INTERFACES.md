# 车牌识别系统架构与通信接口设计

| 属性 | 内容 |
|---|---|
| 适用项目 | REQ-001 车牌识别教学系统 |
| 文档用途 | 帮助学生理解服务端、Qt 管理客户端和嵌入式端的架构、模块及通信契约 |
| 当前实现基线 | Linux 服务端 TASK-001 至 TASK-024；Qt 客户端 TASK-001 至 TASK-022 及 TASK-023 联调修正 |
| 协议事实来源 | `docs/REQ-001.md` |
| 传输环境 | 隔离教学网络中的明文 HTTP 8080、MQTT 1883 |

> 本文是对当前实现的教学说明，不新增或修改公共协议。开发时如本文与 `docs/REQ-001.md` 不一致，必须以 `docs/REQ-001.md` 为准。任何 HTTP 字段、MQTT 字段、错误码、数据库结构或业务流程变更，都应先修改需求规格，再同步修改实现和测试。

## 1. 系统全景

### 1.1 系统组成与职责

```text
                         +------------------------+
                         |    Qt 管理客户端       |
                         | 登录、监控、历史、名单 |
                         +-----------+------------+
                                     | HTTP 查询/写入
                                     | MQTT 管理事件
                                     v
+----------------+   HTTP 上传   +---+--------------------+   SQL   +---------+
|   嵌入式设备   +-------------->| Linux 车牌识别服务端  +-------->| MySQL 8 |
| 拍照、上传、   |               | 鉴权、排队、识别、API |         +---------+
| 接收动作、控杆 |<------+        +-----------+------------+
+----------------+       |                    |
                         | MQTT 设备结果       | MQTT 发布
                         |                    v
                         +-------------+ Mosquitto
```

| 组件 | 核心职责 | 明确不负责 |
|---|---|---|
| 嵌入式端 | 拍照、生成拍摄标识、HTTP 上传、订阅本设备结果、去重、控制闸杆 | 模型推理、历史查询、名单管理 |
| Linux 服务端 | 设备和管理员鉴权、图片存储、有界排队、模型识别、MySQL 记录、HTTP API、MQTT 发布 | 直接控制物理闸杆、保证应用级消息必达 |
| MySQL | 管理员、设备、识别历史、黑白名单的唯一业务数据源 | 向 Qt 或嵌入式端直接提供连接 |
| Mosquitto | MQTT 会话、ACL、QoS 1 消息传递和持久会话 | 保存可查询的业务历史 |
| Qt 管理客户端 | 登录、心跳、实时监控、图片查看、历史、CSV、黑白名单 | 上传图片、调用模型、直连 MySQL、发布抬杆消息 |

黑白名单在首版仅用于管理功能演示，不参与 `OPEN` 或 `KEEP_CLOSED` 决策。Qt 客户端退出不影响设备上传、服务端识别或设备结果发布。

### 1.2 一次识别的完整业务时序

```text
1. 嵌入式设备连接 MQTT，使用持久会话订阅本设备结果主题。
2. 设备收到成功 SUBACK 后拍照，为本次拍摄生成 captureId。
3. 设备通过 multipart HTTP 上传 image、captureId、capturedAt。
4. 服务端完成设备鉴权、严格协议校验、幂等查询和队列槽位预留。
5. 服务端原子保存图片，在 MySQL 插入 PROCESSING/revision=1 记录。
6. 服务端尽力向 Qt 管理主题发布 PROCESSING 快照，并以 HTTP 202 返回 recognitionId。
7. 后台识别 worker 读取图片，依次执行 YOLOv8 定位和 LPRNet 字符识别。
8. 服务端在 MySQL 事务中提交 SUCCEEDED 或 FAILED，并递增 revision。
9. 服务端先尽力发布 Qt 最终快照，再尽力发布设备最终结果。
10. Qt 更新实时页面；设备按 gateAction 决定抬杆或保持关闭。
```

MySQL 是最终业务事实来源，MQTT 只负责实时通知。服务端发布时 Broker 未接受消息的情况下，数据库状态仍然正确，但 MQTT 消息可能永久丢失。

### 1.3 识别状态模型

| 状态 | 初始/最终 | `revision` | `plateNumber` | `errorCode/errorMessage` | `completedAt/durationMs` |
|---|---|---:|---|---|---|
| `PROCESSING` | 初始 | 初始为 1 | `null` | 均为 `null` | 均为 `null` |
| `SUCCEEDED` | 最终 | 必须递增 | 非空规范化车牌 | 均为 `null` | 均非空 |
| `FAILED` | 最终 | 必须递增 | `null` | 均非空 | 均非空 |

最终状态不能返回 `PROCESSING`。容器启动时遗留的 `PROCESSING` 记录会被改为 `FAILED/SERVER_RESTARTED`，不会自动重新推理。

## 2. Linux 服务端架构

### 2.1 架构风格

服务端采用 C++17 单进程模块化单体。它不是微服务集合，但通过 Controller、Service、领域端口和基础设施适配器保持清晰边界。

```text
Crow HTTP Runtime
        |
        v
Controllers + Request Guards
        |
        v
Application Services
        |
        +--------------------+-------------------+------------------+
        v                    v                   v                  v
Domain Ports           Bounded Queue       Image Storage      MQTT Publisher
        |                    |                                      |
        v                    v                                      v
MySQL Repositories     Recognition Worker                         Mosquitto
                             |
                             v
                    YOLOv8 + LPRNet Adapter
```

主要依赖规则如下：

- HTTP Controller 只负责协议解析、鉴权调用、Service 调用和响应组装，不直接执行 SQL 或模型推理。
- Service 负责事务边界之外的业务顺序、状态转换和错误映射。
- Repository 是业务代码访问 MySQL 的唯一入口，使用参数化 SQL。
- 模型适配层只暴露“单图、单车牌”的稳定识别接口，不向 HTTP 层泄漏 ONNX 张量。
- MQTT 发布是数据库提交后的副作用，发布失败不得回滚或修改数据库状态。
- 图片路径由服务端生成，用户不能提交文件路径。

### 2.2 线程与并发模型

| 执行单元 | 当前职责 |
|---|---|
| Crow HTTP worker，固定 4 个 | 读取请求、协议校验、鉴权、受理和查询；不运行模型 |
| 识别 worker，默认 1 个 | 从有界队列取任务、读取并验证图片、执行模型、提交最终状态 |
| MQTT 控制线程 | 建立连接、断线检测和指数退避重连 |
| 主线程 | 组合应用、等待停止信号、按固定顺序关闭模块 |

识别队列默认容量为 20。上传流程必须先预留队列槽位，再保存图片和插入数据库，避免出现“HTTP 已受理但任务无法入队”的状态。队列满时返回 503，并且不保存图片、不创建数据库记录。

每个识别 worker 独立拥有一套 YOLOv8 Session、LPRNet Session、图片解码器和时钟。HTTP 线程不会被 ONNX 推理阻塞。

### 2.3 功能模块

| 模块 | 代码位置 | 主要职责 |
|---|---|---|
| 配置与组合根 | `src/app/config`、`src/app/runtime` | 严格加载配置；创建并连接所有服务、Repository、队列、模型、HTTP 和 MQTT 对象 |
| 领域层 | `src/domain` | 标识符、识别实体、状态组合、Repository/存储/模型/MQTT 端口 |
| HTTP 协议层 | `src/http/protocol` | 请求限制、严格 JSON/query 解码、稳定协议错误 |
| HTTP 中间件 | `src/http/middleware` | `requestId`、请求守卫、访问日志 |
| HTTP 运行时 | `src/http/server` | Crow 路由分派、统一错误响应、响应大小和 3xx 保护 |
| HTTP Controllers | `src/http/controllers` | 健康、认证、心跳、识别、设备上传、名单接口适配 |
| 认证服务 | `src/services/auth`、`src/security` | bcrypt 校验、设备 Token 摘要校验、内存会话和随机 Token |
| 识别受理 | `src/services/recognition/acceptance` | 幂等、队列预留、图片/数据库一致性和任务提交 |
| 识别 worker | `src/services/recognition/worker` | 图片读取校验、模型调用、最终事务和 MQTT 发布 |
| 查询服务 | `src/services/history`、`src/services/csv` | 历史分页和流式 CSV |
| 名单服务 | `src/services/access_list` | 规范化、分页、精确查询、新增、删除和冲突映射 |
| 健康服务 | `src/services/health` | 模型、MySQL、MQTT 和队列状态组合 |
| MySQL | `src/repositories/mysql` | 连接池、migration、schema 校验和 Repository 实现 |
| 模型 | `src/model` | OpenCV 预处理、YOLOv8 后处理、NMS、LPRNet CTC 解码 |
| MQTT | `src/mqtt` | Paho 传输、重连、严格 Payload 和 QoS 1 发布 |
| 队列 | `src/queue` | 有界槽位预留、任务提交、取出和停止 |
| 图片 | `src/storage` | JPEG/PNG 解码校验、原子落盘、读取和失败补偿 |
| 序列化 | `src/serialization` | 精确 JSON、协议时间和 UTF-8 BOM CSV |
| 日志与文本 | `src/logging`、`src/text` | 脱敏结构化日志、UTF-8 与 Unicode 辅助逻辑 |

### 2.4 服务端代码目录

```text
ocrservice/
├── CMakeLists.txt                 顶层工程、公共编译选项和服务端可执行文件
├── src/
│   ├── main.cpp                   零参数生产入口
│   ├── app/
│   │   ├── config/                ServerConfig 和严格加载器
│   │   └── runtime/               ApplicationRuntime、生产组合和启动恢复
│   ├── domain/                    领域对象、标识符和端口
│   ├── http/
│   │   ├── controllers/           access_list/auth/client/device/health/recognition
│   │   ├── middleware/            requestId、守卫和访问日志
│   │   ├── protocol/              请求/查询解码及 HTTP 策略
│   │   └── server/                Crow Runtime 与 Server
│   ├── services/                  auth/health/history/csv/access_list/recognition
│   ├── repositories/mysql/        connection/migration/repositories
│   ├── model/                     ONNX 模型适配和后处理
│   ├── mqtt/                      Paho 发布器、传输和 Payload
│   ├── queue/                     有界识别队列
│   ├── storage/                   图片校验和 POSIX 原子存储
│   ├── security/                  bcrypt、Token 生成
│   ├── serialization/             json/csv/time
│   ├── logging/                   LoggerFactory、LogSanitizer
│   └── text/                      UTF-8/Unicode 工具
├── migrations/                    版本化 MySQL migration
├── models/                        固定 YOLOv8 和 LPRNet ONNX 文件
├── config/                        服务端配置示例
├── deploy/                        MySQL、Mosquitto 和 Compose 辅助文件
├── scripts/                       设备开通、发布镜像等运维脚本
├── tests/                         单元、组件、契约、集成和系统测试
├── Dockerfile                     linux/amd64 应用镜像
└── docker-compose.yml             app、mysql、mqtt 三服务教学部署
```

各子目录的 `CMakeLists.txt` 将实现组织为独立静态库。`src/app/runtime/ProductionApplication.cpp` 是生产组合根，负责按依赖顺序构造模块，并在失败或停止时反向回收资源。

### 2.5 识别模型和数据存储

- YOLOv8 输入为 `float32 [1,3,640,640]`，保持宽高比 letterbox；NMS 后选择置信度最高的一个车牌。
- LPRNet 输入为 `float32 [1,3,24,94]`，输出为 `[T,68]`，通过 CTC 规则合并重复字符并移除 blank。
- 公共 DTO 不暴露置信度、边界框或 ONNX 张量。
- 图片保存为 `YYYY/MM/DD/{recognitionId}.jpg|png`，数据库只保存相对路径。
- 数据库时间按 UTC 保存，对外统一转换为带 `+08:00` 的协议时间。
- 服务端首版有管理员、设备、识别记录和名单四张业务表；不创建 HTTP 会话表或 MQTT Outbox 表。

## 3. Windows Qt 管理客户端架构

### 3.1 架构风格

客户端采用 C++17、Qt 6 Widgets 的单进程事件驱动架构。UI 不解析 JSON，不直接拼装 HTTP/MQTT 请求；应用服务协调状态，基础设施适配器处理传输和严格协议解析。

```text
QWidget / QAbstractTableModel
             |
             v
Application Services
Session / Heartbeat / Realtime / History / CSV / AccessList
             |
             v
HTTP / MQTT / Config / Logging Adapters
             |
             v
Linux 服务端 HTTP  +  Mosquitto MQTT
```

领域 DTO 不依赖 QWidget、`QNetworkReply` 或 `QMqttClient`。MySQL 和模型对客户端完全不可见。

### 3.2 功能模块

| 模块 | 代码位置 | 主要职责 |
|---|---|---|
| 应用入口与组合 | `src/app` | 校验配置、创建服务、登录窗口和主窗口；协调统一退出顺序 |
| 领域 DTO | `src/domain` | 配置、会话、HTTP 信封、识别快照、名单、分页和请求上下文 |
| 配置 | `src/infrastructure/config` | 严格读取程序旁 `config/client.json` |
| HTTP | `src/infrastructure/http` | 异步传输、超时、取消、严格 JSON、各业务 API Client |
| MQTT | `src/infrastructure/mqtt` | MQTT 3.1.1 连接、订阅、重连和事件严格解析 |
| 日志 | `src/infrastructure/logging` | 7 天保留的脱敏 JSON Lines 技术日志 |
| 会话与心跳 | `src/services/SessionService.*`、`HeartbeatService.*` | 内存凭据、代次、到期、30 秒心跳和 HTTP 状态 |
| 实时监控 | `RealtimeRecognitionStore`、`ImageLoadService`、`ReconnectReconciler` | revision 合并、最近 100 条、图片异步加载和断线校准 |
| 历史与 CSV | `HistoryService`、`CsvDownloadService` | 筛选分页、最后提交条件和原子 CSV 保存 |
| 名单 | `AccessListService` | 白/黑名单查询、写入、超时核实和刷新 |
| UI Model | `src/ui/models` | 实时、历史和名单只读表格模型 |
| UI Pages | `src/ui/pages` | 实时监控、历史记录、黑白名单页面 |
| UI Widgets/Windows | `src/ui/widgets`、`src/ui/windows` | 登录、主窗口、导航、状态指示器和新增对话框 |
| UI Resources | `src/ui/resources` | 编译进 Qt Resource System 的登录图片 |

### 3.3 客户端代码目录

```text
plate_client/
├── CMakeLists.txt
├── CMakePresets.json
├── config/client.json.example
├── src/
│   ├── CMakeLists.txt             模块目标和依赖关系
│   ├── app/                       Application、AppController、main
│   ├── domain/                    DTO、错误、分页、请求上下文
│   ├── infrastructure/
│   │   ├── config/                引导配置加载
│   │   ├── http/                  HttpClient 和各 API Client
│   │   ├── mqtt/                  Subscriber 和 EventCodec
│   │   └── logging/               TechnicalLogger
│   ├── services/                  会话、心跳、实时、历史、CSV、名单
│   └── ui/
│       ├── models/                表格模型
│       ├── pages/                 三个业务页面
│       ├── widgets/               导航、状态、对话框
│       ├── windows/               登录窗口和主窗口
│       └── resources/             qrc 和 PNG
├── tests/                         Qt Test 单元、组件、集成、UI 和安全测试
├── scripts/                       Qt MQTT 引导和本地辅助脚本
└── .deps/                         本地 Qt MQTT 依赖，不提交
```

当前 CMake 模块包括 `plate_client_config`、`plate_client_logging`、`plate_client_protocol`、`plate_client_http`、`plate_client_auth`、`plate_client_heartbeat`、`plate_client_mqtt`、`plate_client_realtime`、`plate_client_reconciliation`、`plate_client_history`、`plate_client_access_list`、`plate_client_ui` 和 `plate_client_app`，最终生成 `plate_client.exe`。

### 3.4 异步、线程和旧响应防护

- `QNetworkAccessManager`、`QMqttClient`、QTimer、Services 和全部 QWidget 位于 UI 线程，通过异步回调工作。
- 图片字节只在线程池中使用 `QImageReader` 预检并解码为 `QImage`。
- `QPixmap` 只能回到 UI 线程后创建，后台线程不能访问 QWidget。
- 每次登录有独立 `sessionGeneration`；页面查询、图片和写操作还有各自的 `requestGeneration`。
- 回调应用前同时核对会话代次、请求代次、活动句柄、业务 ID 和筛选快照，旧结果静默丢弃。
- 这套代次机制避免旧登录、旧页面查询或旧图片覆盖当前界面。

### 3.5 实时数据、历史数据和重连

- MQTT 管理事件按 `recognitionId + revision` 合并；同版本重复消息不会重复改变 UI。
- 最新 `PROCESSING` 记录成为当前车辆并触发 HTTP 图片下载。
- 旧车辆的最终事件只更新列表，不抢占新车辆图片区。
- 实时列表只保留本次登录最近 100 条，退出后清空。
- MQTT 重连成功后，只对当前仍为 `PROCESSING` 的记录执行一次 HTTP 详情校准，不自动加载断线期间全部历史。
- 历史、CSV 和名单始终通过 HTTP 访问服务端，MySQL 才是可查询事实来源。

### 3.6 会话与退出生命周期

登录成功后，HTTP Token 与 MQTT 凭据只保存在内存 `SessionContext` 中。退出、关闭、Token 到期或当前会话收到 HTTP 401 时，统一执行幂等终止流程：

1. 使旧会话代次立即失效。
2. 停止心跳和 MQTT 重连调度。
3. 取消图片重试、CSV 原子写入和其他旧 HTTP 请求。
4. 尽力发送注销请求，但不阻塞本地退出。
5. 断开 MQTT 并清除订阅和凭据。
6. 清空实时、历史、名单、图片和会话内存状态。
7. 返回干净登录页或退出程序。

## 4. 通信契约通用规则

### 4.1 基址、传输和限制

- 教学基址：`http://<server-host>:8080`。
- 当前 MQTT：`<server-host>:1883`，MQTT 3.1.1 明文传输。
- Qt 配置使用 `loginBaseUrl` 指定 HTTP 基址，并且明文教学环境必须显式设置 `allowInsecureTransport=true`。
- 当前版本未实现 HTTPS/MQTTS；明文只允许隔离教学网络，不适合互联网或生产环境。
- 原始 URL 最大 8 KiB，请求行与全部 Header 合计最大 80 KiB。
- JSON 请求和响应最大 2 MiB；multipart 整体最大 11 MiB；上传图片 part 最大 10 MiB；Qt 图片下载最大 20 MiB。
- 服务端不提供 3xx、隐式 HEAD 或 OPTIONS，也不补全尾斜杠。

除图片和 CSV 成功响应外，服务端返回 `Content-Type: application/json`。JSON POST 请求使用 `application/json`，允许唯一 `charset=utf-8` 参数。设备上传使用 `multipart/form-data`。

### 4.2 严格 JSON 信封

所有 JSON HTTP 响应精确包含五个根字段：

```json
{
  "success": true,
  "code": "OK",
  "message": "",
  "requestId": "4a4911ce-041c-42ad-bff9-1dd5dc70de66",
  "data": {}
}
```

| 字段 | 类型 | 规则 |
|---|---|---|
| `success` | boolean | 成功为 `true`，失败为 `false` |
| `code` | string | 成功固定 `OK`；失败为稳定错误码 |
| `message` | string | 可展示中文信息，业务逻辑不能解析此自由文本 |
| `requestId` | string | 服务端为本次 HTTP 请求生成的 UUID v4 |
| `data` | object/null | 有 DTO 的成功为对象；空成功和所有失败为 `null` |

客户端必须拒绝未知字段、缺字段、错误类型和错误可空组合。所有 JSON integer 必须在 `[-(2^53-1), 2^53-1]` 内，业务 ID 必须为正数。

### 4.3 标识符和时间

| 标识 | 生成方 | 用途 |
|---|---|---|
| `clientId` | 每个 Qt 安装生成并固化 | HTTP 登录会话绑定及 Qt MQTT Client ID |
| `deviceId` | 运维开通 | 设备身份、MQTT Client ID 和主题组成部分 |
| `captureId` | 嵌入式端每次新拍摄生成 | HTTP 重试幂等键的一部分 |
| `recognitionId` | 服务端生成 | 识别记录、图片资源和消息去重主键 |
| `requestId` | 服务端为每个 HTTP 请求生成 | 日志追踪和错误定位 |

UUID 输入使用无花括号 canonical `8-4-4-4-12` 形式，不能为 nil；服务端输出小写 UUID。`deviceId` 区分大小写，必须匹配 `[A-Za-z0-9][A-Za-z0-9._-]{0,63}`。

所有协议时间精确使用 `YYYY-MM-DDTHH:mm:ss.SSS+08:00`，例如 `2026-08-15T12:30:45.123+08:00`。不接受 `Z`、其他偏移、缺少或多于三位毫秒、非法日期或闰秒。

### 4.4 Bearer 鉴权

```http
Authorization: Bearer <token>
```

- Header 必须恰好出现一次。
- `Bearer` 与 Token 之间必须恰好一个 ASCII 空格。
- Qt 管理 API 使用登录返回的内存 Token。
- 每台嵌入式设备使用自己独立的 HTTP Token。
- Token、密码和完整 Authorization Header 不得写入日志。

## 5. Qt 与服务端 HTTP 接口

### 5.1 接口总表

| 方法 | 路径 | 鉴权 | 成功响应 | 用途 |
|---|---|---|---|---|
| `GET` | `/health` | 无 | 200 JSON | 服务整体健康探测，不是 Qt 心跳 |
| `POST` | `/api/v1/auth/login` | 无 | 200 JSON | Qt 登录并取得 HTTP/MQTT 会话 |
| `POST` | `/api/v1/auth/logout` | Qt Bearer | 200，`data=null` | 注销当前 Token |
| `POST` | `/api/v1/clients/heartbeat` | Qt Bearer | 200，`data=null` | Qt HTTP 会话心跳 |
| `GET` | `/api/v1/recognitions` | Qt Bearer | 200 分页 DTO | 历史记录查询 |
| `GET` | `/api/v1/recognitions/export` | Qt Bearer | 200 CSV | 按筛选导出完整历史 |
| `GET` | `/api/v1/recognitions/{recognitionId}` | Qt Bearer | 200 快照 | 单条详情/重连校准 |
| `GET` | `/api/v1/recognitions/{recognitionId}/image` | Qt Bearer | 200 JPEG/PNG | 下载原始识别图片 |
| `GET` | `/api/v1/access-lists` | Qt Bearer | 200 分页 DTO | 名单分页和搜索 |
| `GET` | `/api/v1/access-lists/lookup` | Qt Bearer | 200 名单 DTO | 按规范化车牌精确查询 |
| `POST` | `/api/v1/access-lists` | Qt Bearer | 200 名单 DTO | 新增白/黑名单 |
| `DELETE` | `/api/v1/access-lists/{id}` | Qt Bearer | 200，`data=null` | 删除名单记录 |
| `POST` | `/api/v1/devices/{deviceId}/recognitions` | 设备 Bearer | 首次 202 JSON | 嵌入式图片上传 |

### 5.2 健康检查

`GET /health`

成功 `data` 精确字段：

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

- 模型和 MySQL 正常时返回 HTTP 200。
- MQTT 断开时返回 HTTP 200，`status=DEGRADED`、`mqtt=DOWN`。
- 模型或 MySQL 不可用时返回 HTTP 503、`SERVICE_UNAVAILABLE`，且 `data=null`。
- Qt 心跳不调用该接口；心跳只验证 HTTP 会话可访问。

### 5.3 登录

`POST /api/v1/auth/login`

请求体精确字段：

```json
{
  "username": "<username>",
  "password": "<password>",
  "clientId": "11111111-2222-4333-8444-555555555555"
}
```

| 字段 | 规则 |
|---|---|
| `username` | 1 至 64 个 Unicode 码点 |
| `password` | 1 至 72 个 UTF-8 字节，不含 NUL |
| `clientId` | 非 nil canonical UUID，每个客户端安装唯一 |

成功 `data` 精确为：

```json
{
  "displayName": "演示管理员",
  "accessToken": "<opaque-memory-token>",
  "expiresAt": "2026-08-15T20:30:00.000+08:00",
  "mqtt": {
    "host": "192.168.137.128",
    "port": 1883,
    "tls": false,
    "username": "<management-mqtt-user>",
    "password": "<management-mqtt-password>",
    "expiresAt": "2026-08-15T20:30:00.000+08:00",
    "eventTopic": "plate/management/recognition-events"
  }
}
```

两个 `expiresAt` 必须完全相同。Token 默认有效 8 小时，只驻留服务端和客户端内存；同一 `clientId` 再次登录会立即使旧 HTTP Token 失效，并会因 MQTT Client ID 相同影响旧 MQTT 连接。

### 5.4 注销与心跳

`POST /api/v1/auth/logout`

- 需要 Qt Bearer Token。
- raw query 和 body 都必须为空，不发送 `{}`。
- 成功 HTTP 200，`data=null`；Token 立即失效。

`POST /api/v1/clients/heartbeat`

```json
{
  "clientId": "11111111-2222-4333-8444-555555555555",
  "appVersion": "0.1.0"
}
```

- 两字段都必须非空，`clientId` 必须与当前 Token 会话绑定值一致。
- 成功 HTTP 200，`data=null`。
- 心跳不延长 Token 有效期，也不探测模型、MySQL 或 MQTT。
- Qt 默认每 30 秒发送一次，单次总时限 5 秒。

### 5.5 `RecognitionSnapshot`

详情、历史分页项和管理 MQTT 事件共用同一个严格 DTO：

```json
{
  "schemaVersion": 1,
  "recognitionId": "a15c7268-b211-4a4c-a765-49b922af1910",
  "revision": 1,
  "deviceId": "device-001",
  "status": "PROCESSING",
  "plateNumber": null,
  "errorCode": null,
  "errorMessage": null,
  "capturedAt": "2026-08-15T12:30:44.000+08:00",
  "startedAt": "2026-08-15T12:30:45.000+08:00",
  "completedAt": null,
  "durationMs": null
}
```

| 字段 | 类型 | 规则 |
|---|---|---|
| `schemaVersion` | integer | 固定为 1 |
| `recognitionId` | string | UUID |
| `revision` | integer | 正整数，最大 `2^53-1` |
| `deviceId` | string | 合法设备 ID |
| `status` | string | `PROCESSING/SUCCEEDED/FAILED` |
| `plateNumber` | string/null | 成功时为 1 至 16 个 Unicode 码点规范化车牌 |
| `errorCode` | string/null | 失败时非空，最多 64 字符 |
| `errorMessage` | string/null | 失败时非空，最多 512 个 Unicode 码点 |
| `capturedAt` | string | 设备拍摄时间 |
| `startedAt` | string | 服务端受理开始时间 |
| `completedAt` | string/null | 最终状态非空 |
| `durationMs` | integer/null | 最终状态非负，最大 `2^53-1` |

所有字段始终存在；可空字段无值时必须显式为 JSON `null`。

### 5.6 识别详情和图片

`GET /api/v1/recognitions/{recognitionId}`

- 成功 HTTP 200，`data` 为完整 `RecognitionSnapshot`。
- 不存在返回 404 `RECOGNITION_NOT_FOUND`。

`GET /api/v1/recognitions/{recognitionId}/image`

- 成功返回原始图片字节，Content-Type 只能为 `image/jpeg` 或 `image/png`。
- 成功体最大 20 MiB，不使用 JSON 信封。
- 记录或文件不存在返回 404 `IMAGE_NOT_FOUND` JSON 失败信封。
- 不返回文件路径或重定向 URL。

### 5.7 历史分页

`GET /api/v1/recognitions`

| query 参数 | 必填 | 规则 |
|---|---|---|
| `startTime` | 是 | 包含边界的 `+08:00` 协议时间 |
| `endTime` | 是 | 不包含边界，必须大于 `startTime` |
| `deviceId` | 否 | 省略表示全部；出现时不能空 |
| `page` | 是 | 1 至 2147483647 |
| `pageSize` | 是 | 首版只能为 100 |

筛选采用 `[startTime, endTime)`，排序固定为 `captured_at DESC, recognition_id DESC`。成功 `data`：

```json
{
  "items": [],
  "page": 1,
  "pageSize": 100,
  "total": 0
}
```

`items` 中每项都是完整快照。页码超过末页时返回空 `items`，不自动改写请求页码。

### 5.8 CSV 导出

`GET /api/v1/recognitions/export`

- query 使用历史相同的 `startTime/endTime/deviceId`，不接受 `page/pageSize`。
- 成功 `Content-Type: text/csv; charset=utf-8`，以 UTF-8 BOM 开头。
- 固定十列：`记录ID,设备,车牌号,状态,错误码,错误信息,拍摄时间,开始时间,完成时间,耗时(ms)`。
- 每行使用 CRLF，字段按 RFC 4180 转义。
- CSV 在服务端和客户端均流式处理，不受 2 MiB JSON 限制。

### 5.9 黑白名单

名单 DTO 精确字段：

```json
{
  "id": 1,
  "listType": "WHITE",
  "plateNumber": "京A12345",
  "remark": "教学测试车辆",
  "createdBy": "演示管理员",
  "createdAt": "2026-08-15T12:00:00.000+08:00"
}
```

| 字段 | 类型 | 规则 |
|---|---|---|
| `id` | integer | 正整数，最大 `2^53-1` |
| `listType` | string | `WHITE` 或 `BLACK` |
| `plateNumber` | string | 1 至 16 个 Unicode 码点的规范化车牌 |
| `remark` | string | 可为空字符串，不能为 `null`，最多 200 个 Unicode 码点 |
| `createdBy` | string | 创建时的管理员显示名快照，非空 |
| `createdAt` | string | `+08:00` 协议时间 |

`GET /api/v1/access-lists`

| query 参数 | 规则 |
|---|---|
| `listType` | `WHITE` 或 `BLACK` |
| `keyword` | 可为空，按规范化车牌字面量包含匹配 |
| `page` | 1 至 2147483647 |
| `pageSize` | 只能为 100 |

成功 `data` 为 `{items,page,pageSize,total}`。排序为 `created_at DESC, id DESC`。

`GET /api/v1/access-lists/lookup?plateNumber=<plate>`

- 服务端先规范化，再全局精确查询。
- 找到返回 200 和名单 DTO；未找到返回 404 `ACCESS_LIST_NOT_FOUND`。

`POST /api/v1/access-lists`

```json
{
  "listType": "WHITE",
  "plateNumber": "京A12345",
  "remark": "教学测试车辆"
}
```

- 三个字段必须全部存在，`remark` 可为空字符串但不能为 `null`，最多 200 个 Unicode 码点。
- 成功返回 200 和新建名单 DTO。
- 车牌已在白名单返回 `ACCESS_LIST_CONFLICT_WHITE`，已在黑名单返回 `ACCESS_LIST_CONFLICT_BLACK`。

`DELETE /api/v1/access-lists/{id}`

- `id` 为 1 至 `2^53-1` 的 ASCII 十进制整数；raw query 和 body 为空。
- 成功 200，`data=null`；不存在返回 404 `ACCESS_LIST_NOT_FOUND`。

车牌规范化规则为：去除规定的首尾 Unicode 空白、仅将 ASCII `a-z` 转大写、拒绝内部空白、长度 1 至 16 个 Unicode 码点。规范化车牌在黑白名单间全局唯一。

### 5.10 Qt HTTP 超时与重试

| 请求 | Qt 总时限 | 自动重试 |
|---|---:|---|
| 登录 | 10 秒 | 否 |
| 注销 | 5 秒 | 否；不阻塞本地退出 |
| 心跳 | 5 秒 | 否，等待下一个 30 秒周期 |
| 详情、历史、名单查询 | 10 秒 | 否 |
| 图片 | 每次 30 秒 | 仅特定网络/超时/500/502/503/504，间隔 1 秒和 3 秒，最多 3 次尝试 |
| 名单新增/删除 | 10 秒 | 不重发写请求；超时后精确查询一次核实 |
| CSV | 120 秒 | 否 |

## 6. MQTT 业务与消息字段

### 6.1 通用参数和会话

- 协议固定 MQTT 3.1.1。
- QoS 固定 1。
- retain 固定 `false`。

| 客户端 | MQTT Client ID | `cleanSession` |
|---|---|---|
| Linux 服务端发布器 | `plate-server` | `true` |
| Qt 管理客户端 | 本地配置 `clientId` UUID 原文 | `true` |
| 嵌入式设备 | `deviceId` | `false` |

设备必须保证 `deviceId` 唯一。相同 Client ID 同时连接时，Broker 会按 MQTT 规则断开旧连接。

### 6.2 主题与 ACL

| 主体 | 允许发布 | 允许订阅 |
|---|---|---|
| 服务端账号 | 管理主题和所有设备结果主题 | 无 |
| Qt 管理账号 | 无 | `plate/management/recognition-events` |
| 某设备账号 | 无 | 仅 `plate/devices/{deviceId}/recognition-results` |

Qt 和设备端必须订阅精确主题，不使用 `#` 或 `+` 通配符。Mosquitto 默认拒绝未授权主题；设备不能订阅管理主题或其他设备主题。

### 6.3 Qt 管理事件

主题：

```text
plate/management/recognition-events
```

- 同时发布 `PROCESSING` 和最终快照。
- Payload 根对象就是完整 `RecognitionSnapshot`，不带 HTTP 五字段信封。
- 不允许增加 `gateAction`、`captureId`、图片路径、置信度或边界框。
- `PROCESSING` 发布前，数据库事务已提交且图片已经可以下载。

### 6.4 嵌入式设备最终结果

主题：

```text
plate/devices/{deviceId}/recognition-results
```

只发布 `SUCCEEDED` 或 `FAILED`。Payload 是完整快照字段加一个 `gateAction`：

```json
{
  "schemaVersion": 1,
  "recognitionId": "a15c7268-b211-4a4c-a765-49b922af1910",
  "revision": 2,
  "deviceId": "device-001",
  "status": "SUCCEEDED",
  "plateNumber": "京A12345",
  "errorCode": null,
  "errorMessage": null,
  "capturedAt": "2026-08-15T12:30:44.000+08:00",
  "startedAt": "2026-08-15T12:30:45.000+08:00",
  "completedAt": "2026-08-15T12:30:45.120+08:00",
  "durationMs": 120,
  "gateAction": "OPEN"
}
```

固定映射：

| `status` | `gateAction` | 设备动作 |
|---|---|---|
| `SUCCEEDED` | `OPEN` | 按设备本地配置抬杆 |
| `FAILED` | `KEEP_CLOSED` | 保持闸杆关闭 |

服务端不发送抬杆持续时间，也没有动作确认 HTTP API 或 MQTT 主题。设备收到重复 QoS 1 消息时必须按 `recognitionId + revision` 去重。

### 6.5 消息可靠性边界

QoS 1 表示 Broker 接受消息后至少一次交付，因此消费者必须处理重复。当前服务端没有 MQTT Outbox、应用级重发或跨重启恢复：

- Broker 未接受服务端发布时，消息可能永久丢失。
- MySQL 最终状态不会因此回滚。
- Qt 可通过历史 API 查询事实，但不会自动补齐全部断线实时事件。
- 嵌入式端没有结果查询 API，未收到最终消息时必须保持闸杆关闭。

## 7. 嵌入式端开发规范

本章面向后续自行开发嵌入式端程序的学生。嵌入式端既是图片数据来源，也是最终闸杆动作的执行者，其实现必须同时包含 HTTP 上传客户端和 MQTT 结果订阅客户端。

### 7.1 嵌入式端职责和边界

必须实现：

- 加载本设备身份和通信配置。
- 使用唯一 `deviceId` 建立 MQTT 持久会话。
- 订阅本设备精确结果主题并等待成功 SUBACK。
- 拍照并生成稳定 `captureId`、`capturedAt`。
- 严格构造 multipart HTTP 请求并处理幂等响应。
- 严格解析最终 MQTT Payload，按业务键去重。
- 只有合法 `SUCCEEDED/OPEN` 才执行一次抬杆动作。
- 断网、重启、超时、非法消息或不确定状态时保持闸杆关闭。

不应实现：

- 直连 MySQL、调用 YOLOv8/LPRNet 或自行决定识别结果。
- 订阅 Qt 管理主题、其他设备主题或通配符主题。
- 上传文件路径、扩展公共 DTO 或向服务端发送动作回执。
- 使用黑白名单决定首版闸杆动作。

### 7.2 设备开通后需要的配置

每台设备必须通过服务端统一运维脚本开通，使 MySQL 设备记录、Mosquitto 密码文件和 ACL 同步。设备侧需要安全保存以下配置：

| 配置 | 含义 |
|---|---|
| `deviceId` | 唯一设备 ID，同时作为 MQTT Client ID |
| `http.baseUrl` | 例如 `http://<server-host>:8080` |
| `http.bearerToken` | 本设备独立 HTTP Token |
| `http.uploadPath` | `/api/v1/devices/{deviceId}/recognitions` |
| `mqtt.host/port/tls` | Broker 地址、端口和当前 `tls=false` |
| `mqtt.username/password` | 本设备独立 MQTT 凭据 |
| `mqtt.clientId` | 必须等于 `deviceId` |
| `mqtt.cleanSession` | 必须为 `false` |
| `mqtt.qos` | 必须为 1 |
| `mqtt.resultTopic` | 本设备精确结果主题 |

配置中的 Token 和密码属于秘密，不能写入源代码、Git、普通日志或命令历史。设备日志只能记录稳定错误分类、非敏感设备 ID、`captureId`、`recognitionId` 和状态。

### 7.3 启动状态机

```text
BOOT
  -> LOAD_CONFIG
  -> GATE_CLOSED
  -> MQTT_CONNECTING
  -> MQTT_CONNECTED
  -> SUBSCRIBING_EXACT_TOPIC
  -> READY_FOR_CAPTURE       只有成功 SUBACK 后进入

任一连接/订阅暂时失败
  -> GATE_CLOSED
  -> BACKOFF
  -> MQTT_CONNECTING

认证、授权、Client ID 或协议配置错误
  -> GATE_CLOSED
  -> CONFIGURATION_ERROR     不继续高频重试，等待人工修复
```

设备不能在“TCP 已连接”或“CONNECT 已成功”时就上传，必须等精确结果主题的订阅成功 SUBACK。这样可减少识别很快完成但设备尚未建立接收通道的竞态。

### 7.4 HTTP 上传接口详细要求

`POST /api/v1/devices/{deviceId}/recognitions`

请求头：

```http
Authorization: Bearer <device-http-token>
Content-Type: multipart/form-data; boundary=<unique-boundary>
```

multipart 精确包含三个且仅三个 part，顺序任意：

| part | 类型 | 设备端要求 |
|---|---|---|
| `image` | binary | 实际 JPEG 或 PNG，非空，最大 10 MiB |
| `captureId` | UTF-8 text | 本次新拍摄生成的 UUID；重试保持不变 |
| `capturedAt` | UTF-8 text | 拍摄时刻，精确 `YYYY-MM-DDTHH:mm:ss.SSS+08:00` |

图片解码后还必须满足宽、高分别不超过 8192，总像素不超过 4000 万。服务端按实际魔数和 OpenCV 解码判断格式，不信任文件扩展名或 multipart MIME。

设备端 multipart 构造必须注意：

- delimiter 和 Header 行使用 CRLF，不使用 LF-only。
- Body 直接以首个 `--boundary\r\n` 开始，不加 preamble 或 epilogue。
- 三个 part 名称区分大小写，各出现一次，不添加未知或重复 part。
- 文本值不要额外 trim 或添加换行；closing delimiter 后只允许空或一个 CRLF。
- 不发送 query 参数，不发送用户可控文件路径。

首次受理成功返回 HTTP 202，`data` 精确为：

```json
{
  "recognitionId": "a15c7268-b211-4a4c-a765-49b922af1910",
  "captureId": "f06a2578-5af0-4ca8-98a9-e585ef8f0ea4",
  "status": "PROCESSING"
}
```

该对象仍位于通用五字段 HTTP 信封的 `data` 中。设备应保存 `captureId -> recognitionId/status` 的最小在途映射，便于日志关联和重启后识别幂等响应。

即使幂等请求以 HTTP 200 返回最终 `status`，该三字段受理 DTO 也不包含 `gateAction`，不能作为抬杆授权。设备只能依据本设备 MQTT 主题上严格合法的最终消息执行动作。

### 7.5 `captureId` 幂等与本地持久化

服务端以 `(deviceId, captureId)` 作为幂等键，并同时比较图片 SHA-256 和拍摄时间：

| 重试内容 | HTTP | 结果 |
|---|---:|---|
| 相同键、相同原始图片字节、相同 `capturedAt`，原记录处理中 | 202 | 返回同一 `recognitionId` 和 `PROCESSING` |
| 相同键、相同图片、相同时间，原记录已完成 | 200 | 返回同一 `recognitionId` 和最终状态 |
| 相同键但图片或拍摄时间不同 | 409 | `CAPTURE_ID_CONFLICT` |

因此一次拍摄在 HTTP 重试期间必须复用完全相同的 `captureId`、`capturedAt` 和原始压缩图片字节。重新编码 JPEG、改变 metadata、重新生成时间或换图，都可能使同一幂等键冲突。

建议设备对尚未确认受理的拍摄持久化以下最小记录：

```text
captureId
capturedAt
image bytes 或可验证的固定本地对象引用
image SHA-256
upload state
可选 recognitionId
```

只有收到合法 200/202 幂等响应后才能把“上传未确认”标记为“服务端已受理”。设备短暂重启后若要重试，必须能够恢复同一组数据。

### 7.6 HTTP 错误处理和重试矩阵

| HTTP/code | 含义 | 设备建议行为 |
|---|---|---|
| 200/202 `OK` | 已受理或幂等返回 | 校验信封和三字段 DTO，保存 `recognitionId` |
| 400 `INVALID_REQUEST` | multipart、UUID、时间或字段非法 | 不盲目重试；修复程序或丢弃坏任务，保持闸杆关闭 |
| 400 `IMAGE_INVALID` | 格式、尺寸或解码非法 | 不重试同一图片；检查摄像头编码 |
| 401 `DEVICE_UNAUTHORIZED` | Token 缺失或无效 | 停止上传，等待重新配置凭据 |
| 403 `DEVICE_FORBIDDEN` | Token 不属于路径设备 | 停止上传，检查 `deviceId` 和配置配对 |
| 403 `DEVICE_DISABLED` | 设备已禁用 | 停止业务，等待管理员启用 |
| 409 `CAPTURE_ID_CONFLICT` | 同一幂等键对应不同内容 | 不覆盖、不换内容重试；记录故障并生成新的拍摄任务 |
| 413 `IMAGE_TOO_LARGE` | 图片超过 10 MiB | 调整拍摄分辨率或编码质量后作为新拍摄处理 |
| 413 `REQUEST_TOO_LARGE` | URL/Header/multipart 整体超限 | 修正请求构造，不重复发送原请求 |
| 500 `IMAGE_STORAGE_ERROR` | 服务端文件保存失败 | 保留原幂等数据，退避后有限重试 |
| 500 `INTERNAL_ERROR` | 未分类服务错误 | 保留原数据，退避后有限重试并告警 |
| 503 `RECOGNITION_QUEUE_FULL` | 识别队列满 | 保留原数据，指数退避并限制上传速率 |
| 503 `DATABASE_UNAVAILABLE` | MySQL 暂不可用 | 保留原数据，退避重试 |
| 网络断开/超时 | 无法确定服务端是否已受理 | 必须用同一幂等数据重试 |

重试应有上限、指数退避和抖动，避免多台设备同时恢复造成请求风暴。教学规模总上传峰值不超过每秒 1 张；设备本地队列也应有容量上限和明确溢出策略。

### 7.7 MQTT 消息解析和去重

设备收到消息后应按以下顺序处理：

1. 确认消息来自本设备精确主题，QoS 为 1。
2. 将 Payload 作为 UTF-8 JSON 根对象解析。
3. 要求字段集合与“完整快照 + `gateAction`”完全相等，拒绝未知或缺失字段。
4. 严格校验 UUID、设备 ID、整数范围、时间、枚举和状态相关可空组合。
5. 要求 `deviceId` 与本机配置完全一致。
6. 只接受 `SUCCEEDED` 或 `FAILED`，拒绝 `PROCESSING`。
7. 校验 `SUCCEEDED -> OPEN`、`FAILED -> KEEP_CLOSED` 映射。
8. 以 `recognitionId + revision` 查询持久或可靠的去重记录。
9. 已执行过相同键则只确认 MQTT 消费，不重复动作。
10. 新消息先安全记录去重/动作意图，再按映射执行一次动作。

若设备同时收到同一 `recognitionId` 的不同 revision，应把 revision 视为单调版本；不执行旧于已经处理版本的消息。虽然当前服务端正常只发布一次最终 revision，防御性实现仍应拒绝倒序状态覆盖。

### 7.8 闸杆安全策略

设备必须采用“失败关闭”原则：

- 上电默认关闭。
- MQTT 未连接、未订阅或处于重连时关闭。
- HTTP 未受理、超时且未确认、收到任何 4xx/5xx 时关闭。
- Payload 解析失败、字段不完整、未知字段、设备 ID 不匹配时关闭。
- `FAILED/KEEP_CLOSED` 时关闭。
- 只有严格合法且未执行过的 `SUCCEEDED/OPEN` 才允许抬杆。
- 抬杆持续时间和复位由设备本地安全配置控制；服务端不提供持续时间。
- 未收到最终 MQTT 结果不能根据 HTTP 202、名单数据或超时猜测抬杆。

首版没有动作确认接口。设备可以在本地日志记录动作结果，但不能自行发明 ACK 主题或调用不存在的 HTTP API。

### 7.9 设备主循环伪代码

```text
load_and_validate_config()
force_gate_closed()

while running:
    if mqtt_not_ready:
        connect(clientId=deviceId, cleanSession=false)
        subscribe(exact_result_topic, qos=1)
        wait_for_successful_suback()
        if failed:
            force_gate_closed()
            backoff()
            continue

    process_pending_mqtt_messages_strictly()

    if capture_triggered and local_upload_queue_has_capacity:
        image = capture_exact_jpeg_or_png_bytes()
        job = persist_new_job(
            captureId=new_uuid(),
            capturedAt=now_in_exact_plus_08_format(),
            imageBytes=image,
            sha256=hash(image))
        enqueue(job)

    for due_job in bounded_upload_queue:
        response = upload_exact_multipart(due_job)
        if response_is_valid_200_or_202:
            persist_recognition_mapping(response)
            mark_upload_accepted(due_job)
        elif response_is_retryable_or_transport_unknown:
            schedule_backoff_using_same_job_bytes(due_job)
        else:
            mark_manual_attention(due_job)
            force_gate_closed()

on_mqtt_message(message):
    result = strict_decode(message)
    if invalid_or_for_other_device(result):
        force_gate_closed()
        return
    if already_processed(result.recognitionId, result.revision):
        return
    if result.status == SUCCEEDED and result.gateAction == OPEN:
        persist_action_dedup_key_before_or_atomically_with_action()
        open_gate_for_local_configured_duration()
    else:
        persist_result_key()
        force_gate_closed()
```

实际程序需要根据设备文件系统、掉电保护和闸杆控制器能力设计“去重记录”和“动作”的原子性。目标是即使 QoS 1 重复投递、应用重启或网络抖动，也不对同一 `recognitionId + revision` 重复抬杆。

### 7.10 嵌入式端测试清单

学生提交嵌入式端程序前，至少验证：

- 唯一 Client ID、`cleanSession=false`、QoS 1、精确主题和成功 SUBACK 门禁。
- MQTT 断线重连后仍能恢复持久会话或重新确认订阅。
- JPEG 和 PNG 合法上传；超大、损坏和错误尺寸图片被正确处理。
- `captureId`、时间、图片三者在网络超时重试中保持完全不变。
- 首次 202、处理中幂等 202、完成后幂等 200 和冲突 409。
- 401/403/400/409/413 不盲目重试；503 和未知传输结果使用有界退避。
- 成功和失败 MQTT Payload 的严格字段、状态和动作校验。
- QoS 1 重复消息只执行一次动作，倒序 revision 不覆盖新状态。
- Broker 在发布前断开时，设备始终保持闸杆关闭。
- 设备重启后能恢复未确认上传的幂等数据和已执行动作去重键。
- 持续每秒 1 张的教学负载下，本地队列有界且程序保持响应。

服务端仓库已经包含 C++ 嵌入式设备模拟器和系统联调入口，可作为协议行为参考：

```text
tests/simulators/embedded/
tests/system/embedded_e2e/test_embedded_e2e.sh
```

模拟器用于验证连接、SUBACK、上传、最终动作、持久会话和重复消息去重，但真实嵌入式实现仍需补充摄像头、掉电存储、物理闸杆和硬件看门狗逻辑。

## 8. 稳定错误码

### 8.1 HTTP 错误码

| HTTP | `code` | 场景 |
|---:|---|---|
| 200/202 | `OK` | 成功 |
| 400 | `INVALID_REQUEST` | JSON、query、multipart、参数或枚举非法 |
| 400 | `IMAGE_INVALID` | 图片格式、尺寸或解码非法 |
| 401 | `AUTH_INVALID_CREDENTIALS` | Qt 用户名或密码错误 |
| 401 | `AUTH_TOKEN_INVALID` | Qt Token 无效 |
| 401 | `AUTH_TOKEN_EXPIRED` | Qt Token 到期 |
| 401 | `DEVICE_UNAUTHORIZED` | 设备 Token 缺失或无效 |
| 403 | `USER_DISABLED` | 管理用户禁用 |
| 403 | `DEVICE_DISABLED` | 设备禁用 |
| 403 | `DEVICE_FORBIDDEN` | Token 与路径设备不匹配 |
| 404 | `RECOGNITION_NOT_FOUND` | 识别记录不存在 |
| 404 | `IMAGE_NOT_FOUND` | 图片记录或文件不存在 |
| 404 | `ACCESS_LIST_NOT_FOUND` | 名单记录不存在 |
| 404 | `ROUTE_NOT_FOUND` | 路径不存在 |
| 405 | `METHOD_NOT_ALLOWED` | 路径存在但方法不允许 |
| 409 | `CAPTURE_ID_CONFLICT` | 相同设备和 captureId 对应不同上传 |
| 409 | `ACCESS_LIST_CONFLICT_WHITE` | 车牌已在白名单 |
| 409 | `ACCESS_LIST_CONFLICT_BLACK` | 车牌已在黑名单 |
| 413 | `IMAGE_TOO_LARGE` | 上传图片超过 10 MiB |
| 413 | `REQUEST_TOO_LARGE` | HTTP 请求某项超过限制 |
| 500 | `IMAGE_STORAGE_ERROR` | 图片保存失败 |
| 500 | `INTERNAL_ERROR` | 未分类内部错误 |
| 503 | `RECOGNITION_QUEUE_FULL` | 队列无槽位 |
| 503 | `DATABASE_UNAVAILABLE` | MySQL 不可用 |
| 503 | `SERVICE_UNAVAILABLE` | 健康探测发现必需依赖不可用 |

### 8.2 识别业务失败码

这些码出现在最终 `FAILED` 快照的 `errorCode`，不是 HTTP 信封错误：

| `errorCode` | 含义 |
|---|---|
| `PLATE_NOT_FOUND` | YOLOv8 未检测到有效车牌 |
| `PLATE_RECOGNITION_FAILED` | LPRNet 输出为空、非法或不合规 |
| `MODEL_INFERENCE_ERROR` | 图片读取校验、解码或模型推理失败 |
| `SERVER_RESTARTED` | 服务启动恢复了遗留处理中记录 |

## 9. 安全边界与已接受限制

- 当前 HTTP 和 MQTT 都是明文，只能部署在隔离教学网络；不能描述为生产安全部署。
- 客户端和设备不直连 MySQL，数据库端口不应向学生客户端网络开放。
- 密码、HTTP Token、MQTT 密码、完整请求体、完整 MQTT Payload 和图片内容不得进入日志。
- 固定教学凭据会随交付物公开，不能用于互联网或生产环境。
- 服务端模型和应用镜像只支持 linux/amd64 CPU，不支持 GPU、CUDA 或 ARM64。
- 一张图片只输出一个最高置信度车牌。
- 图片和识别历史首版不自动删除，需要受控运维管理容量。
- MQTT 没有 Outbox，Broker 未接受的消息不补发；设备未收到最终结果时必须关闭闸杆。
- Qt 实时列表只代表本次在线会话，MySQL 历史接口才是事实查询入口。
- 黑白名单不参与首版放行决策，也不通过 MQTT 广播变更。

## 10. 后续学习与代码阅读顺序

建议学生按以下顺序阅读：

1. `README.md`：当前系统边界、已验证运行方式和发布状态。
2. `docs/REQ-001.md`：所有外部协议、数据结构和验收标准的唯一事实来源。
3. 本文第 1、4、5、6 章：先理解端到端业务和通信契约。
4. 服务端 `src/domain` 和 Qt `src/domain`：理解两端共享概念和严格 DTO。
5. 服务端 Controller、Service、Repository：观察协议、业务和数据访问如何分层。
6. Qt API Client、Service、Page：观察异步传输、状态协调和 UI 如何分层。
7. `tests/`：从严格字段、错误分支和三端系统测试反向理解设计不变量。
8. 嵌入式端开发时，逐条使用本文第 7.10 节和 `docs/REQ-001.md` 验收自己的实现。

部署、运行、MySQL 查看、Docker 镜像分发、服务端重建和 Qt 打包方法见同目录的 `TEACHING-DEPLOYMENT-GUIDE.md`。
