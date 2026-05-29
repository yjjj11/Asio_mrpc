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

---

## 扩展追问：深挖项目与横向对比

### 深挖项目

#### Q: RAW 模式测出框架开销 7.4 μs，这 7.4 μs 具体花在框架哪几个环节了？做过 profiling 吗？

> A: 没有用 perf 或 Callgrind 做拆解——7.4 μs 是 aggregate 均值，通过 RAW 格式的总 QPS 反推的。如果要拆，估计大头是：
>
> - **Asio epoll_wait + socket read/write 系统调用**：每次读写 2 次 syscall（read + write 各一次或合并），约 2-3 μs
> - **内存分配**：`msg_body_` 每次读取前 resize，响应写完后 shrink_to_fit，频繁堆分配
> - **函数路由**：hash 表查找 + `std::function` 虚函数调用 + `std::apply` 参数展开，大约 1-2 μs
> - **Server 端 round-robin**：每次 accept 新请求选择一个 io_context，atomic 操作
>
> 如果真要优化到极致，可以从这几方面入手：使用 `asio::registered_buffer` 避免拷贝、内存池复用 `msg_body_`、`std::function` 换成手写虚函数表消除 indirect call 开销。

#### Q: RAW 格式虽然跳过了 to_msgpack/from_msgpack，但代码里还是经过了 `json::array_t{ payload }` 的构造和 `decode()` 里的数组包装。RAW 里面其实还有 nlohmann 操作，怎么排除的？

> A: 观察很细。RAW 路径确实还有最小化的 json 操作——请求端构造 `json::array_t{ buffer }` 只包了一个字符串（不涉及编解码），响应端 `decode()` 包了 `json::array_t{ 200, "ok", buffer }`。这些是轻量操作且不涉及 MessagePack 的编解码。
>
> 但严格来说 7.4 μs 确实包含了部分 nlohmann 开销，所以它不是一个"绝对零序列化"的基准。不过测试目的在于 **量化 MSGPACK vs 一个极简基准的差异**，验证"序列化是 CPU 瓶颈"这个判断。就算把这部分再减掉，只会拉开 MSGPACK 和 RAW 的差距，结论方向不变——甚至更强。
>
> 真要测纯框架开销，应该用 pre-serialized buffer 直接在 `write()` 中发送，连 json 数组构造都省掉。但那属于雕花了，结论已经够清楚。

#### Q: 并发度扫描里 MSGPACK 在 16 连接就饱和了（49,971 QPS），RAW 要到 32 连接才饱和（125,116 QPS），为什么差一倍？

> A: 这是个好问题，也是"序列化是 CPU 瓶颈"的旁证。
>
> - MSGPACK 模式下，每个请求要构造 JSON 树 → to_msgpack 编码 → 发送 → 接收 → from_msgpack 解码 → 提取参数 → 反方向再来一遍。**CPU 先跑满了**，16 连接已经让 16 个核心饱和，加连接只会增加锁争用和上下文切换，QPS 不再涨。
> - RAW 模式下，去掉编解码后 CPU 负担大幅降低，需要**更多连接（32 条）才能把 CPU 重新喂饱**。此时瓶颈转移到 I/O 层面——socket 读写和 epoll 事件分发的开销占比上升。
>
| 模式 | 瓶颈 | 最优并发 | 达到上限前 QPS 随连接数增长斜率 |
|------|------|---------|----------------|
| MSGPACK | CPU（编解码） | 16 | 约 3,100 QPS/连接 |
| RAW | I/O（网络栈） | 32 | 约 3,900 QPS/连接 |

#### Q: 50 线程共享一条连接只有 14,565 QPS，比 50 独立连接（54k）差这么多，为什么？

> A: 问题出在 **锁争用**。`connection::write()` 里有 `write_mtx_` 互斥锁保护发送队列，50 个线程疯狂抢这把锁：
>
> ```cpp
> std::lock_guard<std::mutex> locker(write_mtx_);
> write_queue_.emplace_back(id, std::move(data));
> if (write_queue_.size() > 1) return;  // 等上一个写完
> ```
>
> 50 线程高频抢锁，大量时间花在线程切换、缓存颠簸和等待上。服务端虽然也用锁，但服务端的锁在 `get_iocontext()` 的 round-robin 上，数据发送路径是无锁的（每个连接自己的写队列独立）。
>
> 这也反过来验证了"独立连接 + 独立 io_context"设计的必要性——**每条连接的数据路径是隔离的，天然无锁**。

#### Q: 为什么选 nlohmann::json 作为所有数据的中间表示？这不是性能瓶颈吗？

> A: 这确实是一个取舍，当初选择有几点考虑：
>
> 1. **免注册**：Protobuf 需要 `.proto` 文件 + 代码生成，我们想要的是一个"函数签名即协议"的体验——注册一个 lambda，框架自动萃取参数类型、编译期生成序列化代码
> 2. **多格式切换**：同一套 json 树通过 `to_msgpack / to_cbor / to_ubjson / to_json` 一行代码切换编解码器，运行时动态选择——protobuf 绑定单一格式
> 3. **header-only 零依赖**：nlohmann 是单头文件，配合 asio 整个框架无编译依赖
>
> 代价就是在性能敏感路径上多了 JSON 树的构造/析构。放在项目语境下（即时通讯，延迟不是硬实时，主要瓶颈在 MySQL 和跨节点转发），这个取舍是合理的。如果做高频交易或 RPC 中间件产品，那必须上 protobuf 甚至 flatbuffers。

---

### 横向对比

#### Q: 跟 gRPC 比性能怎么样？

> A: 不在一个量级。gRPC 的 HTTP/2 头部解析、流控、TLS 握手，单次 RPC 开销通常在百 μs 级别。我们 7.4 μs 的框架开销来自：
>
> - **自定义协议**：Header 定长 24 字节（msg_type / msg_id / req_id / body_len），没有 HTTP 的冗余字段
> - **无 TLS**：性能测试不开 TLS（业务层保活用自定义心跳和 token）
> - **同步 call 走 promise/future**：省去了 HTTP/2 stream 的管理复杂度
>
> gRPC 的优势不在裸性能，而在于**生态**：流式 RPC、拦截器、多语言代码生成、TLS 开箱即用。我们的项目定位是一个极简嵌入式 RPC，适用场景不同。性能取向更接近 brpc / tRPC 这类自定义协议框架。

