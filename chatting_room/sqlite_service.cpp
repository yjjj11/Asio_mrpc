#include <logger.hpp>
#include <mrpc/server.hpp>
#include <mrpc/client.hpp>
#include <string>
#include <vector>
#include <tuple>
#include <memory>
#include <cstdint>
#include <ctime>
#include <asio/signal_set.hpp>
#include "sqlsave.hpp"

using namespace mrpc;

SqliteSaver g_sqlite;

// ==================== 用户 RPC ====================

bool svc_register_user(connection::cptr conn,
                       const std::string& username, const std::string& password) {
    return g_sqlite.register_user(username, password);
}

bool svc_verify_user(connection::cptr conn,
                     const std::string& username, const std::string& password) {
    return g_sqlite.verify_user(username, password);
}

bool svc_user_exists(connection::cptr conn, const std::string& username) {
    return g_sqlite.user_exists(username);
}

// ==================== 消息持久化 RPC ====================

void svc_save_messages(connection::cptr conn,
                       const std::vector<std::tuple<uint64_t, std::string, std::string, std::string, std::string>>& msgs) {
    std::vector<Message> msg_objs;
    msg_objs.reserve(msgs.size());
    for (auto& [sid, f, t, m, ts] : msgs)
        msg_objs.push_back({sid, f, t, m, ts});
    g_sqlite.save(msg_objs);
}

auto svc_load_messages(connection::cptr conn,
                       const std::string& user_a, const std::string& user_b,
                       uint64_t before_seq_id, size_t max_count)
    -> std::vector<std::tuple<uint64_t, std::string, std::string, std::string, std::string>> {
    auto msgs = g_sqlite.load(user_a, user_b, before_seq_id, max_count);
    std::vector<std::tuple<uint64_t, std::string, std::string, std::string, std::string>> result;
    result.reserve(msgs.size());
    for (auto& m : msgs)
        result.emplace_back(m.seq_id, m.from_user, m.to_user, m.msg, m.created_at);
    return result;
}

auto svc_load_messages_after(connection::cptr conn,
                             const std::string& user_a, const std::string& user_b,
                             uint64_t after_seq_id, size_t max_count)
    -> std::vector<std::tuple<uint64_t, std::string, std::string, std::string, std::string>> {
    auto msgs = g_sqlite.load_after(user_a, user_b, after_seq_id, max_count);
    std::vector<std::tuple<uint64_t, std::string, std::string, std::string, std::string>> result;
    result.reserve(msgs.size());
    for (auto& m : msgs)
        result.emplace_back(m.seq_id, m.from_user, m.to_user, m.msg, m.created_at);
    return result;
}

auto svc_load_latest_messages(connection::cptr conn,
                              const std::string& user_a, const std::string& user_b,
                              size_t max_count)
    -> std::vector<std::tuple<uint64_t, std::string, std::string, std::string, std::string>> {
    auto msgs = g_sqlite.load_latest(user_a, user_b, max_count);
    std::vector<std::tuple<uint64_t, std::string, std::string, std::string, std::string>> result;
    result.reserve(msgs.size());
    for (auto& m : msgs)
        result.emplace_back(m.seq_id, m.from_user, m.to_user, m.msg, m.created_at);
    return result;
}

// ==================== 好友 RPC ====================

auto svc_search_users(connection::cptr conn, const std::string& keyword, const std::string& self)
    -> std::vector<std::string> {
    return g_sqlite.search_users(keyword, self);
}

bool svc_send_friend_request(connection::cptr conn,
                             const std::string& from_user, const std::string& to_user) {
    return g_sqlite.send_friend_request(from_user, to_user);
}

auto svc_get_pending_requests(connection::cptr conn, const std::string& username)
    -> std::vector<std::tuple<int, std::string, int64_t>> {
    auto reqs = g_sqlite.get_pending_requests(username);
    std::vector<std::tuple<int, std::string, int64_t>> result;
    result.reserve(reqs.size());
    for (auto& r : reqs)
        result.emplace_back(r.id, std::move(r.from_user), r.created_at);
    return result;
}

