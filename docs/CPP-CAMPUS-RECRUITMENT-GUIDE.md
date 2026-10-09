# C++ 校招简历与项目面试指导

| 属性 | 内容 |
|---|---|
| 适用对象 | 使用本项目学习 C++、Linux 服务端或 Qt 客户端开发的学生 |
| 目标岗位 | C++ 软件开发、Linux C++ 服务端、Qt 客户端、物联网/边缘计算方向校招岗位 |
| 项目基线 | REQ-001 车牌识别教学系统当前实现 |
| 配套文档 | `README.md`、`docs/REQ-001.md`、`docs/SYSTEM-ARCHITECTURE-AND-INTERFACES.md` |

> 本文用于指导学生提炼项目经历、准备故障分析和项目面试。简历只能写本人真正完成、理解并能现场解释的内容。本文中的空指针、死锁和性能案例是基于当前代码风险点设计的训练案例，不代表当前已验收版本仍存在这些缺陷。

## 1. 面试官真正关注什么

校招简历中的项目不是功能清单。写“实现登录、历史查询、黑白名单管理”只能说明做过页面或接口，不能充分说明 C++ 工程能力。面试官更关心以下问题：

- 为什么选择当前软件架构，模块边界如何划分？
- HTTP 线程、识别 worker、MQTT 线程和 Qt UI 线程如何协作？
- C++ 对象所有权、异步回调生命周期和异常安全如何保证？
- 队列、数据库事务、图片文件和 MQTT 发布如何保持业务一致性？
- 网络重复、超时、断线和服务重启时，系统状态如何收敛？
- 如何通过单元测试、组件测试、契约测试和系统测试证明实现正确？
- 出现崩溃、死锁、高 CPU 或高延迟时，能否用证据定位，而不是凭感觉修改？

因此，一条有质量的项目描述通常包含四部分：

```text
业务目标 + 架构方案 + 自己负责的关键技术问题 + 可验证结果
```

## 2. 项目事实速览

写简历前应先能准确说明当前项目：

| 维度 | 当前项目事实 |
|---|---|
| 系统形态 | 嵌入式设备、Linux 服务端、Qt 管理客户端三端系统 |
| 服务端架构 | C++17 单进程模块化单体，Controller/Service/Domain Port/Adapter 分层 |
| 客户端架构 | Qt 6 Widgets 单进程事件驱动客户端，UI、Service、Infrastructure、Domain 分层 |
| 异步模型 | 4 个 HTTP worker、默认 1 个识别 worker、有界队列容量 20、MQTT 后台控制线程 |
| 模型 | OpenCV 预处理，ONNX Runtime CPU 执行 YOLOv8 和 LPRNet |
| 数据 | MySQL 8 保存管理员、设备、识别历史和名单；图片原子落盘 |
| 通信 | Crow HTTP、Mosquitto MQTT 3.1.1、QoS 1、严格 JSON DTO |
| 客户端 | Qt Widgets、Network、Concurrent、MQTT、Qt Test |
| 部署 | linux/amd64 Docker 应用镜像，Compose 独立编排 app、MySQL、Mosquitto |
| 工程验证 | 单元、组件、契约、集成、嵌入式模拟、真实 Qt 和空卷 Compose 验收 |

当前验收报告记录了 Debug/Release 服务端 CTest 各 `30/30`、25 项业务验收、1 Hz 连续 30 张上传和真实 Qt/嵌入式联调等结果。学生只有在自己的环境亲自执行并保留证据后，才能把这些数据写成个人成果。

## 3. 简历项目描述参考

### 3.1 项目名称建议

项目名称应体现技术属性和系统范围，不建议只写“车牌识别系统”。可以从以下名称中选择：

- `基于 C++17 的三端异步车牌识别系统`
- `Linux C++ 车牌识别服务与 Qt 管理客户端`
- `基于 ONNX Runtime 与 MQTT 的边缘车牌识别平台`
- `C++ 模块化车牌识别服务及实时管理客户端`

如果主要应聘 Linux C++ 服务端，优先突出“Linux C++ 服务”和“异步”；如果主要应聘 Qt 岗位，优先突出“Qt 管理客户端”和“实时通信”。

### 3.2 技术栈写法

推荐按层次写，不要把所有库名堆在一行：

```text
技术栈：C++17、CMake/Ninja、GCC、Linux；Crow、OpenCV、ONNX Runtime、
YOLOv8、LPRNet；MySQL 8、Mosquitto、Paho MQTT；Qt 6 Widgets/Network/
Concurrent/MQTT/Test；Docker、Docker Compose、CTest。
```

简历空间较小时，可以根据目标岗位裁剪：

```text
Linux 服务端方向：C++17、Linux、Crow、OpenCV、ONNX Runtime、MySQL、
Paho MQTT、CMake、Docker、CTest。

Qt 客户端方向：C++17、Qt 6 Widgets/Network/Concurrent/MQTT/Test、
CMake、异步 HTTP、MQTT 3.1.1、QAbstractTableModel、QSaveFile。
```

不要写未使用的 Redis、gRPC、Kafka、CUDA、微服务或 Kubernetes。当前服务端是模块化单体，不是微服务。

### 3.3 综合方向简历参考

```text
项目名称：基于 C++17 的三端异步车牌识别系统

技术栈：C++17、Linux、Qt 6、Crow、OpenCV、ONNX Runtime、YOLOv8、
LPRNet、MySQL 8、Mosquitto/Paho MQTT、CMake/Ninja、Docker Compose、CTest。

项目描述：面向 1～10 台嵌入式设备的教学型车牌识别平台。嵌入式端通过
HTTP 上传图片并订阅设备结果，Linux 服务端异步完成图片存储、模型推理和
MySQL 状态持久化，Qt 管理端通过 HTTP 与 MQTT 提供实时监控、历史查询和
名单管理。服务端采用单进程模块化单体，客户端采用 Qt 事件驱动分层架构。

个人工作/技术亮点：
- 按 Controller、Application Service、Domain Port 和 Adapter 划分服务端模块，
  将 HTTP 协议、业务事务、MySQL、模型和 MQTT 解耦，避免 Controller 直接执行
  SQL 或模型推理。
- 设计“有界队列 + 独立识别 worker”异步流水线，先预留容量再保存图片和提交
  PROCESSING 记录，通过 RAII 管理队列槽位、图片补偿和 worker start gate。
- 实现基于 `(deviceId, captureId)` 的上传幂等校验，区分同内容重试与冲突，
  处理图片文件、MySQL 事务和队列提交之间的一致性。
- 建立 HTTP 五字段信封、严格 DTO、状态可空组合、ISO 8601 时间和 MQTT 快照
  契约；通过契约测试防止服务端与 Qt 客户端字段漂移。
- 使用 Docker Compose 独立编排应用、MySQL 和 Mosquitto，完成持久化、健康检查、
  Broker 断线降级、服务重启恢复和三端联调。
```