#### Q: 和 brpc 对比过吗？

> A: 没有实测对比，但从公开数据看：brpc 用 bthread（M:N 协程）+ protobuf，多核扩展性优于 Asio 的回调模型。brpc 单机 QPS 在百万级（多核 + 零拷贝 + 锁无关数据结构）。
>
| 维度 | brpc | mRPC |
|------|------|------|
| 核心代码量 | ~10 万行 | <700 行（header-only） |
| 并发模型 | bthread 协程 | Asio io_context 线程池 |
| 序列化 | protobuf（强制） | msgpack / cbor / json / raw |
| 单机性能 | 百万级 QPS | 54k QPS（16 核） |
| 学习成本 | 高 | 低（单头文件，开箱即用） |
>
> 对比的结论不是"谁更好"，而是**适用场景不同**。brpc 适合做基础架构中间件（百度内部全链路在用），mRPC 适合嵌入到中小规模项目做通信层。

#### Q: 为什么选 Asio 而不是 libuv / libevent / 直接 epoll？

> A: 几个维度的对比：
>
> - **直接 epoll**：需要手写状态机管理 accept / read / write 各种状态，代码量大且容易引入 bug。Asio 把 Proactor 模型封装成了 async_* 接口，配合 callback / future / coroutine 三种编程风格。
> - **libuv**：C 接口，表达能力有限——参数类型擦除到 `void*`，不好跟模板元编程结合。我们的框架大量使用 `function_traits` 编译期萃取参数类型，C 接口做不到这个层级的类型安全。
> - **libevent**：偏 Reactor 模型（你准备好了叫我），Asio 是 Proactor（我叫你的时候已经读好了）。Proactor 在编程模型上少一层缓冲管理。
> - **Asio 最大的价值**：和 C++ 标准库的整合——`std::future`、`std::chrono`、`std::error_code`、`std::coroutine_handle`（C++20）。这让 `call()` 的同步等待可以用 `future.wait_for` 加超时，async 回调可以绑定 `shared_from_this` 管理生命周期。

#### Q: 你提到序列化瓶颈，如果换 protobuf 之外，flatbuffers 和 cap'n proto 呢？

> A: 它们更激进——flatbuffers 连解码步骤都省了：收到 buffer 直接当结构体访问，不需要 parse 阶段。cap'n proto 类似，基于 arena 分配的序列化。
>
> - **flatbuffers 的优势**：在极端延迟场景（微秒级 RPC）很亮眼，和我们的 RAW 格式思路一致——减少中间表示
> - **flatbuffers 的代价**：传输体积更大（需要对齐填充），buffer 构造复杂（逐字段 set），不支持运行时反射。这对我们的场景有影响——框架的函数签名反射和运行时多格式切换是核心设计点，flatbuffers 在这两个方面很受限
> - **结论**：如果项目目标追求极致 QPS（比如目标是冲击 200k+），flatbuffers 值得研究；但如果要保持"零注册、免代码生成"的开发体验，protobuf 是更务实的升级路径

#### Q: 你们用 Redis Pub/Sub 做服务发现，换成 etcd 会有什么不同？

> A: 核心区别在于 **一致性模型** 和 **性能**。
>
| 特性 | Redis Pub/Sub | etcd (Raft) |
|------|--------------|------------|
| 一致性 | 最终一致（可能丢消息） | 线性一致（强一致） |
| 单机 QPS | 10 万+ | ~1 万 |
| 功能集 | Pub/Sub + Hash + TTL | Watch + Lease + 事务 |
| 运维成本 | 低 | 中（需要管理 Raft 集群） |
>
> 服务发现对强一致的需求其实是有限的——节点上线/下线状态延迟几秒感知通常不影响正确性（消息路由有重试和本地缓存兜底）。所以用 Redis Pub/Sub 在这个量级是合理的。
>
> etcd 更适合需要强一致的元数据场景（分布式锁、leader 选举、配置管理）。我们的架构里，节点间没有 leader 选举，也不需要分布式锁——这是一个无状态的对称集群。

#### Q: 如果想在生产环境用你这个框架，你觉得哪些地方必须改进？

> A: 当前的定位是"可工作的 demo + 性能验证"，离生产还有距离：
>
> 1. **TLS**：现在是裸 TCP，在公网部署必须加 TLS（可以用 Asio 的 `ssl::stream` 包装 socket 层，改动不大）
> 2. **熔断和限流**：server 侧没有过载保护，突发流量可能 OOM（写队列无限增长）
> 3. **链路追踪**：没有 tracing ID 贯穿整个调用链（跨节点时排查问题靠翻日志）
> 4. **连接管理**：写队列加个上限做背压——现在 `write_queue_` 可以无限堆积（`do_read_header` 是异步链式的，没有流量控制）
>
> 但如果只是做**嵌入式 RPC 通信层**（两个内部进程间、容器间），以上问题大部分不是瓶颈。这也是这个框架设计的真实使用假设。

---

### 水平扩容与服务发现

#### Q: 简历写"针对单机性能瓶颈，对消息转发水平扩容"，单机瓶颈具体指什么？扩容后怎么验证效果？

> A: 单机瓶颈说穿了就是三个：**内存上限**、**CPU 上限**、**连接数上限**。
>
> - **内存**：每条连接有 socket buffer、发送队列、用户状态。单机几百上千连接还能扛，但 IM 场景大盘用户在线是以万计的。
> - **CPU**：echo 基准 54k QPS 就已经吃满 16 核了，加上业务逻辑（Snowflake、batch_saver、三级路由、序列化）更有富余。
> - **连接数**：Linux 单进程 epoll 管理几十万 fd 没问题，但多线程共享 io_context 后在连接数很大时 epoll 的 O(n) 扫描开销和锁争用就会显现。
>
> 扩容方案就是 5 节点（server-1~5），前端 Nginx 做 L4 负载均衡。验证效果不能只看 echo 基准，要看实际业务链路：
>
> | 场景 | QPS | 瓶颈 |
> |------|-----|------|
> | echo 框架基准 | 54,558 | CPU（序列化） |
> | 消息转发（离线，接收方不在线） | 20,269 | MySQL 批量写 |
> | 消息转发（跨节点，接收方在另一节点） | 10,825 | 跨节点 RPC 往返 |
>
> 离线场景 20k QPS 意味着单节点扛 4k 的话，5 节点理论上能扛 20k 跨节点消息——当然实际上要看消息分布和 Nginx 是否均衡。