auto svc_get_sent_requests(connection::cptr conn, const std::string& username)
    -> std::vector<std::tuple<int, std::string, int64_t>> {
    auto reqs = g_sqlite.get_sent_requests(username);
    std::vector<std::tuple<int, std::string, int64_t>> result;
    result.reserve(reqs.size());
    for (auto& r : reqs)
        result.emplace_back(r.id, std::move(r.to_user), r.created_at);
    return result;
}

/// 处理好友请求，返回 (success, from_user, to_user) 供 server_node 做推送路由
auto svc_handle_friend_request(connection::cptr conn, int request_id, bool accept)
    -> std::tuple<bool, std::string, std::string> {
    using namespace sqlite_orm;
    // 先查请求信息
    FriendRequest req;
    {
        auto& st = g_sqlite.get_storage();
        auto reqs = st.get_all<FriendRequest>(
            where(c(&FriendRequest::id) == request_id)
        );
        if (reqs.empty()) return {false, "", ""};
        req = reqs[0];
    }
    bool ok = g_sqlite.handle_friend_request(request_id, accept);
    if (!ok) return {false, "", ""};
    return {true, req.from_user, req.to_user};
}

auto svc_get_friends(connection::cptr conn, const std::string& username)
    -> std::vector<std::string> {
    return g_sqlite.get_friends(username);
}

// ==================== 扩展 RPC（SQLite 唯一数据源查询） ====================

/// 批量查询未读信息（含最新消息发送者），用于客户端判断是否显示 "new"
/// 返回 vector<(partner, latest_seq, latest_from)>
auto svc_get_unread_info_v2(connection::cptr conn,
                            const std::string& username,
                            const std::vector<std::string>& partners)
    -> std::vector<std::tuple<std::string, uint64_t, std::string>> {
    using namespace sqlite_orm;
    auto& st = g_sqlite.get_storage();
    std::vector<std::tuple<std::string, uint64_t, std::string>> result;
    for (const auto& p : partners) {
        auto rows = st.get_all<Message>(
            where(
                (c(&Message::from_user) == username && c(&Message::to_user) == p) ||
                (c(&Message::from_user) == p && c(&Message::to_user) == username)
            ),
            order_by(&Message::seq_id).desc(),
            limit(1)
        );
        if (!rows.empty()) {
            result.emplace_back(p, rows[0].seq_id, rows[0].from_user);
        }
    }
    return result;
}

// ==================== main ====================

int main(int argc, char* argv[]) {
    uint16_t port = 7777;
    if (argc > 1) port = static_cast<uint16_t>(std::stoi(argv[1]));

    wlog::logger::get().init("logs/sqlite_service.log");

    auto& svr = server::get();
    svr.set_ip_port("0.0.0.0", port);
    svr.run();

    if (!g_sqlite.init("chat_history.db")) {
        LOG_ERROR("SQLite 初始化失败");
        return 1;
    }
    LOG_INFO("SQLite 服务启动成功，数据库: chat_history.db");

    // 注册 RPC
    svr.reg_func("register_user",        svc_register_user);
    svr.reg_func("verify_user",          svc_verify_user);
    svr.reg_func("user_exists",          svc_user_exists);
    svr.reg_func("save_messages",        svc_save_messages);
    svr.reg_func("load_messages",        svc_load_messages);
    svr.reg_func("load_messages_after",  svc_load_messages_after);
    svr.reg_func("load_latest_messages", svc_load_latest_messages);
    svr.reg_func("search_users",         svc_search_users);
    svr.reg_func("send_friend_request",  svc_send_friend_request);
    svr.reg_func("get_pending_requests", svc_get_pending_requests);
    svr.reg_func("get_sent_requests",    svc_get_sent_requests);
    svr.reg_func("handle_friend_request", svc_handle_friend_request);
    svr.reg_func("get_friends",          svc_get_friends);
    svr.reg_func("get_unread_info_v2",   svc_get_unread_info_v2);

    if (!svr.accept()) {
        LOG_ERROR("SQLite Service 启动失败，端口 {} 被占用", port);
        svr.shutdown();
        return 1;
    }

    asio::signal_set signals(svr.main_iocontext(), SIGINT, SIGTERM);
    signals.async_wait([&](std::error_code ec, int sig) {
        if (ec) return;
        LOG_INFO("收到信号 {}, SQLite 服务关闭中...", sig);
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
