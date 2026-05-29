# MRPC 即时通讯系统 — 面试 FAQ

> 所有数据源自实测（16 核 Intel Xeon / 32 GB / Release 编译），
> 详见 [测试报告](./%E6%B5%8B%E8%AF%95%E6%8A%A5%E5%91%8A.md)。

---

## 序列化与网络协议

### Q: 简历写的是 JSON 序列化 53k+ QPS，但你们实际用的是 MessagePack？解释一下两者的关系。

**Situation**：框架需要选择一种网络传输格式，把内存中的请求/响应数据编码后发送到对端。

**Task**：在不引入额外依赖的前提下，选择一种高性能、跨语言兼容的序列化方式。

**Action**：

- 框架统一使用 `nlohmann::json` 作为内存中的数据表示。所有 handler 收/发的参数都是 JSON 数组。
- 写入网络时，通过 `nlohmann::json::to_msgpack()` 将 JSON 树编码为 MessagePack 二进制；读取时用 `from_msgpack()` 解码回 JSON 树。
- 同一套 API 也支持 `to_json / to_bson / to_ubjson / to_cbor`，编译期通过宏 `MSG_FMT_MSGPACK` 切换，**一行代码不改就可以换格式**。
- 简历写 "JSON 序列化" 是为了让读者立刻理解"数据是 JSON 格式组织的"，不增加认知负担。

**Result**：

| 格式 | QPS | 平均延迟 | 角色 |
|------|-----|---------|------|
| JSON（纯文本） | 更低 | 更慢 | 可选 wire format |
| **MessagePack（默认）** | **54,558** | **0.91 ms** | **生产 wire format** |
| RAW（裸二进制，无序列化） | 135,368 | 0.37 ms | 对比基准 |

JSON 和 MessagePack 的核心区别：JSON 是文本格式（可读、膨胀大、解析慢），MessagePack 是二进制格式（紧凑、解析快）。测试的所有数据都是 MessagePack 跑的。

---

### Q: MessagePack 和 Protobuf 的差别是什么？为什么说换 Protobuf 能提升 2-3 倍？

**Situation**：序列化被定位为最大性能瓶颈（占 CPU 60%），需要评估是否有更好的序列化方案。

**Task**：理解 MessagePack 的开销来源，判断 Protobuf 能否消除这个瓶颈。

**Action**：

- 通过 RAW 格式（跳过 nlohmann，裸字节直接发）与 MSGPACK 格式对比，分解每请求的 CPU 时间：

```
RAW         7.4 μs / req   ← 纯框架 + 网络开销
MSGPACK    18.3 μs / req   ← 框架 + 网络 + 序列化
序列化开销  10.9 μs / req   ← 占 60%
```

- 进一步分析这 10.9 μs 的去向：

```
发送端: C++ struct → 拼 json 树 → to_msgpack → 二进制
接收端: 二进制 → from_msgpack → json 树 → 取字段
        ↕ 构造/析构 JSON 树是开销主力
```

- nlohmann::json 是动态类型容器：每个整数、字符串都要分配节点、维护类型 tag、引用计数。1KB payload 在内存里膨胀成一棵包含类型信息的树结构。
- Protobuf 是 schema 驱动：字段在二进制中按固定偏移排列，直接读写，不经过中间树。

**Result**：

| 方案 | QPS 预估 | 原因 |
|------|---------|------|
| MessagePack（当前） | 54k | 经过 JSON 树 |
| Protobuf | **108k-162k** | 去 JSON 树，接近 RAW 水平 |

所以"2-3 倍"不是推测，是上限推算：去掉 60% 序列化开销后，现有 54k QPS 可以接近 RAW 的 135k 水平。

---

### Q: 你刚才说的 RAW 格式是什么？没有序列化数据怎么还原？

**Situation**：需要量化框架本身的开销，排除序列化的干扰。

**Task**：设计一种"零序列化"的传输路径，测量纯框架 + 网络耗时。

**Action**：