上面五条不应全部照抄。学生应选择自己实际完成且能深入回答的 3～4 条。

### 3.4 Linux C++ 服务端方向参考

```text
项目名称：Linux C++ 异步车牌识别服务

技术栈：C++17、Linux、Crow、OpenCV、ONNX Runtime、MySQL Connector/C++、
Paho MQTT、CMake/Ninja、Docker Compose、GoogleTest/CTest。

项目描述：开发单进程模块化车牌识别服务，接收设备 multipart 图片上传，
通过有界队列将 HTTP 受理与 YOLOv8/LPRNet 推理解耦，使用 MySQL 保存识别状态，
并通过 MQTT 向 Qt 管理端和设备发布不同业务快照。

个人工作/技术亮点：
- 设计容量 20 的可预留有界队列和默认单识别 worker，使 HTTP 线程不执行模型
  推理；通过条件变量、移动语义和 RAII reservation 处理并发受理和优雅停止。
- 使用参数化 SQL、连接池和事务实现识别状态机；历史分页采用稳定排序，CSV 使用
  一致性快照和 keyset cursor 流式输出，避免全量结果占用内存。
- 将图片以临时文件、fsync、原子重命名方式落盘；数据库失败时执行文件补偿，
  队列满时保证不产生图片和业务记录。
- 实现 MQTT 后台重连和 QoS 1 发布，明确数据库提交与消息副作用边界；Broker
  不可用时服务保持 HTTP 降级运行，且不回滚最终业务状态。
```

### 3.5 Qt C++ 客户端方向参考

```text
项目名称：Qt 6 车牌识别实时管理客户端

技术栈：C++17、Qt 6 Widgets、Network、Concurrent、MQTT、Qt Test、
CMake/Ninja、HTTP、MQTT 3.1.1。

项目描述：开发 Windows Qt 管理客户端，通过异步 HTTP 完成登录、历史、图片、
CSV 和名单操作，通过 MQTT 接收实时识别快照；客户端不直连 MySQL、不调用模型，
采用 UI、Service、Infrastructure 和 Domain 分层。

个人工作/技术亮点：
- 使用 `sessionGeneration + requestGeneration + recognitionId` 组合校验异步回调，
  防止旧登录、旧查询和旧图片覆盖当前界面。
- 使用 `QPointer`、QObject 上下文连接和统一取消顺序管理 `QNetworkReply`、
  `QFutureWatcher` 与窗口生命周期，处理退出时仍有网络和解码回调的场景。
- 将图片字节在线程池中预检并解码为 `QImage`，回到 UI 线程后再创建 `QPixmap`，
  保持界面响应并遵守 Qt GUI 对象线程约束。
- 通过 `QSaveFile` 流式保存服务端 CSV，禁止 direct-write fallback，确保下载失败
  不覆盖已有文件；使用严格 JSON Codec 拒绝未知字段和非法状态组合。
```

### 3.6 简历量化方式

可以量化，但不能编造。推荐用以下模板：

| 不够具体 | 更好的写法 |
|---|---|
| 优化系统性能 | 在亲自复现的 1 Hz 连续上传场景下，使用 `perf` 定位重复模型初始化热点，将 ONNX Session 改为 worker 级复用；记录修改前后 CPU、P95 和吞吐数据 |
| 编写大量测试 | 构建单元、组件、契约和系统测试；在自己的环境完成 Debug/Release 全量测试，并写明实际通过数量 |
| 解决网络问题 | 通过幂等键处理 HTTP 未知结果重试，通过 QoS 1 消息键去重，并验证 Broker 离线时数据库最终状态不回滚 |
| 实现高并发 | 面向教学规模 1～10 台设备和总峰值 1 张/秒，使用有界队列提供明确过载返回；不要把该规模描述为“百万并发” |

可记录的真实数据包括：

- 压测持续时间、请求数量、成功率、P50/P95/P99。
- CPU、RSS、队列深度、模型单次耗时。
- Debug/Release 测试用例数量和通过结果。
- 崩溃复现概率、修复后的压力轮数。

量化结果必须能说明测试环境、输入和测量口径。

### 3.7 简历常见错误

- 把团队或教学项目全部写成“独立设计并实现”。
- 只列业务页面，不写架构、并发、协议或工程验证。
- 写“保证 MQTT 消息绝不丢失”。当前系统没有 Outbox，不能这样描述。
- 把 QoS 1 说成“恰好一次”。QoS 1 是至少一次，设备必须去重。
- 把 Docker Compose 三容器说成“微服务架构”。
- 写“支持高并发”，但说不清并发模型、队列容量、压测数据和瓶颈。
- 写“使用 AI”，但说不清模型输入输出、预处理、NMS 和 CTC。
- 写了 `perf`、GDB、ASan、TSan，却不能解释用过的命令和证据。

## 4. 通用故障排查方法

遇到问题时，不要从“修改代码”开始。推荐固定使用以下流程：

```text
定义现象
  -> 建立稳定复现
  -> 缩小故障边界
  -> 收集线程、栈、日志、指标和系统调用证据
  -> 提出可证伪假设
  -> 最小修改
  -> 针对性回归
  -> 全量回归和压力复验
  -> 记录根因与预防措施
```

### 4.1 先定义现象

至少回答：

- 是崩溃、卡死、超时、数据错误还是性能退化？
- 只发生在 Debug、Release、Windows、Linux、容器还是所有环境？
- 与上传、模型、MySQL、MQTT、退出或重连中的哪个阶段相关？
- 是否与请求量、图片大小、网络断开或操作顺序相关？
- 最近一次正常提交和第一次异常提交分别是什么？

### 4.2 收集证据而不是猜测

| 问题类型 | 优先证据 |
|---|---|
| 崩溃 | core dump、异常地址、崩溃线程栈、ASan 报告 |
| 卡死/死锁 | 全线程栈、`futex` 等待、锁持有关系、停止顺序 |
| 高 CPU | `top -H`、`pidstat`、`perf stat`、on-CPU 火焰图 |
| 低 CPU 但慢 | 队列深度、数据库等待、网络时延、`strace -T`、off-CPU 分析 |
| 数据不一致 | `requestId`、`recognitionId`、数据库记录、文件、MQTT 时序 |
| Qt 界面异常 | UI 线程栈、事件循环、对象生命周期、代次和活动请求句柄 |

