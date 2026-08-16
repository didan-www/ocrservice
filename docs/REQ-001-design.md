# REQ-001 Linux 车牌识别服务端软件架构设计

| 属性 | 内容 |
|---|---|
| 文档编号 | REQ-001-DESIGN |
| 关联需求 | `docs/REQ-001.md` v1.0 |
| 文档版本 | 1.1 |
| 状态 | 已确认架构基线，可用于任务拆分和实现 |
| 日期 | 2026-08-15 |
| 目标平台 | Linux amd64、Docker、CPU |
| 架构形式 | C++17 单进程模块化单体 |

## 1. 文档目的与约束来源

本文档把 `REQ-001` 的业务需求细化为可实现的软件边界、依赖方向、运行模型、事务顺序和测试结构。它不新增 Qt 或嵌入式公开接口；字段、可空性、错误码、时间格式和 HTTP 状态仍以 `REQ-001` 为唯一业务契约。

设计依据按以下优先级解释：

1. 用户最新确认的需求和技术选择。
2. `docs/REQ-001.md`。
3. `README.md` 与 `AGENTS.md`。

本次用户进一步确认：

- 使用 MySQL Connector/C++ 8、Eclipse Paho MQTT C++、nlohmann/json、spdlog、libxcrypt、OpenSSL、GoogleTest 和 CTest。
- 系统库通过 apt 安装；ONNX Runtime 使用固定版本官方 linux/amd64 包；头文件类依赖和测试依赖通过 CMake `FetchContent` 固定版本。
- 配置优先级为内置教学默认值、`server.json`、环境变量；敏感值只从环境变量读取。
- 应用启动时执行版本化 SQL migration，允许增加非业务表 `schema_migrations`；业务表仍只有四张。
- Qt MQTT 到期时间是客户端逻辑有效期，Mosquitto 不负责到期断开；Qt 到期后主动断开。
- LPRNet 使用 OpenCV BGR、缩放至 `94x24`、`(pixel - 127.5) / 128`，把实际输出 `[1,68,T]` 转置为 `[T,68]` 后执行 CTC。
- 兼容性复审后补充 Mosquitto 同源凭据初始化、Qt 超时预算、严格 query 解码、空 body 注销、CSV 中途失败和 Unicode 实现边界；不改变公开接口字段。

## 2. 设计目标与非目标

### 2.1 设计目标

- 真实 Qt 客户端无需兼容代码即可完成登录、心跳、实时事件、历史、图片、CSV 和名单闭环。
- 嵌入式设备按已定义的 HTTP/MQTT 契约即可上传图片并接收最终闸杆动作。
- HTTP 线程不执行模型推理；过载通过有界队列明确返回 503。
- 图片、MySQL 记录和队列受理之间具有可解释的补偿行为。
- 所有公开 JSON 由唯一严格序列化层生成，防止字段漂移。
- 镜像包含模型及运行库，可在通用 linux/amd64 CPU 上随 Compose 启动。
- 实现保持适合教学阅读和调试，不引入微服务、分布式事务或生产级基础设施。

### 2.2 非目标

- 不设计 MQTT Outbox、业务补发、设备动作回执或可靠必达。
- 不设计 TLS、生产密钥系统、RBAC、刷新令牌、集群和高可用。
- 不让黑白名单参与首版放行决策。
- 不支持多车牌、视频流、GPU、ARM64 或自动清理图片。
- 不把 MySQL 或 Mosquitto 嵌入应用容器。

## 3. 系统上下文与部署边界

```text
Embedded Device
  | POST multipart + per-device Bearer Token
  | subscribe plate/devices/{deviceId}/recognition-results
  v
+--------------------- Docker Compose ----------------------+
|                                                            |
|  +----------------+    SQL     +----------------------+     |
|  | ocrservice-app |----------->| ocrservice-mysql     |     |
|  | C++ service    |            | MySQL 8              |     |
|  +-------+--------+            +----------------------+     |
|          | MQTT publish                                      |
|          v                                                   |
|  +----------------+                                         |
|  | ocrservice-mqtt|                                         |
|  | Mosquitto      |                                         |
|  +-------+--------+                                         |
+----------|-------------------------------------------------+
           | subscribe plate/management/recognition-events
           v
      Qt Management Client
      HTTP login/history/image/CSV/access-list
```

应用是唯一访问 MySQL 的业务进程。Qt 和嵌入式端只访问 HTTP/MQTT。应用容器持久化图片与技术日志；MySQL 和 Mosquitto 使用各自数据卷。

## 4. 技术栈与依赖固定

| 能力 | 选择 | 用途 |
|---|---|---|
| 编译 | GCC、C++17、CMake、CTest | 构建、测试、安装 |
| HTTP | Crow | 路由、HTTP 服务、multipart 接入 |
| JSON | nlohmann/json | 严格请求解析与响应序列化 |
| Unicode | utf8proc | UTF-8 合法性、Unicode code point 遍历、空白分类和字符计数 |
| 图像 | OpenCV 4 | 解码、尺寸校验、预处理、裁剪 |
| 推理 | ONNX Runtime CPU | YOLOv8、LPRNet Session |
| MySQL | MySQL Connector/C++ 8 Classic API | 参数化 SQL、事务、结果映射 |
| MQTT | Eclipse Paho MQTT C++ | MQTT 3.1.1、QoS 1、异步连接与发布 |
| 日志 | spdlog | UTF-8 JSON Lines 日志与滚动文件 |
| 密码 | libxcrypt `crypt_r` | bcrypt 摘要验证 |
| 摘要/随机 | OpenSSL | SHA-256、`RAND_bytes`、常量时间比较 |
| 测试 | GoogleTest + CTest | 单元、组件、契约测试 |

