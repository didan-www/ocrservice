# AGENTS.md

本文件适用于项目根目录及所有子目录。任何 AI 代理或开发者在分析、修改、构建或测试本项目之前，必须遵守以下约束。

## 必读顺序

1. 完整阅读根目录 `README.md`。
2. 完整阅读 `docs/REQ-001.md`。
3. 检查工作区和 Git 状态，保留用户已有修改。
4. 用户最新指示与文档冲突时，以用户最新指示为准，并同步更新需求、README 和相关测试。

## 文档职责

- `docs/REQ-001.md` 是业务范围、HTTP/MQTT 契约、数据库结构和验收标准的唯一事实来源。
- `README.md` 只描述项目入口、当前实际状态、依赖和已经验证过的使用方法。
- 本文件只规定开发纪律，不在此另行发明业务字段或接口。
- 任何公共接口、DTO、错误码、数据库字段或 MQTT 消息变更，必须先修改 `REQ-001.md`，再修改实现和测试。

## 产品边界

- 本项目是教学演示用 Linux 车牌识别服务，不按商业生产系统设计。
- 系统由嵌入式端、Linux 服务端、MySQL、Mosquitto 和 Qt 管理客户端组成。
- 嵌入式端负责拍照、HTTP 上传、订阅本设备 MQTT 最终结果和执行闸杆动作。
- Linux 服务端负责设备鉴权、图片保存、异步排队、模型识别、MySQL 记录、管理端 HTTP API 和 MQTT 发布。
- Qt 客户端只做登录、心跳、实时监控、历史查询、CSV 下载和黑白名单管理，不上传图片、不调用模型、不发布抬杆消息。
- 黑白名单首版只作为管理演示数据，不参与 `OPEN/KEEP_CLOSED` 决策。
- 不增加角色权限、刷新令牌、设备管理 API、视频流识别、摄像头直连、集群、负载均衡或云服务。

## 固定技术基线

- Linux `amd64`、C++17、CMake、GCC。
- Crow HTTP 服务框架。
- OpenCV 4、ONNX Runtime CPU、YOLOv8、LPRNet。
- MySQL 8、Mosquitto、MQTT 3.1.1。
- Docker 应用镜像和 Docker Compose 教学部署。
- 应用镜像不得依赖 NVIDIA GPU、CUDA、ARM64 或宿主机预装模型。
- 不使用 `-march=native` 构建可分发镜像。

## 架构约束

- 使用单进程模块化单体，不拆分业务微服务。
- HTTP 控制器只做协议解析、鉴权和响应组装，不直接执行 SQL 或模型推理。
- 业务服务组织事务、状态转换和调用顺序。
- Repository 是唯一直接访问 MySQL 的业务代码。
- 模型适配层只暴露稳定的单图单车牌识别接口，不向 HTTP 层暴露 ONNX 张量。
- MQTT 发布器不得改变数据库业务状态。
- 识别使用有界内存队列，默认一个工作线程；HTTP 线程不得执行模型推理。
- 图片文件必须在发布 `PROCESSING` 前完整落盘，Qt 收到事件后必须能够立即下载。

## Qt HTTP 契约不变量

- Qt 接口路径、字段和状态以 `REQ-001` 为准，不兼容旧的 `/plate/upload` 或蛇形命名响应。
- JSON 响应精确使用 `success/code/message/requestId/data` 五字段信封。
- Qt DTO 严格拒绝未知字段；服务端不得添加“调试字段”或省略必填字段。
- 失败响应必须 `success=false` 且 `data=null`。
- 无业务数据的成功响应必须 `success=true` 且 `data=null`。
- 所有 JSON 整数必须处于 `[-(2^53-1), 2^53-1]`，业务 ID 使用正数。
- 所有协议时间必须输出带 `+08:00` 的 ISO 8601 字符串。
- `RecognitionSnapshot` 的可空字段必须显式输出 JSON `null`。
- 分页字段固定为 `items/page/pageSize/total`，首版 `pageSize` 只能为 100。
- JSON 成功或错误响应不得超过 2 MiB。
- 图片成功响应只能是 `image/jpeg` 或 `image/png`；图片错误响应仍使用 JSON 失败信封。
- CSV 必须为带 UTF-8 BOM 的 `text/csv; charset=utf-8`，列名和顺序不得改变。
- 服务端不得返回 3xx 重定向。

## 会话与鉴权约束

- Qt 使用用户名和密码登录，服务端签发单一内存 Bearer Token，不实现刷新令牌。
- Token 默认有效期 8 小时；同一 `clientId` 新登录使旧 Token 失效。
- 登录响应中的 HTTP Token 和 MQTT 逻辑有效期必须完全相同。
- Qt MQTT Client ID 使用本地配置中的 `clientId` UUID 原文，不增加服务端前缀。
- 密码只保存安全摘要；HTTP Token 和 MQTT 密码不得写入应用日志。
- 每台设备使用独立 HTTP Bearer Token、MQTT 用户名和主题 ACL。
- 新增设备必须通过统一运维脚本同时更新 MySQL 与 Mosquitto，禁止只改其中一处。

## 嵌入式上传约束

