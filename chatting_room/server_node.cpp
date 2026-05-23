#include <logger.hpp>
#include <mrpc/server.hpp>
#include <mrpc/client.hpp>
#include <unordered_map>
#include <string>
#include <shared_mutex>
#include <vector>
#include <tuple>
#include <algorithm>
#include <sstream>
#include <thread>
#include <cstring>
#include <ctime>
#include <iostream>
#include <iomanip>
#include <random>
#include <memory>
#include <asio/signal_set.hpp>
#include <hiredis/hiredis.h>
#include "redis_inbox.hpp"

using namespace mrpc;

// ==================== 节点配置 ====================
std::string g_node_id = "node-1";

// ==================== 本地连接管理 ====================
std::unordered_map<std::string, std::shared_ptr<connection>> g_local_users;
std::shared_mutex g_local_mutex;

// ==================== 节点间连接 ====================
std::unordered_map<std::string, std::shared_ptr<connection>> g_peer_conns;
std::mutex g_peer_mutex;

// ==================== 直连 Redis ====================
redisContext* g_redis = nullptr;
redisContext* g_sub_ctx = nullptr;
std::mutex g_redis_mutex;

// ==================== Redis（seq_id + token） + SQLite ====================
RedisInbox g_inbox;
std::shared_ptr<connection> g_sqlite_conn;

// ==================== 连接追踪（优雅关闭用） ====================
std::vector<std::shared_ptr<connection>> g_all_connections;
std::mutex g_all_conn_mutex;

// ==================== 辅助函数 ====================

static std::string generate_token() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << dis(gen)
        << std::setw(16) << std::setfill('0') << dis(gen);
    return oss.str();
}

// ==================== 前向声明 ====================
static void mark_user_online(const std::string& username, std::shared_ptr<connection> conn);
static void mark_user_offline(const std::string& username);
static void deliver_message(const std::string& from, const std::string& to,
                            const std::string& msg, uint64_t seq_id, const std::string& ts);
static void deliver_notification(const std::string& target_user, const std::string& event,
                                 const std::string& payload);
static void subscribe_loop();
static void connect_peer(const std::string& node_id, const std::string& host, uint16_t port);

// ==================== 用户 RPC ====================

bool handle_register_user(connection::cptr conn,
                          const std::string& username, const std::string& password) {
    auto ret = g_sqlite_conn->call<bool>("register_user", username, password);
    return ret.error_code() == 200 && ret.value();
}

std::string handle_user_login(connection::cptr conn,
                              const std::string& username, const std::string& password) {
    auto ret = g_sqlite_conn->call<bool>("verify_user", username, password);
    if (ret.error_code() != 200 || !ret.value()) {
        LOG_WARN("用户 {} 登录失败：密码错误或用户不存在", username);
        return {};
    }

    // 生成 token
    std::string token = generate_token();
    g_inbox.save_token(token, username);

    // 标记上线
    mark_user_online(username, conn);
    LOG_INFO("用户 {} 上线（节点 {}）", username, g_node_id);
    return token;
}

std::string handle_token_login(connection::cptr conn, const std::string& token) {
    std::string username = g_inbox.verify_token(token);
    if (username.empty()) {
        LOG_WARN("token 登录失败：token 无效或已过期");
        return {};
    }

    // 刷新 TTL
    g_inbox.save_token(token, username);

    // 标记上线
    mark_user_online(username, conn);
    LOG_INFO("用户 {} token 自动登录（节点 {}）", username, g_node_id);
    return username;
}

bool handle_user_logout(connection::cptr conn,
                        const std::string& username, const std::string& token) {
    if (!token.empty()) {
        g_inbox.delete_token(token);
    }
    mark_user_offline(username);
    return true;
}

// ==================== 在线用户查询 ====================

std::vector<std::string> handle_get_online_users(connection::cptr conn) {
    std::lock_guard rlk(g_redis_mutex);
    redisReply* reply = (redisReply*)redisCommand(g_redis, "SMEMBERS global:online_users");
    if (!reply || reply->type != REDIS_REPLY_ARRAY) {
        if (reply) freeReplyObject(reply);
        return {};
    }
    std::vector<std::string> result;
    result.reserve(reply->elements);
    for (size_t i = 0; i < reply->elements; ++i) {
        if (reply->element[i]->type == REDIS_REPLY_STRING)
            result.emplace_back(reply->element[i]->str);
    }
    freeReplyObject(reply);
    return result;
}