所有版本在根 `CMakeLists.txt` 或 `cmake/Dependencies.cmake` 中固定。禁止依赖浮动分支。Docker 构建阶段通过 apt 安装 GCC、OpenCV、MySQL 客户端开发包、OpenSSL 和 libxcrypt；ONNX Runtime 使用校验 SHA-256 的固定官方 CPU 包；Crow、nlohmann/json、utf8proc、spdlog、Paho C++ 和 GoogleTest 使用固定 tag/commit。

Paho C++ 的 Paho C 依赖作为明确的 CMake 依赖构建或安装，不依赖宿主机偶然存在的库。

## 5. 代码组织与依赖方向

```text
ocrservice/
├── AGENTS.md
├── README.md
├── CMakeLists.txt
├── Dockerfile
├── docker-compose.yml
├── cmake/
│   └── Dependencies.cmake
├── config/
│   └── server.json.example
├── docs/
│   ├── REQ-001.md
│   └── REQ-001-design.md
├── migrations/
│   └── 001_initial.sql
├── models/
│   ├── yolov8_plate.onnx
│   └── lprnet.onnx
├── scripts/
│   ├── init-mosquitto.sh
│   └── provision-device.sh
├── src/
│   ├── main.cpp
│   ├── app/                 # 组合根、生命周期、配置
│   ├── domain/              # 领域类型、状态约束、纯函数
│   ├── http/                # 路由、中间件、严格 DTO/信封
│   ├── services/            # 用例和事务编排
│   ├── repositories/        # Repository 接口和 MySQL 实现
│   ├── queue/               # 有界队列、槽位预留、工作线程
│   ├── model/               # 模型接口、YOLO/LPRNet 适配
│   ├── mqtt/                # 连接、序列化、发布
│   ├── storage/             # 图片原子保存和读取
│   ├── security/            # Token、bcrypt、摘要
│   ├── text/                # UTF-8 校验、Unicode 遍历和规范化辅助
│   ├── serialization/       # 时间、JSON、CSV
│   └── logging/             # 结构化日志和脱敏
└── tests/
    ├── unit/
    ├── component/
    ├── contract/
    ├── integration/
    ├── fixtures/
    └── simulators/
```

依赖规则：

```text
http -> services -> domain
services -> repository interfaces / queue / storage / mqtt interfaces / model interface
repositories(mysql), mqtt(paho), model(onnx), storage(posix) -> domain or service ports
domain -> C++ standard library + text(utf8proc) only
```

控制器不得包含 SQL、文件路径拼装或 ONNX 张量逻辑。Repository 不发布 MQTT。模型适配器不认识 HTTP DTO。MQTT 发布失败不得修改数据库状态。

## 6. 进程、线程与所有权模型

应用为单进程，组合根 `Application` 按所有权顺序持有长期对象：

```text
Application
├── Config (immutable)
├── Logger
├── ModelRuntime (YOLO Session + LPRNet Session)
├── MySqlConnectionPool
├── Repositories
├── ImageStorage
├── SessionStore
├── MqttPublisher
├── RecognitionTaskQueue
├── ApplicationServices
└── HttpServer
```

线程分工：

| 线程 | 默认数量 | 职责 |
|---|---:|---|
| 主线程 | 1 | 启动、信号处理、停止编排 |
| Crow HTTP worker | 4 | 协议解析、鉴权、轻量业务和 SQL；不推理 |
| 识别 worker | 1 | 从有界队列取任务并顺序推理 |
| Paho 内部线程 | 由库管理 | MQTT 网络回调 |
| MQTT 重连控制 | 1 | 退避连接和首次恢复发布 |

Crow 并发数保持小而固定以适配教学负载。MySQL 使用有界连接池，默认 6 个连接；每次 Repository 调用通过 RAII 租约独占一个连接，事务结束归还。连接不可跨线程共享。

`ModelRuntime` 在启动后只读。默认只有一个识别 worker，因此两个 ONNX Session 不需要外部并发锁；若把 `RECOGNITION_WORKERS` 调大，每个 worker 独立持有两个 Session，避免共享可变输入缓冲区。

## 7. 核心领域类型与端口

### 7.1 领域类型

- `RecognitionId`、`CaptureId`：经严格 UUID 解析的值对象。
- `DeviceId`：1 至 64 字符，不能作为文件路径。
- `RecognitionStatus`：`PROCESSING/SUCCEEDED/FAILED`。
- `GateAction`：`OPEN/KEEP_CLOSED`，仅设备消息使用。
- `RecognitionSnapshot`：与 Qt 精确 DTO 一一对应。
- `RecognitionTask`：只含 `recognitionId` 和内部必要定位信息，不保存完整上传请求。
- `RecognitionOutcome`：成功车牌或稳定失败码；不包含 HTTP 信息。
- `HistoryFilter`、`AccessListFilter`：已校验并转换为 UTC 的查询条件。
- `PlateNumber`：通过 `text` 模块严格解码 UTF-8，按 Unicode code point 执行首尾空白去除、ASCII 大写、内部空白拒绝和 1 至 16 字符校验；长度不能按 UTF-8 字节数计算。Unicode 空白判定固定使用 utf8proc 对应版本，并用 Qt 合法/非法 fixture 做跨语言回归，禁止各控制器自行调用 ASCII `isspace`。

### 7.2 关键接口