#### Q: 服务注册与发现具体是怎么用 Redis Pub/Sub 做的？比 ZK 好在哪？

> A: 看 `server_node.cpp` 的代码，核心就三块：
>
> **启动时发现已有节点：**
> ```cpp
> // 读取已有节点列表
> redisReply* reply = redisCommand(g_redis, "HGETALL cluster:nodes");
> for (size_t i = 0; i + 1 < reply->elements; i += 2) {
>     connect_peer(peer_id, host, port);  // 主动建 RPC 连接
> }
> ```
>
> **宣告自己上线：**
> ```cpp
> redisCommand(g_redis, "HSET cluster:nodes %s %s", g_node_id, self_addr);
> redisCommand(g_redis, "PUBLISH cluster:node_join %s=%s", g_node_id, self_addr);
> ```
>
> **订阅线程自动发现新节点（subscribe_loop）：**
> ```cpp
> // 收到 cluster:node_join 事件后：
> auto eq = payload.find('=');
> string peer_id = payload.substr(0, eq);
> string host_port = payload.substr(eq + 1);
> connect_peer(peer_id, host, port);
> ```
>
> 下线时信号处理清理 Redis key 并广播 node_leave。
>
> **为什么不用 ZK？** ZK 的强一致性、leader 选举、分布式锁对我们来说都是过剩能力。我们只需要"新机器上线，通知老机器"这个功能，Redis Pub/Sub 一行 `PUBLISH` 就搞定了。Pub/Sub 不保障可靠投递——订阅者离线期间的消息会丢——但在这个场景无所谓，因为新节点上线后，老节点可以通过读 `cluster:nodes` Hash 补全列表。**最终一致性够用**。

#### Q: Nginx Stream L4 层配了 least_conn，为什么选它而不是 round-robin？

> A: 因为每条连接对应一个登录用户，用户的活跃度不均。一个人可以挂机一整天不说话，另一个人可能每分钟发几十条消息。least_conn 会把新连接分配给当前活跃连接数最少的节点，在长连接场景下比 round-robin 均衡得多。
>
> 配置也很简单：
> ```nginx
> stream {
>     upstream chat_backend {
>         least_conn;
>         server server-1:8881;
>         server server-2:8882;
>         server server-3:8883;
>         server server-4:8884;
>         server server-5:8885;
>     }
>     server { listen 8877; proxy_pass chat_backend; }
> }
> ```
>
> 选 L4（Stream）而非 L7（HTTP）的原因：mRPC 协议是基于 Asio 的自定义 TCP 协议，不是 HTTP。L4 直接透传 TCP 包，不做应用层解析，延迟最低。如果非要用 L7 就得改协议上 HTTP 头，开销不划算。

#### Q: "本地内存→全局缓存→Redis 降级"三级路由具体怎么实现的？Redis 挂了会怎样？

> A: 代码在 `deliver_message()` 里，走三步，越往后越慢：
>
> ```cpp
> static void deliver_message(...) {
>     // Level 1: 本节点在线 → 直接 async_call 捅给连接
>     std::shared_lock lock(g_local_mutex);
>     auto it = g_local_users.find(to);
>     if (it != g_local_users.end()) {
>         it->second->async_call(... "on_message", ...);
>         return;   // 零网络，纯本地
>     }
>
>     // Level 2: 本地缓存命中 → RPC 转发到目标节点，不走 Redis
>     std::shared_lock lock(g_location_mutex);
>     auto it = g_location_cache.find(to);
>     if (it != g_location_cache.end()) {
>         pit->second->async_call(... "remote_deliver", ...);
>         return;   // 零 Redis 查询
>     }
>
>     // Level 3: 缓存 miss → Redis HGET 查 user_location → 回填缓存
>     redisReply* reply = redisCommand(g_redis, "HGET user_location %s", to);
>     g_location_cache[to] = target_node;   // 回填缓存，下次走 Level 2
>     pit->second->async_call(... "remote_deliver", ...);
> }
> ```
>
> 用户上线时同时写缓存和 Redis：`mark_user_online()` 同时做三件事——`g_local_users[username] = conn`、`g_location_cache[username] = node_id`、`Redis HSET user_location + PUBLISH user_status:online`。
>
> **Redis 挂了会怎样？**
>
> - **已在线用户不受影响**：位置缓存在本地内存中，Level 2 直接命中，消息照发
> - **新上线用户位置无法同步到其他节点**：Level 3 降级不可用，跨节点发消息只能靠 Level 2 缓存——如果目标用户之前没被缓存过（新用户刚上线），发往它的消息会路由失败
> - **缓存淘汰**：缓存不设 TTL，而是通过订阅 `user_status:offline` 事件来清除，这是一个 **push 而非 pull** 的设计——连清除都不走 Redis 轮询，完全事件驱动
>
> 这个设计是"缓存优先、Redis 兜底"，不是强一致性的，但 IM 场景够用了。用户聊天的体验取决于"消息能不能送到"，而不是"位置信息是不是强一致"。

---

### 扩展追问：深挖项目

#### Q: 三级缓存的淘汰策略为什么不用 TTL？订阅 offline 事件来清除，如果事件丢了怎么办？

> A: 这是 push vs pull 的选择。
>
> 用 TTL 的话，每个未读的缓存项到期后都要 Redis HGET 重新拉一次——相当于每个用户即使在线，位置缓存也要定期过期，增加不必要的 Redis 负载。缓存规模越大，TTL 的"空转"开销越明显。
>
> 事件驱动的好处是：**没有请求就不干活**。用户在节点 A 上一直在线，其他节点缓存中的位置信息就一直有效，零维护开销。
>
> 事件丢失的风险确实存在（Pub/Sub 不保证可靠投递），但后果很轻微：丢失 offline 事件后，其他节点会认为这个用户还在老节点上，发消息时会尝试 RPC 转发到老节点——老节点收到 `remote_deliver` 后发现用户不在线，消息就投递失败了。发送方收到失败后不会有严重后果，只是这条消息会走离线存储。**这个设计不是 100% 正确的，但 99.9% 的场景下表现得足够好**。

