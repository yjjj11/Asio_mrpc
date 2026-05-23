#include <logger.hpp>
#include <mrpc/server.hpp>
#include <unordered_map>
#include <string>
#include <shared_mutex>
#include <tuple>
#include <algorithm>
#include <asio/signal_set.hpp>
#include "redis_inbox.hpp"
#include "sqlsave.hpp"
#include "task_pool.hpp"
using namespace mrpc;

// 前向声明
void broadcast_user_status(const std::string& username, bool online);

// 在线用户列表: username -> connection指针
std::unordered_map<std::string, std::shared_ptr<connection>> g_online_users;
std::shared_mutex g_online_mutex;

// Redis 会话缓存（最近 10 条消息）
RedisInbox g_inbox;

// SQLite 历史消息持久化
SqliteSaver g_sqlite;

// 异步持久化线程池（单线程，保证 SQLite 写串行化）
TaskPool g_task_pool(1);

// 所有连接的追踪列表（用于服务端关闭时广播通知）
std::vector<std::shared_ptr<connection>> g_all_connections;
std::mutex g_all_conn_mutex;

// 广播服务端关闭通知给所有已连接客户端
void broadcast_shutdown() {
    std::lock_guard<std::mutex> lock(g_all_conn_mutex);
    LOG_INFO("通知 {} 个客户端服务器即将关闭...", g_all_connections.size());
    for (auto& conn : g_all_connections) {
        conn->notify("on_server_shutdown");
    }
}

// 用户 P2P 公网地址信息: username -> (IP字符串, 端口)
struct P2PAddr {
    std::string ip;
    uint16_t port;
};
std::unordered_map<std::string, P2PAddr> g_user_p2p_addr;
std::shared_mutex g_p2p_mutex;

// 用户上线处理函数
bool user_login(connection::cptr conn, const std::string& username) {
    std::unique_lock<std::shared_mutex> lock(g_online_mutex);

    // 检查用户是否已在线
    if (g_online_users.find(username) != g_online_users.end()) {
        LOG_WARN("用户 {} 已在线，拒绝重复登录", username);
        return false;
    }

    // 加入在线列表
    g_online_users[username] = conn;
    LOG_INFO("用户 {} 上线，当前在线人数: {}", username, g_online_users.size());
    lock.unlock();

    broadcast_user_status(username, true);

    return true;
}

// 获取在线用户列表
std::vector<std::string> get_online_users(connection::cptr conn) {
    std::shared_lock<std::shared_mutex> lock(g_online_mutex);
    std::vector<std::string> users;
    for (const auto& pair : g_online_users) {
        users.push_back(pair.first);
    }
    return users;
}

// 用户下线处理函数
bool user_logout(connection::cptr conn, const std::string& username) {
    std::unique_lock<std::shared_mutex> lock(g_online_mutex);

    auto it = g_online_users.find(username);
    if (it != g_online_users.end()) {
        g_online_users.erase(it);
        LOG_INFO("用户 {} 下线，当前在线人数: {}", username, g_online_users.size());
        lock.unlock();

        broadcast_user_status(username, false);
        return true;
    }
    LOG_WARN("用户 {} 不在线，无法下线", username);
    return false;
}

