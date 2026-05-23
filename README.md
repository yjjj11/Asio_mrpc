# Asio-mRPC — 微服务即时通讯系统

基于自研异步 RPC 框架的分布式即时通讯系统，采用三层微服务架构，支持水平扩展、离线消息、实时推送。

## 系统架构

```
┌─────────────────────────────────────────────────────────┐
│                    客户端 (chat_gui / chat_client)        │
│                  ImGui+SDL2 / Terminal                    │
└──────────────────────┬──────────────────────────────────┘
                       │ TCP (mRPC 协议)
┌──────────────────────▼──────────────────────────────────┐
│              Nginx Stream (L4 负载均衡)                    │
│                  端口 8877 → 8888/8889                    │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────┐
│              server_node (业务路由层)                      │
│        无状态水平扩展 / Redis 节点发现 / 消息路由           │
│        注册 RPC: login, send_message, sync_messages ...   │
└──────┬────────────────────────────┬─────────────────────┘
       │ RPC                        │ RPC
┌──────▼────────┐          ┌───────▼─────────────────────┐
│ sqlite_service │          │        Redis                 │
│ (数据持久化层)  │          │  seq_id / token / 在线状态   │
│ SQLite 存储    │          │  pubsub / 节点发现 / 路由    │
└───────────────┘          └─────────────────────────────┘
```

## 技术栈

| 层 | 技术 |
|---|---|
| 语言 | C++20 |
| RPC 框架 | 自研 mRPC（Asio + 模板元编程） |
| 网络模型 | Reactor + 全异步非阻塞 IO |
| 序列化 | JSON / MsgPack / CBOR（nlohmann/json） |
| 注册发现 | Redis Pub/Sub + Hash |
| 数据持久化 | SQLite（sqlite_orm） |
| 负载均衡 | Nginx Stream TCP 代理 |
| 客户端 GUI | ImGui + SDL2 + OpenGL |

## 项目特点

- **自研 RPC 框架**：单头文件 + 600 行核心代码实现完整 RPC 通信，支持 `call`/`async_call`/`notify` 三种调用模型
- **三层微服务拆分**：数据持久化层 + 业务路由层 + 网关层，各层独立部署、独立水平扩缩容
- **离线消息同步**：per-conversation seq_id + 本地 cursor 增量拉取，断线重连消息不丢不重
- **跨节点消息路由**：Redis user_location 映射 + 节点间 RPC 转发，跨服务器精准投递
- **实时推送**：服务端主动 push 模型，毫秒级消息投递 + 在线状态通知

## 功能清单

- [x] 用户注册/登录/Token 自动登录
- [x] 单聊实时消息推送
- [x] 群聊（Redis Pub/Sub 广播）
- [x] 离线消息存取
- [x] 历史聊天记录分页拉取
- [x] 未读消息计数
- [x] 好友系统（搜索/请求/接受/拒绝）
- [x] 在线/离线状态通知
- [x] 多节点水平扩展
- [x] 跨节点消息转发
- [x] GUI 客户端（ImGui + SDL2）
- [x] 终端 CLI 客户端

## 快速开始

### 构建

```sh
mkdir build && cd build
cmake ..
make -j$(nproc)
```

生成的可执行文件在 `bin/` 目录下。

### 启动（四步）

```sh
# 1. 启动 Redis（需先安装）
redis-server &

# 2. 启动 SQLite 持久化服务
./bin/sqlite_service

# 3. 启动业务路由节点（可多开）
./bin/server_node node-1 8888
./bin/server_node node-2 8889   # 另一个终端

# 4. 启动客户端
./bin/chat_gui 8888             # GUI 客户端
# 或
./bin/chat_client 8888          # 终端客户端
```

## 项目结构

```
.
├── include/mrpc/              # mRPC 核心框架（header-only）
│   ├── server.hpp             # 服务端入口
│   ├── client.hpp             # 客户端入口
│   ├── connection.hpp         # TCP 连接封装 + RPC 调用接口
│   ├── router.hpp             # 路由与反射调用
│   ├── coroutine.hpp          # 协程支持
│   └── function_traits.hpp    # 函数特征编译期萃取
├── chatting_room/             # 聊天应用业务代码
│   ├── server_node.cpp        # 业务路由层（网关）
│   ├── server.cpp             # 旧版单体服务器
│   ├── sqlite_service.cpp     # 数据持久化服务
│   ├── sqlsave.hpp            # SQLite ORM 封装
│   ├── redis_inbox.hpp        # Redis 封装（seq_id/token/状态）
│   ├── client_gui.cpp         # ImGui GUI 客户端
│   ├── client.cpp             # 终端 CLI 客户端
│   └── logger.hpp             # 日志配置
├── third/                     # 第三方依赖
├── logs/                      # 运行日志
└── bin/                       # 编译输出
```

## 关于 mRPC 框架

mRPC 是项目自研的轻量级 C++ RPC 框架，核心特性：

- **双向 RPC**：客户端与服务端地位对等，均可被调用
- **多调用风格**：`call`（同步）、`async_call`（Future / 回调）、`notify`（单向通知）、`coro_call`（协程）
- **多序列化格式**：JSON / BJSON / UBJSON / MsgPack / CBOR，运行时切换
- **编译期函数萃取**：`function_traits` 自动识别参数和 `connection` 参数，无需手动注册
- **全异步**：基于 Asio `io_context` 的事件循环，单线程可管理千级连接

## 依赖

- asio 1.18+
- nlohmann/json 3.9+
- spdlog / wlog
- sqlite_orm
- hiredis（Redis C 客户端）
- SQLite3
- Nginx（可选，用于 TCP 负载均衡）
- SDL2 + OpenGL（可选，GUI 客户端）