#### Q: 5 个节点是 O(n²) 全连接，peer 与 peer 之间 10 条双向连接。如果扩容到 10 节点，连接数 45 条，还能撑吗？

> A: 当前是 O(n²) 的全连接 mesh 架构。n=5 时 10 条，n=10 时 45 条，n=20 时 190 条。每一对节点之间都要维持一条长连接，每个节点要维护 n-1 条出站连接 + n-1 条入站连接（被其他节点连过来的）。对于 Asio io_context 管理几千条连接的能力来说，45 条不算什么，真正的问题是维护复杂度——谁和谁连了、断连后怎么重建、新节点加入时已有连接是否都建好了。
>
> 超过 10 个节点的合理方案有两种：
>
> 1. **引入一个"路由节点"（central hub）**：所有节点只连 hub，消息由 hub 中转。连接数从 O(n²) 降到 O(n)，但 hub 成了单点和瓶颈。
> 2. **Service Mesh / Sidecar**：每个节点旁边挂一个 sidecar proxy，节点只需要连本地 sidecar，sidecar 之间维护 mesh 连接。K8s + Envoy 就是这么干的。
>
| 架构 | 连接数 | 延迟 | 复杂度 |
|------|--------|------|--------|
| 全连接 mesh | O(n²) | 最低（直连） | n<10 |
| Central hub | O(n) | +1 hop | hub 高可用问题 |
| Sidecar mesh | O(n²)（proxy 层） | +0.5 hop | 部署重，但成熟 |
>
> 当前项目定位 5 节点，全连接 mesh 是最简单的方案，不再继续往上走了。

#### Q: subscribe_loop 里 redisGetReply 是阻塞的，断连了怎么办？这期间集群事件丢失不要紧吗？

> A: 确实是阻塞调用——subscribe_loop 整个线程挂在 `redisGetReply` 上等 Redis 推消息。断连后当前代码有简单的重连逻辑：
>
> ```cpp
> if (rv != REDIS_OK || !reply) {
>     redisFree(g_sub_ctx);
>     g_sub_ctx = redisConnect(...);
>     redisCommand(g_sub_ctx, "SUBSCRIBE user_status group_chat cluster:node_join");
> }
> ```
>
> 断连期间丢失的事件确实收不到，但后果可控：
> - `user_status` 事件丢失 → 其他节点不知道某个用户下线，会继续尝试 RPC 转发消息到本节点，我们收到 `remote_deliver` 后发现用户不在，返回失败。不影响正确性，只是浪费一次 RPC。
> - `cluster:node_join` 事件丢失 → 新节点上线后老节点没感知到。但只要新节点在线，老节点下次重启（或者有心跳同步机制）就能从 `HGETALL cluster:nodes` 补全。
>
> 生产化改进的话，应该加一个周期性同步机制：比如每 30 秒读一次 `cluster:nodes` 对比当前 peer 连接列表，缺失的就补连。这样即使事件丢失，最长 30 秒也能自动修复。这也是一种**最终一致性**的实践。

#### Q: peer 连接断了有自动重连吗？

> A: 当前没有。`connect_peer()` 只在启动时和新节点加入时被调用一次。如果这条连接因网络问题断开，不会被自动重建——这是一个明确的缺陷。
>
> 要修复的话，应该在 peer 连接上设置 `closed_callback`，触发时自动重新连接：
> ```cpp
> conn->set_closed_callback([node_id, host, port](auto) {
>     // 延迟重连，避免频繁重试
>     auto timer = std::make_shared<asio::steady_timer>(...);
>     timer->expires_after(1s);
>     timer->async_wait([node_id, host, port](auto) {
>         connect_peer(node_id, host, port);
>     });
> });
> ```
>
> 为什么没做？因为压测和 demo 场景下，节点都在本地或同一 Docker 网络内，连接几乎不会断。放到公网或跨机房部署，这是必须修的。

#### Q: Nginx 本身成了单点，怎么办？

> A: 确实 Nginx 在当前架构里是单点——所有连接都走 8877 端口，Nginx 挂了全部客户端都连不上。但在项目当前的定位下（5 节点本地/单机 Docker 部署），这不是问题。
>
> 生产化的标准方案有两种：
>
> 1. **Keepalived + VIP**：两台 Nginx 做主备，通过 VRRP 共享一个虚拟 IP。主挂了 VIP 漂移到备机，客户端重连即可。这是最传统也最成熟的做法。
> 2. **DNS 轮询 + 多入口**：客户端内置多个 IP:Port，轮询/随机选一个连接，挂一个就换一个。省掉 Nginx 层，客户端直连后端——但这样服务发现的职责就得各节点自己承担了。
>
| 方案 | 复杂度 | 切换延迟 | 成本 |
|------|--------|---------|------|
| Keepalived + VIP | 中 | 秒级 | 多一台备机 |
| DNS 轮询 | 低 | 取决于 DNS TTL | 零额外硬件 |
| 客户端直连 + 多 IP | 高（改客户端） | 秒级 | 零 |

---

### 扩展追问：横向对比

#### Q: 如果不用 Redis，换成 etcd 做服务发现会怎样？

> A: 这是两个层面的东西。etcd 基于 Raft 做线性一致，每次写入要大多数节点确认，单机 QPS 大概 ~1 万级别，比 Redis 慢一个数量级。etcd 的 watch 机制比 Pub/Sub 可靠——不会丢事件。但代价是运维成本高：etcd 本身要维护 3 或 5 节点的 Raft 集群。
>
> 在我们的场景里，服务发现对强一致的需求很小——节点上下线延迟几秒感知完全不影响正确性。用 Redis Pub/Sub + 周期性全量同步就够了。**选型的原则是"用合适的一致性换性能"**。
>
| 特性 | Redis Pub/Sub | etcd |
|------|-------------|------|
| 一致性 | 最终一致 | 线性一致 |
| QPS | 10 万+ | ~1 万 |
| 事件可靠性 | 订阅者离线丢失 | Watch 有历史 buffer |
| 运维成本 | 低（已依赖 Redis） | 中（独立 Raft 集群） |

#### Q: 如果不用 Nginx，用 HAProxy 做 L4 代理呢？