// ==================== 消息发送 + 存储 + 路由 ====================

uint64_t handle_send_message(connection::cptr conn,
                             const std::string& from_user,
                             const std::string& to_user,
                             const std::string& message) {
    // 检查目标用户是否存在
    auto exists_ret = g_sqlite_conn->call<bool>("user_exists", to_user);
    if (exists_ret.error_code() != 200 || !exists_ret.value()) {
        LOG_WARN("发送消息给不存在的用户: {} -> {}", from_user, to_user);
        return 0;
    }

    // Per-conversation Seq ID
    uint64_t seq_id = g_inbox.seq_id(from_user, to_user);
    if (seq_id == 0) {
        LOG_ERROR("Redis seq_id 失败，消息丢失: {} -> {}", from_user, to_user);
        return 0;
    }

    std::string timestamp = std::to_string(std::time(nullptr));

    // 异步写入 SQLite（所有消息直接持久化，不再经过 Redis conv ZSET）
    {
        using MsgTuple = std::tuple<uint64_t, std::string, std::string, std::string, std::string>;
        std::vector<MsgTuple> msgs;
        msgs.emplace_back(seq_id, from_user, to_user, message, timestamp);
        g_sqlite_conn->notify("save_messages", msgs);
    }

    // 路由投递
    deliver_message(from_user, to_user, message, seq_id, timestamp);
    return seq_id;
}

bool handle_send_group_message(connection::cptr conn,
                               const std::string& from_user,
                               const std::string& message) {
    // 1. 推送给本节点用户
    {
        std::shared_lock lock(g_local_mutex);
        for (auto& [u, c] : g_local_users) {
            (void)u;
            c->async_call([](uint32_t, const std::string&, const nlohmann::json&){},
                          "on_group_message", from_user, message);
        }
    }

    // 2. Pub/Sub 广播给其他节点
    {
        std::lock_guard rlk(g_redis_mutex);
        redisCommand(g_redis, "PUBLISH group_chat %s|%s", from_user.c_str(), message.c_str());
    }
    return true;
}

// ==================== 消息查询 RPC ====================

auto handle_sync_messages(connection::cptr conn,
                          const std::string& username,
                          const std::string& partner,
                          uint64_t after_seq, size_t limit)
    -> std::vector<std::tuple<uint64_t, std::string, std::string, std::string, std::string>> {
    using MsgTuple = std::tuple<uint64_t, std::string, std::string, std::string, std::string>;
    using MsgVec = std::vector<MsgTuple>;

    // SQLite 作为唯一数据源
    if (after_seq == 0) {
        auto ret = g_sqlite_conn->call<MsgVec>("load_latest_messages", username, partner, limit);
        auto msgs = ret.error_code() == 200 ? ret.value() : MsgVec{};
        std::reverse(msgs.begin(), msgs.end()); // load_latest 返回 DESC，转为 ASC
        return msgs;
    }

    auto ret = g_sqlite_conn->call<MsgVec>("load_messages_after", username, partner, after_seq, limit);
    return ret.error_code() == 200 ? ret.value() : MsgVec{};
}

auto handle_sync_history(connection::cptr conn,
                         const std::string& username,
                         const std::string& partner,
                         uint64_t before_seq_id, size_t limit)
    -> std::vector<std::tuple<uint64_t, std::string, std::string, std::string, std::string>> {
    using MsgTuple = std::tuple<uint64_t, std::string, std::string, std::string, std::string>;
    using MsgVec = std::vector<MsgTuple>;
    auto ret = g_sqlite_conn->call<MsgVec>("load_messages", username, partner, before_seq_id, limit);
    auto msgs = ret.error_code() == 200 ? ret.value() : MsgVec{};
    std::reverse(msgs.begin(), msgs.end());
    return msgs;
}

auto handle_get_unread_info(connection::cptr conn,
                            const std::string& username,
                            const std::vector<std::string>& partners)
    -> std::vector<std::tuple<std::string, uint64_t, std::string>> {
    // SQLite 作为唯一数据源
    using RetType = std::vector<std::tuple<std::string, uint64_t, std::string>>;
    if (partners.empty()) return {};
    auto ret = g_sqlite_conn->call<RetType>("get_unread_info_v2", username, partners);
    return ret.error_code() == 200 ? ret.value() : RetType{};
}