### 4.3 修复后的最低验证

- 为根因增加可稳定失败、修复后稳定通过的测试。
- 执行相关单元/组件测试。
- 执行全部累计回归测试。
- 对并发问题重复执行或压力执行，而不是只运行一次。
- 对性能问题使用完全相同的构建、数据、负载和测量窗口做 A/B 对比。

### 4.4 当前项目常见问题清单

| 问题 | 可能现象 | 当前项目可能位置 | 主要排查手段 |
|---|---|---|---|
| 空指针/悬空指针 | Qt 偶发崩溃、退出时 UAF | `HttpClient`、`ImageLoadService`、窗口异步回调 | GDB、ASan、QObject 所有权和回调上下文 |
| 死锁/永久等待 | 容器停止不退出、worker 不再消费 | 队列 gate、受理 in-flight、MQTT 多锁、连接池等待 | 全线程栈、futex、锁顺序、条件 predicate |
| 数据竞争/旧响应 | 旧图片覆盖新车辆、退出后 UI 又更新 | Qt 网络回调、线程池解码、MQTT 重连校准 | session/request generation、TSan、延迟乱序测试 |
| 协议序列化错误 | 服务端稳定返回 400，但 UI 输入看似正确 | JSON Codec、`QUrlQuery`、multipart 构造 | 原始请求目标、严格契约夹具、服务端稳定错误码 |
| 资源泄漏 | RSS/句柄/连接持续增长 | reply/watcher、MySQL lease、线程、临时文件 | 重复压力、进程指标、对象计数、退出清理检查 |
| 数据不一致 | 图片存在但无记录，或记录永久 PROCESSING | 上传受理、图片补偿、队列提交、启动恢复 | 按 requestId/recognitionId 对齐日志、文件、数据库 |
| CPU/延迟退化 | queueDepth 增长、P95 上升 | 图片解码、模型生命周期、MySQL、磁盘 | 分段时间、top/pidstat、perf、iostat、慢 SQL |

一个真实的协议类问题是“空名单关键字”的 URL 编码：Qt 的 null `QString` 曾被序列化为裸 `keyword`，而严格服务端要求 `keyword=`，导致名单创建成功后自动刷新返回 400。排查关键不是放宽服务端，而是对比原始 URL、POST 成功证据和后续 GET 错误，最终只修正客户端空值编码并增加 raw-target 回归测试。这个案例适合说明严格契约如何帮助定位跨端问题。

## 5. 空指针与悬空指针案例

### 5.1 当前项目中的高风险位置

| 位置 | 风险来源 | 当前防护思路 |
|---|---|---|
| Qt `HttpClient` | `QNetworkReply` 通过 `deleteLater()` 异步销毁 | `QPointer`、QObject 上下文连接、完成状态保护 |
| Qt `ImageLoadService` | 图片请求完成时页面或 Service 已退出 | 活动句柄、会话/请求代次、取消和 `QPointer` |
| Qt `QFutureWatcher` | 线程池解码完成晚于窗口销毁 | watcher 以 Service 为 parent，回调先校验上下文 |
| 服务端 worker 线程 | `ProductionApplication` 销毁 worker 对象早于线程 join | 固定停止顺序，先 join 线程再销毁所有者 |
| MySQL `Lease` | 移动后或 discard 后连接为空 | `operator bool()` 和 `connection()` 内部检查 |
| 队列任务 | 对 `optional task` 或 start gate 的不变量被破坏 | reservation/commit/start gate RAII 和测试 |

空指针和悬空指针不是一回事：空指针通常稳定为 `nullptr`；悬空指针仍保存旧地址，偶尔正常、偶尔崩溃，更难定位。

### 5.2 训练案例：退出时 Qt 图片回调崩溃

#### 现象

用户在实时页面加载大图片时快速点击退出或关闭窗口。程序偶发崩溃，普通登录和查询都正常；图片越大、网络越慢越容易复现。崩溃位置可能出现在：

```text
ImageLoadService 的 finished lambda
RealtimePage 更新 QLabel/QPixmap
HttpClient 读取已经销毁的 QNetworkReply
```

#### 建立复现

1. 使用可控 HTTP 服务延迟图片响应。
2. 连续执行“进入实时页 -> 收到 PROCESSING -> 立即关闭窗口”。
3. 在 Qt Test 中循环该场景，并在回调到达前销毁页面或会话。
4. Debug 构建开启符号；条件允许时构建 AddressSanitizer 诊断版本。

诊断构建参数示例只用于本地实验，需根据 Qt/MinGW 环境调整：

```text
-O1 -g -fno-omit-frame-pointer -fsanitize=address
```

#### 收集证据

- 在 Qt Creator/GDB 中保留崩溃现场，查看崩溃线程和所有 lambda 捕获对象。
- 检查 `this`、页面指针、`QNetworkReply` 和请求 handle 是否已经销毁。
- 查看 QObject parent 关系和 `deleteLater()` 发生的事件循环时机。
- 对照 `sessionGeneration`、`requestGeneration` 和当前 `recognitionId`，判断是否为旧回调。
- 如果 ASan 报告 `heap-use-after-free`，沿 allocation/free/use 三个栈确认对象所有权。

#### 可能根因

假设某次重构把连接写成：

```cpp
connect(reply, &QNetworkReply::finished, [this, page, reply] {
    page->showImage(reply->readAll());
});
```

这个 lambda 没有 QObject 上下文，也捕获了无生命周期保证的裸指针。窗口退出后 `page` 已释放，reply 也可能被 `deleteLater()`，晚到回调产生悬空访问。

#### 修复原则

- 为 `connect` 提供明确 QObject 上下文，使接收对象销毁后连接自动断开。
- 对可能独立销毁的 QObject 使用 `QPointer`，解引用前检查是否为空。
- 页面不直接持有底层 reply；由 Service 统一拥有和取消请求。
- 开始退出时先递增会话代次，使所有旧回调在取消前失效。
- 回调同时核对活动 handle、会话代次、请求代次和业务 ID。
- 后台只生成 `QImage`，回到 UI 线程且页面仍有效时才创建 `QPixmap`。

#### 验证

