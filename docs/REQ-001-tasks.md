# REQ-001 Linux 车牌识别服务端开发任务

| 属性 | 内容 |
|---|---|
| 文档编号 | REQ-001-TASKS |
| 关联需求 | `docs/REQ-001.md` v1.0 |
| 关联设计 | `docs/REQ-001-design.md` v1.1 |
| 文档版本 | 1.0 |
| 日期 | 2026-08-15 |
| 适用范围 | Linux amd64、C++17、Docker 教学版服务端 |

## 1. 使用规则

1. 每个任务在其前置依赖完成后，可以独立实现、构建和测试。
2. “可以并行”只表示文件修改可以并行；需要共享 MySQL、Mosquitto 或 Compose 环境的验收命令仍应串行执行。
3. 任务只能修改“预计修改文件”列出的文件。发现必须修改其他任务所有的文件时，应停止并调整任务依赖，不能并行抢改。
4. 根 `CMakeLists.txt`、公共 CMake helper 和根测试入口只归 TASK-001 所有。TASK-001 必须建立自动发现模块/测试子目录的稳定约定，后续任务不得修改这些公共构建文件。
5. 每个模块使用自己的 `CMakeLists.txt` 和测试子目录，允许并行任务在不同目录独立接入构建。
6. 未经需求变更，不得修改 HTTP 路由、DTO、错误码、数据库四张业务表、MQTT Payload、模型张量或已接受的消息丢失语义。
7. 每个任务提交前必须运行本任务测试；TASK-024 才执行完整系统验收和文档收尾。

## 2. 并行与文件所有权

| 文件范围 | 唯一所有任务 |
|---|---|
| 根构建文件、`cmake/`、根测试入口 | TASK-001 |
| `src/app/config/`、`src/logging/`、`config/` | TASK-002 |
| `src/domain/`、`src/text/`、`src/serialization/time/` | TASK-003 |
| `src/http/protocol/`、`src/serialization/json/` | TASK-004 |
| `src/queue/` | TASK-005 |
| `src/storage/` | TASK-006 |
| `src/model/`、`models/` | TASK-007 |
| `migrations/`、`src/repositories/mysql/connection/`、`src/repositories/mysql/migration/` | TASK-008 |
| `src/repositories/mysql/repositories/` | TASK-009；TASK-015 仅获授权扩展 Recognition 历史 pull cursor |
| `src/security/`、`src/services/auth/` | TASK-010 |
| `deploy/mosquitto/`、设备/Broker 初始化脚本 | TASK-011 |
| `src/mqtt/` | TASK-012 |
| `src/http/server/`、`src/http/middleware/` | TASK-013 |
| `src/http/controllers/auth/`、`src/http/controllers/client/` | TASK-014 |
| `src/http/controllers/recognition/`、History/CSV Service、`src/domain/Ports.*` | TASK-015；`Ports.*` 仅限历史 pull cursor Port |
| `src/http/controllers/access_list/` 和 AccessList Service | TASK-016 |
| `src/http/controllers/device_recognition/`、`src/services/recognition/acceptance/`、`tests/integration/device_upload_api/` | TASK-017；另获最小授权修改 `src/domain/Ports.h` 的 `StorageFailure::alreadyExists`、`src/queue/RecognitionTaskQueue.*`、`tests/unit/queue/**`、`src/storage/PosixFileOps.*`、`src/storage/PosixImageStorage.*`、`tests/component/storage/**`，以及为统一 HTTP Token 逗号规则所需的 TASK-011 `scripts/provision-device.sh`、`tests/component/mosquitto_scripts/tst_mosquitto_scripts.sh` |
| `src/services/recognition/worker/` | TASK-018 |
| `src/app/runtime/`、`src/services/health/`、`src/http/controllers/health/`、`src/main.cpp`、`tests/integration/application_lifecycle/` | TASK-019；另获最小授权修改根 `CMakeLists.txt`，`cmake/PatchCrow121.cmake`、`src/http/server/HttpServer.h`、`src/http/server/HttpServer.cpp`、`tests/unit/http_server/**`、`tests/component/http_runtime/**`，以及 `src/repositories/mysql/connection/MySqlConnectionPool.h`、`src/repositories/mysql/connection/MySqlConnectionPool.cpp`、`tests/component/mysql_migration/**`；例外仅用于 production main 接线、HTTP ready-or-exception 和使用一次性 health-only connection、连接/网络读各 1 秒、总计最多 3 秒的真实 health probe，不得改变六条业务连接行为 |
| Dockerfile、Compose、容器入口和 `.env.example` | TASK-020 |
| Qt 契约夹具和契约测试 | TASK-021 |
| 嵌入式模拟器和其系统测试 | TASK-022 |
| 真实 Qt 联调脚本和测试记录 | TASK-023 |
| README、最终验收报告和发布脚本 | TASK-024 |

建议调度顺序：TASK-001；然后按依赖并行推进 TASK-002 至 TASK-013；TASK-014 至 TASK-017 可并行；TASK-018、TASK-019 依次组合；TASK-020 与 TASK-021 可并行；TASK-022 与 TASK-023 可并行实现但串行运行；最后执行 TASK-024。

## 3. 开发任务

### TASK-001 建立工程、依赖和测试基线

- **目标：** 建立可以在 Linux amd64 上重复配置、构建和运行 CTest 的最小工程。
- **前置依赖：** 无。
- **主要工作：**
  - 固定 GCC/C++17、Crow、nlohmann/json、utf8proc、OpenCV、ONNX Runtime、MySQL Connector/C++、Paho C/C++、spdlog、libxcrypt、OpenSSL 和 GoogleTest 的获取方式与版本。
  - 创建模块/测试子目录自动发现 helper，使后续任务只修改自己的模块目录。
  - 建立 `ocrservice_core`、`ocrservice` 和测试目标的编译选项；禁止 `-march=native`。
  - 增加最小 smoke test，验证依赖可链接和 CTest 可执行。