// ==================== 好友 RPC ====================

auto handle_search_users(connection::cptr conn, const std::string& keyword, const std::string& self)
    -> std::vector<std::string> {
    auto ret = g_sqlite_conn->call<std::vector<std::string>>("search_users", keyword, self);
    return ret.error_code() == 200 ? ret.value() : std::vector<std::string>{};
}

bool handle_send_friend_request(connection::cptr conn,
                                const std::string& from_user,
                                const std::string& to_user) {
    auto ret = g_sqlite_conn->call<bool>("send_friend_request", from_user, to_user);
    if (ret.error_code() != 200 || !ret.value()) return false;

    // 推送通知给目标用户
    deliver_notification(to_user, "on_new_friend_request", from_user);
    return true;
}

auto handle_get_pending_requests(connection::cptr conn, const std::string& username)
    -> std::vector<std::tuple<int, std::string, int64_t>> {
    using RetType = std::vector<std::tuple<int, std::string, int64_t>>;
    auto ret = g_sqlite_conn->call<RetType>("get_pending_requests", username);
    return ret.error_code() == 200 ? ret.value() : RetType{};
}

auto handle_get_sent_requests(connection::cptr conn, const std::string& username)
    -> std::vector<std::tuple<int, std::string, int64_t>> {
    using RetType = std::vector<std::tuple<int, std::string, int64_t>>;
    auto ret = g_sqlite_conn->call<RetType>("get_sent_requests", username);
    return ret.error_code() == 200 ? ret.value() : RetType{};
}

bool handle_handle_friend_request(connection::cptr conn, int request_id, bool accept) {
    using RetType = std::tuple<bool, std::string, std::string>;
    auto ret = g_sqlite_conn->call<RetType>("handle_friend_request", request_id, accept);
    if (ret.error_code() != 200) return false;
    auto result = ret.value();
    bool ok = std::get<0>(result);
    if (!ok) return false;
    const auto& req_from_user = std::get<1>(result);
    const auto& req_to_user = std::get<2>(result);
    if (accept && !req_from_user.empty()) {
        deliver_notification(req_from_user, "on_friend_request_accepted", req_to_user);
    }
    return true;
}

auto handle_get_friends(connection::cptr conn, const std::string& username)
    -> std::vector<std::string> {
    auto ret = g_sqlite_conn->call<std::vector<std::string>>("get_friends", username);
    return ret.error_code() == 200 ? ret.value() : std::vector<std::string>{};
}

// ==================== 内部 RPC：节点间转发 ====================

void remote_deliver(connection::cptr conn,
                    const std::string& to_user,
                    const std::string& from_user,
                    const std::string& msg,
                    uint64_t seq_id,
                    const std::string& ts) {
    std::shared_lock lock(g_local_mutex);
    auto it = g_local_users.find(to_user);
    if (it != g_local_users.end()) {
        it->second->async_call([](uint32_t, const std::string&, const nlohmann::json&){},
                               "on_message", from_user, msg, seq_id, ts);
    }
}

void remote_notify(connection::cptr conn,
                   const std::string& target_user,
                   const std::string& event,
                   const std::string& payload) {
    std::shared_lock lock(g_local_mutex);
    auto it = g_local_users.find(target_user);
    if (it != g_local_users.end()) {
        it->second->async_call([](uint32_t, const std::string&, const nlohmann::json&){},
                               event, payload);
    }
}

// ==================== 在线状态管理 ====================

static void mark_user_online(const std::string& username, std::shared_ptr<connection> conn) {
    {
        std::unique_lock lock(g_local_mutex);
        g_local_users[username] = std::move(conn);
    }
    {
        std::lock_guard rlk(g_redis_mutex);
        redisCommand(g_redis, "SADD global:online_users %s", username.c_str());
        redisCommand(g_redis, "HSET user_location %s %s", username.c_str(), g_node_id.c_str());
        redisCommand(g_redis, "PUBLISH user_status %s:online", username.c_str());
    }
}

static void mark_user_offline(const std::string& username) {
    {
        std::unique_lock lock(g_local_mutex);
        g_local_users.erase(username);
    }
    {
        std::lock_guard rlk(g_redis_mutex);
        redisCommand(g_redis, "SREM global:online_users %s", username.c_str());
        redisCommand(g_redis, "HDEL user_location %s", username.c_str());
        redisCommand(g_redis, "PUBLISH user_status %s:offline", username.c_str());
    }
}