```cpp
class IRecognitionRepository {
public:
    virtual std::optional<RecognitionRecord> findByCapture(
        const DeviceId&, const CaptureId&) = 0;
    virtual void insertProcessing(Transaction&, const NewRecognition&) = 0;
    virtual RecognitionRecord finalize(
        Transaction&, const RecognitionId&, const RecognitionOutcome&) = 0;
    virtual std::vector<RecognitionRecord> failInterruptedOnStartup(
        Transaction&, TimePoint completedAt) = 0;
};

class IPlateRecognizer {
public:
    virtual RecognitionOutcome recognize(const cv::Mat& bgrImage) = 0;
};

class IImageStorage {
public:
    virtual StoredImage saveAtomically(const SaveImageCommand&) = 0;
    virtual ImageFile openForRead(const RelativeImagePath&) = 0;
    virtual void removeBestEffort(const RelativeImagePath&) noexcept = 0;
};

class IMqttPublisher {
public:
    virtual PublishAttempt publishManagement(const RecognitionSnapshot&) = 0;
    virtual PublishAttempt publishDeviceFinal(
        const RecognitionSnapshot&, GateAction) = 0;
};
```

接口返回领域结果或显式错误，不抛出带 SQL、密码、绝对路径的异常到控制器。基础设施异常在 service 边界映射为稳定业务错误和脱敏技术日志。

## 8. HTTP 架构

### 8.1 请求管线

```text
Crow route
 -> RequestId middleware
 -> body/header/URL size guard
 -> content-type guard
 -> Qt or device authentication
 -> strict query/JSON/multipart decoder
 -> application service
 -> exact DTO serializer
 -> response size/content-type guard
 -> access log
```

每个请求进入时生成 UUID `requestId`。服务调用和日志上下文携带该 ID；客户端传入的同名头不覆盖服务端 ID。

顶层异常边界只返回五字段失败信封。未分类异常为 HTTP 500 `INTERNAL_ERROR`，不得输出堆栈、SQL、Token 或路径。

### 8.2 严格 JSON

nlohmann/json 只作为语法树和序列化工具，不直接对外暴露。每个请求 DTO 定义唯一的允许字段集合：

1. 根值必须是对象。
2. 实际 key 集合必须与规定集合完全相等。
3. 再逐字段验证 JSON 类型、长度、枚举、范围和组合状态。
4. 禁止隐式字符串转数字、数字转字符串或缺字段默认值。

响应不使用反射式“序列化所有成员”。每个 DTO 有显式 `toJsonExact()`，按 `REQ-001` 构造固定字段；可空字段始终写 `null`。统一 `EnvelopeWriter` 是五字段信封的唯一生成入口。

所有 JSON 整数在序列化前执行 safe integer 检查。时间模块只接受毫秒精度、偏移严格为 `+08:00` 的 ISO 8601，转换为 UTC 后进入 Repository；输出统一生成 `.SSS+08:00`，不依赖宿主机时区。

查询参数由统一 `StrictQueryDecoder` 从原始 query string 解析。Qt 端按 UTF-8 生成百分号编码；服务端只把 `%HH` 解码一次，再执行严格 UTF-8 校验，禁止二次解码。`+` 始终是字面加号，不能按 `application/x-www-form-urlencoded` 规则转换为空格，因此 `+08:00` 时间既可原样传输，也可编码为 `%2B08%3A00`。非法百分号、非法 UTF-8、重复参数、未知参数或缺少必填参数统一返回 `INVALID_REQUEST`。