- **预计修改文件：**
  - `CMakeLists.txt`
  - `cmake/Dependencies.cmake`
  - `cmake/AddOcrModule.cmake`
  - `cmake/AddOcrTest.cmake`
  - `tests/CMakeLists.txt`
  - `.gitignore`
  - `tests/smoke/CMakeLists.txt`
  - `tests/smoke/tst_smoke.cpp`
- **测试：** 配置 Debug/Release；构建全部目标；运行 `ctest -R smoke --output-on-failure`。
- **验收标准：** 空构建目录可成功配置和构建；smoke test 通过；依赖版本固定；宿主机缺少 ONNX Runtime 等偶然安装时仍可按文档构建。
- **是否可以并行：** 否。它是所有任务的构建前置。

### TASK-002 实现配置加载和脱敏日志

- **目标：** 实现内置默认值、JSON、环境变量三级配置和 UTF-8 JSON Lines 日志。
- **前置依赖：** TASK-001。
- **主要工作：**
  - 实现配置合并、严格字段/类型/范围校验和敏感变量必填校验。
  - 实现图片、日志、模型、MySQL、MQTT、HTTP、队列等配置对象。
  - 日志只记录允许字段，统一脱敏密码、Token、Authorization、MQTT 密码和控制字符。
  - 配置日志只输出非敏感摘要。
- **预计修改文件：**
  - `src/app/config/CMakeLists.txt`
  - `src/app/config/ServerConfig.h`
  - `src/app/config/ServerConfigLoader.cpp`
  - `src/logging/CMakeLists.txt`
  - `src/logging/LoggerFactory.cpp`
  - `src/logging/LogSanitizer.cpp`
  - `config/server.json.example`
  - `tests/unit/config/**`
  - `tests/unit/logging/**`
- **测试：** 覆盖优先级、未知字段、非法类型/范围、敏感值缺失和日志脱敏。
- **验收标准：** 合法配置产生确定结果；非法配置启动前失败；日志中搜索不到测试密码、Bearer Token、MQTT 密码、完整请求体和绝对图片路径。
- **是否可以并行：** 是，可与 TASK-003 并行。

### TASK-003 实现文本、时间和领域类型

- **目标：** 建立不依赖 HTTP、SQL、OpenCV 或 MQTT 的核心领域模型。
- **前置依赖：** TASK-001。
- **主要工作：**
  - 使用 utf8proc 实现严格 UTF-8 解码、Unicode code point 计数、首尾空白和内部空白判断。
  - 实现 UUID、DeviceId、PlateNumber、状态、GateAction、筛选条件、快照、结果和端口接口。
  - 实现 `+08:00` 毫秒时间解析、UTC 内部表示和固定格式输出。
  - 固化状态/null 组合、revision 和 `SUCCEEDED/FAILED -> gateAction` 纯函数。
- **预计修改文件：**
  - `src/text/**`
  - `src/domain/**`
  - `src/serialization/time/**`
  - `tests/unit/domain/**`
  - `tests/unit/text/**`
  - `tests/unit/time/**`
- **测试：** 覆盖中文车牌、Unicode 空白、非法 UTF-8、字符数边界、UUID、时间偏移、状态组合、safe integer 和 GateAction。
- **验收标准：** 所有领域不变量可由单元测试独立验证；长度按 Unicode code point 而不是字节；内部类型不包含 HTTP、SQL 或 ONNX 类型。
- **是否可以并行：** 是，可与 TASK-002 并行。

### TASK-004 实现严格 HTTP 协议与 JSON 序列化

- **目标：** 建立所有控制器复用的严格请求解析、查询解码、信封和 DTO 序列化能力。
- **前置依赖：** TASK-003。
- **主要工作：**
  - 实现五字段成功/失败信封、精确 DTO serializer 和稳定错误映射。
  - 实现登录、心跳、名单和分页请求的严格 JSON decoder，拒绝未知/缺失字段和隐式类型转换。
  - 实现 raw query 单次百分号解码、字面 `+`、重复键/未知键/非法 UTF-8 拒绝。
  - 实现 JSON 2 MiB、safe integer、Content-Type 和禁止 3xx 的协议辅助。
- **预计修改文件：**
  - `src/http/protocol/**`
  - `src/serialization/json/**`
  - `tests/unit/http_protocol/**`
- **测试：** 使用合法/非法信封、三种 RecognitionSnapshot 状态、分页、`+08:00`、中文、`%25`、`_`、反斜杠、重复 query 和响应上限夹具。
- **验收标准：** 精确字段集合与 REQ-001 一致；未知字段必拒绝；所有失败响应 `data=null`；序列化结果可被 Qt 严格 codec 接受。
- **是否可以并行：** 是，TASK-003 完成后可与 TASK-005、TASK-006、TASK-007、TASK-008 并行。

### TASK-005 实现有界队列和工作线程基础设施

- **目标：** 实现先预留槽位再受理上传的有界内存队列。
- **前置依赖：** TASK-003。
- **主要工作：**
  - 实现 queue reservation RAII、提交、取消、判满、阻塞消费和停止。
  - 实现默认单 worker、可配置容量及 start gate，避免数据库提交前取走任务。
  - 定义停止时丢弃未开始执行机会但保留数据库 PROCESSING 的行为。
- **预计修改文件：**
  - `src/queue/**`
  - `tests/unit/queue/**`
- **测试：** 覆盖容量 0/1/边界、并发预留、取消归还、start gate、停止唤醒和禁止重复提交。
- **验收标准：** 队列永不超过容量；满载可立即判定；未提交 reservation 自动归还；测试无死锁和数据竞争。
- **是否可以并行：** 是，可与 TASK-004、TASK-006、TASK-007、TASK-008 并行。

