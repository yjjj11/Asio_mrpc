#include <logger.hpp>
#include <mrpc/client.hpp>
#include <iostream>
#include <string>
#include <limits>
#include <mutex>
#include <atomic>
#include <vector>
#include <tuple>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <set>

using namespace mrpc;

// 持久化会话消息存储: target_user -> vector<(sender, message, seq_id)>
// sender = "__me__"（自己）或对方用户名, seq_id=0 表示未知, ts 为 Unix 时间戳字符串
std::unordered_map<std::string, std::vector<std::tuple<std::string, std::string, uint64_t, std::string>>> g_chat_history_map;
std::mutex g_chat_map_mutex;
std::string g_current_chat_target;
bool g_in_chat = false;
bool g_in_group_chat = false;

// 全局：群聊历史
std::vector<std::pair<std::string, std::string>> g_group_chat_history;
std::mutex g_group_mutex;

// 本地缓存的在线用户列表（由推送通知实时维护）
std::vector<std::string> g_online_users_cache;

// 已同步的最大 seq_id（客户端增量拉取用，暂不持久化到文件，重启后从头同步）
uint64_t g_last_seq_id = 0;
std::mutex g_online_cache_mutex;
std::string g_self_username; // 当前登录用户，用于过滤自身推送
bool g_in_hub = false;       // 是否在 hub 页面，需要重绘
std::atomic<bool> g_server_offline{false}; // 服务器是否已断开

// 每个会话已知的最小 seq_id（用于 -g 翻查更早历史）
std::unordered_map<std::string, uint64_t> g_conv_min_seq;
std::mutex g_conv_min_seq_mutex;

// 每个会话收到推送的最大 seq_id（用于聊天记录去重）
std::unordered_map<std::string, uint64_t> g_conv_max_seq;
std::mutex g_conv_max_seq_mutex;

// 增量拉取游标（用于 sync_messages 的 after_seq，与推送跟踪 g_conv_max_seq 分离）
std::unordered_map<std::string, uint64_t> g_pull_cursor;
std::mutex g_pull_cursor_mutex;

// 各会话未读消息数（在 hub 界面显示 【N条新消息】）
std::unordered_map<std::string, size_t> g_unread_counts;
std::mutex g_unread_mutex;

// 好友列表
std::vector<std::string> g_friends_list;
std::mutex g_friends_mutex;

// 待处理的好友请求 (request_id, from_user, created_at)
std::vector<std::tuple<int, std::string, int64_t>> g_pending_requests;
std::mutex g_req_mutex;

// 前向声明
static bool is_online(const std::string& username);

// 清屏
void clear_screen() {
    system("clear");
}