时间、设备号、车牌号和 keyword 的业务校验必须发生在 URL 解码之后。名单 keyword 中 `%`、`_` 和 `\` 的 SQL LIKE 字面量转义也必须发生在 URL 解码之后；例如 `%25` 只还原为一个 `%`，随后再由 Repository mapper 转义，不能成为通配符。

### 8.3 路由与控制器

| 控制器 | 路由 | Service |
|---|---|---|
| HealthController | `GET /health` | HealthService |
| AuthController | `POST /api/v1/auth/login`、`POST /api/v1/auth/logout` | AuthService |
| ClientController | `POST /api/v1/clients/heartbeat` | SessionService |
| RecognitionController | 详情、历史、图片、CSV | HistoryService/CsvService |
| AccessListController | 查询、lookup、新增、删除 | AccessListService |
| DeviceRecognitionController | 设备 multipart 上传 | RecognitionService |

路由只注册 `REQ-001` 中的接口；不提供旧 `/plate/upload`、设备管理 API 或调试 API。不存在路由、方法错误和 Crow 解析错误也必须转成统一 JSON 失败信封，不返回 HTML。

图片成功响应直接流式发送已打开文件描述符，Content-Type 来自数据库中已验证的 `image_mime`。CSV 使用分页游标按固定排序分批读取并流式转义输出，第一块先写 UTF-8 BOM 和固定十列表头，不在内存构建完整 CSV。

Qt 注销请求的 body 长度固定允许为 0；即使请求携带 `Content-Type: application/json`，控制器也不得强制把空 body 解析为 `{}`。非空 body、`Content-Length` 与实际 body 不一致或 chunked body 含任何字节均按 `INVALID_REQUEST` 拒绝。

### 8.4 Qt 超时兼容预算

Qt 的总时限从发出请求持续到完整响应体接收完成，服务端实现和联调测试必须遵守以下硬边界：

| 接口类别 | Qt 总时限 | 服务端实现约束 |
|---|---:|---|
| 登录 | 10 秒 | 只执行用户查询和 bcrypt 验证，不等待 MQTT 连接 |
| 注销、心跳 | 5 秒 | 只操作内存会话；心跳不探测 MySQL、模型或 MQTT |
| 详情、历史、名单查询 | 10 秒 | 使用已定义索引和有界分页，不执行模型推理 |
| 名单新增、删除 | 10 秒 | 单次事务且不在 HTTP 层自动重放写操作 |
| 图片 | 30 秒 | 校验后直接流式读取文件，不重新解码或识别 |
| CSV | 120 秒 | 使用同一筛选/排序游标持续分批输出，不在内存聚合完整结果 |

服务端不得通过 3xx 延长流程。图片和 CSV 在发送成功响应头之前失败时返回严格 JSON 失败信封；发送成功响应头之后发生文件、数据库或写 socket 错误时，必须异常终止当前 HTTP 连接，使 Qt 得到传输失败并放弃 `QSaveFile`，不得正常结束响应或补写伪造 CSV 行。

## 9. Qt 鉴权与会话

### 9.1 密码验证

`AuthService` 通过 `AdminUserRepository` 按用户名查询用户。bcrypt 使用 libxcrypt `crypt_r` 验证；日志中只记录结果码和用户 ID，不记录用户名与密码组合。用户名不存在时执行一次固定 bcrypt 摘要验证，降低明显时序差异。

### 9.2 Token 生成与存储

登录成功后 OpenSSL `RAND_bytes` 生成 32 字节随机数并编码为不透明 Token。`SessionStore` 只驻留内存，内部维护：

- `token -> Session`。
- `clientId -> token`。

两个索引由同一互斥量保护。同一 `clientId` 新登录时，在同一临界区删除旧 Token 并插入新 Token。Session 保存 user ID、显示名快照、clientId、签发和到期时间。注销立即删除两个索引。鉴权时区分无效与已过期 Token，并惰性清理过期条目。

应用重启后所有 Token 自然失效，符合内存会话约束。心跳只验证 Token、clientId 和请求结构，不刷新到期时间，也不探测依赖。

### 9.3 Qt MQTT 逻辑到期

登录响应中的 HTTP 与 MQTT `expiresAt` 使用同一时间对象序列化，保证字节内容一致。管理 MQTT 账号是固定 Broker 凭据；Mosquitto 不按该时间撤销连接。Qt 必须在 `expiresAt` 到期或 HTTP 会话失效时主动断开 MQTT。此行为是已接受的教学版限制，不描述为 Broker 强制过期。

## 10. 设备鉴权与上传受理

### 10.1 设备鉴权

设备 Token 先做 SHA-256，再按摘要查询 `devices`。Repository 只返回 Token 所属设备和 enabled 状态。路径 `deviceId` 与 Token 归属不一致返回 `DEVICE_FORBIDDEN`；禁用返回 `DEVICE_DISABLED`；Token 无效返回 `DEVICE_UNAUTHORIZED`。摘要比较使用 OpenSSL 常量时间函数。

### 10.2 multipart 和图片校验

上传控制器先限制整个请求，再解析精确三个且不重复的 part。图片最大 10 MiB。服务端不相信文件名、扩展名或 multipart MIME：

1. 检查 JPEG/PNG 魔数。
2. 使用 OpenCV `imdecode` 实际解码。
3. 验证宽、高和总像素。
4. 以解码确认的格式决定 `.jpg/.png` 和 MIME。
5. 对原始压缩字节计算 SHA-256。

未知、重复、缺失 part 和空图片均在进入 service 前拒绝。

### 10.3 并发幂等与队列预留

`RecognitionService::acceptUpload` 使用以下顺序：

```text
authenticate and validate request
 -> query (deviceId, captureId)
 -> existing: compare imageSha256 + capturedAt
      -> same: return original response
      -> different: 409 CAPTURE_ID_CONFLICT
 -> reserve one queue slot atomically
      -> unavailable: 503, no file, no row
 -> generate recognitionId and server relative path
 -> atomically save original compressed bytes
 -> begin MySQL transaction
 -> insert PROCESSING revision=1
 -> commit transaction
 -> arm queue task behind a per-task start gate
 -> attempt PROCESSING management publish
 -> open start gate so worker may infer
 -> return HTTP 202