### TASK-006 实现图片校验和原子存储

- **目标：** 提供上传图片解码校验、SHA-256、原子保存、读取和补偿删除能力。
- **前置依赖：** TASK-002、TASK-003。
- **主要工作：**
  - 校验 JPEG/PNG 魔数、压缩大小、宽高、总像素、OpenCV 解码和实际格式。
  - 生成不可由用户控制的相对路径和扩展名。
  - 实现临时文件、完整写入、fsync、rename、目录 fsync 和安全读取。
  - 实现数据库失败后的 best-effort 删除及文件缺失映射。
- **预计修改文件：**
  - `src/storage/**`
  - `tests/unit/storage/**`
  - `tests/component/storage/**`
- **测试：** 覆盖合法 JPEG/PNG、伪 MIME、10 MiB/8192/4000 万边界、解码失败、短写、rename 失败、清理失败和路径穿越输入。
- **验收标准：** 只保存实际可解码 JPEG/PNG；成功返回后文件完整可读；用户输入不能进入路径；失败不留下正式文件。
- **是否可以并行：** 是，可与 TASK-004、TASK-005、TASK-007、TASK-008 并行。

### TASK-007 实现 YOLOv8 与 LPRNet 模型适配器

- **目标：** 实现固定模型的启动校验、预处理、推理、后处理和稳定领域结果。
- **前置依赖：** TASK-001、TASK-003。
- **主要工作：**
  - 引入经确认的 `yolov8_plate.onnx` 和 `lprnet.onnx`，校验 SHA-256、节点名、shape 和有限输出。
  - 实现 YOLO letterbox、无 sigmoid 输出、置信度筛选、NMS、坐标还原和最高置信度单车牌选择。
  - 实现 LPRNet BGR、94x24、`(pixel-127.5)/128`、`[1,68,T] -> [T,68]`、blank=0 CTC 和字符表。
  - 将异常映射为稳定模型失败码，不泄漏 ONNX 内部信息。
- **预计修改文件：**
  - `models/yolov8_plate.onnx`
  - `models/lprnet.onnx`
  - `models/LICENSES.md`
  - `src/model/**`
  - `tests/model/**`
  - `tests/fixtures/images/**`
- **测试：** ONNX checker/Runtime smoke、零张量、固定框/NMS、固定 crop/CTC、中国车牌教学样例和无车牌样例。
- **验收标准：** shape/hash 不符启动失败；多框选择规则确定；固定样例输出可重复；公开接口只返回领域结果。
- **是否可以并行：** 是，可与 TASK-004、TASK-005、TASK-006、TASK-008 并行。

### TASK-008 实现 MySQL 连接和版本化迁移

- **目标：** 从空 MySQL 8 数据库创建需求规定的四张业务表和技术迁移表。
- **前置依赖：** TASK-002、TASK-003。
- **主要工作：**
  - 实现连接池、UTC 会话时区、60 秒启动重试和参数化执行基础。
  - 实现 advisory lock、按文件版本执行、SHA-256 校验和 `schema_migrations`。
  - 创建四张业务表、索引、约束和固定演示管理员/设备数据。
  - 重复启动不覆盖密码、凭据或业务数据。
- **预计修改文件：**
  - `migrations/001_initial.sql`
  - `src/repositories/mysql/connection/**`
  - `src/repositories/mysql/migration/**`
  - `tests/component/mysql_migration/**`
- **测试：** MySQL 8 容器中测试空库、重复执行、checksum 漂移、并发迁移、连接失败和 UTC 会话。
- **验收标准：** 空库一次启动成功；重复启动无数据重置；迁移被修改后拒绝启动；表、索引、字符集和约束与 REQ-001 完全一致。
- **是否可以并行：** 是，可与 TASK-004、TASK-005、TASK-006、TASK-007 并行。

### TASK-009 实现 MySQL Repository

- **目标：** 实现管理员、设备、识别记录和名单 Repository 的全部数据操作。
- **前置依赖：** TASK-003、TASK-008。
- **主要工作：**
  - 实现按用户名、设备 Token 摘要、capture key 和 recognitionId 查询。
  - 实现 PROCESSING 插入、条件 finalize、启动恢复和事务内最终读取。
  - 实现历史 `[start,end)`、固定排序、分页/total 和 CSV 游标。
  - 实现名单分页、lookup、全局唯一新增、幂等删除和 LIKE 字面量转义。
- **预计修改文件：**
  - `src/repositories/mysql/repositories/**`
  - `tests/component/repositories/**`