- 新增 `MSG_FMT_RAW` 格式位（router.hpp:54），在 `connection::write()` 中检测该标志位：
  - 不调用 `to_msgpack`，直接从 JSON 数组中提取 payload 字符串的底层缓冲区（`.get_ptr<json::string_t*>()`），裸写
- 在 `router::decode()` 中对应处理：
  - RAW 请求包：包装为 `json::array_t{ buffer }`
  - RAW 响应包：包装为 `json::array_t{ 200, "ok", buffer }`
- handler 代码完全不变——对业务层是透明的。

**Result**：

```
echo RAW QPS: 135,368   (vs MSGPACK 54,558)
RAW 延迟:     0.37 ms   (vs MSGPACK 0.91 ms)
每请求耗时:   7.4 μs    (vs MSGPACK 18.3 μs)
```

RAW 格式证明了框架本身的开销只有 7.4 μs，也证明了序列化是剩余的 60%。

---

## 性能与测试

### Q: 53k QPS 是怎么测出来的？为什么选 50 条连接？

**Situation**：需要建立性能基准，确定最优并发度。

**Task**：设计压测方案，找到系统的吞吐上限和瓶颈。

**Action**：

- 编写 `stress_test.cpp`，支持 `echo`、`echo_raw`、`echo_reactor` 等多种测试模式。
- 服务端：server 模式，32 个 io_context，`thread_per_io=1`，Reactor 模型。
- 客户端：50 独立 TCP 连接，每条绑定独立 io_context + 线程，无锁并发。
- 先扫一遍并发数找到最优值：

```
conns=1      3,191
conns=2      6,797
conns=4     14,093
conns=8     24,671
conns=16    49,971   ← 最优（= CPU 核心数）
conns=32    49,055
conns=64    50,711
conns=128   50,883
conns=256   48,474
```

- 16 连接已达 CPU 饱和，50 连接只是方便横向对比不同测试模式（echo / echo_reactor / echo_shared）。
- 15s 稳定运行，统计总请求数 ÷ 耗时，并记录 P50/P90/P99。

**Result**：54,558 QPS，P50 1ms，P99 5ms。结论：最优并发数 = CPU 核心数，超配不能继续提升。

---

### Q: 你说序列化是最大瓶颈，怎么证明的？

**Situation**：需要定量的证据，不能靠猜测"JSON 可能比较慢"。

**Task**：设计实验，隔离序列化开销，测量其占比。

**Action**：

- 实现 RAW 格式（见前文），在不改变任何 handler 代码的前提下跳过序列化。
- 控制变量：同一台机器、同样 50 连接、同样 1KB payload，仅改变序列化方式。

**Result**：

```
RAW       135,368 QPS   0.37 ms 延迟   7.4 μs CPU/req
MSGPACK    54,558 QPS   0.91 ms 延迟  18.3 μs CPU/req
差值       80,810 QPS   0.54 ms 延迟  10.9 μs CPU/req
```

序列化占单次 RPC 总 CPU 的 **10.9 ÷ 18.3 ≈ 60%**。结论可复现：换成纯 JSON 文本格式占比会更高，换成裸二进制占比降到 0%。

---

### Q: 为什么帧听端口是 8877 而不是 8881？Nginx 在这里是什么角色？

**Situation**：5 个 server_node 分别监听 8881-8885，客户端通过一个统一入口访问。

**Task**：设计负载均衡层，使连接均匀分布到各后端节点。

**Action**：

- Nginx Stream 模块工作在 L4 层，直接透传 TCP，不解析应用层协议。
- 选择 least_conn 策略而非 round-robin：因为每条连接对应一个登录用户，用户活跃度不均，least_conn 能把新连接分配给当前负载最低的节点。
- 客户端只连接 8877，Nginx 转发到其中一个 8881-8885 节点。

**Result**：

```
client → 8877 (Nginx Stream, least_conn)
              ├→ 8881 (server_node-1)
              ├→ 8882 (server_node-2)
              ├→ 8883 (server_node-3)
              ├→ 8884 (server_node-4)
              └→ 8885 (server_node-5)
```