```

start gate 防止极快推理在线程竞争下先发布 final revision 2、随后才发布 PROCESSING revision 1。无论 PROCESSING 发布成功或失败，发布尝试返回后都必须打开 gate；MQTT 失败不能阻塞识别。

数据库唯一键是并发幂等的最终裁决。若两个首次请求同时查询为空：

1. 两者可分别预留槽位并写入不同 `recognitionId` 文件。
2. 一个插入成功；另一个收到唯一键冲突。
3. 失败方回滚、删除自己文件、释放自己槽位，再查询胜出记录。
4. 摘要和拍摄时间相同则返回胜出记录；否则返回 409。

这样不会把并发唯一键冲突误报为 500，也不会残留第二个任务。

`QueueReservation` 使用 RAII：未显式提交时析构自动归还槽位。容量统计包含已预留但尚未可执行的任务，确保已受理任务一定能入队。

## 11. 图片存储与补偿

`PosixImageStorage` 只接受服务端生成的 `recognitionId`、UTC 日期和已验证格式，不接受用户路径。相对路径固定为 `YYYY/MM/DD/{recognitionId}.jpg|png`。

原子保存步骤：

1. 在目标目录创建不可预测的临时文件，使用 `O_CREAT|O_EXCL`。
2. 循环写入全部原始压缩字节。
3. `fsync` 临时文件并关闭。
4. 原子 `rename` 到最终文件名。
5. `fsync` 父目录。

失败时删除临时文件。最终文件成功而数据库插入失败时执行 best-effort 删除；删除失败记录脱敏日志，但返回数据库/存储对应错误，不伪造受理成功。

图片下载先从 MySQL 获取相对路径和 MIME，再以 `IMAGE_ROOT` 目录文件描述符为根安全打开；规范化结果必须仍位于根目录内。记录或文件缺失统一返回 404 `IMAGE_NOT_FOUND`。首版不自动删除图片。

## 12. 识别队列与工作线程

有界队列内部由 mutex、condition variable、deque、reserved count 和 accepting 标志组成。公开操作只有：

- `tryReserve()`：非阻塞获取 RAII 槽位。
- `commit(task)`：把预留转换为可见任务。
- `take(stopToken)`：worker 等待任务或停止。
- `stopAccepting()`：拒绝新预留。
- `requestStop()`：唤醒等待线程。

`queueDepth` 包含等待任务，不包含正在推理的任务；`queueCapacity` 是配置容量。已预留槽位单独计数，但判满使用 `queued + reserved`。

worker 对每个任务：

1. 等待 start gate。
2. 读取数据库中的 PROCESSING 记录和图片路径。
3. 从磁盘解码原图。
4. 调用 `IPlateRecognizer`。
5. 使用单调时钟计算 `durationMs`，墙上时钟生成 `completedAt`。
6. 事务执行条件更新：只允许当前 `PROCESSING` 变成最终状态，revision 加一。
7. 提交后构造完整最终快照。
8. 依次尽力发布管理最终事件和本设备最终结果。

单任务异常被捕获并转换为 `FAILED/MODEL_INFERENCE_ERROR` 或明确的内部失败映射，不能终止 worker。最终数据库更新失败时记录错误，不发布与数据库不一致的最终消息；该记录留为 PROCESSING，由重启恢复机制处理。

## 13. 模型适配设计

### 13.1 模型文件基线

| 模型 | 文件 | SHA-256 | 节点与形状 |
|---|---|---|---|
| 车牌检测 | `yolov8_plate.onnx` | `bfa426b74d4b619207cca55b296ce3603fb784755a205791af0d0dab4fcf6c04` | `images [1,3,640,640] -> output0 [1,5,8400]` |
| 字符识别 | `lprnet.onnx` | `c78e54070d0e2b8a6f8d548feb52b75321d5f464bcaadb679ec7ee31433cffa4` | `input [1,3,24,94] -> output [1,68,18]` |

车牌检测模型来源为 GitHub 仓库 `Semihocakli/turkish-plate-recognition-w-yolov8-onnx-to-engine-cpp` 的 Git blob `65f6af7f12f711e27fe7d2e0636fa9f8ec20624d`。模型元数据声明 Ultralytics AGPL-3.0；教学交付必须保留许可证和来源说明。

原文件 `yolov8n.onnx` 的 SHA-256 为 `b6afb3d5a109f860f68f2d10e1d8f23565197d72779e46b2ce3f9f28f9e5fcdc`，它是 COCO 80 类模型，输出 `[batch,84,anchors]`，不得复制为 `models/yolov8_plate.onnx` 或用于本服务。

下载的检测模型主要面向土耳其车牌。它满足技术张量契约，但不宣称中国车牌生产精度；教学验收必须使用实际演示图片做固定夹具测试。

### 13.2 启动校验

启动创建两个 Session，并严格校验：

- 文件可读且 SHA-256 与构建清单一致。
- 输入输出数量各为 1。
- 节点名、float32 类型和所有静态维度完全一致。
- YOLO 元数据类别数为 1；输出最后两维为 `5x8400`。
- LPRNet 类别维为 68、时间步为 18。

任一不匹配均启动失败。不能通过自动猜测轴、截断类别或接受动态 shape 来继续运行。

### 13.3 YOLOv8 处理

1. BGR 转 RGB。
2. letterbox 到 `640x640`，保持比例，记录 scale 和两边 padding。
3. 转 float32，除以 255，HWC 转 NCHW，构造 `[1,3,640,640]`。
4. 运行 `images -> output0`。
5. 按 `[x,y,w,h,score] x 8400` 读取；输出已经含 score，不再 sigmoid。
6. 过滤 score `< YOLO_CONFIDENCE`。
7. 转换为角点坐标，执行 NMS，IoU 阈值 `YOLO_NMS_IOU`。
8. 用 letterbox 参数还原原图坐标并裁剪到边界。
9. 丢弃空框，选择 NMS 后置信度最高的一框。

无框返回 `PLATE_NOT_FOUND`。

### 13.4 LPRNet 处理

1. 从原始 BGR 图按检测框裁剪。
2. OpenCV 直接把 BGR crop 缩放为 `94x24`，不转换 RGB。
3. 转 float32，逐通道执行 `(pixel - 127.5) / 128.0`。
4. HWC 转 NCHW，构造 `[1,3,24,94]`。
5. 运行 `input -> output`，得到 `[1,68,18]`。
6. 去掉 batch 维并转置为 `[18,68]`。
7. 每个时间步 argmax；合并连续相同索引，再删除 blank 0。
8. 按 `REQ-001` 固定字符表映射并执行车牌规范化校验。

空结果、越界索引或不合规结果返回 `PLATE_RECOGNITION_FAILED`；ONNX Runtime 异常返回 `MODEL_INFERENCE_ERROR`。

## 14. MySQL 与 Repository

### 14.1 migration

应用在 HTTP 启动前执行 `migrations/NNN_name.sql`。增加技术表：

```sql
CREATE TABLE schema_migrations (
  version BIGINT UNSIGNED NOT NULL,
  name VARCHAR(128) NOT NULL,
  checksum CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  applied_at DATETIME(3) NOT NULL,
  PRIMARY KEY (version)
) ENGINE=InnoDB;
```

它不属于业务表。迁移器按文件名前缀排序，使用 MySQL advisory lock 防止并发执行；已执行版本必须匹配文件名和 SHA-256，checksum 不同则启动失败。每个迁移尽可能使用事务，但考虑 MySQL DDL 隐式提交，迁移脚本必须幂等且一次只推进一个版本。

`001_initial.sql` 创建 `REQ-001` 的四张业务表和固定演示数据。重复启动不得重置密码、修改已有凭据或清空业务记录。

### 14.2 Repository 规则

- 所有 SQL 使用 prepared statement 参数绑定。
- 数据库连接建立后执行 `SET time_zone = '+00:00'`。
- `DATETIME(3)` 只映射 UTC time point。
- 行到领域对象的映射集中在 Repository mapper，并复验状态组合。
- 识别分页固定 `captured_at DESC, recognition_id DESC`。
- 名单分页固定 `created_at DESC, id DESC`。
- `%`、`_`、`\` 在名单 keyword LIKE 查询前显式转义，并指定 `ESCAPE`。
- total 和所有对外 ID 在出库后检查 JSON safe integer。

`finalize` 使用条件更新保证状态机：

```sql
UPDATE recognition_logs
SET revision = revision + 1, status = ?, ...
WHERE recognition_id = ? AND status = 'PROCESSING';
```

受影响行数必须为 1，然后在同一事务读取最终记录。名单新增依赖全局唯一键处理并再次查询冲突记录的 `list_type`，映射为 WHITE 或 BLACK 稳定错误码。

## 15. MQTT 架构

### 15.1 连接与线程安全

`PahoMqttPublisher` 使用一个 `async_client`，Client ID 固定 `plate-server`、MQTT 3.1.1、clean session true。连接状态由原子值维护，回调只更新状态和唤醒重连控制，不访问 Repository。

重连采用有上限的指数退避并加入小幅抖动；应用启动时 Broker 不可用不阻止 HTTP。发布入口用互斥量串行化 Payload 构造和 Paho publish 调用，保证单进程调用顺序。QoS 1、retain false 固定，调用方不能覆盖。

### 15.2 Payload

管理消息直接使用与 HTTP 相同的 `RecognitionSnapshotSerializer`，根对象无信封，字段集合完全相等，绝不包含 `gateAction`。

设备消息复用快照字段并由专用 serializer 增加唯一字段 `gateAction`。只有最终状态可序列化；`SUCCEEDED -> OPEN`、`FAILED -> KEEP_CLOSED` 在领域纯函数中定义。

主题由 `TopicBuilder` 根据经过校验的 `deviceId` 生成，禁止从上传请求直接传入主题字符串。

### 15.3 已接受的丢失语义

发布是提交数据库后的 best effort 副作用。若 Paho/Broker 未接受：

- 返回失败结果并记录脱敏日志。
- 不回滚数据库。
- 不写 Outbox、不创建补发任务、不改变 revision。
- Qt 或设备可能永久收不到该事件。
- 设备未收到最终结果时保持闸杆关闭。

Paho 自身为当前连接中已接收的 QoS 1 发布执行协议重传，不把它扩展为跨应用重启的业务重发。

启动恢复是唯一特例：本次启动从 PROCESSING 改成 `FAILED/SERVER_RESTARTED` 的记录保存在内存列表；MQTT 首次连通时每条尽力发布一次，调用返回后从列表删除，不论成功失败都不再重试。

## 16. 配置设计

配置合并顺序固定：

```text
compiled teaching defaults
  < /app/config/server.json (存在时)
  < environment variables