- 新增“延迟图片 + 立即退出”Qt Test。
- 重复执行至少数十轮并检查无崩溃、无旧图片更新。
- 再运行登录、实时、历史、CSV、名单和退出全量测试。
- 使用 ASan 诊断版本确认不再报告 UAF；正常 Debug/Release 构建也必须通过。

#### 面试表达参考

```text
我遇到过退出时偶发崩溃。先通过延迟图片响应和快速关闭稳定复现，GDB/ASan
定位到异步 lambda 捕获的 QWidget 裸指针已经释放。修复时没有简单增加判空，
而是重新明确对象所有权：使用 QObject 上下文连接和 QPointer，并在统一退出流程
开始时让 sessionGeneration 失效，再通过活动请求代次拒绝晚到回调。最后增加了
延迟响应退出测试和重复压力回归。
```

### 5.3 训练案例：服务端队列任务空状态

`RecognitionTaskQueue::take()` 最终会取出已提交任务。如果一次错误重构先把 pending 节点放进可消费队列，再构造 `task`，worker 可能看到 gate 已打开但 `optional task` 仍为空。

排查过程：

1. 现象是 HTTP 已返回 202 后 worker 崩溃，崩溃栈集中在队列 `take()`。
2. 用 core dump 和 GDB 检查队首节点的 `task.has_value()`、gate 和队列状态。
3. 在队列单元测试中插入并发屏障，放大“节点可见早于任务构造”的窗口。
4. 证明根因是发布顺序破坏，不是简单随机空指针。
5. 修复为先在私有 pending 节点中无异常移动构造 task，再持锁 splice 到共享队列；通过 start gate 控制 worker 真正开始。
6. 使用 reservation 析构自动释放容量，gate 析构自动打开，防止异常路径遗留永久等待。

这种问题的关键不是在 `take()` 中随手加一个 `if`。如果静默跳过空任务，数据库可能永久停留在 `PROCESSING`。正确修复必须恢复队列、数据库和任务受理的整体不变量。

## 6. 死锁与永久等待案例

### 6.1 先区分三种“卡住”

| 类型 | 特征 | 本项目可能位置 |
|---|---|---|
| 真死锁 | 多个线程形成锁等待环，永远无法前进 | MQTT 多锁顺序、未来错误的跨模块嵌套加锁 |
| 条件等待未唤醒 | 线程在条件变量等待，但状态/通知遗漏 | 队列 start gate、受理停止等待 `inFlight=0` |
| 正常阻塞或外部慢 | 没有锁环，线程等待 MySQL、网络、模型或 join | 连接池 acquire、模型 `Session::Run`、优雅停止 |

不要看到 `futex_wait` 就直接结论为死锁。条件变量、线程 join 和连接池等待都会表现为 futex。

### 6.2 训练案例：服务停止时一直等待

#### 现象

发送 `SIGTERM` 后容器不退出，HTTP 不再接受新请求，但进程长时间存在。线程栈可能显示：

```text
主线程：RecognitionAcceptanceService::stopAcceptingAndWait -> condition_variable::wait
某 HTTP 线程：正在 acceptUpload 或等待下游资源
worker：RecognitionTaskQueue::take -> condition_variable::wait
```

#### 排查步骤

1. 记录停止开始时间、最后一个 `requestId` 和是否仍有在途上传。
2. 使用 `top -H -p <pid>` 判断线程是否消耗 CPU。
3. 使用 GDB：

```bash
sudo gdb -p <pid>
(gdb) set pagination off
(gdb) info threads
(gdb) thread apply all bt
```

4. 或使用只读线程栈工具：

```bash
sudo pstack <pid>
sudo strace -f -ttT -p <pid>
```

5. 检查 `inFlight_` 是否归零、哪条请求持有 `InFlightGuard`、是否有异常路径未调用 `leave()`。
6. 检查停止顺序是否被改变：必须先 `stopAcceptingAndWait()`，再停止 HTTP，再停止队列和 join worker。

#### 假设根因一：丢失 RAII guard

如果开发者把 `InFlightGuard` 改成手工 `++inFlight_`/`--inFlight_`，某个图片保存或 Repository 异常提前 return，计数永远不减。主线程的 predicate `inFlight_ == 0` 永不成立。

修复：恢复作用域 RAII guard；所有 return 和异常都由析构统一 `leave()`，随后 `notify_all()`。增加每个失败分支与并发停止测试。

#### 假设根因二：start gate 未打开

受理流程提交队列后，会先尽力发布 `PROCESSING`，再打开 start gate。如果 MQTT 发布抛异常且异常分支遗漏 `gate.open()`，worker 会一直等待队首 gate，后续任务也无法越过队首。

当前实现通过 `TaskStartGate` 析构时 `openNoThrow()` 兜底。修复测试应模拟 MQTT 拒绝和抛异常，验证 worker 仍处理任务。

#### 假设根因三：锁顺序反转

`PahoMqttPublisher` 有 lifecycle、connection 和 publish 三类 mutex。如果线程 A 持 lifecycle mutex 等 connection mutex，线程 B 在回调中持 connection mutex 再等待 lifecycle mutex，就会形成环。

修复原则：

- 明确全局锁顺序，禁止反向获取。
- 持锁区域只修改内存状态，不调用第三方库、日志回调或可能重入的 observer。
- `join()` 必须在释放 lifecycle mutex 后执行，否则被 join 线程无法取得锁完成退出。
- 能使用原子状态的简单读写不要扩大为嵌套锁。

### 6.3 如何证明修复有效

- 为每个异常返回分支增加停止并发测试。
- 使用带超时的测试检测永久等待，但不能只通过“增大超时”掩盖问题。
- 重复执行停止、MQTT 断线和队列满载场景。
- 可使用 ThreadSanitizer 诊断数据竞争，但 TSan 不会自动证明所有逻辑死锁已消失。
- 对锁顺序问题保留修复前后的全线程栈和锁依赖图。

### 6.4 面试表达参考

```text
程序停止卡住时，我先区分是 CPU 忙循环、外部 I/O 慢还是锁等待。全线程栈显示
主线程在等待 inFlight 归零，而某异常分支提前返回后没有减少计数。根因是把 RAII
guard 改成了手工计数。修复后所有路径由析构归还计数并通知条件变量，同时增加
Repository/存储/MQTT 异常与 SIGTERM 并发测试。这个问题不能靠增加停止超时解决，
因为那会留下不确定的业务状态。
```

## 7. CPU 高与业务执行慢的排查

### 7.1 CPU 高和业务慢不是同一个问题

