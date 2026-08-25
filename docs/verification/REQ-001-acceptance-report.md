# REQ-001 最终验收报告

## 报告状态

本报告对应服务端 TASK-024。开发者门禁和主 Agent 独立代码审查、构建、回归、三端联调、发布归档恢复验证均已通过，REQ-001 最终验收完成。

## 被测基线

- 服务端产品源码 revision：`f54e0df4f2860d5d276d78fa0c6696fee8d8f3bf`。
- Qt 客户端 revision：`60c3bb80350f83d3fc704454f3c48e5c46ff57b0`。
- 目标平台：`linux/amd64` CPU。
- 工具：GCC 11.4.0、CMake 3.22.1、Docker 29.1.3、Docker Compose 2.40.3、宿主 Mosquitto 2.0.11、容器 Mosquitto 2.0.20、MySQL 8.4。

## 最终门禁

| 门禁 | 开发者结果 | 主 Agent 独立复验 | 最终证据 |
|---|---|---|---|
| 干净 Debug 构建与全量 CTest | 通过 | 通过 | 全新构建 `245/245`；CTest `30/30` |
| 干净 Release 构建与全量 CTest | 通过 | 通过 | 全新构建 `245/245`；CTest `30/30` |
| 真实 MySQL 与 Mosquitto 无跳过测试 | 通过 | 通过 | 每种配置 MySQL CTest `2/2`、migration `16/16`、Repository `14/14`，无 skip；真实 Mosquitto wrapper 通过 |
| 空数据卷 Docker Compose | 通过 | 通过 | 三服务 healthy；模型、MySQL、图片/日志/Broker 持久化、匿名 MQTT 拒绝、重启恢复均通过；清理容器/卷/网络 `0/0/0`，宿主 Mosquitto 恢复 active |
| 嵌入式模拟器完整联调 | 通过 | 通过 | `mqttAuth=1 httpAuth=1 acl=2 processing=1 failed=1 success=1 persistent=1 duplicate=1 brokerLoss=1 rate1Hz=30` |
| 真实 Windows Qt 客户端完整联调 | 通过 | 通过 | 15 项 flags 全为 1；MQTT 临时恢复、永久鉴权错误时 HTTP 在线、401 和到期返回登录均通过；清理 `0/0/0` |
| 每秒 1 张、容器重启与 Broker 离线 | 通过 | 通过 | 1 Hz 连续 30 张得到 30 个唯一结果；重启恢复为 `FAILED/2/SERVER_RESTARTED`；Broker 离线时 HTTP 为 `DEGRADED/MQTT DOWN` 且数据库最终状态正确 |
| 固定 tag 的 linux/amd64 发布镜像与归档 | 通过 | 通过 | tag、平台、模型哈希、`ldd`、归档 SHA-256 通过；仅移除 tag 后由归档恢复为原 image ID |

## 发布脚本静态与负向门禁

- Bash 语法、`git diff --check`、UTF-8/LF、单 LF EOF、文件权限和秘密扫描通过。
- 发布输入 clean gate 的产品范围相对 HEAD 无 tracked 或 untracked 变化。
- 仓库内 canonical 路径、`..` 逃逸、符号链接 ancestor 和既有目标均在调用 Docker 前被拒绝。
- 并发晚创建目标场景被原子 no-clobber 发布拒绝；既有目标字节保持不变，临时文件被清理。
- 最终归档在目标目录内创建临时文件，并通过同目录硬链接的原子 no-replace 语义发布，不覆盖任何时刻创建的目标文件。

## AC-001 至 AC-025