```

开发环境可把同结构文件挂载到 `/app/config/server.json`。未知 JSON 配置键、错误类型、非法范围和必需敏感变量缺失均启动失败。日志只输出非敏感的最终配置摘要。

除 `REQ-001` 已列项外，连接所必需的配置为：

| 环境变量 | 敏感 | 用途 |
|---|---|---|
| `MYSQL_USER` | 否 | 应用数据库账号 |
| `MYSQL_PASSWORD` | 是 | 应用数据库密码 |
| `MQTT_SERVER_USERNAME` | 否 | 服务端发布账号，默认 `plate-server` |
| `MQTT_SERVER_PASSWORD` | 是 | 服务端发布密码 |
| `MQTT_MANAGEMENT_USERNAME` | 否 | 登录响应中的 Qt MQTT 账号 |
| `MQTT_MANAGEMENT_PASSWORD` | 是 | 登录响应中的 Qt MQTT 密码 |

敏感项不允许写入 `server.json`，也不设编译默认值。固定演示凭据只能由教学 Compose 的环境文件/初始化流程注入，并附隔离网络警告。

四个 MQTT 环境变量是应用与 Broker 静态账号的唯一事实来源。教学 Compose 的 `app` 与 `mqtt` 服务必须读取同一份 `.env`/secret；禁止再维护另一份手工密码副本。`mqtt` 容器入口在启动 Broker 前执行 `init-mosquitto.sh`，使用 `mosquitto_passwd` 更新 password 文件并原子生成基础 ACL：服务端发布账号只允许写管理主题和设备结果主题，管理端账号只允许读管理主题。设备账号和设备 ACL 由 `provision-device.sh` 维护在独立片段中，再与基础 ACL 原子合并。

初始化脚本必须校验四项非空、用户名不重复、目标文件权限和 `mosquitto_passwd` 返回值；失败则 Broker 不启动。脚本和 Compose 日志不得输出密码。应用只读取同源变量用于 Paho 连接和登录响应，不负责创建 Broker 用户。端到端验收必须使用一次真实登录返回的 MQTT 凭据完成 CONNECT 和管理主题 SUBACK，以发现 password/ACL 漂移。

`MQTT_PUBLIC_HOST` 是 Qt 可访问地址，不能自动使用 Compose 服务名 `mqtt`。启动只校验非空，部署验收通过真实 Qt 连接验证可达性。

## 17. 启动、健康与停止

### 17.1 启动

```text
load and validate config
 -> initialize logger
 -> validate/create image and log roots
 -> hash, load and validate both ONNX models
 -> connect MySQL with retry, maximum 60 seconds
 -> acquire migration lock and migrate/verify
 -> transactionally fail stale PROCESSING rows
 -> create repositories/services/queue
 -> start recognition workers
 -> start HTTP server
 -> start MQTT background connection