| CPU | 业务延迟 | 常见原因 |
|---|---|---|
| 高 | 高 | 模型重复初始化、图像处理热点、忙循环、过度序列化 |
| 高 | 正常 | 正常模型推理占满一个 worker 核心，吞吐仍满足目标 |
| 低 | 高 | MySQL/网络/磁盘等待、队列等待、锁竞争、线程池饥饿 |
| 低 | 正常 | 系统空闲或异步等待 |

第一步应先回答“慢在哪一段”，而不是直接打开火焰图。

### 7.2 利用当前业务字段分段

当前系统可以使用以下证据：

- HTTP 访问日志中的请求 `durationMs`：受理或查询接口耗时。
- `/health` 中的 `queueDepth/queueCapacity`：识别队列是否积压。
- 快照 `startedAt/completedAt/durationMs`：服务停留和 worker 执行时间。
- MySQL 最终状态和错误码：判断是否只是 MQTT/UI 未更新。
- MQTT 连接状态：区分业务完成与通知丢失。

可近似拆分：

```text
端到端耗时       = completedAt - capturedAt
服务端停留时间   = completedAt - startedAt
worker 处理耗时  = durationMs
其余时间         ≈ 受理落盘 + 排队 + 最终事务
```

“其余时间”只是近似，因为 `durationMs` 不包含最终数据库事务和 MQTT 发布，不能直接当作精确队列时间。

### 7.3 从系统到线程的排查顺序

```bash
# 1. 观察进程与线程
top -H -p <pid>
pidstat -p <pid> -t 1

# 2. 观察整体 CPU、调度和缺页
sudo perf stat -p <pid> -e cycles,instructions,cache-misses,context-switches,cpu-migrations,page-faults -- sleep 30

# 3. 低 CPU 但慢时观察系统调用耗时
sudo strace -f -ttT -p <pid>

# 4. 查看全线程栈
sudo gdb -batch -ex 'set pagination off' -ex 'thread apply all bt' -p <pid>
```

这些是教学诊断命令，不是仓库已验证的部署命令。执行前应在自己的实验环境确认权限和工具版本，不要在生产系统随意附加调试器。

### 7.4 逐模块检查

| 模块 | CPU 高时看什么 | 延迟高时看什么 |
|---|---|---|
| HTTP/Crow | JSON/multipart 是否重复扫描，是否有忙循环 | 请求体读取、鉴权 Repository、响应流写入 |
| 图片存储 | SHA/解码是否重复过多 | fsync、磁盘空间、卷性能、文件锁 |
| 队列 | 自旋、频繁 notify、锁竞争 | queueDepth 是否持续增长，worker 是否被 gate 阻塞 |
| 模型 | `Ort::Session::Run`、OpenCV resize/letterbox、NMS/CTC | 模型是否每请求重载，输入尺寸和 worker 数量 |
| MySQL | 行映射或序列化 | 连接池租约、锁等待、慢 SQL、磁盘 I/O |
| MQTT | Payload 序列化或重连忙循环 | Broker 连接、DNS、网络和 publish 接受状态 |
| Qt | UI 线程是否做图片解码/大循环 | 网络超时、线程池饥饿、旧请求未取消 |

### 7.5 MySQL 与磁盘检查

业务慢但 CPU 不高时，应进一步查看：

```sql
SHOW FULL PROCESSLIST;
SHOW ENGINE INNODB STATUS;
```

重点关注连接池是否耗尽、事务是否长期不提交、是否出现锁等待，以及历史/名单查询是否使用预期索引。不要在日志或命令输出中泄漏数据库密码。

磁盘侧可以观察：

```bash
iostat -xz 1
pidstat -d -p <pid> 1
```

如果 `await` 高、磁盘利用率接近饱和，并且 HTTP 上传耗时集中在图片落盘或 fsync，火焰图不会给出完整答案，因为线程大部分时间处于 off-CPU 等待。

## 8. `perf` 火焰图案例

### 8.1 案例假设

假设一次重构把 `OnnxPlateRecognizer` 从“每个 worker 启动时构造一次”移动到“每处理一张图片构造一次”。现象如下：

- 1 Hz 上传时单个 CPU 核心长期接近 100%。
- `queueDepth` 逐步增加。
- 单张 `durationMs` 明显增大。
- 功能结果仍正确，因此普通功能测试不会失败。

当前正确实现是在 `ProductionApplication` 中为每个 worker 构造一个 recognizer，每个 recognizer 常驻一对 YOLOv8/LPRNet ONNX Session。下面的案例用于演示如何发现这种性能回归。

### 8.2 建立可比较的实验

固定以下变量：

- 同一台 Linux amd64 虚拟机和 CPU 配额。
- 同一 Release 或 RelWithDebInfo 优化级别。
- 同一组图片、相同上传频率和持续时间。
- `RECOGNITION_WORKERS=1`、固定队列容量。
- MySQL、MQTT 和模型文件相同。

建议生成带符号并保留 frame pointer 的性能诊断构建：

```bash
cmake -S . -B build-perf -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO='-O2 -g -fno-omit-frame-pointer' \
  <本机其余依赖参数>
cmake --build build-perf --target ocrservice --parallel
```

该命令是分析模板，`<本机其余依赖参数>` 必须按当前项目 README 中的依赖配置补齐。不能把这个模板写成“已经验证成功”的构建命令。

### 8.3 初步观察

```bash
top -H -p <pid>
pidstat -p <pid> -t 1
sudo perf stat -p <pid> -- sleep 30
```

如果高 CPU 集中在唯一识别 worker，而 HTTP、MQTT 和主线程大多休眠，分析范围可以缩小到图片解码、预处理、模型和后处理。

### 8.4 采集 on-CPU 火焰图

Ubuntu 主机需要安装与内核匹配的 `perf`。Brendan Gregg FlameGraph 脚本不是本仓库依赖，应由学生在自己的实验目录从可信来源准备。

原生进程采集示例：

```bash
sudo perf record -F 99 -g --call-graph dwarf -p <pid> -- sleep 60
sudo perf script > perf.script
<FlameGraph目录>/stackcollapse-perf.pl perf.script > perf.folded
<FlameGraph目录>/flamegraph.pl \
  --title 'ocrservice recognition worker on-CPU' \
  perf.folded > ocrservice-cpu.svg
```

Compose 容器可以从宿主机取得应用容器的宿主 PID：

```bash
APP_PID=$(docker inspect --format '{{.State.Pid}}' ocrservice-app)
sudo perf record -F 99 -g --call-graph dwarf -p "$APP_PID" -- sleep 60
```