---

## 架构设计

### Q: 消息跨节点转发是怎么实现的？一条消息从发送到接收经历了哪些步骤？

**Situation**：用户发送消息，目标用户可能登录在不同节点上。

**Task**：实现跨节点的消息路由，保证送达。

**Action**：

`handle_send_message` 的完整路径：

```
send_message(from, to, payload)
  │
  ├─ Snowflake::next_id()            // 本地生成消息 ID，无网络
  ├─ g_batch_saver.push(msg)         // 入异步批量队列
  └─ deliver_message(from, to, msg)
       │
       ├─ to 在本节点在线？
       │    → 直接 async_call("on_message")，零网络
       ├─ to 在位置缓存中？
       │    → RPC remote_deliver → 目标节点 → on_message
       └─ 缓存未命中
            → Redis HGET user_location
            → 更新缓存 → RPC remote_deliver
```

**Result**：在线互发场景（sender 在 node-1，receiver 在 node-5）测得 **10,825 QPS**，P50 5ms，P99 20ms。主要开销是一次跨节点 RPC（序列化 + 网络往返 + 反序列化）。

---

### Q: 用户位置缓存（三级路由）是怎么设计的？Redis 挂了会怎样？

**Situation**：每条消息都要查 Redis 获取目标用户所在节点——高频访问 Redis 会成为瓶颈。

**Task**：减少 Redis 查询次数，同时保证 Redis 宕机时核心链路不中断。

**Action**：

```
用户上线 → mark_user_online()
  ├─ g_local_users[user_id] = conn         // 本节点内存
  ├─ g_location_cache[user_id] = node_id   // 全局本地缓存
  └─ Redis HSET user_location + Pub/Sub user_status:online

消息路由 → deliver_message()
  ① g_local_users 命中 → 本地交付，零网络
  ② g_location_cache 命中 → RPC 转发，零 Redis
  ③ Redis HGET → 更新缓存 → RPC 转发（仅首次或缓存淘汰）
```

**Result**：Redis 宕机时：
- 已在线的用户连接不受影响（本地缓存仍在，消息路由正常）
- 新上线用户的位置无法同步到其他节点，新连接受影响
- 这是一个"缓存优先、Redis 兜底"的设计，不是强一致性，但够用

---

### Q: 服务注册与发现怎么做的？

**Situation**：多节点部署，节点需要自动感知彼此的存在，不能手动配置 IP:Port。

**Task**：实现动态服务发现，新节点上线后自动被集群发现并建立连接。

**Action**：

- 节点启动：将 `node_id:port` 写入 Redis `cluster:nodes` hash，通过 Pub/Sub 广播 `cluster:node_join` 事件。
- 已有节点：订阅 `cluster:*` 频道，收到事件后读取 `cluster:nodes`，发现新节点就调用 `connect_peer()` 建立双向 RPC 连接。
- 节点下线：信号处理（SIGINT/SIGTERM）清理 Redis key 并广播 `cluster:node_leave`，Redis key 本身有过期 TTL 兜底。
- peer 连接建立后，缓存对端支持哪些 RPC 方法，后续跨节点调用直接走已有连接。

**Result**：启动 5 节点顺序任意，最后一个节点启动后数秒内所有 peer 连接全部就绪。

---

## 数据库与存储

### Q: 为什么引入 Snowflake 替代 Redis INCR？

**Situation**：消息需要全局唯一的 seq_id，原始方案用 Redis INCR 生成。

**Task**：消除每次消息产生的网络 RTT，同时消除 Redis 单点对 seq_id 可用性的影响。

**Action**：

- 实现 Snowflake 算法（snowflake.hpp）：
  - 41-bit 毫秒时间戳（epoch 2024-01-01）
  - 10-bit node_id（从启动参数解析，如 "node-1" → 1）
  - 12-bit 序列号（单毫秒内自增）
  - 互斥锁保护，序列号耗尽时自旋等下一毫秒

**Result**：