// 发送私聊消息：写 conv ZSET，超 10 条则持久化到 SQLite，返回 seq_id
uint64_t send_message(connection::cptr conn, const std::string& from_user, const std::string& to_user, const std::string& message) {
    uint64_t seq_id = g_inbox.next_seq_id();
    if (seq_id == 0) {
        LOG_ERROR("Redis next_seq_id 失败，消息丢失: {} -> {}", from_user, to_user);
        return 0;
    }

    // 写入会话 ZSET（conv:<from>:<to>，字典序）
    if (!g_inbox.push_conv(from_user, to_user, seq_id, from_user, to_user, message)) {
        LOG_ERROR("Redis ZADD 失败，消息丢失: {} -> {}", from_user, to_user);
        return 0;
    }

    // 超出 10 条则弹出最旧消息，持久化到 SQLite
    auto evicted = g_inbox.trim_conv(from_user, to_user, 10);
    if (!evicted.empty()) {
        // 异步持久化到 SQLite，不阻塞消息发送
        auto msgs = std::make_shared<std::vector<Message>>();
        msgs->reserve(evicted.size());
        for (auto& [sid, f, t, m] : evicted) {
            msgs->push_back({sid, std::move(f), std::move(t), std::move(m), ""});
        }
        g_task_pool.post([msgs] { g_sqlite.save(*msgs); });
    }

    // 如果目标在线，直接实时推送
    std::shared_lock<std::shared_mutex> lock(g_online_mutex);
    auto it = g_online_users.find(to_user);
    if (it != g_online_users.end()) {
        LOG_INFO("推送私聊: {} -> {}: {}", from_user, to_user, message);
        it->second->async_call([](uint32_t, const std::string&, const nlohmann::json&){},
            "on_message", from_user, message, seq_id);
    } else {
        LOG_INFO("用户 {} 不在线，消息已 Redis 缓存: {} -> {}", to_user, from_user, to_user);
    }
    return seq_id;
}

// 发送群聊消息（广播给所有在线用户）
bool send_group_message(connection::cptr conn, const std::string& from_user, const std::string& message) {
    std::shared_lock<std::shared_mutex> lock(g_online_mutex);

    LOG_INFO("群聊广播: {}: {}", from_user, message);

    // 遍历所有在线用户，一一推送消息
    for (const auto& user_pair : g_online_users) {
        const auto& target_conn = user_pair.second;
        target_conn->async_call([](uint32_t, const std::string&, const nlohmann::json&){},
            "on_group_message", from_user, message);
    }
    return true;
}

// 广播用户上下线状态给所有在线客户端（异步推送，不阻塞）
void broadcast_user_status(const std::string& username, bool online) {
    std::shared_lock<std::shared_mutex> lock(g_online_mutex);
    for (const auto& user_pair : g_online_users) {
        user_pair.second->async_call([](uint32_t, const std::string&, const nlohmann::json&){},
            "on_user_status_changed", username, online);
    }
}

// ==================== P2P 打洞 RPC ====================

// 1. 客户端上报 P2P UDP 端口
bool report_p2p_port(connection::cptr conn, const std::string& username, uint16_t udp_port) {
    std::unique_lock<std::shared_mutex> lock(g_p2p_mutex);

    // 获取客户端的公网 IP（从 TCP 连接地址中提取）
    std::string peer_ip = conn->get_peer_ip();

    g_user_p2p_addr[username] = {peer_ip, udp_port};
    LOG_INFO("用户 {} 上报 P2P 地址: {}:{}", username, peer_ip, udp_port);
    return true;
}

// 2. 请求 P2P 连接 - 返回目标用户的公网地址，并通知目标用户准备打洞
std::tuple<std::string, uint16_t> request_p2p_connect(
    connection::cptr conn,
    const std::string& from_user,
    const std::string& to_user) {

    LOG_INFO("P2P 请求: {} -> {}", from_user, to_user);

    // 先获取目标用户的地址
    std::shared_lock<std::shared_mutex> lock(g_p2p_mutex);
    auto it = g_user_p2p_addr.find(to_user);
    if (it == g_user_p2p_addr.end()) {
        LOG_WARN("用户 {} 没有 P2P 地址", to_user);
        return {"", 0};  // 返回空表示无法 P2P
    }

    P2PAddr target_addr = it->second;

    // 通知目标用户：有人想和你 P2P 聊天，同时发送发起方的地址
    auto from_it = g_user_p2p_addr.find(from_user);
    if (from_it != g_user_p2p_addr.end()) {
        std::shared_lock<std::shared_mutex> conn_lock(g_online_mutex);
        auto conn_it = g_online_users.find(to_user);
        if (conn_it != g_online_users.end()) {
            // 通知 B：A 想和你 P2P，A 的地址是 xxx
            conn_it->second->async_call([](uint32_t, const std::string&, const nlohmann::json&){},
                "on_p2p_request",
                from_user, from_it->second.ip, from_it->second.port);
            LOG_INFO("已通知 {} 准备 P2P 打洞", to_user);
        }
    }

    // 返回目标用户的地址给发起方
    return {target_addr.ip, target_addr.port};
}