容器内必须运行对应的带符号 profiling 二进制或 profiling 镜像，否则宿主采样可能只能看到有限符号。容器 profiling 还可能受 `perf_event_paranoid`、符号文件和容器安全配置影响。应在隔离教学环境中调整最小必要权限，分析结束后恢复；不要为了方便长期给业务容器增加特权。

### 8.5 如何阅读火焰图

- 横向宽度表示采样占比，不表示时间先后。
- 纵向表示调用栈深度。
- 最宽的“平台”是主要 on-CPU 路径。
- 先看业务入口，再看库函数；不要只因为某个函数在顶层就断定它有问题。

正常识别负载可能主要出现：

```text
RecognitionWorker::process
  -> ImageDecoder/OpenCV imdecode
  -> OnnxPlateRecognizer::recognize
     -> letterbox / cv::resize / 数据布局转换
     -> Ort::Session::Run                 正常主要推理热点
     -> NMS / CTC decode
```

重复初始化回归可能出现异常宽栈：

```text
RecognitionWorker::process
  -> OnnxPlateRecognizer::OnnxPlateRecognizer
     -> 文件 SHA-256
     -> Ort::Session::Session
     -> ONNX 图加载/优化/内存分配
```

如果 `Ort::Session` 构造、模型文件读取和图初始化在每张图路径中占据大量采样，就能证明问题不是“模型推理本来就慢”，而是对象生命周期错误。

### 8.6 根因与修改

根因：重构破坏了“每个 worker 独占并常驻一对 Session”的设计，把昂贵资源变成了每请求资源。

修改：

- 在应用组合阶段为每个 worker 构造一个 `OnnxPlateRecognizer`。
- worker 长期持有 recognizer 引用或明确所有权。
- 每个 worker 独占 Session，避免在多个 worker 间共享可变输入缓冲。
- 单次任务只创建必要的输入/输出对象，不重新读取和校验模型文件。

### 8.7 A/B 验证

使用与修改前完全相同的负载重新采集：

- CPU 利用率。
- `perf stat` 的 cycles、instructions、cache-misses。
- `durationMs` 的 P50/P95/P99。
- 队列最大深度和是否持续增长。
- 处理完成数量、成功率和结果一致性。
- 修改后的火焰图。

验收目标不是“火焰图中没有 ONNX”。`Ort::Session::Run` 仍可能是最大热点，这是正常业务成本。目标是消除重复 Session 构造和模型文件加载，并证明吞吐/延迟改善且识别结果没有回归。

### 8.8 如果火焰图显示别的热点

| 热点 | 可能原因 | 下一步 |
|---|---|---|
| `cv::imdecode` | 输入图片过大或解码成本高 | 固定输入比较；检查设备编码分辨率，不能绕过安全解码校验 |
| `cv::resize/copyMakeBorder` | letterbox 预处理成本 | 检查是否重复执行、是否存在多余 Mat 拷贝 |
| `memcpy`/vector 扩容 | NCHW 转换频繁分配 | 检查 worker 级缓冲复用，但要保持线程隔离 |
| NMS | 候选框过多或实现复杂度异常 | 检查阈值、排序和候选数量 |
| JSON/CSV | 过度聚合或重复序列化 | 检查流式路径和响应大小 |
| mutex/futex 很宽 | 锁竞争或等待 | 转向全线程栈、`perf lock` 或 off-CPU 分析 |

## 9. 面试官项目问题汇总

以下问题按校招面试常见顺序组织。学生不应背固定句子，而应能结合具体类、状态和测试说明。

### 9.1 项目概述与架构

| 问题 | 回答要点 |
|---|---|
| 1. 用两分钟介绍项目。 | 三端职责、服务端异步识别、Qt 管理端、HTTP/MQTT 分工、本人负责内容和一个可验证结果 |
| 2. 为什么服务端选择模块化单体而不是微服务？ | 教学规模小、部署简单、事务和调试成本低；通过模块边界保留可测试性，不为分布式而分布式 |
| 3. 服务端各层如何依赖？ | Controller 处理协议，Service 组织业务，Domain 定义稳定类型/Port，Repository/Model/Storage/MQTT 是 Adapter |
| 4. 为什么 Controller 不能直接执行 SQL 或模型？ | 降低协议与业务耦合、便于事务复用和单元测试、防止 HTTP 线程被推理阻塞 |
| 5. Qt 客户端为什么也要分层？ | UI 不解析协议；Service 管状态；Infrastructure 处理网络；Domain DTO 可独立测试 |
| 6. HTTP 和 MQTT 为什么同时使用？ | HTTP 适合请求响应、查询和写操作；MQTT 适合实时事件。MySQL/HTTP 是历史事实，MQTT 不是 |
| 7. 项目中最重要的架构决策是什么？ | 可选有界队列解耦、严格契约、数据库提交与 MQTT 副作用分离、异步代次防旧响应；必须说明取舍 |
| 8. 如果设备数量扩大 100 倍，先改什么？ | 先测量；模型吞吐、队列、连接池、Broker 和存储会成为瓶颈。不能直接宣称加线程；需容量规划和可能的任务/推理拆分 |

### 9.2 C++ 对象模型与异常安全

| 问题 | 回答要点 |
|---|---|
| 9. 项目中如何使用 RAII？ | 队列 reservation/start gate、图片失败补偿、MySQL lease、线程/对象所有权、Qt QObject parent |
| 10. `unique_ptr`、`shared_ptr` 和引用如何选择？ | 组合根独占用 `unique_ptr`；跨生命周期共享状态/日志用 `shared_ptr`；依赖存在期由组合根保证时用引用 |
| 11. 为什么 QueueReservation 禁止复制？ | 一个槽位只能有一个归还责任；复制会重复 commit/cancel，移动语义转移责任 |
| 12. `optional` 和 `variant` 在项目中解决什么？ | optional 表达确实可能无值；variant 表达成功/明确失败分类，避免异常承担普通业务分支 |
| 13. 析构函数为什么不能抛异常？ | 栈展开时再次抛异常会 terminate；清理使用 no-throw/best-effort 并通过日志记录 |
| 14. 如何避免悬空指针？ | 明确所有权、join 后销毁、Qt QObject 上下文、QPointer、取消和代次校验，不只靠判空 |
| 15. 移动后对象应满足什么状态？ | 可析构、可赋值、责任已转移；如 InFlightGuard 将 owner 置空，Lease 转移连接 |
| 16. 为什么不能在所有地方都用 `shared_ptr`？ | 所有权模糊、环引用、额外同步成本，掩盖销毁顺序；应优先单一所有权 |

