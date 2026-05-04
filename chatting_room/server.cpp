#include <logger.hpp>
#include <mrpc/server.hpp>
#include <unordered_map>
#include <string>
#include <shared_mutex>
#include <queue>

using namespace mrpc;

// 在线用户列表: username -> connection指针
std::unordered_map<std::string, std::shared_ptr<connection>> g_online_users;
std::shared_mutex g_online_mutex;

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
        return true;
    }
    LOG_WARN("用户 {} 不在线，无法下线", username);
    return false;
}

// 发送私聊消息给指定用户 (推送模式)
bool send_message(connection::cptr conn, const std::string& from_user, const std::string& to_user, const std::string& message) {
    std::shared_lock<std::shared_mutex> lock(g_online_mutex);

    // 检查目标用户是否在线
    auto it = g_online_users.find(to_user);
    if (it == g_online_users.end()) {
        LOG_WARN("用户 {} 不在线，无法发送消息", to_user);
        return false;
    }

    // 直接推送消息到目标客户端
    auto target_conn = it->second;
    LOG_INFO("推送私聊: {} -> {}: {}", from_user, to_user, message);
    target_conn->call<3000, int>("on_message", from_user, message);
    return true;
}

// 发送群聊消息（广播给所有在线用户）
bool send_group_message(connection::cptr conn, const std::string& from_user, const std::string& message) {
    std::shared_lock<std::shared_mutex> lock(g_online_mutex);

    LOG_INFO("群聊广播: {}: {}", from_user, message);

    // 遍历所有在线用户，一一推送消息
    for (const auto& user_pair : g_online_users) {
        const auto& target_conn = user_pair.second;
        target_conn->call<3000, int>("on_group_message", from_user, message);
    }
    return true;
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
    lock.unlock();

    // 通知目标用户：有人想和你 P2P 聊天，同时发送发起方的地址
    auto from_it = g_user_p2p_addr.find(from_user);
    if (from_it != g_user_p2p_addr.end()) {
        std::shared_lock<std::shared_mutex> conn_lock(g_online_mutex);
        auto conn_it = g_online_users.find(to_user);
        if (conn_it != g_online_users.end()) {
            // 通知 B：A 想和你 P2P，A 的地址是 xxx
            conn_it->second->call<3000, int>("on_p2p_request",
                from_user, from_it->second.ip, from_it->second.port);
            LOG_INFO("已通知 {} 准备 P2P 打洞", to_user);
        }
    }

    // 返回目标用户的地址给发起方
    return {target_addr.ip, target_addr.port};
}

int main() {
    wlog::logger::get().init("logs/chatting_room.log");

    auto& server = server::get();
    server.set_ip_port("127.0.0.1", 8888);
    server.set_server_name("chat_server");
    server.run();

    // 注册RPC函数
    server.reg_func("user_login", user_login);
    server.reg_func("user_logout", user_logout);
    server.reg_func("get_online_users", get_online_users);
    server.reg_func("send_message", send_message);
    server.reg_func("send_group_message", send_group_message);

    server.accept();
    system("clear"); // 清屏
    LOG_INFO("聊天室服务器启动成功，监听端口: 8888");

    server.wait_shutdown();
    wlog::logger::get().shutdown();
    return 0;
}