- 固定接口为 `POST /api/v1/devices/{deviceId}/recognitions`。
- multipart 必须包含 `image`、`captureId` 和 `capturedAt`，不接受未知字段。
- `image` 只接受 JPEG/PNG，压缩体最大 10 MiB。
- 解码后宽、高分别不超过 8192，总像素不超过 4000 万。
- `captureId` 必须为 UUID；服务端按 `(deviceId, captureId)` 幂等。
- 相同键和相同图片摘要返回原记录；相同键但图片或拍摄时间不同返回 409。
- 队列满时返回 503，不保存图片、不创建识别记录。
- 设备必须先成功建立 MQTT 持久会话和订阅，再发起图片上传。

## MQTT 不变量

- QoS 固定为 1，消息固定为非 retain。
- Qt 管理主题为 `plate/management/recognition-events`，只允许完整 `RecognitionSnapshot`，不得包含 `gateAction`。
- 设备主题为 `plate/devices/{deviceId}/recognition-results`，只发布最终结果并增加 `gateAction`。
- `SUCCEEDED` 对应 `OPEN`；`FAILED` 对应 `KEEP_CLOSED`。
- Qt 使用 `cleanSession=true`；设备使用唯一 Client ID 和 `cleanSession=false`。
- Broker 开启持久化，但首版不实现应用级 MQTT Outbox 或业务重发。
- 若服务端发布时 Broker 未接受消息，结果可能永久丢失；这是用户已接受的教学版限制，不得在实现或 README 中宣称可靠必达。
- 设备对重复 QoS 1 消息按 `recognitionId + revision` 去重，未收到最终消息时保持闸杆关闭。

## 状态和一致性约束

- 新任务初始为 `PROCESSING/revision=1`。
- 最终状态只能是 `SUCCEEDED` 或 `FAILED`，revision 必须递增。
- 状态更新、时间和可空字段必须满足 `REQ-001` 的组合约束。
- 容器启动时，遗留 `PROCESSING` 记录改为 `FAILED/SERVER_RESTARTED`，不自动重新推理。
- 队列槽位必须先预留，再保存图片和提交数据库记录，避免已受理任务无法入队。
- 数据库失败时清理新建图片；文件清理失败只记录脱敏技术日志。
- 不自动删除识别图片；图片缺失时下载接口返回 404 JSON 失败信封。

## 数据库约束

- 只由 Linux 服务端访问 MySQL，Qt 和嵌入式端不得直连数据库。
- 所有数据库时间按 UTC 保存，对外转换为 `+08:00`。
- 使用参数化 SQL，不拼接用户输入。
- 识别查询排序固定为 `captured_at DESC, recognition_id DESC`。
- 名单查询排序固定为 `created_at DESC, id DESC`。
- 时间筛选使用 `[startTime, endTime)`。
- 名单车牌执行首尾去空白、ASCII 字母大写、拒绝内部空白、长度 1 至 16。
- 规范化车牌全局唯一，不能同时存在于黑白名单。
- 首版不创建 MQTT Outbox 表。

## Docker 与配置约束

- 应用镜像只运行 Linux 服务程序，不在同一容器启动 MySQL 或 Mosquitto。
- Compose 分别运行应用、MySQL 和 Mosquitto，并为数据库、Broker、图片和日志配置持久化卷。
- 模型和运行库打包进应用镜像。
- MySQL 是应用启动的必需依赖；MQTT 暂时不可用时应用可启动并后台重连。
- Compose 必须使用 healthcheck，不得把 `depends_on` 当作服务已经就绪。
- 固定演示凭据只能用于隔离教学网络，文档和日志必须明确警告。

## 日志和安全

- 日志不得记录密码、Bearer Token、MQTT 密码、完整图片、完整请求体或完整 MQTT 载荷。
- 记录 `requestId`、可选 `recognitionId`、模块、稳定错误码和必要的脱敏上下文。
- 文件路径由服务端生成，禁止使用用户提供路径，防止路径穿越。
- 上传格式必须通过实际解码确认，不信任扩展名或 multipart MIME。
- 本项目使用明文 HTTP/MQTT 仅限隔离教学网络，不得描述为安全生产部署。

## 修改纪律

- 优先使用现有模块和约定，保持实现简单直接。
- 不进行与当前任务无关的重构、重命名或全仓格式化。
- 修改协议时同步更新 `REQ-001`、协议测试、Qt 契约测试和嵌入式模拟测试。
- 不得为了让测试通过而放宽严格字段、时间、分页或状态校验。
- 不得把规划命令写成 README 中“已验证命令”；只有实际执行成功的命令才能记录。

## 最低验证要求

- 单元测试：信封、DTO、时间、整数、车牌规范化、状态组合和模型后处理。
- 组件测试：MySQL Repository、上传幂等、队列满载、文件清理和 MQTT ACL。
- 契约测试：复用 Qt 客户端合法/非法 JSON 样例，确保服务端输出可被严格解析。
- 集成测试：登录、心跳、PROCESSING、图片下载、最终事件、历史、CSV 和名单闭环。
- 嵌入式模拟测试：先订阅、再上传、接收最终动作、QoS 重复去重。
- Docker 验收：从空数据卷启动 Compose，确认初始化、健康检查、持久化和重启恢复。
- 已接受限制测试：Broker 在发布前断开时允许消息丢失，但数据库最终状态必须正确且服务不得崩溃。