> A: 两者都可以做 L4 TCP 代理，差异在细节：
>
> - **性能**：HAProxy 在 L4 代理场景公认比 Nginx 稍快（Nginx 的 Stream 模块晚于 HAProxy，优化程度略低），但差别在个位数百分比级别，不足以为此换掉 Nginx
> - **配置**：Nginx 的 stream 配置更简洁，和我们的 Docker 部署打通（镜像就是 nginx:alpine），HAProxy 要额外维护一套配置语法和运维工具
> - **健康检查**：HAProxy 原生支持 TCP 健康检查（检查连接是否建成功），Nginx Stream 的健康检查需要 nginx-plus（商业版）或者额外脚本
>
> 在我们项目里选 Nginx 的原因很简单：**团队熟悉 Nginx，配置 10 行搞定，够用**。如果上生产环境做更精细的流量管理，HAProxy 也是合理的选择。

#### Q: 为什么不考虑 gRPC 的双向流来做推送？反而用 RPC async_call？

> A: gRPC 双向流确实适合推送场景——服务端可以随时通过 stream 把消息推给客户端，不需要客户端轮询。
>
> 但代价是引入了额外的复杂度：
> - 双向流需要全双工连接管理，每条连接上 multiplex 多个 stream
> - gRPC 的 HTTP/2 头压缩、流控、TLS 握手——这些都是开销
> - 如果客户端连接断开了，stream 断了需要重建
>
> 我们的方案是**对称 RPC**：客户端登录后，服务端持有客户端的 `connection` 指针。要推送消息时，直接 `conn->async_call("on_message", ...)`——对连接来说，服务端和客户端是对等的，都可以发起调用。这比双向流轻量得多：
>
> ```cpp
> // 服务端主动推消息（不需要客户端轮询，不需要双向流）
> conn->async_call([](auto...) {}, "on_message", from_user, msg, seq_id, ts);
> ```
>
> 本质上这就是"服务端调客户端"的 RPC，用同一套 call/async_call 机制做的。比起双向流少了 stream 的管理状态，比起轮询少了无意义的请求。
>
> **和 gRPC 双向流对比**：
>
| 维度 | mRPC 双向 RPC | gRPC 双向流 |
|------|--------------|-------------|
| 协议 | 自定义 TCP | HTTP/2 |
| 推送机制 | async_call("on_message") | Write() on stream |
| 连接管理 | 对称，双方平等 | client/server 角色分明 |
| 开销 | 低（24 字节 header） | 较高（HTTP/2 帧） |
| 生态 | 自研 | 多语言代码生成 |

#### Q: 如果把这个系统改成 K8s 部署，架构里的哪些组件可以用 K8s 原生能力替代？

> A: K8s 天然覆盖了我们的两个组件：
>
> **① Nginx 被 K8s Service 替代**
>
> 对应关系：
> - `Service`（type: ClusterIP 或 NodePort）替代 Nginx 做 L4 负载均衡
> - `kube-proxy` 的 iptables/IPVS 规则替代 least_conn（IPVS 支持 wrr、lc、sh 等调度算法）
> - `readinessProbe` + `livenessProbe` 替代健康检查
> - 去掉最外层的 Nginx，pod-to-pod 直接走 ClusterIP 通信
>
> **② Redis Pub/Sub 服务发现被 K8s DNS + Headless Service 替代**
>
> - K8s 里 pod 用 StatefulSet 部署，每个 pod 有固定的 DNS 名称（server-0.svc.cluster.local, server-1.svc...）
> - 启动时通过 DNS SRV 记录或 `headless service` 自动发现所有 peers
> - 不需要 Redis Pub/Sub 做集群发现——K8s 的服务注册是基础设施层的
>
> **③ 不能被替代的组件**
>
> - **Redis 的用户位置缓存 + 状态广播**：这是业务层的数据，不是基础设施。`user_location` 存储用户登录在哪台 pod，`user_status` 频道做在线状态广播——这些 K8s 管不了
> - **MySQL 和批量写入**：数据持久化层，K8s 只负责调度 pod，底层存储仍然要挂载 PV 或使用 RDS
>
| 组件 | K8s 替代方案 | 说明 |
|------|-------------|------|
| Nginx | Service + kube-proxy | IPVS 调度 |
| Redis 服务发现 | StatefulSet DNS + Headless Service | 自动 DNS 解析 |
| Redis 位置缓存 | 无（仍需 Redis） | 业务数据，非基础设施 |
| MySQL | StatefulSet + PV / 云 RDS | 数据持久化 |
>
> 迁移到 K8s 后，原来最复杂的"手动配 Nginx + Redis 发现"变成了 "kubectl apply -f deployment.yaml"——这也是 K8s 的核心理念：让基础设施层面的问题不再需要业务层代码去解决。

#### Q: 如果现在不只有 5 节点，要扩展支持 1000 万用户，这个架构最大的瓶颈在哪？

> A: 这个问题从当前架构出发，逐层升级：
>
> **第一层：MySQL 写入瓶颈**
> 当前 20k QPS 下 MySQL 还能撑。1000 万用户，假如 DAU 10%、每人每天 50 条消息 = 5000 万条/天 ≈ 580 QPS——MySQL 8.0 完全扛得住。但如果峰值（比如节日群发）到 10 万 QPS，就需要分库分表或换分布式数据库（TiDB）。
>
> **第二层：Redis 单机瓶颈**
> 当前 Redis 承担三个职责：token 管理、在线状态、用户位置。单机 Redis 10 万 QPS 是上限。1000 万用户级别需要 Redis Cluster 或 Proxy 分片。
>
> **第三层：节点间 O(n²) 全连接**
> 100 个节点 = 4950 条 peer 连接，每个节点维护 99 条出站 + 99 条入站。虽然 Asio 扛得住这么多连接，但维护成本已经不可接受了。必须改成 central hub 或 Sidecar mesh。
>
> **第四层：消息推送风暴**
> 如果做全量广播（比如系统通知），N 个节点各广播一次 = N 条消息，每条消息每个节点推送给 M 个在线用户 = N × M 次推送。100 节点 + 每节点 2000 在线 = 20 万次推送。需要用**层级聚合**——只给每个节点推一次，节点内部再做本地广播。
>
> 也就是说：从 5 节点到 1000 万用户，存储层（分库分表）、缓存层（Redis Cluster）、通信层（非 O(n²)）、推送层（层级聚合）全部要换一轮。当前架构在 5 节点/几千同时在线的量级是合理的，再往上就是另一个系统的复杂度了。