// ==================== 消息路由 ====================

static void deliver_message(const std::string& from, const std::string& to,
                            const std::string& msg, uint64_t seq_id, const std::string& ts) {
    // 1. 查本节点
    {
        std::shared_lock lock(g_local_mutex);
        auto it = g_local_users.find(to);
        if (it != g_local_users.end()) {
            it->second->async_call([](uint32_t, const std::string&, const nlohmann::json&){},
                                   "on_message", from, msg, seq_id, ts);
            return;
        }
    }

    // 2. Redis 路由到目标节点
    std::lock_guard rlk(g_redis_mutex);
    redisReply* reply = (redisReply*)redisCommand(g_redis, "HGET user_location %s", to.c_str());
    if (reply && reply->type == REDIS_REPLY_STRING) {
        std::string target_node = reply->str;
        freeReplyObject(reply);
        std::lock_guard plk(g_peer_mutex);
        auto pit = g_peer_conns.find(target_node);
        if (pit != g_peer_conns.end()) {
            pit->second->async_call([](uint32_t, const std::string&, const nlohmann::json&){},
                                    "remote_deliver", to, from, msg, seq_id, ts);
        }
    } else {
        if (reply) freeReplyObject(reply);
        LOG_INFO("消息无法投递：用户 {} 不在线", to);
    }
}

static void deliver_notification(const std::string& target_user, const std::string& event,
                                 const std::string& payload) {
    // 1. 查本节点
    {
        std::shared_lock lock(g_local_mutex);
        auto it = g_local_users.find(target_user);
        if (it != g_local_users.end()) {
            it->second->async_call([](uint32_t, const std::string&, const nlohmann::json&){},
                                   event, payload);
            return;
        }
    }

    // 2. Redis 路由
    std::lock_guard rlk(g_redis_mutex);
    redisReply* reply = (redisReply*)redisCommand(g_redis, "HGET user_location %s", target_user.c_str());
    if (reply && reply->type == REDIS_REPLY_STRING) {
        std::string target_node = reply->str;
        freeReplyObject(reply);
        std::lock_guard plk(g_peer_mutex);
        auto pit = g_peer_conns.find(target_node);
        if (pit != g_peer_conns.end()) {
            pit->second->async_call([](uint32_t, const std::string&, const nlohmann::json&){},
                                    "remote_notify", target_user, event, payload);
        }
    } else {
        if (reply) freeReplyObject(reply);
    }
}

// ==================== Pub/Sub 订阅线程 ====================

static void subscribe_loop() {
    {
        redisReply* reply = (redisReply*)redisCommand(g_sub_ctx, "SUBSCRIBE user_status group_chat");
        if (reply) freeReplyObject(reply);
    }

    while (true) {
        redisReply* reply = nullptr;
        int rv = redisGetReply(g_sub_ctx, (void**)&reply);
        if (rv != REDIS_OK || !reply) {
            LOG_ERROR("Pub/Sub 连接断开，尝试重连...");
            if (reply) freeReplyObject(reply);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            redisFree(g_sub_ctx);
            g_sub_ctx = redisConnect("127.0.0.1", 6379);
            if (g_sub_ctx) {
                redisReply* r = (redisReply*)redisCommand(g_sub_ctx, "SUBSCRIBE user_status group_chat cluster:node_join");
                if (r) freeReplyObject(r);
            }
            continue;
        }

        if (reply->type != REDIS_REPLY_ARRAY || reply->elements < 3) {
            freeReplyObject(reply);
            continue;
        }

        std::string channel = reply->element[1]->str ? reply->element[1]->str : "";
        std::string payload = reply->element[2]->str ? reply->element[2]->str : "";

        if (channel == "user_status") {
            auto colon = payload.rfind(':');
            if (colon == std::string::npos) { freeReplyObject(reply); continue; }
            std::string username = payload.substr(0, colon);
            std::string status = payload.substr(colon + 1); // "online" / "offline"

            // 无论本地/远端，主动推送状态变更给本节点所有客户端
            {
                std::shared_lock lock(g_local_mutex);
                for (auto& [u, c] : g_local_users) {
                    (void)u;
                    c->async_call([](uint32_t, const std::string&, const nlohmann::json&){},
                                  "on_user_status_changed", username, status == "online");
                }
            }
        } else if (channel == "group_chat") {
            auto pipe = payload.find('|');
            if (pipe == std::string::npos) { freeReplyObject(reply); continue; }
            std::string from = payload.substr(0, pipe);
            std::string msg = payload.substr(pipe + 1);
            if (from.empty()) { freeReplyObject(reply); continue; }

            std::shared_lock lock(g_local_mutex);
            for (auto& [u, c] : g_local_users) {
                (void)u;
                c->async_call([](uint32_t, const std::string&, const nlohmann::json&){},
                              "on_group_message", from, msg);
            }
        } else if (channel == "cluster:node_join") {
            auto eq = payload.find('=');
            if (eq == std::string::npos) { freeReplyObject(reply); continue; }
            std::string peer_id = payload.substr(0, eq);
            std::string host_port = payload.substr(eq + 1);
            if (peer_id == g_node_id) { freeReplyObject(reply); continue; }
            auto colon = host_port.rfind(':');
            if (colon == std::string::npos) { freeReplyObject(reply); continue; }
            std::string host = host_port.substr(0, colon);
            uint16_t peer_port = static_cast<uint16_t>(std::stoi(host_port.substr(colon + 1)));
            connect_peer(peer_id, host, peer_port);
        }

        freeReplyObject(reply);
    }
}

