#include <logger.hpp>
#include <mrpc/server.hpp>
#include <unordered_map>
#include <string>
#include <shared_mutex>
#include <tuple>
#include <algorithm>
#include <random>
#include <sstream>
#include <iomanip>
#include <ctime>
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

// Redis seq_id + token 管理
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

/// 生成随机 token（32 字符 hex）
std::string generate_token() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << dis(gen)
        << std::setw(16) << std::setfill('0') << dis(gen);
    return oss.str();
}

// 用户注册
bool register_user(connection::cptr conn, const std::string& username, const std::string& password) {
    return g_sqlite.register_user(username, password);
}

// 用户登录处理函数（验证密码，返回 token）
std::string user_login(connection::cptr conn, const std::string& username, const std::string& password) {
    // 验证密码
    if (!g_sqlite.verify_user(username, password)) {
        LOG_WARN("用户 {} 登录失败：密码错误或用户不存在", username);
        return {};
    }

    std::unique_lock<std::shared_mutex> lock(g_online_mutex);

    // 检查用户是否已在线
    if (g_online_users.find(username) != g_online_users.end()) {
        LOG_WARN("用户 {} 已在线，拒绝重复登录", username);
        return {};
    }

    // 生成 token 并存入 Redis（30 分钟 TTL）
    std::string token = generate_token();
    g_inbox.save_token(token, username);

    // 加入在线列表
    g_online_users[username] = conn;
    LOG_INFO("用户 {} 上线，当前在线人数: {}", username, g_online_users.size());
    lock.unlock();

    broadcast_user_status(username, true);

    return token;
}