### 9.3 并发、队列和停止

| 问题 | 回答要点 |
|---|---|
| 17. HTTP 线程为什么不执行模型？ | 推理耗时且抖动大，会占用 HTTP worker；队列让受理快速返回并提供背压 |
| 18. 为什么队列要有界？ | 限制内存和等待时间，过载时明确 503；无界队列只会把过载变成延迟和 OOM |
| 19. 为什么先预留队列槽位再保存图片？ | 避免创建图片/数据库记录后发现无法入队，保证已受理任务可执行 |
| 20. start gate 解决什么竞态？ | 任务提交后先完成 PROCESSING 发布尝试，再允许 worker 开始，避免最终处理越过该发布尝试；它不承诺最终 MQTT 一定晚于 HTTP 响应字节到达设备 |
| 21. 条件变量为什么必须带 predicate？ | 防止虚假唤醒和丢失状态；等待的是真实状态条件，不是某次通知本身 |
| 22. 如何排查死锁？ | 稳定复现、全线程栈、锁等待环、futex 区分、检查锁顺序和持锁外部调用 |
| 23. 为什么 join 要在释放 mutex 后？ | 被 join 线程可能需要同一 mutex 更新退出状态，否则自造死锁 |
| 24. 服务端停止顺序为何严格？ | 先停止受理并等 in-flight，再停 HTTP、队列、worker、MQTT、连接池，防止依赖先销毁 |
| 25. 增加识别 worker 是否一定变快？ | 不一定；CPU 核数、ONNX 内部线程、内存带宽和锁竞争可能使吞吐下降，需基准测试 |

### 9.4 HTTP、幂等与一致性

| 问题 | 回答要点 |
|---|---|
| 26. 为什么上传返回 202？ | 服务端只完成受理并进入异步处理，不代表识别已经完成 |
| 27. `captureId` 如何实现幂等？ | 唯一键 `(deviceId,captureId)`，同时比较图片 SHA 和 capturedAt；相同返回原记录，不同返回 409 |
| 28. 网络超时后设备为什么不能生成新 captureId 重试？ | 无法确认服务端是否已受理；新 ID 会生成重复业务记录，必须复用原始图片、时间和 ID |
| 29. 队列满为什么不能先保存图片？ | 会产生不可执行的孤儿业务状态；当前保证 503 时无图片无记录 |
| 30. 图片、数据库和队列无法用一个 ACID 事务，怎么处理？ | 固定操作顺序、RAII 补偿、数据库唯一约束、原子文件保存和明确失败语义 |
| 31. 为什么严格拒绝未知 JSON 字段？ | 防止两端协议漂移和错误字段被静默忽略，契约问题尽早暴露 |
| 32. 为什么 JSON integer 限制在 `2^53-1`？ | Qt JSON number/JavaScript 兼容的双精度安全整数边界，避免跨语言精度丢失 |
| 33. 为什么所有时间固定 `+08:00`，数据库存 UTC？ | 对外协议一致、教学场景清晰；内部 UTC 避免存储歧义，输出统一转换 |
| 34. HTTP 401、403、409、503 分别表达什么？ | 认证失败、已认证但无权限/禁用、资源/幂等冲突、暂时不可用/过载 |

### 9.5 MQTT

| 问题 | 回答要点 |
|---|---|
| 35. QoS 1 是否会重复？ | 会，是至少一次；消费者按 `recognitionId + revision` 去重 |
| 36. `cleanSession=false` 为什么用于设备？ | 保留持久会话和 Broker 已接受的离线 QoS 消息；Qt 实时视图使用 clean session |
| 37. 为什么设备必须收到 SUBACK 后才上传？ | 确保结果接收通道建立，减少识别完成但设备尚未订阅的竞态 |
| 38. 为什么管理消息没有 `gateAction`？ | Qt 不是闸杆控制链路；管理快照和设备结果职责分离 |
| 39. Broker 断线时识别结果怎么办？ | 数据库照常提交，发布失败记录日志；无 Outbox，消息可能永久丢失，设备保持关闭 |
| 40. 如何实现真正可靠的业务消息？ | 事务 Outbox、持久发布状态、重试、幂等消费者和监控；同时说明当前首版为何没做 |
| 41. retain 为什么是 false？ | 识别事件不是主题当前配置状态；避免新订阅者收到旧动作并误执行 |
| 42. ACL 如何限制设备？ | 每设备独立账号，只读本设备精确结果主题；默认拒绝其他读写 |

### 9.6 MySQL 与数据设计

| 问题 | 回答要点 |
|---|---|
| 43. Repository 模式的价值是什么？ | SQL 集中、参数化、错误映射、Service 可 mock，防止 Controller 到处访问数据库 |
| 44. 连接池为什么需要租约超时？ | 防止无限等待；池关闭、耗尽和连接补建失败可映射为明确不可用 |
| 45. Lease 归还前为什么 rollback/reset？ | 防止上个请求遗留事务、autocommit、schema 或时区污染下个请求 |
| 46. 历史分页为什么固定稳定排序？ | captured_at 相同时用 recognition_id 打破平局，避免翻页重复或遗漏 |
| 47. CSV 为什么用 keyset cursor 而不是一次读完？ | 控制内存；避免大 OFFSET；保持一致性快照并分批流式发送 |
| 48. 名单跨黑白全局唯一如何保证？ | 规范化后单列唯一约束，冲突时事务内读取已有 list_type 映射稳定错误码 |
| 49. 如何排查 MySQL 慢？ | access log/分段耗时、连接池状态、processlist、InnoDB status、执行计划和索引，不先盲目加索引 |

### 9.7 模型与性能

| 问题 | 回答要点 |
|---|---|
| 50. YOLOv8 前处理做了什么？ | 640x640 letterbox、RGB、归一化、NCHW，并保存缩放/填充用于坐标还原 |
| 51. NMS 的作用是什么？ | 去除高度重叠候选；当前选择 NMS 后最高置信度一个车牌 |
| 52. LPRNet 如何解码？ | 每时间步 argmax，合并连续相同索引，移除 blank，再做车牌规范化校验 |
| 53. 为什么每个 worker 独立 Session？ | 避免共享可变输入/缓冲，生命周期清晰；Session 启动加载一次，不按请求重建 |
| 54. CPU 高时如何用 perf？ | 先定位线程和负载，再 perf stat/record；火焰图区分 Session::Run 正常热点与 Session 构造异常热点 |
| 55. 业务慢但 CPU 低怎么办？ | 查 queue、MySQL、磁盘、网络、锁和 off-CPU；on-CPU 火焰图可能没有答案 |
| 56. 如何证明优化有效？ | 固定环境和输入做 A/B，比较 CPU、P95/P99、吞吐、队列深度，并跑结果正确性回归 |