---

### 并发优化与存储

#### Q: 简历写"引入雪花算法全局 ID 替代 Redis INCR"，解决了什么问题？两种方案怎么对比？

> A: 之前 seq_id 生成走 `Redis INCR global:msg_seq`，每条消息发出去之前先等 Redis 返回递增后的 ID。一次网络 RTT（本机 0.2-0.5ms）在单条消息 2-5ms 的链路里占比很可观。而且 Redis 如果挂了，seq_id 也拿不到，整个发消息流程就卡死了。
>
> 换 Snowflake 后在本地生成，0 网络开销：
> ```cpp
> uint64_t next_id() {
>     uint64_t now = now_ms() - EPOCH_MS;      // 41-bit ms 时间戳
>     // ...seq_ 自增 + 自旋等下一毫秒...
>     return (now << 22) | (node_id_ << 12) | seq_;
> }
> ```
>
> | 方案 | 每次调用 | 依赖 | 上限 |
> |------|---------|------|------|
> | Redis INCR | 1 次网络 RTT | Redis 必须可用 | Redis QPS 上限 |
> | **Snowflake** | **本地计算，0 网络** | **无外部依赖** | **4096 ID/ms/节点** |
>
> Snowflake 的代价是时钟回拨问题。当前代码没有处理——如果系统时间往回跳，可能生成重复 ID。生产环境可以用 NTP 的 `-x` 选项（渐进式校准不回跳），或者在检测到回拨时等待直到超过上次记录的时间。

#### Q: 消息离线投递 20k+ QPS 是怎么测出来的？什么场景叫"离线"？

> A: 见 `stress_test.cpp` 的 `test_message_throughput()`，50 对用户（100 个账号）持续 15 秒互相发消息，每对里的"接收方"不在线（未登录）。
>
> 那为什么"离线"反而 QPS 高呢？因为接收方不在线时消息投递路径变短了：
>
> ```
> ✅ 离线场景（20,269 QPS）:
> send_message → Snowflake → batch_saver.push → deliver_message
>     → 查 g_local_users → 不在 → 查 g_location_cache → 不在 → Redis HGET → 不在线 → 结束
>     全部本地操作，无跨节点 RPC
>
> ❌ 跨节点在线场景（10,825 QPS）:
> send_message → Snowflake → batch_saver.push → deliver_message
>     → 查 g_local_users → 不在 → 查 g_location_cache → node-5
>     → RPC remote_deliver(node-5) → 序列化 → 网络传输 → node-5 反序列化
>     → 查 g_local_users → 找到 → async_call on_message
>     + 响应再返回
>     多出一次完整跨节点 RPC
> ```
>
> 所以"20k 离线"和"10.8k 跨节点"之间差一倍，本质上就是差了一次跨节点 RPC 的序列化 + 网络往返。
>
> 还有人会问："那为什么 echo 基准有 54k，消息转发才 20k？" 区别在于：echo 只处理一个 string 参数，消息转发有 from+to+msg+seq_id+timestamp 五个字段的序列化 + Snowflake next_id + batch_saver.push + 三级路由查询。业务逻辑的每一步都有开销。

#### Q: MySQL 连接池 + 批量异步写入具体是怎么实现的？连接池为什么设 16？

> A: 两个组件配合：
>
> **MySQL 连接池（mysql_save.hpp）**:
> ```cpp
> // pool_size = min(hardware_concurrency(), 16)，本机 16
> // borrow() 时队列为空则阻塞等待
> // release() 后 condition_variable 通知等待者
> // RAII 封装 AutoConn，保证用完后自动归还
> ```
>
> **批量异步写入（batch_saver.hpp）**:
> ```cpp
> // 后台独立线程 flush_loop
> // push() 将消息追加到内存队列，满 50 条或 100ms 超时触发
> // flush 时一次性 BEGIN + 多行 INSERT + COMMIT
> ```
>
> 为什么是 50 条 / 100ms？平衡点：
>
| 参数 | 设太小 | 设太大 |
|------|--------|--------|
| batch 大小 | 事务频繁，QPS 低 | 单事务太大，超 max_allowed_packet |
| 超时时间 | 攒不够就刷，变相退化为单条写 | 宕机丢太多，离线消息延迟高 |
>
> 50 条不超过 MySQL 默认 64MB 的 max_allowed_packet，100ms 延迟用户体感不明显。之前 SQLite 串行阶段每条消息一次 INSERT，QPS 只有 170——同一条 INSERT 用批量方式包在事务里，QPS 直接跳到 20k+。**这就是"攒批"的威力**。
>
> 连接池 16 的依据：`std::min(hardware_concurrency(), 16)`。本机 16 核所以 16 个连接。压测时 CPU 瓶颈在序列化不在 MySQL（20k QPS 时 MySQL 不积压），16 够用。

#### Q: 构建索引具体做了什么？为什么它们能缓解 MySQL 瓶颈？

> A: `MySqlSaver::init()` 里创建了这些索引：
> ```cpp
> // messages 表主键 + 复合索引
> PRIMARY KEY (seq_id, from_user, to_user)
> INDEX idx_conv (from_user, to_user, seq_id)
>
> // friend_requests 索引
> INDEX idx_fr_to (to_user)
> INDEX idx_fr_from (from_user)
>
> // friends 表主键 + 索引
> PRIMARY KEY (user_a, user_b)
> INDEX idx_fb (user_b)
> ```
>
> 最关键的索引是 `idx_conv`——消息拉取和未读查询都用它：
> ```sql
> -- 拉取历史消息
> SELECT ... FROM messages WHERE
>   (from_user='A' AND to_user='B') OR (from_user='B' AND to_user='A')
>   ORDER BY seq_id DESC LIMIT 20;
>
> -- 未读查询用同样的条件 + seq_id > after_seq
> ```
>
> 没有 `idx_conv` 的话，这个查询要走全表扫描。1TB 的消息表扫描一次几秒甚至几分钟。有了索引后只需要在 `(from_user, to_user, seq_id)` 的 B+ 树上做 range scan，百万级消息也是毫秒级响应。
>
> 在 20k QPS 的写入压力下，索引维护（INSERT 时更新 B+ 树）当然有开销，但相比全表扫描的查询开销，索引的收益远大于成本。压测时瓶颈在序列化，不在 MySQL 层面，说明索引策略目前是到位的。