// 按任意键继续
void press_any_key() {
    if (std::cin.fail()) {
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    std::cout << "\n按回车键继续...";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cin.get();
}

// 辅助函数：将 Unix 时间戳字符串格式化为 "HH:MM" 或 "MM-DD HH:MM"
static std::string fmt_ts(const std::string& ts_str) {
    if (ts_str.empty()) return "";
    std::time_t t = static_cast<std::time_t>(std::stoull(ts_str));
    std::tm* lt = std::localtime(&t);
    if (!lt) return "";
    std::ostringstream oss;
    oss << std::put_time(lt, "%m-%d %H:%M");
    return oss.str();
}

// 重新渲染私聊界面（seq_id 去重，保证推送消息与拉取消息不会重复显示）
void redraw_chat_ui(const std::string& target_name) {
    std::cout << "\033[H\033[2J" << std::flush;
    std::cout << "========================================" << std::endl;
    std::cout << "       与 " << target_name << " 私聊中        " << std::endl;
    std::cout << "   \033[32m[推送模式] 消息实时到达\033[0m        " << std::endl;
    std::cout << "========================================" << std::endl;

    auto it = g_chat_history_map.find(target_name);
    if (it != g_chat_history_map.end()) {
        LOG_INFO("redraw {}: {} total msgs", target_name, it->second.size());
        std::unordered_set<uint64_t> seen;
        for (const auto& msg : it->second) {
            uint64_t sid = std::get<2>(msg);
            if (sid > 0 && !seen.insert(sid).second) continue;
            const auto& sender = std::get<0>(msg);
            const auto& text  = std::get<1>(msg);
            const auto& ts    = std::get<3>(msg);
            std::string ts_display = fmt_ts(ts);
            if (!ts_display.empty()) ts_display += "  ";
            if (sender == "__me__") {
                std::cout << ts_display << "\033[32m我\033[0m: " << text << std::endl;
            } else if (sender == "__system__") {
                std::cout << ts_display << "\033[33m" << text << "\033[0m" << std::endl;
            } else {
                std::cout << ts_display << "\033[34m" << sender << "\033[0m: " << text << std::endl;
            }
        }
    }

    std::cout << "----------------------------------------" << std::endl;
    std::cout << " 输入消息 ('quit'退出, '-g'翻查历史): " << std::endl;
    std::cout << " > " << std::flush;
}

// 绘制 Hub 主界面（好友列表 + 在线用户列表 + 未读数）
void draw_hub_ui() {
    clear_screen();
    std::cout << "========================================" << std::endl;
    std::cout << "           聊天室客户端                 " << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << " 当前用户: " << g_self_username << " [在线]" << std::endl;

    // 显示待处理好友请求提醒
    {
        std::lock_guard<std::mutex> lk(g_req_mutex);
        if (!g_pending_requests.empty()) {
            std::cout << " \033[33m你有 " << g_pending_requests.size() << " 条待处理的好友请求！（输入 f 查看）\033[0m" << std::endl;
        }
    }
    std::cout << "========================================" << std::endl;

    // 构建好友用户集合（快速查找）
    std::set<std::string> friend_set;
    {
        std::lock_guard<std::mutex> lk(g_friends_mutex);
        friend_set.insert(g_friends_list.begin(), g_friends_list.end());
    }

    // 在线非好友集合
    std::vector<std::string> online_non_friends;
    {
        std::lock_guard<std::mutex> lock(g_online_cache_mutex);
        for (const auto& u : g_online_users_cache) {
            if (u != g_self_username && friend_set.find(u) == friend_set.end())
                online_non_friends.push_back(u);
        }
    }

    // 构建完整的可聊天用户列表：好友（在线在前）+ 在线非好友
    std::vector<std::string> chat_users;
    std::vector<std::string> friend_status; // "【在线】" or "【离线】"
    {
        std::lock_guard<std::mutex> lk(g_friends_mutex);
        // 好友：在线在前
        for (const auto& f : g_friends_list) {
            if (is_online(f)) {
                chat_users.push_back(f);
                friend_status.push_back("【在线】");
            }
        }
        for (const auto& f : g_friends_list) {
            if (!is_online(f)) {
                chat_users.push_back(f);
                friend_status.push_back("【离线】");
            }
        }
        // 在线非好友
        for (const auto& u : online_non_friends) {
            chat_users.push_back(u);
            friend_status.push_back("【在线】");
        }
    }

    if (chat_users.empty()) {
        std::cout << " 暂无可聊天的用户" << std::endl;
        std::cout << " 输入 f 搜索好友" << std::endl;
    } else {
        std::lock_guard<std::mutex> lk(g_unread_mutex);
        for (size_t i = 0; i < chat_users.size(); ++i) {
            std::cout << "  " << (i + 1) << ". " << chat_users[i] << friend_status[i];
            // 未读消息计数
            auto uit = g_unread_counts.find(chat_users[i]);
            if (uit != g_unread_counts.end()) {
                if (uit->second > 0) {
                    std::cout << "\033[67G\033[31m【" << uit->second << "条新消息】\033[0m";
                } else {
                    std::cout << "\033[67G\033[31m【new】\033[0m";
                }
            }
            std::cout << std::endl;
        }
    }
    std::cout << "----------------------------------------" << std::endl;
    std::cout << " \033[36m[输入编号私聊]  f-好友  g-群聊  r-刷新  q-退出登录\033[0m" << std::endl;
    std::cout << " > " << std::flush;
}

// 重新渲染群聊界面
void redraw_group_chat_ui() {
    std::cout << "\033[H\033[2J" << std::flush;
    std::cout << "========================================" << std::endl;
    std::cout << "           聊天室群聊                   " << std::endl;
    std::cout << "   \033[35m[广播模式] 全员可见\033[0m               " << std::endl;
    std::cout << "========================================" << std::endl;

    for (const auto& msg : g_group_chat_history) {
        if (msg.first == "__me__") {
            std::cout << "\033[32m我\033[0m: " << msg.second << std::endl;
        } else {
            std::cout << "\033[35m" << msg.first << "\033[0m: " << msg.second << std::endl;
        }
    }

    std::cout << "----------------------------------------" << std::endl;
    std::cout << " 输入消息 (输入 'quit' 退出): " << std::endl;
    std::cout << " > " << std::flush;
}

// 接收私聊消息的RPC回调（含 seq_id + timestamp，用于更新进度 + 渲染时去重）
int on_message(connection::cptr conn, const std::string& from_user, const std::string& message, uint64_t seq_id, const std::string& timestamp) {
    bool hub_user = false;
    bool third_party = false;
    {
        std::lock_guard<std::mutex> lock(g_chat_map_mutex);
        LOG_INFO("on_message: from={}, msg={}, seq={}, map_size={}", from_user, message, seq_id,
                 g_chat_history_map[from_user].size());
        g_chat_history_map[from_user].emplace_back(from_user, message, seq_id, timestamp);
        if (seq_id > g_last_seq_id) g_last_seq_id = seq_id;
        {
            std::lock_guard<std::mutex> lk(g_conv_min_seq_mutex);
            auto it = g_conv_min_seq.find(from_user);
            if (it == g_conv_min_seq.end() || seq_id < it->second) {
                g_conv_min_seq[from_user] = seq_id;
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_conv_max_seq_mutex);
            auto it = g_conv_max_seq.find(from_user);
            if (it == g_conv_max_seq.end() || seq_id > it->second) {
                g_conv_max_seq[from_user] = seq_id;
            }
        }
        // 记录推送的 seq_id，供渲染时去重（redraw_chat_ui 按 seq_id 去重）
        if (g_in_chat && from_user == g_current_chat_target) {
            redraw_chat_ui(g_current_chat_target);
        } else if (g_in_chat && from_user != g_current_chat_target) {
            third_party = true;
        }
        if (g_in_hub) {
            hub_user = true;
        }
    }
    // 在 hub 界面时更新未读计数并重绘
    if (hub_user) {
        {
            std::lock_guard<std::mutex> lk(g_unread_mutex);
            g_unread_counts[from_user]++;
        }
        draw_hub_ui();
    }
    // 正与 B 聊天时，C 发来消息 → 通知但不重绘
    if (third_party) {
        {
            std::lock_guard<std::mutex> lk(g_unread_mutex);
            g_unread_counts[from_user]++;
        }
        std::cout << "\n\033[33m[来自 " << from_user << " 的新消息]\033[0m" << std::endl;
        std::cout << " > " << std::flush;
    }
    return 0;
}

// 接收群聊消息的RPC回调
int on_group_message(connection::cptr conn, const std::string& from_user, const std::string& message) {
    if (from_user == g_self_username) return 0; // 自己发的由本地添加为"我"，避免重复
    std::lock_guard<std::mutex> lock(g_group_mutex);
    g_group_chat_history.emplace_back(from_user, message);

    if (g_in_group_chat) {
        redraw_group_chat_ui();
    }
    return 0;
}

// 用户上下线推送通知
int on_user_status_changed(connection::cptr conn, const std::string& username, bool online) {
    if (username == g_self_username) return 0; // 自身变化无视

    LOG_DEBUG("推送通知: 用户 {} {}", username, online ? "上线" : "下线");

    // 服务端断开时，不更新在线列表
    if (g_server_offline) return 0;
    {
        std::lock_guard<std::mutex> lock(g_online_cache_mutex);
        if (online) {
            if (std::find(g_online_users_cache.begin(), g_online_users_cache.end(), username) == g_online_users_cache.end()) {
                g_online_users_cache.push_back(username);
            }
        } else {
            auto it = std::find(g_online_users_cache.begin(), g_online_users_cache.end(), username);
            if (it != g_online_users_cache.end()) {
                g_online_users_cache.erase(it);
            }
        }
    }

    if (g_in_hub) {
        draw_hub_ui();
    }

    return 0;
}

// 服务端关闭通知
int on_server_shutdown(connection::cptr conn) {
    g_server_offline = true;
    std::cout << "\n\033[31m[系统] 服务器已关闭，进入离线模式\033[0m" << std::endl;
    return 0;
}

// ==================== 好友推送回调 ====================

// 收到新的好友请求
int on_new_friend_request(connection::cptr conn, const std::string& from_user) {
    LOG_INFO("收到好友请求: {}", from_user);
    // 追加到待处理列表（暂不保存 created_at，服务端为主）
    {
        // 检查是否已在列表中
        std::lock_guard<std::mutex> lk(g_req_mutex);
        for (const auto& [id, f, _] : g_pending_requests) {
            if (f == from_user) return 0; // 已存在
        }
        // 没有 id，设 0 表示无 id（待刷新）
        g_pending_requests.emplace_back(0, from_user, 0);
    }
    if (g_in_hub) draw_hub_ui();
    return 0;
}

// 好友请求被接受
int on_friend_request_accepted(connection::cptr conn, const std::string& by_user) {
    LOG_INFO("好友请求已被接受: {}", by_user);
    // 刷新好友列表
    auto ret = conn->call<std::vector<std::string>>("get_friends", g_self_username);
    if (ret.error_code() == 200) {
        std::lock_guard<std::mutex> lk(g_friends_mutex);
        g_friends_list = ret.value();
    }
    if (g_in_hub) draw_hub_ui();
    return 0;
}

// 从文件恢复游标（per-conversation 增量拉取进度）
void load_cursors(const std::string& username) {
    std::ifstream f("cursor_" + username + ".txt");
    if (!f) return;
    std::string line;
    std::lock_guard<std::mutex> lk(g_pull_cursor_mutex);
    while (std::getline(f, line)) {
        auto pos = line.rfind(':');
        if (pos == std::string::npos) continue;
        g_pull_cursor[line.substr(0, pos)] = std::stoull(line.substr(pos + 1));
    }
}

// 保存游标到文件（增量拉取进度，供下次登录恢复）
void save_cursors(const std::string& username) {
    std::ofstream f("cursor_" + username + ".txt");
    if (!f) return;
    std::lock_guard<std::mutex> lk(g_pull_cursor_mutex);
    for (const auto& [target, seq] : g_pull_cursor) {
        f << target << ":" << seq << "\n";
    }
}

// ==================== Token 持久化 ====================

// 从 session 文件读取 token
std::string load_session_token() {
    std::ifstream f("session_token.txt");
    if (!f) return {};
    std::string line;
    if (!std::getline(f, line)) return {};
    auto pos = line.rfind(':');
    if (pos == std::string::npos) return {};
    return line.substr(pos + 1); // 返回 token 部分
}

// 保存 username:token 到 session 文件
void save_session_token(const std::string& username, const std::string& token) {
    std::ofstream f("session_token.txt");
    if (!f) return;
    f << username << ":" << token << "\n";
}

// 删除 session 文件
void remove_session_token() {
    std::remove("session_token.txt");
}

// ==================== 未读消息 ====================

// 登录后调用：从服务端拉取各会话最新 seq_id，与本地游标对比得出未读数
void fetch_unread_counts(std::shared_ptr<connection> conn, const std::string& username) {
    // 收集所有已知 partner（游标 + 好友 + 在线用户 + chat_history_map 中有过消息的）
    std::vector<std::string> partners;
    {
        std::lock_guard<std::mutex> lk(g_pull_cursor_mutex);
        for (const auto& [p, _] : g_pull_cursor)
            partners.push_back(p);
    }
    {
        std::lock_guard<std::mutex> lk(g_friends_mutex);
        for (const auto& f : g_friends_list) {
            if (std::find(partners.begin(), partners.end(), f) == partners.end())
                partners.push_back(f);
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_online_cache_mutex);
        for (const auto& u : g_online_users_cache) {
            if (u != username && std::find(partners.begin(), partners.end(), u) == partners.end())
                partners.push_back(u);
        }
    }
    // Bug 4 fix: 也包括 chat_history_map 中已有的 partner（通过 push 通知接收过消息的陌生人）
    {
        std::lock_guard<std::mutex> lk(g_chat_map_mutex);
        for (const auto& [p, _] : g_chat_history_map) {
            if (p != username && std::find(partners.begin(), partners.end(), p) == partners.end())
                partners.push_back(p);
        }
    }
    if (partners.empty()) return;

    // handle_get_unread_info 现在返回 (partner, latest_seq, latest_from)
    auto ret = conn->call<std::vector<std::tuple<std::string, uint64_t, std::string>>>("get_unread_info", username, partners);
    if (ret.error_code() != 200 || ret.value().empty()) return;

    std::lock_guard<std::mutex> lk(g_unread_mutex);
    for (const auto& [partner, latest_seq, latest_from] : ret.value()) {
        uint64_t cursor = 0;
        {
            std::lock_guard<std::mutex> lk(g_pull_cursor_mutex);
            auto it2 = g_pull_cursor.find(partner);
            if (it2 != g_pull_cursor.end()) cursor = it2->second;
        }
        if (cursor > 0 && latest_seq > cursor) {
            g_unread_counts[partner] = static_cast<size_t>(latest_seq - cursor);
        } else if (cursor == 0 && latest_from != username) {
            // 从未进入过的会话且最新消息不是自己发的 → 标记为 new
            g_unread_counts[partner] = 0;
        }
        // cursor == 0 && latest_from == username: 自己发的消息，不显示 new
    }
}

// ==================== 好友系统 UI ====================

// 辅助函数：检查用户名是否在在线缓存中
static bool is_online(const std::string& username) {
    std::lock_guard<std::mutex> lock(g_online_cache_mutex);
    return std::find(g_online_users_cache.begin(), g_online_users_cache.end(), username) != g_online_users_cache.end();
}

// 好友管理菜单
void draw_friend_menu(std::shared_ptr<connection> conn, const std::string& username) {
    while (true) {
        clear_screen();
        std::cout << "========================================" << std::endl;
        std::cout << "           好友管理                    " << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << " 1. 搜索用户" << std::endl;
        std::cout << " 2. 查看好友请求" << std::endl;
        std::cout << " 3. 查看已发送的请求" << std::endl;
        std::cout << " 4. 刷新好友列表" << std::endl;
        std::cout << " 5. 返回" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << " 请选择操作: ";

        std::string input;
        std::cin >> input;
        if (std::cin.fail()) {
            std::cin.clear();
            if (std::cin.eof()) return;
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
        }

        if (input == "1") {
            // 搜索用户
            clear_screen();
            std::cout << "=============== 搜索用户 ===============" << std::endl;
            std::cout << " 请输入用户名关键字: ";
            std::string keyword;
            std::cin >> keyword;

            auto ret = conn->call<std::vector<std::string>>("search_users", keyword, username);
            if (ret.error_code() != 200 || ret.value().empty()) {
                std::cout << " \033[33m未找到匹配的用户\033[0m" << std::endl;
                press_any_key();
                continue;
            }

            auto users = ret.value();
            std::cout << " 匹配到 " << users.size() << " 个用户:" << std::endl;
            std::cout << "----------------------------------------" << std::endl;
            for (size_t i = 0; i < users.size(); ++i) {
                std::cout << "  " << (i + 1) << ". " << users[i];
                if (is_online(users[i]))
                    std::cout << " \033[32m【在线】\033[0m";
                else
                    std::cout << " \033[33m【离线】\033[0m";
                std::cout << std::endl;
            }
            std::cout << "----------------------------------------" << std::endl;
            std::cout << " 选择用户发送好友请求 (0 取消): ";

            int choice;
            std::cin >> choice;
            if (std::cin.fail() || choice <= 0 || choice > (int)users.size()) {
                std::cin.clear();
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                continue;
            }

            std::string target = users[choice - 1];
            auto send_ret = conn->call<bool>("send_friend_request", username, target);
            if (send_ret.error_code() == 200 && send_ret.value()) {
                std::cout << " \033[32m好友请求已发送给 " << target << "\033[0m" << std::endl;
            } else {
                std::cout << " \033[33m发送失败：请求已存在或已是好友\033[0m" << std::endl;
            }
            press_any_key();

        } else if (input == "2") {
            // 查看待处理的好友请求
            auto ret = conn->call<std::vector<std::tuple<int, std::string, int64_t>>>("get_pending_requests", username);
            if (ret.error_code() != 200 || ret.value().empty()) {
                std::cout << " \033[33m暂无待处理的好友请求\033[0m" << std::endl;
                press_any_key();
                continue;
            }

            auto reqs = ret.value();
            clear_screen();
            std::cout << "============ 好友请求 ============" << std::endl;
            for (size_t i = 0; i < reqs.size(); ++i) {
                auto& [id, from, ts] = reqs[i];
                std::cout << " " << (i + 1) << ". " << from << std::endl;
            }
            std::cout << "----------------------------------------" << std::endl;
            std::cout << " \033[36m选择请求编号处理 (0 返回)\033[0m" << std::endl;
            std::cout << " > ";

            int choice;
            std::cin >> choice;
            if (std::cin.fail() || choice <= 0 || choice > (int)reqs.size()) {
                std::cin.clear();
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                continue;
            }

            auto& [req_id, req_from, _] = reqs[choice - 1];
            std::cout << " 接受 " << req_from << " 的好友请求? (y/n): ";
            std::string yn;
            std::cin >> yn;
            if (yn == "y" || yn == "Y") {
                auto handle_ret = conn->call<bool>("handle_friend_request", req_id, true);
                if (handle_ret.error_code() == 200 && handle_ret.value()) {
                    std::cout << " \033[32m已添加 " << req_from << " 为好友！\033[0m" << std::endl;
                    // 刷新好友列表
                    auto f_ret = conn->call<std::vector<std::string>>("get_friends", username);
                    if (f_ret.error_code() == 200) {
                        std::lock_guard<std::mutex> lk(g_friends_mutex);
                        g_friends_list = f_ret.value();
                    }
                } else {
                    std::cout << " \033[31m操作失败\033[0m" << std::endl;
                }
            } else {
                conn->call<bool>("handle_friend_request", req_id, false);
                std::cout << " \033[33m已拒绝\033[0m" << std::endl;
            }
            press_any_key();

        } else if (input == "3") {
            // 查看已发送的请求
            auto ret = conn->call<std::vector<std::tuple<int, std::string, int64_t>>>("get_sent_requests", username);
            if (ret.error_code() != 200 || ret.value().empty()) {
                std::cout << " \033[33m暂无已发送的待处理请求\033[0m" << std::endl;
                press_any_key();
                continue;
            }

            clear_screen();
            std::cout << "========== 已发送的请求 ==========" << std::endl;
            for (auto& [id, to, ts] : ret.value()) {
                std::cout << "  → " << to << " \033[33m[等待接受]\033[0m" << std::endl;
            }
            std::cout << "----------------------------------------" << std::endl;
            press_any_key();

        } else if (input == "4") {
            // 刷新好友列表
            auto ret = conn->call<std::vector<std::string>>("get_friends", username);
            if (ret.error_code() == 200) {
                std::lock_guard<std::mutex> lk(g_friends_mutex);
                g_friends_list = ret.value();
                std::cout << " \033[32m好友列表已刷新\033[0m" << std::endl;
            }
            press_any_key();

        } else if (input == "5" || input == "q") {
            break;
        }
    }
}

// 聊天界面 (推送模式)
void chat_room(std::shared_ptr<connection> conn, const std::string& my_name, const std::string& target_name) {
    if (g_server_offline) {
        std::cout << "\033[31m[系统] 服务器已断开，无法进入聊天\033[0m" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return;
    }

    // 进入聊天，清除该会话的未读计数
    {
        std::lock_guard<std::mutex> lk(g_unread_mutex);
        g_unread_counts.erase(target_name);
    }

    // 从本地 cursor 读取 after_seq，加载最新 20 条或 cursor 之后的消息
    uint64_t after_seq = 0;
    {
        std::lock_guard<std::mutex> lk(g_pull_cursor_mutex);
        auto it = g_pull_cursor.find(target_name);
        if (it != g_pull_cursor.end()) after_seq = it->second;
    }

    // 加载消息（SQLite 作为唯一数据源）
    {
        auto ret = conn->call<std::vector<std::tuple<uint64_t, std::string, std::string, std::string, std::string>>>(
            "sync_messages", my_name, target_name, after_seq, (size_t)20);
        if (ret.error_code() == 200 && !ret.value().empty()) {
            LOG_INFO("sync_messages returned {} msgs for {}", ret.value().size(), target_name);
            std::lock_guard<std::mutex> lock(g_chat_map_mutex);
            for (auto& [seq_id, from, to, msg, ts] : ret.value()) {
                if (from == my_name)
                    g_chat_history_map[target_name].emplace_back("__me__", msg, seq_id, ts);
                else
                    g_chat_history_map[target_name].emplace_back(from, msg, seq_id, ts);
                if (seq_id > g_conv_max_seq[target_name])
                    g_conv_max_seq[target_name] = seq_id;
                if (seq_id > g_last_seq_id) g_last_seq_id = seq_id;
            }
            // 更新拉取游标（取结果中最大 seq_id）
            uint64_t max_seen = std::get<0>(ret.value().back());
            {
                std::lock_guard<std::mutex> lk(g_pull_cursor_mutex);
                auto it = g_pull_cursor.find(target_name);
                if (it == g_pull_cursor.end() || max_seen > it->second)
                    g_pull_cursor[target_name] = max_seen;
            }
        }
    }

    // 按 seq_id 排序消息，确保显示顺序 = 时间顺序（旧→新）
    {
        std::lock_guard<std::mutex> lock(g_chat_map_mutex);
        auto& msgs = g_chat_history_map[target_name];
        std::sort(msgs.begin(), msgs.end(), [](const auto& a, const auto& b) {
            return std::get<2>(a) < std::get<2>(b);
        });
    }

    std::string input;
    bool skip_redraw = false;

    {
        std::lock_guard<std::mutex> lock(g_chat_map_mutex);
        g_current_chat_target = target_name;
        g_in_chat = true;
    }

    // 首次进入：先绘制界面（不使用 cin.ignore 避免阻塞）
    // hub 的 operator>> 遗留的 '\n' 由下面第一次 getline 消费并跳过
    bool first_draw = true;

    while (true) {
        if (std::cin.fail()) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }

        // 重绘界面（-g 已完成显式重绘，跳过本次循环顶部的重绘）
        if (!skip_redraw) {
            std::lock_guard<std::mutex> lock(g_chat_map_mutex);
            redraw_chat_ui(target_name);
        }
        skip_redraw = false;

        std::getline(std::cin, input);
        if (first_draw) {
            first_draw = false;
            if (input.empty()) continue; // 跳过 hub 遗留的空行
        }

        if (input == "quit" || input == "exit") {
            break;
        }

        if (!input.empty()) {
            // -g 指令：从 SQLite 翻查更早历史消息
            if (input.rfind("-g", 0) == 0) {
                size_t limit = 10;
                if (input.size() > 3) {
                    try { limit = std::stoul(input.substr(3)); }
                    catch (...) {}
                }
                uint64_t before_seq;
                {
                    std::lock_guard<std::mutex> lk(g_conv_min_seq_mutex);
                    auto it = g_conv_min_seq.find(target_name);
                    if (it == g_conv_min_seq.end() || it->second == 0) {
                        std::cout << "\033[33m没有更多历史消息\033[0m" << std::endl;
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        continue;
                    }
                    before_seq = it->second;
                }
                auto hret = conn->call<std::vector<std::tuple<uint64_t, std::string, std::string, std::string, std::string>>>(
                    "sync_history", my_name, target_name, before_seq, limit);
                if (hret.error_code() == 200 && !hret.value().empty()) {
                    std::lock_guard<std::mutex> lock(g_chat_map_mutex);
                    uint64_t new_min = UINT64_MAX;
                    // 结果 ASC（1,2,3），倒序迭代（3,2,1）逐个 prepend，
                    // 这样最早的消息最终位于 front
                    for (auto it = hret.value().rbegin(); it != hret.value().rend(); ++it) {
                        auto& [seq_id, from, to, msg, ts] = *it;
                        auto pos = g_chat_history_map[target_name].begin();
                        if (from == my_name) {
                            g_chat_history_map[target_name].emplace(pos, "__me__", msg, seq_id, ts);
                        } else {
                            g_chat_history_map[target_name].emplace(pos, from, msg, seq_id, ts);
                        }
                        if (seq_id < new_min) new_min = seq_id;
                    }
                    std::lock_guard<std::mutex> lk(g_conv_min_seq_mutex);
                    g_conv_min_seq[target_name] = new_min;

                    // 显式重绘一次当前聊天界面（不回到上一层）
                    {
                        std::lock_guard<std::mutex> lock_redraw(g_chat_map_mutex);
                        redraw_chat_ui(target_name);
                    }
                    skip_redraw = true; // 跳过 while 循环顶部的重复重绘
                } else {
                    std::cout << "\033[33m没有更多历史消息\033[0m" << std::endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                continue;
            }

            if (g_server_offline) {
                std::cout << "\033[31m[系统] 服务器已断开，无法发送消息\033[0m" << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                break;
            }
            auto send_ret = conn->call<uint64_t>("send_message", my_name, target_name, input);
            uint64_t send_seq_id = (send_ret.error_code() == 200) ? send_ret.value() : 0;
            auto local_ts = std::to_string(std::time(nullptr));
            {
                std::lock_guard<std::mutex> lock(g_chat_map_mutex);
                g_chat_history_map[target_name].emplace_back("__me__", input, send_seq_id, local_ts);
                if (send_seq_id > 0) {
                    if (send_seq_id > g_last_seq_id) g_last_seq_id = send_seq_id;
                    {
                        std::lock_guard<std::mutex> lk(g_conv_max_seq_mutex);
                        auto it = g_conv_max_seq.find(target_name);
                        if (it == g_conv_max_seq.end() || send_seq_id > it->second) {
                            g_conv_max_seq[target_name] = send_seq_id;
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lk(g_conv_min_seq_mutex);
                        auto it = g_conv_min_seq.find(target_name);
                        if (it == g_conv_min_seq.end() || send_seq_id < it->second) {
                            g_conv_min_seq[target_name] = send_seq_id;
                        }
                    }
                    // 更新拉取游标（自己发的消息也算已同步）
                    {
                        std::lock_guard<std::mutex> lk(g_pull_cursor_mutex);
                        auto it = g_pull_cursor.find(target_name);
                        if (it == g_pull_cursor.end() || send_seq_id > it->second)
                            g_pull_cursor[target_name] = send_seq_id;
                    }
                } else {
                    g_chat_history_map[target_name].emplace_back("__system__", "【对方用户不存在，消息发送失败】", 0, local_ts);
                }
            }
        }
    }

    // 退出聊天
    std::lock_guard<std::mutex> lock(g_chat_map_mutex);
    g_in_chat = false;
    g_current_chat_target.clear();
}

// 群聊界面
void group_chat_room(std::shared_ptr<connection> conn, const std::string& my_name) {
    if (g_server_offline) {
        std::cout << "\033[31m[系统] 服务器已断开，无法进入群聊\033[0m" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return;
    }

    std::string input;

    {
        std::lock_guard<std::mutex> lock(g_group_mutex);
        g_in_group_chat = true;
    }

    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    while (true) {
        {
            std::lock_guard<std::mutex> lock(g_group_mutex);
            redraw_group_chat_ui();
        }

        std::getline(std::cin, input);

        if (input == "quit" || input == "exit") {
            break;
        }

        if (!input.empty()) {
            if (g_server_offline) {
                std::cout << "\033[31m[系统] 服务器已断开，无法发送消息\033[0m" << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                break;
            }
            conn->notify("send_group_message", my_name, input);
            {
                std::lock_guard<std::mutex> lock(g_group_mutex);
                g_group_chat_history.emplace_back("__me__", input);
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_group_mutex);
    g_in_group_chat = false;
}

// 登录后的在线用户列表主界面
void online_user_hub(std::shared_ptr<connection> conn, const std::string& username) {
    g_in_hub = true;

    // 进入 hub 时初始化好友列表
    {
        auto ret = conn->call<std::vector<std::string>>("get_friends", username);
        if (ret.error_code() == 200) {
            std::lock_guard<std::mutex> lk(g_friends_mutex);
            g_friends_list = ret.value();
        }
    }
    // 初始化待处理好友请求
    {
        auto ret = conn->call<std::vector<std::tuple<int, std::string, int64_t>>>("get_pending_requests", username);
        if (ret.error_code() == 200) {
            std::lock_guard<std::mutex> lk(g_req_mutex);
            g_pending_requests = ret.value();
        }
    }
    // 此时好友列表已就绪，拉取未读消息计数（含离线好友的消息）
    fetch_unread_counts(conn, username);

    while (true) {
        draw_hub_ui();

        std::string input;
        std::cin >> input;
        if (std::cin.fail()) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
        }

        if (input == "q" || input == "quit" || input == "exit") {
            save_cursors(username);
            // 通知服务端下线并清除 token
            conn->notify("user_logout", username, load_session_token());
            remove_session_token();
            {
                std::lock_guard<std::mutex> lock(g_online_cache_mutex);
                g_online_users_cache.clear();
            }
            {
                std::lock_guard<std::mutex> lk(g_unread_mutex);
                g_unread_counts.clear();
            }
            {
                std::lock_guard<std::mutex> lk(g_friends_mutex);
                g_friends_list.clear();
            }
            {
                std::lock_guard<std::mutex> lk(g_req_mutex);
                g_pending_requests.clear();
            }
            break;
        } else if (input == "f" || input == "F") {
            if (g_server_offline) {
                std::cout << "\033[31m[系统] 服务器已断开\033[0m" << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            g_in_hub = false;
            draw_friend_menu(conn, username);
            g_in_hub = true;
        } else if (input == "g" || input == "G") {
            if (g_server_offline) {
                std::cout << "\033[31m[系统] 服务器已断开\033[0m" << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            g_in_hub = false;
            group_chat_room(conn, username);
            g_in_hub = true;
        } else if (input == "r" || input == "R") {
            if (g_server_offline) {
                std::cout << "\033[31m[系统] 服务器已断开\033[0m" << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            // 异步刷新在线用户列表，不阻塞 UI
            conn->async_call([](uint32_t ec, const std::string&, const nlohmann::json& data) {
                if (ec == 200 && !data.is_null()) {
                    try {
                        auto users = data.get<std::vector<std::string>>();
                        std::lock_guard<std::mutex> lock(g_online_cache_mutex);
                        g_online_users_cache = std::move(users);
                    } catch (...) {}
                }
            }, "get_online_users");
            continue;
        } else {
            try {
                int idx = std::stoi(input);
                // 构建可聊天用户列表（好友 + 在线非好友）
                std::vector<std::string> chat_users;
                std::vector<std::string> friends_copy;
                {
                    std::lock_guard<std::mutex> lk(g_friends_mutex);
                    friends_copy = g_friends_list;
                }
                std::set<std::string> friend_set(friends_copy.begin(), friends_copy.end());
                // 好友：在线在前
                for (const auto& f : friends_copy)
                    if (is_online(f)) chat_users.push_back(f);
                for (const auto& f : friends_copy)
                    if (!is_online(f)) chat_users.push_back(f);
                // 在线非好友
                {
                    std::lock_guard<std::mutex> lock(g_online_cache_mutex);
                    for (const auto& u : g_online_users_cache) {
                        if (u != username && friend_set.find(u) == friend_set.end())
                            chat_users.push_back(u);
                    }
                }
                if (idx >= 1 && idx <= (int)chat_users.size()) {
                    g_in_hub = false;
                    chat_room(conn, username, chat_users[idx - 1]);
                    g_in_hub = true;
                }
            } catch (...) {
                // 无效输入
            }
        }
    }
    g_in_hub = false;
}

void print_menu() {
    std::cout << "========================================" << std::endl;
    std::cout << "           聊天室客户端                 " << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << " 状态: 未登录" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  1. 用户登录" << std::endl;
    std::cout << "  2. 用户注册" << std::endl;
    std::cout << "  5. 退出程序" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << " 请选择操作: ";
}

int main(int argc, char* argv[]) {
    uint16_t port = 8877;
    if (argc > 1) port = static_cast<uint16_t>(std::stoi(argv[1]));

    wlog::logger::get().init("logs/chatting_room_client.log");
#ifdef _LOG_CONSOLE
    spdlog::default_logger()->sinks().pop_back(); // 移除控制台输出，只写文件
#endif

    auto& client = mrpc::client::get();
    client.run();

    // 注册接收消息的回调函数到客户端的 router
    client.router().reg_handle("on_message", on_message);
    client.router().reg_handle("on_group_message", on_group_message);
    client.router().reg_handle("on_user_status_changed", on_user_status_changed);
    client.router().reg_handle("on_server_shutdown", on_server_shutdown);
    client.router().reg_handle("on_new_friend_request", on_new_friend_request);
    client.router().reg_handle("on_friend_request_accepted", on_friend_request_accepted);

    // 连接到服务器
    clear_screen();
    std::cout << "正在连接聊天室服务器 (127.0.0.1:" << port << ")..." << std::endl;
    auto conn = client.connect("127.0.0.1", port);
    if (!conn) {
        std::cerr << "连接服务器失败！请确认服务器已启动。" << std::endl;
        return 1;
    }
    conn->start_heartbeat(10); // 每10秒发送心跳保活

    // 检测连接断开（服务端崩溃或网络断开）
    conn->set_closed_callback([](const std::shared_ptr<connection>&) {
        g_server_offline = true;
        std::cout << "\n\033[31m[系统] 与服务器断开连接\033[0m" << std::endl;
    });

    std::string current_username;
    bool is_logged_in = false;

    // === 尝试 token 自动登录 ===
    {
        std::ifstream sf("session_token.txt");
        if (sf) {
            std::string line;
            if (std::getline(sf, line)) {
                auto pos = line.rfind(':');
                if (pos != std::string::npos) {
                    std::string saved_user = line.substr(0, pos);
                    std::string saved_token = line.substr(pos + 1);
                    auto ret = conn->call<std::string>("token_login", saved_token);
                    if (ret.error_code() == 200 && !ret.value().empty()) {
                        current_username = ret.value();
                        is_logged_in = true;
                        g_self_username = current_username;

                        // 保存最新 username:token（万一 username 有变化）
                        save_session_token(current_username, saved_token);

                        load_cursors(current_username);
                        auto init_ret = conn->call<std::vector<std::string>>("get_online_users");
                        if (init_ret.error_code() == 200) {
                            std::lock_guard<std::mutex> lock(g_online_cache_mutex);
                            g_online_users_cache = init_ret.value();
                        }

                        // 直接进入 hub
                        online_user_hub(conn, current_username);
                        // 从 hub 退出
                        is_logged_in = false;
                        g_self_username.clear();
                        current_username.clear();
                    }
                }
            }
        }
    }

    while (true) {
        clear_screen();
        print_menu();

        int choice;
        std::cin >> choice;
        if (std::cin.fail()) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            choice = 0;
        }

        switch (choice) {
            case 1: { // 登录
                clear_screen();
                std::cout << "=============== 用户登录 ===============" << std::endl;
                std::cout << " 请输入用户名: ";
                std::cin >> current_username;
                std::cout << " 请输入密码: ";
                std::string password;
                std::cin >> password;

                std::cout << " 正在登录..." << std::endl;
                auto ret = conn->call<std::string>("user_login", current_username, password);
                if (ret.error_code() == 200 && !ret.value().empty()) {
                    // 保存 token 到本地文件
                    save_session_token(current_username, ret.value());
                    std::cout << " \033[32m登录成功！\033[0m 欢迎 " << current_username << " 加入聊天室！" << std::endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    is_logged_in = true;
                    g_self_username = current_username;

                    // 恢复上次的游标（per-conversation 已同步的最大 seq_id）
                    load_cursors(current_username);

                    // 初始化本地用户列表缓存
                    auto init_ret = conn->call<std::vector<std::string>>("get_online_users");
                    if (init_ret.error_code() == 200) {
                        std::lock_guard<std::mutex> lock(g_online_cache_mutex);
                        g_online_users_cache = init_ret.value();
                    }

                    // 进入在线用户列表主界面
                    online_user_hub(conn, current_username);
                    // 从 hub 退出 = 用户已下线
                    is_logged_in = false;
                    g_self_username.clear();
                    current_username.clear();
                } else {
                    std::cout << " \033[31m登录失败！\033[0m 用户名可能已存在" << std::endl;
                    current_username.clear();
                    press_any_key();
                }
                break;
            }
            case 2: { // 注册
                clear_screen();
                std::cout << "=============== 用户注册 ===============" << std::endl;
                std::cout << " 请输入用户名: ";
                std::string new_user;
                std::cin >> new_user;
                std::cout << " 请输入密码: ";
                std::string reg_password;
                std::cin >> reg_password;

                std::cout << " 正在注册..." << std::endl;
                auto reg_ret = conn->call<bool>("register_user", new_user, reg_password);
                if (reg_ret.error_code() == 200 && reg_ret.value()) {
                    std::cout << " \033[32m注册成功！\033[0m 请返回登录" << std::endl;
                } else {
                    std::cout << " \033[31m注册失败！\033[0m 用户名可能已存在" << std::endl;
                }
                press_any_key();
                break;
            }
            case 3: { // 查看在线用户
                clear_screen();
                std::cout << "============= 在线用户列表 =============" << std::endl;
                auto ret = conn->call<std::vector<std::string>>("get_online_users");
                if (ret.error_code() == 200) {
                    auto users = ret.value();
                    std::cout << " 共有 " << users.size() << " 位用户在线" << std::endl;
                    std::cout << "----------------------------------------" << std::endl;
                    if (users.empty()) {
                        std::cout << " 暂无在线用户" << std::endl;
                    } else {
                        for (size_t i = 0; i < users.size(); ++i) {
                            std::cout << "  " << (i + 1) << ". " << users[i];
                            if (users[i] == current_username) {
                                std::cout << " \033[32m(我)\033[0m";
                            }
                            std::cout << std::endl;
                        }
                    }
                    std::cout << "----------------------------------------" << std::endl;
                } else {
                    std::cout << " \033[31m获取在线用户列表失败！\033[0m" << std::endl;
                }
                press_any_key();
                break;
            }
            case 4: { // 选择用户私聊
                clear_screen();
                if (!is_logged_in) {
                    std::cout << "============= 选择用户私聊 =============" << std::endl;
                    std::cout << " \033[31m请先登录！\033[0m" << std::endl;
                    press_any_key();
                    break;
                }

                std::cout << "============= 选择用户私聊 =============" << std::endl;
                auto ret = conn->call<std::vector<std::string>>("get_online_users");
                if (ret.error_code() != 200) {
                    std::cout << " \033[31m获取在线用户列表失败！\033[0m" << std::endl;
                    press_any_key();
                    break;
                }

                auto users = ret.value();
                std::vector<std::string> other_users;
                for (const auto& user : users) {
                    if (user != current_username) {
                        other_users.push_back(user);
                    }
                }

                if (other_users.empty()) {
                    std::cout << " 暂无其他在线用户" << std::endl;
                    press_any_key();
                    break;
                }

                std::cout << " 可聊天的用户:" << std::endl;
                std::cout << "----------------------------------------" << std::endl;
                for (size_t i = 0; i < other_users.size(); ++i) {
                    std::cout << "  " << (i + 1) << ". " << other_users[i] << std::endl;
                }
                std::cout << "----------------------------------------" << std::endl;
                std::cout << " 请选择要聊天的用户编号 (0 取消): ";

                int choice;
                std::cin >> choice;
                if (std::cin.fail()) {
                    std::cin.clear();
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                } else if (choice > 0 && choice <= (int)other_users.size()) {
                    std::string target_user = other_users[choice - 1];
                    chat_room(conn, current_username, target_user);
                }
                break;
            }
            case 5: { // 进入群聊
                clear_screen();
                if (!is_logged_in) {
                    std::cout << "============= 进入群聊 =============" << std::endl;
                    std::cout << " \033[31m请先登录！\033[0m" << std::endl;
                    press_any_key();
                    break;
                }
                group_chat_room(conn, current_username);
                break;
            }
            case 6: { // 退出
                clear_screen();
                std::cout << "============= 退出程序 ===============" << std::endl;
                if (is_logged_in && !current_username.empty()) {
                    save_cursors(current_username);
                    std::cout << " 正在为用户 " << current_username << " 下线..." << std::endl;
                    conn->notify("user_logout", current_username, load_session_token());
                    remove_session_token();
                    std::cout << " \033[32m下线成功！\033[0m" << std::endl;
                }
                std::cout << " 正在断开连接..." << std::endl;
                client.shutdown();
                clear_screen();
                std::cout << "========================================" << std::endl;
                std::cout << "           感谢使用聊天室               " << std::endl;
                std::cout << "               再见！                   " << std::endl;
                std::cout << "========================================" << std::endl;
                client.wait_shutdown();
                wlog::logger::get().shutdown();
                return 0;
            }
            default: {
                clear_screen();
                std::cout << "\033[31m无效的选择，请重新输入！\033[0m" << std::endl;
                press_any_key();
                break;
            }
        }
    }

    client.shutdown();
    wlog::logger::get().shutdown();
    return 0;
}