// ==================== 连接其他节点 ====================

static void connect_peer(const std::string& node_id, const std::string& host, uint16_t port) {
    auto& client = mrpc::client::get();
    auto conn = client.connect(host, port);
    if (conn) {
        std::lock_guard lock(g_peer_mutex);
        g_peer_conns[node_id] = conn;
        LOG_INFO("已连接到节点 {} ({}:{})", node_id, host, port);
    } else {
        LOG_WARN("无法连接到节点 {} ({}:{})", node_id, host, port);
    }
}

// ==================== main ====================

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "用法: " << argv[0] << " <node_id> <port>\n";
        std::cerr << "示例: " << argv[0] << " node-1 8888\n";
        return 1;
    }

    g_node_id = argv[1];
    uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));

    wlog::logger::get().init("logs/server_node_" + g_node_id + ".log");

    auto& svr = server::get();
    svr.set_ip_port("0.0.0.0", port);
    svr.run();

    // ---- 连接 Redis ----
    g_redis = redisConnect("127.0.0.1", 6379);
    if (!g_redis || g_redis->err) {
        LOG_ERROR("Redis 连接失败");
        return 1;
    }

    g_sub_ctx = redisConnect("127.0.0.1", 6379);
    if (!g_sub_ctx || g_sub_ctx->err) {
        LOG_ERROR("Redis 订阅连接失败");
        return 1;
    }

    // ---- 初始化 RedisInbox ----
    if (!g_inbox.connect()) {
        LOG_ERROR("RedisInbox 连接失败 (127.0.0.1:6379)");
        return 1;
    }

    // ---- 启动 client（用于连接其他节点和 sqlite_service）----
    auto& client = mrpc::client::get();
    client.run();

    // ---- 注册 RPC ----
    svr.reg_func("register_user",          handle_register_user);
    svr.reg_func("user_login",             handle_user_login);
    svr.reg_func("token_login",            handle_token_login);
    svr.reg_func("user_logout",            handle_user_logout);
    svr.reg_func("get_online_users",       handle_get_online_users);
    svr.reg_func("send_message",           handle_send_message);
    svr.reg_func("send_group_message",     handle_send_group_message);
    svr.reg_func("sync_messages",          handle_sync_messages);
    svr.reg_func("sync_history",           handle_sync_history);
    svr.reg_func("get_unread_info",        handle_get_unread_info);
    svr.reg_func("search_users",           handle_search_users);
    svr.reg_func("send_friend_request",    handle_send_friend_request);
    svr.reg_func("get_pending_requests",   handle_get_pending_requests);
    svr.reg_func("get_sent_requests",      handle_get_sent_requests);
    svr.reg_func("handle_friend_request",  handle_handle_friend_request);
    svr.reg_func("get_friends",            handle_get_friends);

    // 内部 RPC（其他节点调用）
    svr.reg_func("remote_deliver",         remote_deliver);
    svr.reg_func("remote_notify",          remote_notify);

    // ---- 新连接回调 ----
    svr.set_on_accept_callback([](std::shared_ptr<connection> conn) {
        {
            std::lock_guard lock(g_all_conn_mutex);
            g_all_connections.push_back(conn);
        }

        conn->set_closed_callback([](const std::shared_ptr<connection>& closed_conn) {
            std::string username;
            {
                std::unique_lock lock(g_local_mutex);
                for (auto it = g_local_users.begin(); it != g_local_users.end(); ++it) {
                    if (it->second == closed_conn) {
                        username = it->first;
                        g_local_users.erase(it);
                        break;
                    }
                }
            }

            if (!username.empty()) {
                {
                    std::lock_guard rlk(g_redis_mutex);
                    redisCommand(g_redis, "SREM global:online_users %s", username.c_str());
                    redisCommand(g_redis, "HDEL user_location %s", username.c_str());
                    redisCommand(g_redis, "PUBLISH user_status %s:offline", username.c_str());
                }
                LOG_INFO("用户 {} 因连接断开自动下线（节点 {}）", username, g_node_id);
            }

            {
                std::lock_guard lock(g_all_conn_mutex);
                auto it = std::find(g_all_connections.begin(), g_all_connections.end(), closed_conn);
                if (it != g_all_connections.end()) g_all_connections.erase(it);
            }
        });
    });

    // ---- 连接 SQLite Service（必须在 accept 之前，否则客户端连进来时 handler 会访问空指针）----
    g_sqlite_conn = client.connect("127.0.0.1", 7777);
    if (!g_sqlite_conn) {
        LOG_ERROR("SQLite Service 连接失败 (127.0.0.1:7777)");
        return 1;
    }
    LOG_INFO("已连接 SQLite Service (127.0.0.1:7777)");

    if (!svr.accept()) {
        LOG_ERROR("Server Node 启动失败，端口 {} 被占用", port);
        svr.shutdown();
        return 1;
    }

    LOG_INFO("Server Node {} 启动成功，监听端口: {}", g_node_id, port);

    // ---- 启动 Pub/Sub 订阅线程（accept 后启动，确保 handler 已就绪）----
    std::thread pubsub_thread(subscribe_loop);
    pubsub_thread.detach();

    // ---- 自动注册：发现已有节点 + 宣告自己 ----
    {
        redisReply* reply = (redisReply*)redisCommand(g_redis, "HGETALL cluster:nodes");
        if (reply && reply->type == REDIS_REPLY_ARRAY) {
            for (size_t i = 0; i + 1 < reply->elements; i += 2) {
                std::string peer_id = reply->element[i]->str ? reply->element[i]->str : "";
                std::string host_port = reply->element[i + 1]->str ? reply->element[i + 1]->str : "";
                if (peer_id.empty() || host_port.empty() || peer_id == g_node_id) continue;
                auto colon = host_port.rfind(':');
                if (colon == std::string::npos) continue;
                std::string host = host_port.substr(0, colon);
                uint16_t peer_port = static_cast<uint16_t>(std::stoi(host_port.substr(colon + 1)));
                connect_peer(peer_id, host, peer_port);
            }
        }
        if (reply) freeReplyObject(reply);

        std::string self_addr = "127.0.0.1:" + std::to_string(port);
        redisCommand(g_redis, "HSET cluster:nodes %s %s", g_node_id.c_str(), self_addr.c_str());
        redisCommand(g_redis, "PUBLISH cluster:node_join %s=%s", g_node_id.c_str(), self_addr.c_str());
        LOG_INFO("已注册到集群: {} = {}", g_node_id, self_addr);
    }

    // ---- 信号处理 ----
    asio::signal_set signals(svr.main_iocontext(), SIGINT, SIGTERM);
    signals.async_wait([&](std::error_code ec, int sig) {
        if (ec) return;
        LOG_INFO("收到信号 {}, Server Node {} 关闭中...", sig, g_node_id);
        // 从集群注册表中移除自己
        redisCommand(g_redis, "HDEL cluster:nodes %s", g_node_id.c_str());
        {
            std::shared_lock lock(g_local_mutex);
            for (auto& [u, c] : g_local_users) {
                (void)u;
                c->notify("on_server_shutdown");
            }
        }
        auto timer = std::make_shared<asio::steady_timer>(svr.main_iocontext());
        timer->expires_after(std::chrono::milliseconds(200));
        timer->async_wait([&, timer](std::error_code) {
            svr.shutdown();
        });
    });

    svr.wait_shutdown();
    wlog::logger::get().shutdown();
    return 0;
}