### 9.8 Qt 客户端

| 问题 | 回答要点 |
|---|---|
| 57. 为什么不用同步 HTTP？ | 会阻塞 UI 事件循环；QNetworkAccessManager 异步完成并由 Service 协调 |
| 58. `sessionGeneration` 和 `requestGeneration` 分别解决什么？ | 前者隔离不同登录，后者隔离同会话的多次请求；共同拒绝晚到结果 |
| 59. `QPointer` 与普通指针有什么区别？ | QObject 销毁后自动变空，适合观察异步 QObject；不替代所有权设计 |
| 60. 为什么后台线程用 `QImage` 而不用 `QPixmap`？ | QPixmap 依赖 GUI 资源，应在 GUI 线程创建和使用 |
| 61. CSV 为什么使用 `QSaveFile`？ | 同目录临时文件和原子 commit；失败不破坏原文件，禁用 direct-write fallback |
| 62. MQTT 永久错误为什么不退出 HTTP 会话？ | 两条通信链路独立；历史/名单仍可通过 HTTP 使用，只停止当前 MQTT 自动重连 |
| 63. Qt 退出时的正确顺序是什么？ | 会话代次失效、停调度、取消旧请求/CSV、尽力注销、断 MQTT、清状态、销毁窗口 |

### 9.9 测试、部署与安全

| 问题 | 回答要点 |
|---|---|
| 64. 单元、组件、契约、集成测试分别测什么？ | 纯逻辑；真实/可控依赖边界；两端字段协议；完整业务链路 |
| 65. 为什么需要契约测试？ | Qt 严格拒绝未知字段，服务端与客户端必须共享合法/非法样例防止漂移 |
| 66. Docker 镜像里为什么不同时启动 MySQL 和 Mosquitto？ | 进程职责、升级、健康、持久化和故障隔离；Compose 负责三服务编排 |
| 67. `depends_on` 为什么不等于服务就绪？ | 只代表启动顺序；必须 healthcheck，应用内部仍需连接重试 |
| 68. `/health` 为什么 MQTT DOWN 仍返回 200？ | MQTT 降级不影响 HTTP 和数据库业务，避免容器因已接受降级反复重启 |
| 69. 为什么当前明文 HTTP/MQTT 不能上互联网？ | 无传输加密和生产密钥管理，教学固定凭据公开；仅限隔离网络 |
| 70. 日志如何避免泄密？ | 不记录密码、Token、Authorization、完整 Payload/图片；只记 requestId、稳定错误码和脱敏上下文 |
| 71. 你如何证明项目是自己做的？ | 展示提交、设计取舍、失败记录、调试证据和自己能现场解释/修改的代码，不靠背诵功能 |

## 10. 面试追问链练习

面试官通常不会停在第一个问题。以下是几条常见追问链：

### 10.1 有界队列追问链

```text
为什么异步？
-> 为什么不能无界？
-> 队列满时怎么处理？
-> 为什么先预留再落盘？
-> 预留后数据库失败怎么办？
-> 停止时已取出和未取出的任务分别怎么办？
-> 如果增加到 4 个 worker，哪些对象不能共享？
```

### 10.2 MQTT 追问链

```text
为什么用 QoS 1？
-> 是否会重复？
-> 如何去重？
-> Broker 断线是否会丢？
-> cleanSession=false 能解决所有丢失吗？
-> 为什么还需要 Outbox？
-> 当前为什么没有实现 Outbox？
```

### 10.3 Qt 生命周期追问链

```text
为什么会出现旧响应？
-> 只调用 abort 是否足够？
-> deleteLater 什么时候真正销毁？
-> QPointer 能解决什么、不能解决什么？
-> sessionGeneration 与 requestGeneration 为什么都需要？
-> 退出时为什么先让代次失效再 cancelAll？
```

### 10.4 性能追问链

```text
CPU 高怎么发现？
-> 为什么先看 top -H？
-> perf stat 和 perf record 分别做什么？
-> 火焰图横轴是什么？
-> Ort::Session::Run 很宽一定是 bug 吗？
-> 低 CPU 高延迟为什么火焰图没答案？
-> 如何做可信的优化 A/B？
```

## 11. 学生准备清单

### 11.1 写简历前

- 能在白纸上画出三端系统和服务端/Qt 分层。
- 能指出自己真正修改过的类、测试和提交。
- 能解释一个架构取舍、一个并发问题和一个协议问题。
- 在自己的环境重新构建并执行测试，记录真实结果。
- 准备 2 分钟项目介绍和 30 秒精简版。
- 删除不能深入回答的技术名词。

### 11.2 面试前

- 复习 C++ RAII、移动语义、智能指针、异常安全和内存模型基础。
- 复习 mutex、condition_variable、虚假唤醒、锁顺序和线程停止。
- 复习 HTTP 状态码、幂等、multipart、MQTT QoS/retain/session。
- 复习 MySQL 事务、索引、连接池、稳定分页和锁等待。
- 复习 Qt 事件循环、线程亲和性、QObject 生命周期、signal/slot。
- 实际打开一次 GDB 全线程栈和一次 `perf` 报告，不能只背命令。

### 11.3 项目演示时

建议按以下顺序演示：

1. 展示三容器健康状态和服务端 `/health`。
2. 启动 Qt，解释 HTTP 与 MQTT 状态为什么分开。
3. 启动嵌入式模拟器，强调先 SUBACK 再上传。
4. 展示 PROCESSING、图片下载、最终状态和设备动作。
5. 展示历史/CSV/名单，说明 MySQL 是事实来源。
6. 主动说明 Broker 发布前断线可能丢消息、名单不参与放行等边界。
7. 展示测试报告或自己复现的性能/调试证据。

## 12. 最后建议

高质量校招项目表达不是“我用了多少框架”，而是：

```text
我理解系统为什么这样设计；
我能指出关键状态和不变量；
我能用 C++ 工程方法实现并验证；
我遇到问题时能建立证据链；
我知道当前方案的边界，也知道进一步演进需要付出什么成本。
```

学生可以参考本文组织简历，但最终应把描述改成自己的工作范围、数据和语言。面试官很容易通过连续追问判断候选人是亲自完成、深入理解，还是只复制了一段项目介绍。