| 验收项 | 开发者结果 | 主 Agent 独立复验 | 最终证据 |
|---|---|---|---|
| AC-001 | 通过 | 通过 | 真实 Qt 登录 flag=1，严格登录契约通过 |
| AC-002 | 通过 | 通过 | Qt 合法/非法夹具契约门禁与服务端严格 DTO 测试通过 |
| AC-003 | 通过 | 通过 | 真实 Qt heartbeat flag=1，服务端 HTTP 200/OK |
| AC-004 | 通过 | 通过 | 真实 Qt `http401=1 expiry=1`，严格 401 失败信封通过 |
| AC-005 | 通过 | 通过 | 嵌入式先 SUBACK 后上传，PROCESSING 管理事件和数据库记录通过 |
| AC-006 | 通过 | 通过 | 上传幂等回归通过，同键同内容不重复受理 |
| AC-007 | 通过 | 通过 | 上传冲突回归通过，返回 `CAPTURE_ID_CONFLICT` |
| AC-008 | 通过 | 通过 | 队列满载与图片/数据库补偿回归通过 |
| AC-009 | 通过 | 通过 | 图片体积、尺寸、像素和实际解码限制回归通过 |
| AC-010 | 通过 | 通过 | 模型 NMS、最高置信度和坐标还原单元测试通过 |
| AC-011 | 通过 | 通过 | 数据库 `SUCCEEDED`，设备执行 `OPEN`，真实 Qt final=1 |
| AC-012 | 通过 | 通过 | 数据库 `FAILED`，设备执行 `KEEP_CLOSED` |
| AC-013 | 通过 | 通过 | 重复发布后仅一个唯一结果和一次动作 |
| AC-014 | 通过 | 通过 | 严格 MQTT 契约拒绝管理消息 `gateAction` |
| AC-015 | 通过 | 通过 | 历史 `[start,end)`、固定排序、分页/total 及真实 Qt history=1 |
| AC-016 | 通过 | 通过 | 真实 Qt image=1，JPEG MIME、字节和 SHA 匹配 |
| AC-017 | 通过 | 通过 | 真实 Qt csv=1；UTF-8 BOM、10 列、CRLF 通过；独立复验 SHA-256 为 `01a2567e0fe944ac8bb26250ca8e6f1216cc4c5951f4ec8b787f1f42bcb7a3f4` |
| AC-018 | 通过 | 通过 | 新增/冲突/删除 flags 全为 1，`409/ACCESS_LIST_CONFLICT_WHITE` 正确，最终名单为空 |
| AC-019 | 通过 | 通过 | `PROCESSING -> FAILED/2/SERVER_RESTARTED`，未重新推理 |
| AC-020 | 通过 | 通过 | Broker 离线时服务存活、数据库最终正确，重连不补发 |
| AC-021 | 通过 | 通过 | provision 脚本同步 MySQL、密码文件和 ACL；错误凭据及跨主题访问被拒绝 |
| AC-022 | 通过 | 通过 | 空数据卷三容器初始化、健康、持久化和固定账号闭环通过 |
| AC-023 | 通过 | 通过 | 1 Hz 连续 30 张，`accepted=30 uniqueResults=30 actionsExecuted=30` |
| AC-024 | 通过 | 通过 | 真实 Windows Qt 15 项 flags 全为 1，清理 `0/0/0` |
| AC-025 | 通过 | 通过 | 嵌入式认证、SUBACK、上传、最终动作、持久会话和 QoS 去重通过 |

## 发布物

| 项目 | 结果 |
|---|---|
| 固定 tag | `ocrservice:req-001-v1.0.0` |
| source revision | `f54e0df4f2860d5d276d78fa0c6696fee8d8f3bf` |
| immutable image ID | `sha256:713379038dd66a9b65b008e5ad7277de04431206c01dcbcba08e4ce0c817d94a` |
| local daemon RepoDigest | `ocrservice@sha256:713379038dd66a9b65b008e5ad7277de04431206c01dcbcba08e4ce0c817d94a`；这是本地 daemon 元数据，不是已验证的 registry digest |
| registry RepoDigest | 未推送、未验证，不能用 image ID 或 local daemon RepoDigest 冒充 |
| 平台 | `linux/amd64` |
| 镜像归档 | 仓库外生成，153239040 bytes，模式 `0644` |
| 镜像归档 SHA-256 | `004d11d9721262863c4df0649293a2ee4d16018914581d3fc525a9457f07bc99` |
| YOLOv8 模型 SHA-256 | `bfa426b74d4b619207cca55b296ce3603fb784755a205791af0d0dab4fcf6c04` |
| LPRNet 模型 SHA-256 | `c78e54070d0e2b8a6f8d548feb52b75321d5f464bcaadb679ec7ee31433cffa4` |
| 归档恢复验证 | 通过；仅移除固定 tag 后执行 `docker load`，恢复为原 image ID，平台与模型哈希正确，`ldd` 无 `not found` |

公网 GitHub 下载在开发者验收中发生连接重置/拒绝。成功构建使用本机缓存的官方 ONNX Runtime 1.20.1 归档经临时 HTTP 服务提供，归档 SHA-256 精确匹配 Dockerfile 固定值；构建完成后临时服务已停止。

## 已确认限制

- 本项目只适用于隔离教学网络；固定演示凭据和明文 HTTP/MQTT 不属于生产安全方案。
- 服务端没有 MQTT Outbox 或应用级业务重发。Broker 未接受的发布可能永久丢失，但数据库最终状态必须正确且服务不得崩溃。
- MySQL 是历史事实来源，MQTT 不是可靠历史来源；设备未收到最终消息时必须保持闸杆关闭。
- 黑白名单首版只用于管理演示，不参与 `OPEN/KEEP_CLOSED` 决策。
- 模型只支持固定 ONNX shape、字符表和单图最高置信度单车牌；教学样例结果不能外推为生产准确率。
- 应用镜像只支持 `linux/amd64` CPU，不支持 GPU、CUDA 或 ARM64。