- **测试：** 对真实 MySQL 容器覆盖参数绑定、事务回滚、并发 capture、条件 finalize、分页排序、safe integer、名单冲突和 `%/_/\` 查询。
- **验收标准：** 无字符串拼接 SQL；并发幂等只产生一条记录；状态机只能完成一次；分页与 CSV 过滤/排序一致；名单全局唯一。
- **是否可以并行：** 是，可与 TASK-010、TASK-011、TASK-013 并行。

### TASK-010 实现管理员鉴权和内存会话

- **目标：** 实现 bcrypt 登录验证、单 Token 会话、到期、注销和心跳验证服务。
- **前置依赖：** TASK-002、TASK-003、TASK-004。
- **主要工作：**
  - 使用 `crypt_r` 验证 bcrypt；不存在用户执行固定摘要验证。
  - 使用 `RAND_bytes` 生成不透明 Token，仅内存保存。
  - 同一 clientId 新登录原子撤销旧 Token；鉴权区分无效和过期。
  - 登录时从同一时间对象生成 HTTP/MQTT `expiresAt`；注销和心跳不访问外部依赖。
- **预计修改文件：**
  - `src/security/**`
  - `src/services/auth/**`
  - `tests/unit/auth/**`
- **测试：** 使用 Repository mock 覆盖正确/错误密码、未知用户、旧 Token、到期、并发同 clientId、注销、心跳 clientId 不匹配和敏感信息清理。
- **验收标准：** Token 不落盘不入日志；同 clientId 只有一个有效会话；两个到期时间字节一致；服务单测不依赖 MySQL/MQTT。
- **是否可以并行：** 是，可与 TASK-009、TASK-011、TASK-013 并行。

### TASK-011 实现 Mosquitto 初始化和设备开通脚本

- **目标：** 使用同源环境变量初始化 Broker 静态账号，并提供可重复执行的设备开通流程。
- **前置依赖：** TASK-002、TASK-008。
- **主要工作：**
  - 实现 `init-mosquitto.sh`，原子生成/更新 password、基础 ACL 和设备 ACL 合并结果。
  - 固定服务端账号仅写两个主题范围，管理账号仅读管理主题。
  - 实现 `provision-device.sh`，串行更新 MySQL、设备 password/ACL、重载 Broker并输出嵌入式配置。
  - 对单行 secret 保持独立字符规则：HTTP Token 拒绝逗号，设备 MQTT 密码允许逗号。
  - 明确部分失败步骤和幂等重跑结果，不宣称跨系统事务。
- **预计修改文件：**
  - `scripts/init-mosquitto.sh`
  - `scripts/provision-device.sh`
  - `deploy/mosquitto/mosquitto.conf`
  - `deploy/mosquitto/acl.base.template`
  - `tests/component/mosquitto_scripts/**`
- **测试：** 在临时卷和 MySQL/Mosquitto 容器中覆盖空初始化、重复运行、缺少或非法 secret、HTTP Token 逗号拒绝、MQTT 密码逗号接受、账号重复、设备只能订阅自己主题和脚本中途失败。
- **验收标准：** 应用/Broker 使用同一组变量；管理凭据可 CONNECT+SUBACK；设备无法订阅其他设备或管理主题；脚本日志无密码。
- **是否可以并行：** 是，可与 TASK-009、TASK-010、TASK-013 并行。

### TASK-012 实现 MQTT 发布器和事件序列化

- **目标：** 实现 Paho 异步连接、重连、QoS 1 发布及管理/设备精确 Payload。
- **前置依赖：** TASK-003、TASK-004、TASK-011。
- **主要工作：**
  - 使用固定 Client ID `plate-server`、MQTT 3.1.1、clean session true、QoS 1、retain false。
  - 实现有上限退避重连和线程安全状态；Broker 不可用不阻止 HTTP 启动。
  - 管理消息复用 RecognitionSnapshot serializer 且无 `gateAction`；设备消息只在最终状态增加一个 `gateAction`。
  - 发布失败只返回结果和记录脱敏日志，不写 Outbox、不修改数据库。
- **预计修改文件：**
  - `src/mqtt/**`
  - `tests/unit/mqtt_payload/**`
  - `tests/component/mqtt_publisher/**`
- **测试：** 使用 Mosquitto 覆盖 CONNECT、重连、ACL、QoS、retain、主题、重复 QoS 消息、非法状态和 Broker 离线。
- **验收标准：** Payload 字段精确；管理消息永不包含 `gateAction`；设备只收到本设备最终结果；Broker 离线不会抛出到业务线程或改变业务状态。
- **是否可以并行：** 是，依赖完成后可与 TASK-009、TASK-010、TASK-013 并行。

### TASK-013 实现 Crow HTTP 运行时和中间件

- **目标：** 建立统一请求管线、认证挂接点、异常边界和可注册控制器的 HTTP 运行时。
- **前置依赖：** TASK-002、TASK-004。
- **主要工作：**
  - 实现 requestId、body/header/URL 限制、Content-Type、Bearer 解析、严格 decoder 和访问日志中间件。
  - 提供控制器注册接口，由 TASK-019 组合，不在中央文件硬编码后续业务路由。
  - 将路由不存在、方法错误、Crow 解析错误和未分类异常转换为严格 JSON 信封。
  - 禁止所有 3xx；支持 JSON、图片和 CSV 三种响应模式及发送后异常中断。
- **预计修改文件：**
  - `src/http/server/**`
  - `src/http/middleware/**`
  - `tests/unit/http_server/**`
  - `tests/component/http_runtime/**`
- **测试：** 可控 controller 覆盖 404/405/400/413/500、未知 Content-Type、超限、异常、requestId、二进制错误信封和 CSV 断流。
- **验收标准：** 所有错误无 HTML、无堆栈、无 3xx；JSON 不超过 2 MiB；控制器可独立注入 mock Service 测试。
- **是否可以并行：** 是，可与 TASK-009、TASK-010、TASK-011、TASK-012 并行。

### TASK-014 实现登录、注销和心跳 HTTP API

- **目标：** 提供 Qt 可直接使用的认证、注销和心跳路由。
- **前置依赖：** TASK-009、TASK-010、TASK-013。
- **主要工作：**
  - 实现登录严格请求和响应，包含 Token、显示名、相同到期时间和完整 MQTT 配置。
  - 实现空 body 且可带 JSON Content-Type 的注销；拒绝任何非空 body。
  - 实现心跳 clientId/appVersion 校验和严格空成功信封。
  - 所有业务 401 返回稳定失败信封；登录不等待 MQTT。
- **预计修改文件：**
  - `src/http/controllers/auth/**`
  - `src/http/controllers/client/**`
  - `tests/integration/auth_api/**`
- **测试：** HTTP 测试覆盖登录成功/失败、严格字段、旧 Token、到期、心跳、空注销、非空注销和 10/5 秒 Qt 时限。
- **验收标准：** 满足 AC-001 至 AC-004；响应可被 Qt codec 直接解析；登录、注销和心跳均不因 MQTT 离线阻塞。
- **是否可以并行：** 是，可与 TASK-015、TASK-016、TASK-017 并行；四者文件互斥。

### TASK-015 实现识别详情、历史、图片和 CSV API

- **目标：** 完成 Qt 历史日志相关的只读 HTTP 能力。
- **前置依赖：** TASK-004、TASK-006、TASK-009、TASK-013。
- **主要工作：**
  - 实现详情、历史 `[start,end)`、可选 deviceId、固定 pageSize=100 和稳定排序。
  - 实现 JPEG/PNG 文件流式下载和所有错误的 JSON 信封。
  - 实现带 BOM、固定十列表头、空值为空字段、CRLF、RFC 4180 必要引用的 UTF-8 CSV 输出；设备列使用 `deviceId`，状态/错误码使用稳定枚举，时间精确为 `+08:00`，耗时为十进制毫秒。
  - 扩展领域和 MySQL Repository 为 pre-header pull cursor：在返回 CSV 成功响应前完成租约、只读一致性快照以及固定 64 行首批的查询和完整映射；后续在同一快照内以 `(captured_at, recognition_id)` 参数化 keyset 分批续读，禁止 OFFSET 和全结果内存聚合，之后的查询、映射、序列化或 socket 失败必须中止连接。
  - 分页和 CSV 必须复用同一个 HistoryFilter 和 Repository 排序。
- **预计修改文件：**
  - `src/domain/Ports.*`
  - `src/repositories/mysql/repositories/MySqlRepositories.*`
  - `src/services/history/**`
  - `src/services/csv/**`
  - `src/serialization/csv/**`
  - `src/http/controllers/recognition/**`
  - `tests/component/repositories/tst_mysql_repositories.cpp`
  - `tests/integration/history_api/**`
- **测试：** 覆盖详情 404、分页边界、时间/设备筛选、图片 MIME/大小/缺失、CSV BOM/列/空值/CRLF/转义、pre-header 503/500、next/mapper/serializer/socket 断流、真实 MySQL 至少跨三批的完整顺序/一致性快照/租约释放和 Qt 10/30/120 秒时限。
- **验收标准：** 满足 AC-015 至 AC-017；分页和 CSV 记录集合/顺序一致；CSV 中途失败使客户端收到传输失败而不是完整成功。
- **是否可以并行：** 是，可与 TASK-014、TASK-016、TASK-017 并行。

### TASK-016 实现黑白名单 HTTP API

- **目标：** 完成名单分页、精确查询、新增和删除业务。
- **前置依赖：** TASK-003、TASK-004、TASK-009、TASK-013。
- **主要工作：**
  - 实现 Unicode 车牌规范化、WHITE/BLACK 分页、keyword 字面量包含和 lookup。
  - 实现全局唯一新增、跨名单稳定冲突码和完整 AccessListRecord。
  - 实现按 ID 幂等删除；不存在返回 404；成功返回严格空信封。
  - 四路由在 runtime 传输保护和 Bearer 格式校验后先鉴权，再解码 query/path/body；POST/DELETE 拒绝非空 raw query。
  - DELETE 复用正数 safe integer decoder 并允许前导零；动态 DELETE `/lookup` 在鉴权后按非法 ID 返回 400。
  - 创建人使用 AuthSession 的用户 ID/显示名快照；Service 自有可注入 UTC 时钟在 insert 前单次采样 `createdAt`。
  - Repository `unavailable` 映射 503，显式名单冲突映射对应 409，lookup/remove 未命中映射 404，其他技术失败映射 500。
  - 不增加名单 MQTT，也不参与 GateAction。
- **预计修改文件：**
  - `src/services/access_list/**`
  - `src/http/controllers/access_list/**`
  - `tests/integration/access_list_api/**`
- **测试：** 覆盖中文/ASCII 大小写、Unicode 空白、`%/_/\`、分页、lookup、重复、跨名单冲突、删除、四路由鉴权/decoder 优先级、POST/DELETE query/body、DELETE safe ID 与 `/lookup`、创建人/时钟、Repository 错误映射和 10 秒时限。
- **验收标准：** 满足 AC-018；返回 DTO 字段精确；名单全局唯一；查询参数只解码一次；无 MQTT 副作用。
- **是否可以并行：** 是，可与 TASK-014、TASK-015、TASK-017 并行。

### TASK-017 实现设备鉴权和上传受理 API

- **目标：** 实现设备 multipart 上传、幂等、队列预留和 PROCESSING 发布流程。
- **前置依赖：** TASK-004、TASK-005、TASK-006、TASK-009、TASK-012、TASK-013。
- **主要工作：**
  - 验证设备 Token 的可见 ASCII/长度/逗号规则、归属/enabled、路径 deviceId、multipart 精确三字段和 capturedAt/captureId。
  - 先检查幂等，再预留队列槽位；满载立即 503，且不保存文件、不插库。
  - 保存图片并在事务内插入 PROCESSING；失败按顺序补偿。
  - 提交队列后返回 202；同键同摘要返回原记录，不同输入返回 409。
  - PROCESSING 提交且图片可下载后，尽力发布管理事件。
  - 固定严格 CRLF multipart、鉴权和错误优先级、startedAt 采样、无覆盖存储、最多 8 个 UUID v4 候选以及并发唯一冲突裁决。
  - 增强 QueueReservation，使 reserve 预分配 commit 节点/gate，正常有效 commit 不分配、不抛且不漏任务；Acceptance service 提供 `stopAcceptingAndWait()` 供 TASK-019 组合。
- **预计修改文件：**
  - `src/services/recognition/acceptance/**`
  - `src/http/controllers/device_recognition/**`
  - `tests/integration/device_upload_api/**`
  - `src/domain/Ports.h`（仅 `StorageFailure::alreadyExists`）
  - `src/queue/RecognitionTaskQueue.*`、`tests/unit/queue/**`
  - `src/storage/PosixFileOps.*`、`src/storage/PosixImageStorage.*`、`tests/component/storage/**`
  - `scripts/provision-device.sh`、`tests/component/mosquitto_scripts/tst_mosquitto_scripts.sh`（仅统一 HTTP Token 逗号规则所需的最小 TASK-011 ownership correction）
- **测试：** 覆盖鉴权和错误优先级、严格 multipart delimiter/header/CRLF、未知字段、格式/大小/尺寸、串行和并发 capture 重试、UUID/路径碰撞、冲突、队列满、存储失败、数据库失败、图片立即下载、MQTT 返回失败/抛异常、每个 202 恰一任务和 stopAccepting 与 in-flight 等待。
- **验收标准：** 满足 AC-005 至 AC-009、AC-023 的受理部分；任何 202 都有唯一 PROCESSING 记录、完整图片和一次队列任务；满载无残留。
- **是否可以并行：** 是，可与 TASK-014、TASK-015、TASK-016 并行。

### TASK-018 实现识别 worker、最终事务和 MQTT 副作用

- **目标：** 消费队列任务，完成模型识别、最终状态事务和两类最终事件发布。
- **前置依赖：** TASK-007、TASK-009、TASK-012、TASK-017。
- **主要工作：**
  - worker 以阻塞 `run() noexcept` 循环消费队列；已取出任务完成，停止时未取出任务由队列丢弃，线程创建和 join 留给 TASK-019。
  - 按 recognitionId 读取 PROCESSING 记录，严格核对并完整读取图片元数据/字节/签名，使用 worker 私有可注入解码器产生自持有 BGR 缓冲后调用模型接口。
  - 单调耗时覆盖查询、读图、校验、解码和模型，负差归零并饱和到 `2^53-1`；`completedAt=max(nowUtc, startedAt)`。
  - 在单事务中从 PROCESSING 条件更新为 SUCCEEDED 或 FAILED，revision 递增并回读最终快照。
  - 每个有效任务只调用一次 finalize；仅返回已提交最终记录时先发布管理完整快照，再按最终状态映射发布设备结果和 GateAction。
  - 记录缺失/已完成、查询失败、条件更新失败或异常不推理或不发布；不回读猜测、不重试。
  - 两次 MQTT 发布分别捕获拒绝和异常，管理发布失败仍尝试设备发布；只记录脱敏稳定分类，不回滚、不改 revision、不重试业务消息。
- **预计修改文件：**
  - `src/services/recognition/worker/**`
  - `tests/integration/recognition_worker/**`
- **测试：** mock 模型/Repository/Storage/解码器/MQTT/时钟覆盖成功、三类显式模型失败、模型异常、图片缺失/短读/元数据或签名不符/损坏解码、墙钟回拨、耗时饱和、查询与 finalize 的各类失败及异常、提交前不发布、严格发布顺序和相互独立失败、串行及并发重复任务、停止边界、单任务异常后继续、自持有 BGR 生命周期和日志脱敏。
- **验收标准：** 满足 AC-010 至 AC-012、AC-020；数据库是最终事实；MQTT Payload 可独立恢复完整状态；失败场景设备动作为 KEEP_CLOSED。
- **是否可以并行：** 是，TASK-017 完成后可与尚未完成的 TASK-014、TASK-015、TASK-016 并行。

### TASK-019 实现应用组合、健康检查、启动恢复和停止

- **目标：** 将所有模块装配为单进程服务并实现确定的生命周期。
- **前置依赖：** TASK-002 至 TASK-018 全部完成。
- **主要工作：**
  - production main 固定零参数，从 `/app/config/server.json` 加载配置、从 `/app/migrations` 执行迁移；仅测试构造可注入路径，不增加公开 env/CLI。
  - 按设计顺序加载配置、bootstrap console logger、目录、最终 rotating file logger、每 worker 私有 recognizer/decoder/clock、MySQL、迁移、Recognition Repository、恢复、其余 Repository、Service、队列、HTTP 和 MQTT；bootstrap logger 只用于最终 logger 建立前的脱敏启动错误，后续统一使用最终 logger，每个已创建 logger 都在停止或回滚最后 flush；每 worker 独立持有一对 YOLO/LPRNet Session。
  - HTTP 监听前保证模型/MySQL 可用；HTTP 等待必须 ready 或 exception 二选一，禁止 sleep、盲目超时和预绑定竞态；MQTT 不可用时以 DEGRADED 启动。
  - 启动事务将遗留 PROCESSING 改为 FAILED/SERVER_RESTARTED；Repository 非 value 为致命启动失败。首次 MQTT 连接时每条严格先管理 final 后设备 final，各主题最多一次、独立处理失败，删除后不因重连重试，且不入队、不推理。
  - 实现 `/health`：MySQL 使用 pool 不可变配置创建不进入业务池的一次性 health-only connection，连接与网络读超时各 1 秒，UTC/session 配置后固定执行一次 `SELECT 1`，完整探针总计最多 3 秒且无重试；不得占用或改变六条业务连接。同时读取模型/MQTT/队列状态。200 使用既有精确六字段 UP/DEGRADED DTO，模型或 MySQL DOWN 返回 503 `SERVICE_UNAVAILABLE` 且 `data=null`。
  - 在创建任何线程前阻塞 SIGINT/SIGTERM，由主线程 POSIX `sigwait` 同步停止；严格执行 acceptance stop/wait、HTTP stop/join、queue requestStop、worker join、MQTT stop、pool close、logger flush。启动失败按已完成阶段逆序幂等回滚。
- **预计修改文件：**
  - `src/app/runtime/**`
  - `src/services/health/**`
  - `src/http/controllers/health/**`
  - `src/main.cpp`
  - `tests/integration/application_lifecycle/**`
  - 根 `CMakeLists.txt`（仅把占位 main 切换为 production `src/main.cpp`）
  - `cmake/PatchCrow121.cmake`、`src/http/server/HttpServer.h`、`src/http/server/HttpServer.cpp`、`tests/unit/http_server/**`、`tests/component/http_runtime/**`（仅 ready-or-exception）
  - `src/repositories/mysql/connection/MySqlConnectionPool.h`、`src/repositories/mysql/connection/MySqlConnectionPool.cpp`、`tests/component/mysql_migration/**`（仅使用一次性 health-only connection、connect/read 各 1 秒、总计最多 3 秒且不改变业务 pool 的真实 health probe）
- **测试：** 使用 production-neutral fake seam 覆盖精确启动顺序、bootstrap/final logger 切换和各失败阶段最后 flush、每阶段失败的逆序回滚、MySQL 超时、模型不匹配、多 worker 私有依赖、占用端口 ready-or-exception、MQTT 降级、健康六字段/503 null、SIGINT/SIGTERM、严格停止顺序和 PROCESSING 恢复；真实 MySQL 组件测试覆盖 `SELECT 1` 成功、业务 pool 全部租出时 health probe 仍独立成功且业务连接数量不变、health-only connection 无重试；真实故障注入暂停 MySQL，验证网络读超时、完整探针 3 秒边界、503 `data=null`，恢复 MySQL 后下一探针恢复 UP。
- **验收标准：** 满足 AC-019；启动/停止无悬挂线程；HTTP 启动异常可观测且非零退出；MySQL/模型/恢复失败非零退出；仅 MQTT 失败仍提供 HTTP；恢复记录两个主题各最多尝试一次且绝不重新推理；正常停止与启动回滚顺序精确。
- **是否可以并行：** 否。它是模块组合任务，必须在 TASK-002 至 TASK-018 后串行执行。

### TASK-020 实现 Docker 镜像和 Compose 教学部署

- **目标：** 交付单独应用镜像及 app/MySQL/Mosquitto 三容器 Compose。
- **前置依赖：** TASK-011、TASK-019。
- **主要工作：**
  - 多阶段构建 linux/amd64 Release，镜像包含应用、运行库、migration、模型、许可证和 CA。
  - Compose 配置三服务、healthcheck、启动依赖、网络、端口和 images/logs/mysql/mosquitto 持久卷。
  - MQTT 容器入口执行同源 secret 初始化；应用内网连接 `mysql:3306`、`mqtt:1883`，对外下发 `MQTT_PUBLIC_HOST`。
  - 提供隔离教学网络 `.env.example`，不提交真实 secret。
- **预计修改文件：**
  - `Dockerfile`
  - `.dockerignore`
  - `docker-compose.yml`
  - `.env.example`
  - `deploy/app/entrypoint.sh`
  - `deploy/mosquitto/entrypoint.sh`
  - `tests/system/docker_compose/**`
- **测试：** 从空卷构建/启动、等待 healthcheck、检查三个容器、重启持久化、确认镜像不依赖宿主机 OpenCV/ONNX/model。
- **验收标准：** 满足 AC-022 的部署部分；应用镜像不运行 MySQL/Mosquitto；固定模型 hash 通过；外部 Qt/设备可达 8080/1883。
- **是否可以并行：** 是，可与 TASK-021 并行，文件范围不相交。

### TASK-021 建立 Qt 严格契约测试门禁

- **目标：** 使用真实 Qt fixture/严格解析器验证服务端 HTTP 和管理 MQTT 输出。
- **前置依赖：** TASK-004、TASK-012、TASK-014 至 TASK-019。
- **主要工作：**
  - 导入或调用 Qt 合法/非法信封、RecognitionSnapshot、分页和 AccessListRecord fixture。
  - 对服务端输出执行 exact key、null、safe integer、时间和 MIME 断言，并由 Qt parser 做最终解析。
  - 覆盖 raw query 的 `+08:00`、中文、百分号和空 body 注销。
  - 覆盖图片/CSV 错误 JSON、CSV 中途断流和 Qt 总时限。
- **预计修改文件：**
  - `tests/fixtures/qt/**`
  - `tests/contract/qt_http/**`
  - `tests/contract/qt_mqtt/**`
  - `scripts/run-qt-contract-tests.sh`
- **测试：** 独立启动服务端测试实例/Mock Repository，运行 `ctest -R contract.qt --output-on-failure`。
- **验收标准：** AC-002、AC-014 的非法输出必失败；所有合法 HTTP/MQTT DTO 可被 Qt 严格解析；测试不修改 Qt 项目文件。
- **是否可以并行：** 是，可与 TASK-020 并行。

### TASK-022 实现嵌入式设备模拟器和系统联调

- **目标：** 用可执行模拟器验证设备先订阅、上传、最终动作和 QoS 去重闭环。
- **前置依赖：** TASK-012、TASK-017、TASK-018、TASK-020、TASK-021。
- **主要工作：**
  - 模拟唯一 Client ID、MQTT 3.1.1、cleanSession=false、QoS 1 和设备 ACL。
  - 收到 SUBACK 后上传 multipart；验证 202、PROCESSING 和最终结果。
  - 按 recognitionId+revision 去重并映射 OPEN/KEEP_CLOSED；无消息时保持关闭。
  - 覆盖断线持久会话、重复 QoS 消息、错误设备凭据和 Broker 发布前离线的已接受限制。
- **预计修改文件：**
  - `tests/simulators/embedded/**`
  - `tests/system/embedded_e2e/**`
  - `scripts/run-embedded-e2e.sh`
- **测试：** 对 Compose 环境运行成功、失败、重复、断线、ACL 和每秒 1 张受理场景。
- **验收标准：** 满足 AC-005、AC-011 至 AC-013、AC-020、AC-021、AC-023、AC-025；模拟器从开通脚本输出即可连接，不使用服务端内部配置。
- **是否可以并行：** 是，可与 TASK-023 并行实现；共享 Compose 的实际系统测试必须串行。

### TASK-023 使用真实 Qt 客户端完成管理端联调

- **目标：** 验证服务端与完成 TASK-001 至 TASK-022 的真实 Qt 客户端全部业务交互。
- **前置依赖：** TASK-014 至 TASK-021；Qt 客户端自身 TASK-001 至 TASK-022 全部完成并通过。
- **主要工作：**
  - 使用唯一非零 Qt clientId、`http://192.168.137.128:8080` 和 `allowInsecureTransport=true`。
  - 验证登录、心跳、MQTT CONNECT+SUBACK、PROCESSING/最终快照、图片、历史、CSV 和名单。
  - 验证 MQTT 暂时/永久错误不错误终止 HTTP 会话，以及 HTTP 401/到期退出。
  - 保存脱敏的测试结果，不记录密码、Token、完整 Payload 或图片。
- **预计修改文件：**
  - `tests/system/qt_e2e/**`
  - `scripts/run-qt-e2e.sh`
  - `docs/verification/qt-e2e-result.md`
- **测试：** 对 Compose 环境运行真实 Qt 可执行程序和可控测试数据；人工核对 UI，脚本核对服务端数据库/MQTT/HTTP 结果。
- **验收标准：** 满足 AC-001 至 AC-004、AC-014 至 AC-018、AC-024；Qt 无兼容代码即可完成全部管理场景；HTTP/MQTT 状态相互独立。
- **是否可以并行：** 是，可与 TASK-022 并行实现；共享 Compose 的实际系统测试必须串行。

### TASK-024 执行最终验收、镜像打包和文档收尾

- **目标：** 对 AC-001 至 AC-025 做最终门禁，生成可交付镜像和可重复执行记录。
- **前置依赖：** TASK-020、TASK-021、TASK-022、TASK-023。
- **主要工作：**
  - 从干净构建目录和空数据卷运行全部单元、组件、契约、集成和系统测试。
  - 执行每秒 1 张持续上传、容器重启、Broker 离线和三端完整演示。
  - 构建带固定 tag/digest 的 linux/amd64 镜像，记录模型和镜像 SHA-256。
  - 只把实际验证成功的构建、启动、设备开通和联调命令写入 README。
  - 生成 AC-001 至 AC-025 的逐项结果和已知限制报告。
- **预计修改文件：**
  - `README.md`
  - `scripts/build-release-image.sh`
  - `docs/verification/REQ-001-acceptance-report.md`
- **测试：** 执行项目全量 CTest、Compose 空卷测试、Qt 联调和嵌入式模拟器联调。
- **验收标准：** AC-001 至 AC-025 全部有可复核结果；镜像可在无开发依赖的 linux/amd64 主机配合 Compose 启动；报告明确保留 Broker 消息可能丢失等教学限制。
- **是否可以并行：** 否。最终任务必须在所有实现和联调任务完成后串行执行。

## 4. 验收标准追踪

| 需求验收 | 主任务 | 最终复验 |
|---|---|---|
| AC-001 | TASK-010、TASK-014、TASK-021 | TASK-023、TASK-024 |
| AC-002 | TASK-004、TASK-014、TASK-021 | TASK-023、TASK-024 |
| AC-003 | TASK-010、TASK-014 | TASK-023、TASK-024 |
| AC-004 | TASK-010、TASK-014 | TASK-023、TASK-024 |
| AC-005 | TASK-012、TASK-017 | TASK-022、TASK-024 |
| AC-006 | TASK-009、TASK-017 | TASK-022、TASK-024 |
| AC-007 | TASK-017 | TASK-022、TASK-024 |
| AC-008 | TASK-005、TASK-017 | TASK-022、TASK-024 |
| AC-009 | TASK-006、TASK-017 | TASK-022、TASK-024 |
| AC-010 | TASK-007 | TASK-022、TASK-024 |
| AC-011 | TASK-007、TASK-009、TASK-012、TASK-018 | TASK-022、TASK-024 |
| AC-012 | TASK-007、TASK-009、TASK-012、TASK-018 | TASK-022、TASK-024 |
| AC-013 | TASK-012、TASK-022 | TASK-024 |
| AC-014 | TASK-004、TASK-012、TASK-021 | TASK-023、TASK-024 |
| AC-015 | TASK-009、TASK-015、TASK-021 | TASK-023、TASK-024 |
| AC-016 | TASK-006、TASK-015、TASK-021 | TASK-023、TASK-024 |
| AC-017 | TASK-009、TASK-015、TASK-021 | TASK-023、TASK-024 |
| AC-018 | TASK-003、TASK-009、TASK-016、TASK-021 | TASK-023、TASK-024 |
| AC-019 | TASK-009、TASK-019 | TASK-024 |
| AC-020 | TASK-012、TASK-018、TASK-022 | TASK-024 |
| AC-021 | TASK-011、TASK-022 | TASK-024 |
| AC-022 | TASK-020 | TASK-024 |
| AC-023 | TASK-005、TASK-017、TASK-022 | TASK-024 |
| AC-024 | TASK-021、TASK-023 | TASK-024 |
| AC-025 | TASK-022 | TASK-024 |

## 5. 完成定义

单个任务只有同时满足以下条件才算完成：

1. 前置依赖已经验收，且未擅自改变其公开接口。
2. 只修改本任务文件范围，没有覆盖并行任务文件。
3. 本任务列出的正常、错误、边界和并发测试全部通过。
4. 新代码没有记录密码、Token、完整 Payload、完整图片或绝对路径。
5. 没有放宽严格 JSON、时间、safe integer、状态组合、分页、ACL 或模型契约。
6. 构建无新增未说明依赖，测试不依赖开发者机器上的生产数据。
7. 测试失败时能定位到领域、存储、Repository、模型、MQTT、HTTP 或系统层中的具体边界。