// 增量拉取消息：after_seq=0 返回最新 limit 条（冷启动），否则返回 seq > after_seq 的消息
std::vector<std::tuple<uint64_t, std::string, std::string, std::string>> sync_messages(
    connection::cptr conn, const std::string& username,
    const std::string& partner, uint64_t after_seq, size_t limit)
{
    using MsgTuple = std::tuple<uint64_t, std::string, std::string, std::string>;

    if (after_seq == 0) {
        // 冷启动：返回最新 limit 条
        auto redis_msgs = g_inbox.pull_recent(username, partner, limit);
        if (redis_msgs.size() >= limit) return redis_msgs;

        size_t remaining = limit - redis_msgs.size();
        if (!redis_msgs.empty()) {
            uint64_t earliest = std::get<0>(redis_msgs[0]);
            auto sql_msgs = g_sqlite.load(username, partner, earliest, remaining);
            // sql_msgs DESC（最新在前），逆转成 ASC（最早在前）
            std::reverse(sql_msgs.begin(), sql_msgs.end());
            std::vector<MsgTuple> result;
            result.reserve(limit);
            for (auto& m : sql_msgs)
                result.emplace_back(m.seq_id, m.from_user, m.to_user, m.msg);
            for (auto& t : redis_msgs)
                result.push_back(std::move(t));
            return result;
        } else {
            // Redis 无数据，从 SQLite 取最新消息
            auto sql_msgs = g_sqlite.load_latest(username, partner, remaining);
            std::reverse(sql_msgs.begin(), sql_msgs.end());
            std::vector<MsgTuple> result;
            result.reserve(sql_msgs.size());
            for (auto& m : sql_msgs)
                result.emplace_back(m.seq_id, m.from_user, m.to_user, m.msg);
            return result;
        }
    }

    // 增量拉取：返回 seq > after_seq 的消息，双指针归并 SQLite + Redis
    auto sql_msgs = g_sqlite.load_after(username, partner, after_seq, limit);
    size_t remaining = limit - sql_msgs.size();
    auto redis_msgs = g_inbox.pull_after(username, partner, after_seq, remaining);

    std::vector<MsgTuple> result;
    result.reserve(limit);
    auto sql_it = sql_msgs.begin();
    auto redis_it = redis_msgs.begin();
    while (result.size() < limit && (sql_it != sql_msgs.end() || redis_it != redis_msgs.end())) {
        if (sql_it == sql_msgs.end()) {
            result.emplace_back(std::get<0>(*redis_it), std::get<1>(*redis_it),
                                std::get<2>(*redis_it), std::get<3>(*redis_it));
            ++redis_it;
        } else if (redis_it == redis_msgs.end()) {
            result.emplace_back(sql_it->seq_id, sql_it->from_user, sql_it->to_user, sql_it->msg);
            ++sql_it;
        } else if (sql_it->seq_id < std::get<0>(*redis_it)) {
            result.emplace_back(sql_it->seq_id, sql_it->from_user, sql_it->to_user, sql_it->msg);
            ++sql_it;
        } else {
            result.emplace_back(std::get<0>(*redis_it), std::get<1>(*redis_it),
                                std::get<2>(*redis_it), std::get<3>(*redis_it));
            ++redis_it;
        }
    }
    return result;
}

// 从 SQLite 拉取更早的历史消息（翻页，聊天界面输入 -g 时调用）
std::vector<std::tuple<uint64_t, std::string, std::string, std::string>> sync_history(
    connection::cptr conn, const std::string& username,
    const std::string& partner, uint64_t before_seq_id, size_t limit)
{
    auto msgs = g_sqlite.load(username, partner, before_seq_id, limit);
    std::vector<std::tuple<uint64_t, std::string, std::string, std::string>> result;
    result.reserve(msgs.size());
    // load() 返回 DESC（最新在前），反转成 ASC 以便客户端顺序 prepend
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
        result.emplace_back(it->seq_id, it->from_user, it->to_user, it->msg);
    }
    return result;
}

