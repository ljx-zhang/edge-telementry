# Edge Telemetry 学习指南

这份指南对应 v1.0。建议不要一上来逐行读代码，先追踪一条事件从产生到确认的完整生命周期。

## 1. 先理解系统承诺

系统采用“至少一次投递 + 接收端幂等”：

1. 边缘端先把事件写入本地 SQLite，再尝试发送。
2. 接收端把事件写入自己的 SQLite，提交成功后才发送 ACK。
3. 边缘端收到匹配的 ACK 后，删除本地待发送行，并在 `edge_meta` 中递增累计确认数。
4. ACK 丢失时，边缘端会重复发送；接收端依靠 `event_id` 主键去重。

它不声称网络层恰好发送一次。可靠性来自“允许重发，但业务结果只生效一次”。

## 2. 推荐阅读顺序

### 第一步：数据模型

阅读 `src/model.hpp`。`Event` 包含：

- `id`：幂等键，格式为 `device_id:sequence`；
- `device_id`：设备身份；
- `sequence`：持久化递增序号；
- `timestamp_ms`：设备采样时间；
- `value`：模拟传感器值。

思考：为什么不能只用时间戳当唯一 ID？同一毫秒可能产生多条数据，时钟也可能回拨。

### 第二步：SQLite RAII 与事务

阅读 `src/store.hpp` 和 `src/store.cpp`：

- `Database` 在构造函数中打开数据库，在析构函数中关闭，体现 RAII；
- `Statement` 自动释放 `sqlite3_stmt`；
- `EdgeStore::append` 把“读取序号、插入事件、递增序号”放在一个事务中；
- `EdgeStore::acknowledge` 把“删除待发送行、递增累计确认数”放在一个事务中；
- `ReceiverStore::insert_if_new` 使用 `INSERT OR IGNORE` 实现幂等。

重点验证：如果进程在事务中间退出，SQLite 只能看到完整提交或完整回滚。

### 第三步：边缘端的两个线程

阅读 `src/edge_agent.cpp`：

- generator 线程负责产生和持久化事件；
- uploader 线程查询未确认事件、发送、等待 ACK；
- 两个线程分别打开 SQLite 连接，通过 WAL 和 busy timeout 协作；
- `--max-pending` 达到上限后暂停生成，形成背压。
- `--max-pending-bytes` 根据活跃待发送数据的估算字节数触发第二道背压；
- 成功 ACK 的延迟会汇总为 P50、P95 和 P99。

这里的背压策略是“暂停上游”。真实设备若不能暂停，需要改成丢弃策略、降采样或扩大持久化容量，并明确记录损失。

### 第四步：协议与连接

阅读 `src/net.hpp` 和 `src/net.cpp`。当前协议是一行制表符分隔文本：

```text
EVENT<TAB>1<TAB>event_id<TAB>device_id<TAB>sequence<TAB>timestamp_ms<TAB>value
ACK<TAB>1<TAB>event_id
```

`TcpSocket` 负责关闭 socket，`send_all` 处理一次 `send` 没有发完全部字节的情况，`read_line` 处理 TCP 没有消息边界的问题。

### 第五步：接收端与故障注入

阅读 `src/telemetry_receiver.cpp`：

- 每个连接交给固定数量的 worker 之一处理；worker 从有界连接队列取任务，主线程退出时关闭队列并等待全部 worker；
- 数据先入库，再 ACK；
- `--drop-ack-every N` 模拟“已经入库但 ACK 丢失”；
- `--processing-delay-ms N` 模拟慢服务端。

## 3. 必做实验

正常闭环：

```powershell
.\scripts\integration_demo.ps1
```

ACK 丢失和背压：

```powershell
.\scripts\fault_injection_demo.ps1
```

完整自动测试与基准：

```powershell
ctest --test-dir build --output-on-failure
.\scripts\benchmark.ps1
```

观察四组数字：边缘端 total/acknowledged/pending，以及接收端 rows/distinct event_id。正确结果应满足：

```text
generated total = acknowledged total
pending = 0
receiver rows = distinct event_id = edge total
```

背压实验是例外：接收端不存在，pending 应等于 `--max-pending`，且不会继续增长。

## 4. 建议自己完成的练习

1. 把 `--max-pending` 从 10 改成 30，预测离线实验结果后再运行。
2. 设置 `--drop-ack-every 2`，观察 duplicates 和 retries 如何变化。
3. 设置 `--processing-delay-ms 200` 和较快采样间隔，观察积压增长。
4. 为协议增加校验和，构造损坏消息验证拒绝路径。
5. 把逐条 ACK 改成批量 ACK，比较吞吐量和故障恢复复杂度。

## 5. 面试时应能解释的问题

- 为什么必须先落盘再确认接收？
- 为什么 ACK 丢失会导致重复发送？
- 接收端怎样保证重复发送不会重复入库？
- `BEGIN IMMEDIATE` 保护了哪几个状态变化？
- WAL 改善了什么，又没有解决什么？
- 为什么队列必须有上限？满了以后有哪些策略？
- 当前版本有哪些生产环境缺口？

最后一个问题可以回答：缺少 TLS、认证、外部监控导出、正式 schema migration 和批量事务。当前字节上限是待发送载荷估算值，不是物理 SQLite 文件硬配额。这些限制应在面试中主动说明。