#### Q: 从 170 msg/s（SQLite 串行）到 20k QPS（MySQL + 批量），这 100 倍的飞跃是怎么做到的？

> A: 这个对比来自架构演变的阶段三到阶段四的跨越。170 msg/s 是从**每条消息串行 3 次 RPC + 2 次 Redis + 1 次 SQLite INSERT** 这么低效的路径测出来的。
>
> 具体落差别看每个环节：
>
> | 环节 | 阶段三（SQLite） | 阶段四（MySQL+批量） | 效果 |
> |------|----------------|--------------------|------|
> | user_exists | RPC 查 SQLite（网络等） | **去掉** | +52% |
> | seq_id | Redis INCR（网络 RTT） | **Snowflake 本地生成** | 0 网络 |
> | 写入 | RPC + SQL INSERT（串行等磁盘） | **内存入队立即返回 + 后台批量写** | +数十倍 |
> | 用户位置 | 每次都 Redis HGET | **三级缓存优先，零 Redis 命中** | 0 网络（缓存命中时） |
> | 总网络往返/条 | 3-4 次 | **0-1 次** | — |
>
> 最关键的一点：**把同步写入变成了异步写入**。消息的存储路径从"等待磁盘确认再返回"变成了"入内存队列就返回，后台线程慢慢往 MySQL 刷"。用户感知到的延迟从毫秒级的 write wait 降到了微秒级的 queue push。20k QPS 下 MySQL 的 CPU 并不饱和——意味着瓶颈还没到存储层，在序列化和路由上。

---

### 扩展追问：深挖项目

#### Q: Snowflake 的 node_id 从节点名称里解析"node-1"→1，"node-5"→5，如果部署时命名不规范（如"node-a"）呢？

> A: 目前 `parse_node_id` 的实现是找最后一个 `-` 后面的数字，如果解析失败就返回 0。这确实是个隐形的故障点——所有 node_id=0 的节点生成的 ID 在序列号段上会冲突（虽然时间戳段不同，极端情况下会重复）。
>
> 生产化改进方向：
> 1. 环境变量显式指定 `NODE_ID`，不依赖命名解析
> 2. 启动时用 Redis `SETNX` 做 node_id 分配——启动时抢一个没被占用的 ID
> 3. 或者直接用 `std::hash(hostname) % 1024` 做 hash 分配，大概率不冲突
>
> 当前是 demo 级别，手动保证 5 个节点命名规范不成问题，但上生产必须改成方案 1 或 2。

#### Q: Snowflake 的回拨问题怎么处理？万一时钟跳了，确实会生成重复 ID 吗？

> A: 当前代码没有处理时钟回拨：
> ```cpp
> if (now == last_ms_) { seq_++; }       // 正常步进
> else { seq_ = 0; last_ms_ = now; }     // 新的毫秒，seq_ 重置
> ```
>
> 如果 `now_ms()` 返回的值比 `last_ms_` 小（时钟回拨），`seq_` 会被重置为 0 且 `last_ms_` 被设为更早的时间。后续产生的 ID 就可能和回拨前同一毫秒内的 ID 重复。
>
> 几种处理方案：
>
| 方案 | 实现难度 | 效果 |
|------|---------|------|
| **等时间追上来** | 低 | 阻塞直到 `now >= last_ms_`，简单可靠但会短暂 hang |
| **记录上次回拨** | 中 | 检测到回拨后切换备用 seq 区间或用 reserve ID |
| **NTP -x 选项** | 零 | 渐进式校准，不会回跳，但需要运维保障 |
|
> 业界通用的做法是方案 1（等待）配合 NTP 的 `-x`。Twitter 的原始 Snowflake 实现也是这种思路——最多等几毫秒而已，对系统整体影响很小。

#### Q: 连接池是 borrow() 时队列为空就阻塞等待，那如果所有连接都被占满，请求会积压很大延迟。怎么避免？

> A: 确实有这个问题——`borrow()` 的 `cv_.wait()` 是阻塞的，如果 16 个连接全被占用，第 17 个请求的线程就挂住了。
>
> 目前在 batch 模式下，`save()` 由单一的 flush 线程调用，不存在多线程抢连接的情况——所以**没有并发问题**。但其他操作（`register_user`、`verify_user`、`search_users` 等）是在 server 的 io_context 线程上直接调用的`borrow()`，如果这些操作频繁且慢，确实可能耗尽连接池。
>
> 改进方向：
> 1. **加超时**：`cv_.wait_for(lock, 5s)`，超时后返回错误，避免线程无限等
> 2. **动态扩容**：当等待时间超过阈值时临时创建新连接，用完后放回池子
> 3. **异步化**：io_context 的 handler 不应该阻塞等连接，应该用 async callback / future 做异步查询
>
> 当前架构里消息写入走批处理不担心，其他操作频率低且快（微秒级），所以 16 连接够用。

#### Q: "去掉 user_exists" 这个优化只在特定场景成立，如果从客户端直接调 send_message 岂不是可以给不存在的用户发消息？这不安全。

> A: 确实。这个优化的**前提假设**是"客户端注册用户后才能发消息，对方必然是存在的"。仔细想这个假设是否成立：
>
> - 正常流程：注册 → 加好友 → 发消息，target 一定是存在用户
> - 但如果有恶意客户端伪造请求、或者服务端 API 暴露了 send_message 给未认证的连接，确实可能出现发给不存在用户的情况
>
> 所以正确的优化不是"删掉 user_exists"，而是**把它从同步路径上移到更合适的位置**：
> 1. 发送时不做校验（用户存在性由注册和好友关系保证）
> 2. 存入 MySQL 后、异步 flush 之前不做校验
> 3. 如果真要防君子，可以在注册入口做校验，发送链路信任前置校验
>
> 这是一个典型的"性能 vs 安全性"权衡。我们的场景是 Demo + 压测，去掉了无问题。生产环境应该用方案 2 或保留校验但走缓存（Redis SET 存合法用户列表），避免每次消息查 MySQL。