// 从 Redis 反向拉取会话上下文（seq <= before_seq，用于重启后填充展示窗口）
std::vector<std::tuple<uint64_t, std::string, std::string, std::string>> get_context_messages(
    connection::cptr conn, const std::string& username,
    const std::string& partner, uint64_t before_seq, size_t limit)
{
    auto redis_msgs = g_inbox.pull_before(username, partner, before_seq, limit);
    // pull_before 返回 DESC（最新在前），逆转成 ASC 供客户端顺序展示
    std::reverse(redis_msgs.begin(), redis_msgs.end());
    return redis_msgs;
}

int main() {
    wlog::logger::get().init("logs/chatting_room.log");

    auto& server = server::get();
    server.set_ip_port("127.0.0.1", 8888);
    server.run();

    // 连接 Redis
    if (!g_inbox.connect()) {
        LOG_ERROR("Redis 连接失败 (127.0.0.1:6379)，请确保 Redis 已启动");
        return 1;
    }

    // 初始化 SQLite（历史消息持久化）
    if (!g_sqlite.init("chat_history.db")) {
        LOG_ERROR("SQLite 初始化失败");
        return 1;
    }

    // 注册RPC函数
    server.reg_func("user_login", user_login);
    server.reg_func("user_logout", user_logout);
    server.reg_func("get_online_users", get_online_users);
    server.reg_func("send_message", send_message);
    server.reg_func("send_group_message", send_group_message);
    server.reg_func("sync_messages", sync_messages);
    server.reg_func("sync_history", sync_history);
    server.reg_func("get_context_messages", get_context_messages);

    // 新连接回调：追踪连接 + 空闲超时断开时自动下线用户
    server.set_on_accept_callback([](std::shared_ptr<connection> conn) {
        // 加入连接追踪列表
        {
            std::lock_guard<std::mutex> lock(g_all_conn_mutex);
            g_all_connections.push_back(conn);
        }

        conn->set_closed_callback([](const std::shared_ptr<connection>& closed_conn) {
            // 从连接追踪列表中移除
            {
                std::lock_guard<std::mutex> lock(g_all_conn_mutex);
                auto it = std::find(g_all_connections.begin(), g_all_connections.end(), closed_conn);
                if (it != g_all_connections.end()) g_all_connections.erase(it);
            }

            std::string offline_username;
            {
                std::unique_lock<std::shared_mutex> lock(g_online_mutex);
                for (auto it = g_online_users.begin(); it != g_online_users.end(); ++it) {
                    if (it->second == closed_conn) {
                        LOG_INFO("用户 {} 因连接断开自动下线", it->first);
                        offline_username = it->first;
                        g_online_users.erase(it);
                        break;
                    }
                }
            }
            if (!offline_username.empty()) {
                broadcast_user_status(offline_username, false);
            }
        });
    });

    if (!server.accept()) {
        LOG_ERROR("服务端启动失败，端口被占用");
        server.shutdown();
        return 1;
    }
    system("clear"); // 清屏
    LOG_INFO("聊天室服务器启动成功，监听端口: 8888");

    // 注册信号处理：优雅关闭，通知所有客户端
    asio::signal_set signals(server.main_iocontext(), SIGINT, SIGTERM);
    signals.async_wait([&](std::error_code ec, int sig) {
        if (ec) return;
        LOG_INFO("收到信号 {}, 服务端关闭中...", sig);
        broadcast_shutdown();
        // 延迟关闭，让 notify 异步写完成
        auto timer = std::make_shared<asio::steady_timer>(server.main_iocontext());
        timer->expires_after(std::chrono::milliseconds(200));
        timer->async_wait([&, timer](std::error_code) {
            server.shutdown();
        });
    });

    server.wait_shutdown();
    wlog::logger::get().shutdown();
    return 0;
}
