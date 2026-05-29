# Asio-mRPC — 分布式即时通信系统

基于自研异步 RPC 框架的分布式即时通信系统，采用三层架构（数据持久化 + 业务转发 + 网关），支持水平扩展、离线消息、实时推送。

## 系统架构

```
┌─────────────────────────────────────────────────────────┐
│                    客户端 (chat_gui / chat_client)        │
│                  ImGui+SDL2 / Terminal                    │
└──────────────────────┬──────────────────────────────────┘
                       │ TCP (mRPC 协议)
┌──────────────────────▼──────────────────────────────────┐
│         Nginx Stream (L4 负载均衡, least_conn)            │
│                  端口 8877                                │
│          后端: 8881 / 8882 / 8883 / 8884 / 8885          │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────┐
│            server_node × 5 (业务转发层)                    │
│      无状态水平扩展 / Redis 节点发现 / 消息转发             │
│      注册: login, send_message, sync_messages ...          │
│      全局 ID: Snowflake 本地生成                           │
│      存储: 批量异步写入 MySQL（攒批 50 条 / 100ms）        │
└──────┬────────────────────────────┬─────────────────────┘
       │ Redis Pub/Sub              │ RPC                  │
┌──────▼────────┐          ┌───────▼──────────────────────┐
│  sqlite_service│          │        Redis                  │
│  (数据持久化)   │          │  user_location / 在线状态    │
│  用户/好友 RPC  │          │  Pub/Sub / 集群节点发现      │
└──────┬────────┘          └──────────────────────────────┘
       │ MySQL
┌──────▼────────┐
│    MySQL 8.0   │
│   messages /   │
│   users /      │
│   friends      │
└───────────────┘
```

## 技术栈

| 层 | 技术 |
|---|---|
| 语言 | C++17/20 |
| RPC 框架 | 自研 mRPC（Asio + 模板元编程，header-only） |
| 网络模型 | Reactor + io_context 线程池（32 线程） |
| 序列化 | MessagePack（默认）/ JSON / CBOR（nlohmann/json） |
| 注册发现 | Redis Pub/Sub + Hash |
| 数据持久化 | MySQL 8.0（连接池 + 批量异步写入） |
| 全局 ID | Snowflake 算法（本地生成，去中心化） |
| 负载均衡 | Nginx Stream TCP 代理（least_conn） |
| 客户端 GUI | ImGui + SDL2 + OpenGL |
| 容器化 | Docker + docker-compose |

## 性能指标