#### Q: 批量写入宕机丢 50 条，能不能做到完全不丢？

> A: 当前设计是 50 条或 100ms 刷一次。消息入内存队列就返回成功——客户端收到 seq_id 就认为发送完成了。如果在攒批期间服务器崩了，这批消息就丢了。
>
| 方案 | 可靠性 | 性能影响 | 实现成本 |
|------|--------|---------|---------|
| 当前（内存 batch） | 最多丢 50 条 | 高（攒批） | 低 |
| WAL（预写日志） | 不丢 | 中（多一次磁盘写） | 中 |
| 双写 + 确认 | 不丢 | 低（两倍网络+磁盘） | 高 |
|
> WAL 是常用的改进：push() 时先追加到本地磁盘日志（顺序写，微秒级），后台 flush 成功后标记删除。启动时回放未刷出的 WAL。这样即使宕机，重启后数据完整——代价是每条消息多一次磁盘 write，但顺序写比随机写快得多，对吞吐影响不大。
>
> 不过 IM 场景通常接受**宽松的可靠性**——TCP 保证字节不丢，但不保证应用层消息不丢。丢失最近几十条通常是可接受的（用户刷新即可）。如果是金融交易或支付，那是另外一回事了。

---

### 扩展追问：横向对比

#### Q: 你这里用的是自增 seq_id 加 Snowflake，业界常见的 ID 方案还有哪些？差异在哪？

> A: 常见方案分三类：
>
| 方案 | 代表 | 优点 | 缺点 |
|------|------|------|------|
| 自增 ID | MySQL AUTO_INCREMENT | 简单、有序 | 依赖 DB、分片后复杂 |
| 分布式 ID 生成器 | **Snowflake**、美团 Leaf、百度 uid-generator | 去中心化、高性能 | 时钟问题、位数有限 |
| UUID | UUIDv4 | 完全去中心 | 128bit 太长、无序（B+ 树插入慢） |
|
> Snowflake 很适合 IM 场景——40 多万年不重复、本地生成、天然有序（按时间排序）。相比之下 UUID 是 128 位且完全乱序，当主键时导致 MySQL B+ 树频繁页分裂，写入性能差 3-5 倍。
>
> 美团 Leaf 在 Snowflake 上做了改进：通过 ZK 分配 workerId，解决了节点命名冲突问题。百度 uid-generator 则使用数据库号段分配，避免时钟依赖。当前项目量级下 Snowflake 够用了，再往上推可以预分配一段 ID 序列来减少互斥锁争用。

#### Q: 为什么选了 MySQL 而不是 TiDB / PostgreSQL / 单机 Redis 全量存储？

> A: 选型的核心约束：项目是 C++ 栈，hiredis / mysqlclient 已经是依赖了，再加新存储需要重写适配层。
>
| 方案 | 为什么不选 |
|------|-----------|
| **Redis 全量** | 内存成本太高（1TB 消息 ≈ 几十万/月），没有范围查询能力 |
| **PostgreSQL** | 需要 libpq，和 mysqlclient 差不多的能力，没有质变的优势 |
| **TiDB** | 太重，5 节点 Demo 部署不需要分布式 NewSQL |
| **SQLite（改良）** | 再优化也突破不了单线程写入上限（~5k TPS），无法并发 |
>
> MySQL 16 连接并发写 + 批量攒批，是当前场景下性价比最高的方案。如果数据量上去了（十亿级），TiDB 或 TiFlash 是自然的迁移路径。

#### Q: 换成 MongoDB / 时序数据库之类的方案会不会更适合 IM 存储场景？

> A: IM 消息存储的核心负载特征：**写多、读少、按时间范围查询**。每条消息写一次（append-only），读是用户打开聊天时拉最近几十条，历史记录翻页也是顺序的 range scan。
>
| 数据库 | 写入模式 | 范围查询 | 运维 |
|--------|---------|---------|------|
| MySQL（InnoDB） | 行锁 + B+ 树插入 | ✅ 强（聚簇索引） | 成熟 |
| MongoDB | 文档插入 | ✅（默认 _id 有序） | 较重 |
| 时序 DB（InfluxDB） | 最优（LSM-Tree） | ✅ | 不擅长大字段 |
|
> 从纯场景匹配度看，MongoDB 的文档模型（一条消息一个 doc）比 MySQL 的行模型更自然。但考虑到我们的技术栈已经是 C++ + libmysqlclient，改动存储层需要重新设计序列化、连接池、查询接口——收益不足以覆盖改造成本。
>
> 一句话：**MySQL 不是 IM 存储的最佳方案，但它是在当前约束下的最优选择**。

#### Q: 20k QPS 是离线场景，如果峰值流量是这个的 10 倍（200k QPS），架构哪些部分先撑不住？

> A: 从大概率出问题到小概率排队：
>
> **① 攒批队列积压**：batch_saver 的内存队列是无界 `std::vector`，入队列快（微秒级）但出队列慢（MySQL 写入 ~100μs/批）。200k QPS 下每秒 200k 条入队，MySQL 每秒只能写约 200 批（一批 50 条 = 10k 条/秒），积压速度 > 消耗速度，内存很快爆掉。需要加**背压**——队列长度上限，超了就让客户端重试。
>
> **② 连接池耗尽**：200k QPS 下 16 个 MySQL 连接每秒每连接处理 ~12,500 次 INSERT。如果 batch 的 flush 线程占用了大部分连接，其他读操作（用户注册、好友查询）的延时会飙升。
>
> **③ 网络带宽**：每条消息约 1KB payload + 序列化开销 ≈ 1.5KB wire。200k × 1.5KB ≈ 300MB/s ≈ 2.4Gbps。千兆网卡先打满，万兆网卡还能撑。
>
> **④ Nginx epoll 循环**：200k QPS + 数千在线连接，Nginx 的事件循环和 least_conn 调度也会成为瓶颈，可以考虑换成更轻量的 TCP 代理。
>
> 总结改进优先级：
>
> | 问题 | 优先级 | 方案 |
> |------|--------|------|
> 批处理积压 + OOM | P0 | 队列加背压上限 |
> MySQL 连接池不够 | P1 | 扩容到 32-64 |
> 网络带宽 | P2 | 升级万兆 / 压缩 payload |
> Nginx 瓶颈 | P3 | HAProxy / DPDK |
