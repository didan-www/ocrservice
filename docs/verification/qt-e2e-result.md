# TASK-023 真实 Qt 客户端联调验证记录

## 验证范围

本记录对应 `docs/REQ-001-tasks.md` 中的 TASK-023。验证对象是实际 Windows Qt 客户端、隔离 Linux Docker Compose 服务端、MySQL 和 Mosquitto，不使用 Qt HTTP/MQTT mock 代替系统联调。

TASK-024 未在本次验证中执行。

## 被测版本

- 服务端基线提交：`1450f2c0c06225c003ad703f47ed324c71f98a35`。
- Qt 客户端提交：`60c3bb80350f83d3fc704454f3c48e5c46ff57b0`。
- TASK-023 服务端脚本和本记录属于基于上述服务端基线的待提交工作区内容。

## 验证环境

### Windows Qt 端

- Windows 11 Home China，64 位，版本 `10.0.26200`。
- Qt `6.11.1`。
- MinGW GCC `13.1.0`。
- CMake `3.30.5`。
- Ninja `1.12.1`。
- Qt MQTT 使用客户端仓库内已构建的运行库。

### Linux 服务端

- Ubuntu `22.04`，Linux `6.8.0-136-generic`，`x86_64`。
- Docker `29.1.3`。
- Docker Compose `2.40.3`。
- 服务端、MySQL 和 Mosquitto 使用 TASK-023 独立 Compose project、端口和持久化卷。
- 受保护的既有 MySQL 容器和宿主 Mosquitto 不属于 TASK-023 清理范围。

## 已验证命令

以下是实际通过的 Windows PowerShell 调用形式。尖括号内容必须由运行环境安全提供，不得把真实凭据写入命令记录或仓库。

```powershell
$env:QT_E2E_USERNAME = '<QT_LOGIN_USERNAME>'
$env:QT_E2E_PASSWORD = '<QT_LOGIN_PASSWORD>'

powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File '<SERVER_REPOSITORY>\tests\system\qt_e2e\run-qt-e2e.ps1' `
  -Executable '<QT_CLIENT_EXECUTABLE>' `
  -QtMqttBin '<QT_MQTT_RUNTIME_DIRECTORY>' `
  -QtBin '<QT_RUNTIME_DIRECTORY>' `
  -MingwBin '<MINGW_RUNTIME_DIRECTORY>' `
  -Slice Full
```

凭据只通过当前进程环境传入；HTTP Token、MQTT 密码和完整响应载荷不写入本记录。

## Full 验证结果

开发者 Full 和主 Agent独立 Full 均退出成功，以下 15 项结果全部为 `1`：

| 结果字段 | 验证内容 | 开发者 Full | 主 Agent独立 Full |
|---|---|---:|---:|
| `login` | 用户名密码登录 | 1 | 1 |
| `heartbeat` | HTTP 心跳在线 | 1 | 1 |
| `mqttSuback` | MQTT CONNECT 后收到 QoS 1 SUBACK | 1 | 1 |
| `processing` | 接收 `PROCESSING` 管理事件 | 1 | 1 |
| `final` | 接收最终 `SUCCEEDED` 事件 | 1 | 1 |
| `image` | 下载、解码并显示识别图片 | 1 | 1 |
| `history` | 历史查询返回当前识别记录 | 1 | 1 |
| `csv` | 原生保存对话框导出并严格校验 CSV | 1 | 1 |
| `accessCreate` | 创建白名单记录 | 1 | 1 |
| `accessConflict` | 跨名单重复返回稳定冲突 | 1 | 1 |
| `accessDelete` | 确认并删除名单记录 | 1 | 1 |
| `mqttTemporary` | Broker 暂时中断后恢复连接 | 1 | 1 |
| `mqttPermanentHttp` | MQTT 永久错误时 HTTP 仍在线 | 1 | 1 |
| `http401` | 旧会话收到严格 401 后返回登录 | 1 | 1 |
| `expiry` | 短有效期会话到期后返回登录 | 1 | 1 |

开发者最终 Full 的 CSV SHA-256：

```text
a021b3f5a10cde94e6bcce5a3f56d5444adf6cd3905f2a805c09af3d0f7a9b93
```

主 Agent独立 Full 的 CSV SHA-256：

```text
0828c3edb357ebad37fffcdf5a55966fcf9af6d0afab2fda2fcebf592c44e3a5
```

两个哈希来自两次独立运行生成的 CSV，运行时间等业务数据不同，因此不要求哈希相同；每次运行都独立通过下述 CSV 契约校验。

## 脱敏关键证据

- 图片独立探针返回 HTTP 200、`image/jpeg`，且下载内容与受控图片摘要匹配。
- 名单创建成功，跨名单重复返回 `409/ACCESS_LIST_CONFLICT_WHITE`，删除后数据库受控车牌状态为 `0:NONE`。
- Broker 凭据轮换前存在成功管理端连接；轮换后证据为 `mqttConnect=0`、`mqttAttempts=2`、`mqttAuthRejected=1`。
- MQTT 永久状态精确显示“未连接（配置或授权错误）”，同时 HTTP 状态精确显示“在线”。
- 同一 `clientId` 会话被替换后，Qt 通过真实 logout POST 触发旧 Token 校验；最终停止隔离应用并刷新日志后，存在精确 `POST:401:AUTH_TOKEN_INVALID` 证据。
- 短有效期验证先在同一 Qt ProcessId 下观察到主窗口，再观察到登录窗口；最终日志中成功 POST 计数相对到期前基线严格增加。
- 全部证据只保留方法、状态、稳定错误码和计数，不包含密码、Token、完整 MQTT Payload、完整请求体或完整响应体。

## CSV 契约

两次 Full 均验证：

- 文件以 UTF-8 BOM 开头。
- 表头严格为需求规定的 10 列，列名和顺序不变。
- 所有记录使用 CRLF，文件以 CRLF 结尾。
- 文件包含本次受控设备和识别记录。
- 本记录不保存完整 CSV 内容。

## 隔离和清理

开发者 Full 和主 Agent独立 Full 的最终清理结果均为：

```text
containers=0 volumes=0 networks=0 protected=true hostMosquitto=active
```

这表示 TASK-023 Compose project 的容器、卷和网络均已删除，受保护 MySQL 仍运行，宿主 Mosquitto 仍为 active。最终检查未发现残留 `plate_client` 或 TASK-023 runner 进程。

## 外部测试环境残留

早期缺少 Qt 运行库的启动探针在 Windows 桌面留下一个由 `csrss` 承载的 `plate_client.exe - 系统错误` 对话框。它属于外部测试环境残留，不属于当前 Qt 进程，也不是 Compose 资源。

验证脚本通过当前 `plate_client` ProcessId 和确认窗口双重约束完成 UI Automation。整个 TASK-023 验证过程未关闭、操作或终止该 `csrss` 对话框或进程。

## 已接受的 MQTT 限制

本项目没有应用级 MQTT Outbox 或业务重发。若服务端发布时 Broker 未接受消息，MQTT 结果可能永久丢失；数据库最终状态必须保持正确，服务不得崩溃。该限制仅适用于隔离教学环境，不得将当前实现描述为消息可靠必达。

TASK-023 验证了 Qt 管理端的暂时断线恢复，以及永久认证或授权错误不终止 HTTP 会话；上述已接受限制保持不变。