| 场景 | QPS | 平均延迟 |
|------|-----|---------|
| RPC 框架基准（echo, 16 连接, 1KB） | **54,000+** | 0.91 ms |
| 框架单次 RPC 开销（裸二进制） | 135,000 | 0.37 ms /**7.4 μs** |
| 消息离线投递（去冗余校验） | **20,000+** | 2.46 ms |
| 跨节点在线转发（接收方在另一节点） | **10,800+** | 4.57 ms |

> 详细测试数据见 [测试报告](build/测试报告.md)

## 项目特点

- **自研 RPC 框架**：单头文件 + 600 行核心代码，支持 `call`/`async_call`/`notify`/`coro_call`
- **三层分布式架构**：数据持久化 + 无状态业务转发 + 网关，各层独立水平扩缩容
- **批量异步写入**：MySQL 攒批写入（50 条 / 100ms 超时），消除同步 I/O 瓶颈
- **去中心化全局 ID**：Snowflake 替代 Redis INCR，消除网络 RTT 和单点故障
- **三级缓存路由**：本地内存 → 全局缓存 → Redis 回退，Redis 不可用时降级保障
- **跨节点消息转发**：节点间 RPC 投递，支持 5+ 节点水平扩展
- **Docker 一键部署**：docker-compose 启动 5 节点集群 + MySQL + Redis + Nginx

## 功能清单

- [x] 用户注册/登录/Token 自动登录
- [x] 单聊实时消息推送
- [x] 群聊（Redis Pub/Sub 广播）
- [x] 离线消息存取 + 未读计数
- [x] 历史聊天记录分页拉取
- [x] 好友系统（搜索/请求/接受/拒绝）
- [x] 在线/离线状态广播
- [x] 多节点水平扩展 + 跨节点转发
- [x] GUI 客户端（ImGui + SDL2）
- [x] 终端 CLI 客户端
- [x] Docker 容器化部署

## 快速开始

### 本地构建

```sh
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc) server_node sqlite_service chat_client
```

生成的可执行文件在 `bin/` 目录下。

### 启动（五节点集群）

```sh
# 1. 启动 MySQL + Redis
redis-server &
# MySQL 需在 3306 端口运行，默认用户 chat_user/chat_pass

# 2. 启动数据持久化服务
./bin/sqlite_service 7777 &

# 3. 启动 5 个业务节点
./bin/server_node node-1 8881 &
./bin/server_node node-2 8882 &
./bin/server_node node-3 8883 &
./bin/server_node node-4 8884 &
./bin/server_node node-5 8885 &

# 4. 启动 Nginx 负载均衡（配置见 docker/nginx.conf）
nginx -c /path/to/docker/nginx.conf

# 5. 启动客户端（通过 Nginx 连接集群）
./bin/chat_client 127.0.0.1 8877
```

### Docker 部署

```sh
cd docker && docker compose up -d
# 服务端 8 个容器（mysql / redis / nginx / sqlite-service / 5×server_node）
# 客户端通过宿主机 8877 端口连接
```

## 项目结构

```
.
├── include/mrpc/              # mRPC 核心框架（header-only）
│   ├── server.hpp             # 服务端入口（Reactor 线程池）
│   ├── client.hpp             # 客户端入口
│   ├── connection.hpp         # TCP 连接 + RPC 调用接口
│   ├── router.hpp             # 路由 + 反射调用
│   ├── coroutine.hpp          # 协程支持
│   └── function_traits.hpp    # 函数特征编译期萃取
├── chatting_room/             # 分布式聊天应用
│   ├── server_node.cpp        # 业务转发节点（主服务）
│   ├── sqlite_service.cpp     # 数据持久化服务
│   ├── batch_saver.hpp        # MySQL 批量异步写入
│   ├── mysql_save.hpp         # MySQL 连接池 + 查询封装
│   ├── snowflake.hpp          # Snowflake 全局 ID 生成
│   ├── redis_inbox.hpp        # Redis 封装（token/状态）
│   ├── stress_test.cpp        # 压测工具
│   ├── echo_server.cpp        # RPC 基准测试服务端
│   ├── client_gui.cpp         # ImGui GUI 客户端
│   ├── client.cpp             # 终端 CLI 客户端
│   └── logger.hpp             # 日志配置
├── third/                     # 第三方依赖（header-only）
├── docker/                    # Docker 容器化配置
│   ├── Dockerfile             # 多阶段构建
│   ├── docker-compose.yml     # 容器编排（5 节点集群）
│   └── nginx.conf             # Nginx Stream 配置
├── docs/                      # 文档
│   ├── 面试FAQ.md             # 面试高频问题
│   ├── 架构演变.md             # 架构演进历程
│   └── ARCHITECTURE.md        # 原始架构设计
├── logs/                      # 运行日志
└── bin/                       # 编译输出
```

## 关于 mRPC 框架

mRPC 是项目自研的轻量级 C++ RPC 框架，核心特性：

- **双向 RPC**：客户端与服务端地位对等，均可被调用
- **多调用风格**：`call`（同步）、`async_call`（Future / 回调）、`notify`（单向通知）、`coro_call`（协程）
- **多序列化格式**：JSON / BJSON / UBJSON / MsgPack / CBOR / RAW，运行时切换
- **编译期函数萃取**：`function_traits` 自动识别参数类型，无需手动注册
- **全异步 Reactor 模型**：io_context 线程池，32 线程管理数千连接

## 依赖

| 依赖 | 用途 | 是否必须 |
|------|------|---------|
| asio 1.18+ | 异步网络库 | 是（header-only） |
| nlohmann/json | JSON 序列化 | 是（header-only） |
| spdlog | 日志 | 是（header-only） |
| hiredis | Redis 客户端 | 服务端必须 |
| MySQL 8.0 | 数据持久化 | 服务端必须 |
| Nginx | TCP 负载均衡（可选，多节点需配置） | 可选 |
| SDL2 + OpenGL | GUI 客户端 | 可选 |
| SQLite3 | 历史兼容 | 可选 |

## 面试 FAQ

常见问题与深度解析见 [面试FAQ.md](docs/面试FAQ.md)。