// Token 自动登录
std::string token_login(connection::cptr conn, const std::string& token) {
    // 验证 token
    std::string username = g_inbox.verify_token(token);
    if (username.empty()) {
        LOG_WARN("token 登录失败：token 无效或已过期");
        return {};
    }

    std::unique_lock<std::shared_mutex> lock(g_online_mutex);

    // 如果已在在线列表，更新连接指针
    auto it = g_online_users.find(username);
    if (it != g_online_users.end()) {
        LOG_INFO("用户 {} token 重连，更新连接", username);
        it->second = conn;
    } else {
        g_online_users[username] = conn;
    }

    // 刷新 token TTL
    g_inbox.save_token(token, username);

    LOG_INFO("用户 {} token 自动登录成功，当前在线人数: {}", username, g_online_users.size());
    lock.unlock();

    broadcast_user_status(username, true);

    return username;
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
bool user_logout(connection::cptr conn, const std::string& username, const std::string& token) {
    // 清除 Redis 中的 token
    if (!token.empty()) {
        g_inbox.delete_token(token);
    }

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

// 发送私聊消息：写 SQLite，路由投递到目标用户
uint64_t send_message(connection::cptr conn, const std::string& from_user, const std::string& to_user, const std::string& message) {
    // 检查目标用户是否存在
    if (!g_sqlite.user_exists(to_user)) {
        LOG_WARN("发送消息给不存在的用户: {} -> {}", from_user, to_user);
        return 0;
    }

    // Per-conversation seq_id
    uint64_t seq_id = g_inbox.seq_id(from_user, to_user);
    if (seq_id == 0) {
        LOG_ERROR("Redis seq_id 失败，消息丢失: {} -> {}", from_user, to_user);
        return 0;
    }

    std::string timestamp = std::to_string(std::time(nullptr));

    // 直接写入 SQLite（异步持久化）
    auto msgs = std::make_shared<std::vector<Message>>();
    msgs->push_back(Message{seq_id, from_user, to_user, message, timestamp});
    g_task_pool.post([msgs] { g_sqlite.save(*msgs); });

    // 如果目标在线，直接实时推送
    std::shared_lock<std::shared_mutex> lock(g_online_mutex);
    auto it = g_online_users.find(to_user);
    if (it != g_online_users.end()) {
        LOG_INFO("推送私聊: {} -> {}: {}", from_user, to_user, message);
        it->second->async_call([](uint32_t, const std::string&, const nlohmann::json&){},
            "on_message", from_user, message, seq_id, timestamp);
    } else {
        LOG_INFO("用户 {} 不在线，消息已持久化: {} -> {}", to_user, from_user, to_user);
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
std::vector<std::tuple<uint64_t, std::string, std::string, std::string, std::string>> sync_messages(
    connection::cptr conn, const std::string& username,
    const std::string& partner, uint64_t after_seq, size_t limit)
{
    using MsgTuple = std::tuple<uint64_t, std::string, std::string, std::string, std::string>;

    // SQLite 作为唯一数据源
    if (after_seq == 0) {
        auto msgs = g_sqlite.load_latest(username, partner, limit);
        std::reverse(msgs.begin(), msgs.end()); // DESC → ASC
        std::vector<MsgTuple> result;
        result.reserve(msgs.size());
        for (auto& m : msgs)
            result.emplace_back(m.seq_id, m.from_user, m.to_user, m.msg, m.created_at);
        return result;
    }

    auto msgs = g_sqlite.load_after(username, partner, after_seq, limit);
    std::vector<MsgTuple> result;
    result.reserve(msgs.size());
    for (auto& m : msgs)
        result.emplace_back(m.seq_id, m.from_user, m.to_user, m.msg, m.created_at);
    return result;
}

// 从 SQLite 拉取更早的历史消息（翻页，聊天界面输入 -g 时调用）
std::vector<std::tuple<uint64_t, std::string, std::string, std::string, std::string>> sync_history(
    connection::cptr conn, const std::string& username,
    const std::string& partner, uint64_t before_seq_id, size_t limit)
{
    auto msgs = g_sqlite.load(username, partner, before_seq_id, limit);
    std::vector<std::tuple<uint64_t, std::string, std::string, std::string, std::string>> result;
    result.reserve(msgs.size());
    // load() 返回 DESC（最新在前），反转成 ASC 以便客户端顺序 prepend
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
        result.emplace_back(it->seq_id, it->from_user, it->to_user, it->msg, it->created_at);
    }
    return result;
}

// 批量查询各会话最新 seq_id + 发送者（用于计算未读消息数，SQLite 作为唯一数据源）
std::vector<std::tuple<std::string, uint64_t, std::string>> get_unread_info(
    connection::cptr conn,
    const std::string& username,
    const std::vector<std::string>& partners)
{
    std::vector<std::tuple<std::string, uint64_t, std::string>> result;
    for (const auto& p : partners) {
        auto msgs = g_sqlite.load_latest(username, p, 1);
        if (!msgs.empty()) {
            result.emplace_back(p, msgs[0].seq_id, msgs[0].from_user);
        }
    }
    return result;
}

// ==================== 好友系统 RPC ====================

// 搜索用户（关键字模糊匹配，排除自己）
std::vector<std::string> search_users(connection::cptr conn, const std::string& keyword, const std::string& self) {
    return g_sqlite.search_users(keyword, self);
}

// 发送好友请求
bool send_friend_request(connection::cptr conn, const std::string& from_user, const std::string& to_user) {
    bool ok = g_sqlite.send_friend_request(from_user, to_user);
    if (!ok) return false;

    // 如果目标在线，推送通知
    std::shared_lock<std::shared_mutex> lock(g_online_mutex);
    auto it = g_online_users.find(to_user);
    if (it != g_online_users.end()) {
        it->second->async_call([](uint32_t, const std::string&, const nlohmann::json&){},
            "on_new_friend_request", from_user);
    }
    return true;
}

// 获取待处理的好友请求（别人发给我的）
std::vector<std::tuple<int, std::string, int64_t>> get_pending_requests(connection::cptr conn, const std::string& username) {
    auto reqs = g_sqlite.get_pending_requests(username);
    std::vector<std::tuple<int, std::string, int64_t>> result;
    result.reserve(reqs.size());
    for (auto& r : reqs)
        result.emplace_back(r.id, std::move(r.from_user), r.created_at);
    return result;
}

// 获取已发送但未处理的请求
std::vector<std::tuple<int, std::string, int64_t>> get_sent_requests(connection::cptr conn, const std::string& username) {
    auto reqs = g_sqlite.get_sent_requests(username);
    std::vector<std::tuple<int, std::string, int64_t>> result;
    result.reserve(reqs.size());
    for (auto& r : reqs)
        result.emplace_back(r.id, std::move(r.to_user), r.created_at);
    return result;
}

// 按 ID 查询好友请求（供 handle_friend_request 推送用）
static FriendRequest get_request_by_id(int request_id) {
    using namespace sqlite_orm;
    auto& st = g_sqlite.get_storage();
    auto reqs = st.get_all<FriendRequest>(
        where(c(&FriendRequest::id) == request_id)
    );
    if (reqs.empty()) return {};
    return reqs[0];
}

// 处理好友请求
bool handle_friend_request(connection::cptr conn, int request_id, bool accept) {
    // 先查询请求信息，用于后续推送
    FriendRequest req = get_request_by_id(request_id);
    bool ok = g_sqlite.handle_friend_request(request_id, accept);
    if (!ok) return false;

    if (accept && !req.from_user.empty()) {
        std::shared_lock<std::shared_mutex> lock(g_online_mutex);
        auto it = g_online_users.find(req.from_user);
        if (it != g_online_users.end()) {
            it->second->async_call([](uint32_t, const std::string&, const nlohmann::json&){},
                "on_friend_request_accepted", req.to_user);
        }
    }
    return true;
}

// 获取好友列表
std::vector<std::string> get_friends(connection::cptr conn, const std::string& username) {
    return g_sqlite.get_friends(username);
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
    server.reg_func("register_user", register_user);
    server.reg_func("user_login", user_login);
    server.reg_func("token_login", token_login);
    server.reg_func("user_logout", user_logout);
    server.reg_func("get_online_users", get_online_users);
    server.reg_func("send_message", send_message);
    server.reg_func("send_group_message", send_group_message);
    server.reg_func("sync_messages", sync_messages);
    server.reg_func("sync_history", sync_history);
    server.reg_func("get_unread_info", get_unread_info);

    // 好友系统 RPC
    server.reg_func("search_users", search_users);
    server.reg_func("send_friend_request", send_friend_request);
    server.reg_func("get_pending_requests", get_pending_requests);
    server.reg_func("get_sent_requests", get_sent_requests);
    server.reg_func("handle_friend_request", handle_friend_request);
    server.reg_func("get_friends", get_friends);

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