```

MySQL 60 秒持续不可用、migration 失败或模型不匹配时进程以非零状态退出。MQTT 不可用时继续启动，`/health` 为 DEGRADED。

HTTP 对外监听前，模型和 MySQL 必须已就绪，避免短暂接受无法处理的请求。

### 17.2 健康检查

`HealthService` 读取：模型启动状态、MySQL 轻量探测、MQTT 原子连接状态、queue depth/capacity。它通过专用精确 DTO 生成 `REQ-001` 规定字段。模型或 MySQL DOWN 返回 503 失败信封；仅 MQTT DOWN 返回 200 的 DEGRADED 数据对象。

Docker healthcheck 调用 `/health`，Compose `depends_on` 只辅助启动顺序，应用仍执行自身重试。

### 17.3 停止

SIGTERM/SIGINT 只在信号安全处理器中设置停止标志并唤醒主线程。主线程依次：

1. 让上传路由返回 503 并停止新队列预留。
2. 停止 HTTP 接受新连接。
3. 请求 worker 在当前阶段结束后停止，并丢弃尚未开始任务的内存执行机会；数据库记录保持 PROCESSING。
4. 断开 MQTT。
5. 关闭连接池并 flush 日志。

ONNX Runtime 单次 `Run` 不假设可中断。Compose 设置有限 `stop_grace_period`；超时后容器可强制结束。下次启动把所有遗留 PROCESSING 标记为 `FAILED/SERVER_RESTARTED`，不重新推理。

## 18. Docker 与交付

Dockerfile 使用多阶段构建：

- builder：安装编译依赖、固定第三方版本并构建 Release 和测试。
- runtime：只包含应用、运行库、migration、模型、许可证和必要 CA 证书。

构建显式指定 `linux/amd64`，不使用 `-march=native`。模型复制到 `/app/models` 并在构建或启动时校验 SHA-256。

Compose 三服务：

| 服务 | 网络暴露 | 卷 |
|---|---|---|
| app | `8080:8080` | images、logs、只读 config |
| mysql | Compose 内网 3306 | mysql-data |
| mqtt | `1883:1883` | mosquitto-data、password、ACL |

Mosquitto 开启 persistence、`allow_anonymous false` 和默认拒绝 ACL。`provision-device.sh` 串行更新 MySQL、密码文件和 ACL，再重载 Broker。跨 MySQL 与文件系统无法原子提交，脚本必须在失败时报告已完成步骤并可安全重跑，不宣称分布式事务。

**教学联调配置矩阵**

| 参与端 | 配置 | 当前虚拟机教学值 | 来源与约束 |
|---|---|---|---|
| Qt | `clientId` | 非零且每个安装唯一的 UUID | Qt `config/client.json`，不得复制同一值到多个客户端 |
| Qt | `loginBaseUrl` | `http://192.168.137.128:8080` | Qt `config/client.json`，无反向代理路径前缀 |
| Qt | `allowInsecureTransport` | `true` | 仅隔离教学网络 |
| Qt MQTT | host/port/tls、账号、主题 | `192.168.137.128` / `1883` / `false` | 只能使用登录响应；主题为 `plate/management/recognition-events` |
| 服务端应用 | MySQL/MQTT 内部地址 | `mysql:3306` / `mqtt:1883` | Compose 服务名，只在容器网络使用 |
| 服务端登录响应 | `MQTT_PUBLIC_HOST` | `192.168.137.128` | 必须是 Qt 所在主机可达地址，不能下发 `mqtt` |
| 嵌入式 HTTP | base URL、Bearer Token | `http://192.168.137.128:8080`、设备开通输出 | 上传接口固定为 `/api/v1/devices/{deviceId}/recognitions` |
| 嵌入式 MQTT | host/port、账号、Client ID、主题 | `192.168.137.128:1883`、设备开通输出 | 唯一 Client ID、`cleanSession=false`，只订阅本设备结果主题 |

上述 IP 是当前虚拟机联调值，不写死进应用镜像；镜像交给其他环境使用时，只替换外部可达地址和每端凭据，不改变 HTTP 路由、DTO 或 MQTT 主题格式。

## 19. 日志、错误和安全

spdlog 输出一行一个 UTF-8 JSON 对象，最少包含：

```text
timestamp, level, module, event, code, requestId,
recognitionId?, deviceId?, durationMs?
```