| 方案 | 每条消息 | 可用性影响 |
|------|---------|-----------|
| Redis INCR | +1 次网络 RTT | Redis 挂则 seq_id 不可用 |
| **Snowflake** | **本地生成，0 网络** | **Redis 挂不影响 seq_id** |

---

### Q: 批量写入（MessageBatchSaver）的设计细节：为什么是 50 条 / 100ms？

**Situation**：每条消息都执行一次 INSERT 会导致 MySQL 成为瓶颈（同步等待磁盘）。

**Task**：将写入从"每条消息 1 次事务"变为"攒批后 1 次事务"。

**Action**：

- `MessageBatchSaver` 有一个后台线程，`push()` 将消息追加到队列：
  - 队列长度 ≥ 50 → 唤醒 flush 线程
  - 否则等待 100ms 超时
- flush 线程将队列中所有消息取出，执行 `BEGIN + 多行 INSERT ... ON DUPLICATE KEY UPDATE + COMMIT`。
- 50 条和 100ms 是经验值：50 条保证单批不超过 MySQL max_allowed_packet，100ms 保证离线消息延迟用户不感知。

**Result**：去掉 `user_exists` 冗余查询后，离线消息存储吞吐从 ~13k 提升到 **20,269 QPS**。

---

### Q: 连接池大小怎么确定的？为什么是 16？

**Situation**：MySQL 连接需要管理，不能每次请求都新建连接。

**Task**：确定连接池容量，兼顾并发和资源。

**Action**：

- `MySQLPool` 初始化 `pool_size` 默认 `std::min(hardware_concurrency(), 16u)`。
- `borrow()` 从队列取一个空闲连接，没有则阻塞等待（`condition_variable`）。
- `release()` 用完后归还。
- `AutoConn` RAII 封装，作用域结束自动归还。

**Result**：离线场景压测中 MySQL 不是瓶颈（20k QPS 时 MySQL 无积压，CPU 不饱和），16 连接恰好够用。

---

## 容错与生产化

### Q: 批量写入如果宕机会丢消息吗？怎么改进？

- **会**。当前设计是"先入内存队列就返回成功"，极端宕机丢失最多 50 条（攒批未刷出的）。
- 改进方向：WAL（Write-Ahead Log），每条消息先写本地磁盘日志，启动时回放未刷出的消息。但当前项目定位即时通讯，IM 场景少量丢消息是可接受的（TCP 保证不丢字节，不保证应用层不丢消息）。
- 如果要做金融级别可靠：双写到本地 + 远程，确认落盘再返回客户端。

---

### Q: 你做的这些优化里哪个提升最大？

分两类看：

1. **对 QPS 提升最大的**：去掉 `user_exists` 冗余 MySQL 查询。它把一条消息链路上的同步等待点去掉了，配合连接池 + 批量写，QPS 从 ~13k 提到 20k（+52%）。
2. **对架构可用性提升最大的**：Snowflake 替代 Redis INCR。消除了一次网络 RTT 的等待，同时 Redis 挂了 seq_id 不受影响。如果一个 Redis 宕机导致消息发不了，IM 系统就不可用了——这个改动防止了那种情况。

---

## 服务发现与集群

### Q: ZK 在分布式系统里很常见，你们为什么不用了？

> A: 杀鸡不用牛刀。我们需要的只是"新机器上线，通知老机器"这个功能，ZK 的强一致性、leader 选举、分布式锁对我们来说都是过剩能力。后来用 Redis Pub/Sub 替换了 ZK，因为 Redis 已经在项目里，一行 `PUBLISH` 就能做到。

### Q: 不用 ZK 之后，手动配置节点的方式能支撑多少节点？

> A: 实测 5 节点。静态配置的扩展上限取决于节点间全连接的网络复杂度（O(n²) 连接），而不是配置维护的复杂度。超过 10 个节点建议换服务网格或 Sidecar 模式。后来引入 Redis Pub/Sub 自动发现后，节点扩缩容无需任何手动配置。