日志由服务端字段构造，不直接序列化请求或异常对象。禁止记录密码、Token、Authorization、完整请求体、完整 MQTT Payload、图片字节和绝对路径。自由文本中的换行和控制字符转义，防止日志注入。

稳定错误映射集中在 `ErrorMapper`。控制器只能返回 `REQ-001` 已定义 code；内部 Connector、Paho、OpenCV、ONNX、filesystem 错误只进入脱敏日志。错误 `message` 是简体中文展示文本，客户端逻辑不得依赖。

明文 HTTP/MQTT、固定演示凭据和 AGPL 模型只用于隔离教学环境。README、Compose 示例和镜像说明必须保留限制，不使用“安全生产部署”或“消息可靠必达”等表述。

## 20. 测试架构

### 20.1 单元测试

- 精确字段集合、五字段信封、null 组合和 safe integer。
- `+08:00` 时间解析、UTC 转换、毫秒输出和 `[start,end)`。
- UUID、设备 ID、UTF-8/Unicode 车牌规范化、LIKE 转义和 CSV 转义。
- query 单次百分号解码、`+08:00`、中文、`%`、`_`、`\`、重复键和非法 UTF-8。
- 状态转换、revision、GateAction 映射。
- YOLO letterbox/NMS/坐标还原和 LPRNet 转置/CTC/字符表。
- 队列 Reservation RAII、判满、停止和 start gate 顺序。

### 20.2 组件测试

- MySQL 8 容器执行 migration、checksum、四张业务表和技术表。
- Repository 参数绑定、排序、事务、唯一冲突和条件 finalize。
- 图片临时写、fsync/rename、数据库失败清理和文件缺失。
- Paho 与 Mosquitto 测试 QoS、retain、主题和 ACL。
- Compose 从空卷启动后验证同源管理/发布凭据、password 文件和 ACL，并用登录下发凭据完成真实 SUBACK。

### 20.3 契约测试

从 Qt 客户端复用合法/非法 JSON fixture。每个服务端 Qt 响应执行 key 集合完全相等断言，并用 Qt 侧严格解析器做最终验证。MQTT 管理 Payload 同样复用 `RecognitionSnapshot` fixture；设备 Payload 单独断言只多 `gateAction`。

### 20.4 集成与系统测试

- 登录、旧 Token 失效、心跳、注销和到期。
- 空 body 且带 JSON Content-Type 的注销、各类 Qt 总时限和 CSV 中途断流。
- 设备先 SUBACK 后上传、PROCESSING、图片立即下载、最终结果。
- 同 captureId 串行/并发重试、冲突、队列满和数据库失败补偿。
- 历史、详情、CSV、名单全流程。
- Broker 断开时数据库正确且允许消息丢失。
- 容器重启恢复 PROCESSING。
- 从空卷启动 Compose，使用真实 Qt 和嵌入式模拟器联调。

模型测试至少包含：

1. 启动校验两个固定 SHA-256 和张量。
2. YOLO 零张量 smoke test 输出 `[1,5,8400]` 且均为有限值。
3. 中国车牌教学图片固定集的检测框回归测试。
4. LPRNet 固定 crop 的字符结果回归测试。
5. 端到端图片得到预期最终状态；准确率只按教学样例记录，不外推为生产指标。

## 21. 需求追踪

| 需求/验收 | 架构落点 |
|---|---|
| AC-001 至 AC-004 | 第 8、9 节严格 HTTP 与内存会话 |
| AC-005 至 AC-009 | 第 10、11、12 节上传、幂等、队列和存储 |
| AC-010 至 AC-012 | 第 12、13、14 节模型与最终事务 |
| AC-013、AC-014、AC-020 | 第 15 节 MQTT Payload、QoS 和丢失语义 |
| AC-015 至 AC-018 | 第 8、14 节查询、CSV 和名单 Repository |
| AC-019 | 第 17 节启动恢复 |
| AC-021、AC-022 | 第 18 节设备脚本和 Compose |
| AC-023 | 第 6、10、12 节线程和有界队列 |
| AC-024、AC-025 | 第 20 节契约与三端系统测试 |

## 22. 实现阶段不可改变的不变量

1. Qt JSON 字段集合、可空性、时间、分页和信封不得放宽。
2. 管理 MQTT 不得包含 `gateAction`；设备结果只发最终状态。
3. HTTP worker 不得执行模型推理。
4. 队列槽位必须先于图片和数据库记录预留。
5. 数据库提交且图片可下载后才允许发布 PROCESSING。
6. 最终数据库事务提交后才允许发布最终消息。
7. MQTT 失败不得回滚或修改数据库，不增加 Outbox。
8. `yolov8n.onnx` COCO 模型不得冒充车牌模型。
9. LPRNet 必须按 BGR 和确认的归一化/轴转置处理。
10. 四张业务表保持不变；`schema_migrations` 仅用于技术版本管理。
11. 只有实际通过的构建、测试和运行命令才能写入 README。

## 23. 已知限制

- 检测模型符合单类车牌张量契约，但训练数据偏向土耳其车牌；中国车牌效果以教学样例实测为准。
- Broker 未接受的发布可能永久丢失，设备因此可能不抬杆。
- Qt MQTT 到期由客户端主动断开，Broker 不强制到期。
- 停止期间尚未完成的任务由下次启动标记失败，不恢复推理。
- 图片不自动清理，长期运行需要人工管理持久化卷。
- 固定凭据、明文通信、单进程和单 worker 只面向隔离教学演示。

这些限制是有意保留的首版边界，后续实现不得隐藏或描述为生产级能力。
