#include <logger.hpp>
#include <mrpc/client.hpp>
#include <iostream>
#include <string>
#include <limits>
#include <mutex>
#include <atomic>
#include <vector>
#include <tuple>

using namespace mrpc;

// 持久化会话消息存储: target_user -> vector<(sender, message)>
// sender 为对方用户名 或 "__me__"（自己发的消息），永不清理
std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> g_chat_history_map;
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

// 重新渲染私聊界面
void redraw_chat_ui(const std::string& target_name) {
    std::cout << "\033[H\033[2J" << std::flush;
    std::cout << "========================================" << std::endl;
    std::cout << "       与 " << target_name << " 私聊中        " << std::endl;
    std::cout << "   \033[32m[推送模式] 消息实时到达\033[0m        " << std::endl;
    std::cout << "========================================" << std::endl;

    auto it = g_chat_history_map.find(target_name);
    if (it != g_chat_history_map.end()) {
        for (const auto& msg : it->second) {
            if (msg.first == "__me__") {
                std::cout << "\033[32m我\033[0m: " << msg.second << std::endl;
            } else {
                std::cout << "\033[34m" << msg.first << "\033[0m: " << msg.second << std::endl;
            }
        }
    }

    std::cout << "----------------------------------------" << std::endl;
    std::cout << " 输入消息 (输入 'quit' 退出): " << std::endl;
    std::cout << " > " << std::flush;
}

// 绘制 Hub 主界面（在线用户列表）
void draw_hub_ui() {
    clear_screen();
    std::cout << "========================================" << std::endl;
    std::cout << "           聊天室客户端                 " << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << " 当前用户: " << g_self_username << " [在线]" << std::endl;
    std::cout << "========================================" << std::endl;
    {
        std::lock_guard<std::mutex> lock(g_online_cache_mutex);
        std::vector<std::string> other_users;
        for (const auto& u : g_online_users_cache) {
            if (u != g_self_username) other_users.push_back(u);
        }
        if (other_users.empty()) {
            std::cout << " 暂无其他在线用户" << std::endl;
        } else {
            for (size_t i = 0; i < other_users.size(); ++i) {
                std::cout << "  " << (i + 1) << ". " << other_users[i] << "【在线】" << std::endl;
            }
        }
    }
    std::cout << "----------------------------------------" << std::endl;
    std::cout << " \033[36m[输入编号私聊]  g-群聊  r-刷新  q-退出登录\033[0m" << std::endl;
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

// 接收私聊消息的RPC回调（含 seq_id，用于更新已同步进度）
int on_message(connection::cptr conn, const std::string& from_user, const std::string& message, uint64_t seq_id) {
    {
        std::lock_guard<std::mutex> lock(g_chat_map_mutex);
        g_chat_history_map[from_user].emplace_back(from_user, message);
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
        if (g_in_chat && from_user == g_current_chat_target) {
            redraw_chat_ui(g_current_chat_target);
        }
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

// 聊天界面 (推送模式)
void chat_room(std::shared_ptr<connection> conn, const std::string& my_name, const std::string& target_name) {
    if (g_server_offline) {
        std::cout << "\033[31m[系统] 服务器已断开，无法进入聊天\033[0m" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return;
    }

    // 进入聊天时拉取最近 10 条消息
    {
        auto ret = conn->call<std::vector<std::tuple<uint64_t, std::string, std::string, std::string>>>(
            "get_recent_messages", my_name, target_name, (size_t)10);
        if (ret.error_code() == 200 && !ret.value().empty()) {
            uint64_t known_max = 0;
            {
                std::lock_guard<std::mutex> lk(g_conv_max_seq_mutex);
                auto it = g_conv_max_seq.find(target_name);
                if (it != g_conv_max_seq.end()) known_max = it->second;
            }
            std::lock_guard<std::mutex> lock(g_chat_map_mutex);
            uint64_t min_seq = UINT64_MAX;
            for (auto& [seq_id, from, to, msg] : ret.value()) {
                if (seq_id <= known_max) continue; // 推送已到达，跳过避免重复
                if (from == my_name) {
                    g_chat_history_map[target_name].emplace_back("__me__", msg);
                } else {
                    g_chat_history_map[target_name].emplace_back(from, msg);
                }
                if (seq_id > g_last_seq_id) g_last_seq_id = seq_id;
                if (seq_id < min_seq) min_seq = seq_id;
            }
            if (min_seq != UINT64_MAX) {
                std::lock_guard<std::mutex> lk(g_conv_min_seq_mutex);
                g_conv_min_seq[target_name] = min_seq;
            }
        }
    }

    std::string input;

    {
        std::lock_guard<std::mutex> lock(g_chat_map_mutex);
        g_current_chat_target = target_name;
        g_in_chat = true;
    }

    // 清除输入缓冲区
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    while (true) {
        // 重绘界面
        {
            std::lock_guard<std::mutex> lock(g_chat_map_mutex);
            redraw_chat_ui(target_name);
        }

        std::getline(std::cin, input);

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
                auto hret = conn->call<std::vector<std::tuple<uint64_t, std::string, std::string, std::string>>>(
                    "sync_history", my_name, target_name, before_seq, limit);
                if (hret.error_code() == 200 && !hret.value().empty()) {
                    std::lock_guard<std::mutex> lock(g_chat_map_mutex);
                    uint64_t new_min = UINT64_MAX;
                    for (auto& [seq_id, from, to, msg] : hret.value()) {
                        // 结果已按 seq_id ASC 排列，顺序 prepend 到前端
                        auto pos = g_chat_history_map[target_name].begin();
                        if (from == my_name) {
                            g_chat_history_map[target_name].emplace(pos, "__me__", msg);
                        } else {
                            g_chat_history_map[target_name].emplace(pos, from, msg);
                        }
                        if (seq_id < new_min) new_min = seq_id;
                    }
                    std::lock_guard<std::mutex> lk(g_conv_min_seq_mutex);
                    g_conv_min_seq[target_name] = new_min;
                    redraw_chat_ui(target_name);
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
            conn->notify("send_message", my_name, target_name, input);
            {
                std::lock_guard<std::mutex> lock(g_chat_map_mutex);
                g_chat_history_map[target_name].emplace_back("__me__", input);
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
            conn->notify("user_logout", username);
            {
                std::lock_guard<std::mutex> lock(g_online_cache_mutex);
                g_online_users_cache.clear();
            }
            break;
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
                // 从本地缓存构建可聊天用户列表（排除自己）
                std::vector<std::string> other_users;
                {
                    std::lock_guard<std::mutex> lock(g_online_cache_mutex);
                    for (const auto& u : g_online_users_cache) {
                        if (u != username) {
                            other_users.push_back(u);
                        }
                    }
                }
                if (idx >= 1 && idx <= (int)other_users.size()) {
                    g_in_hub = false;
                    chat_room(conn, username, other_users[idx - 1]);
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
    std::cout << "  5. 退出程序" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << " 请选择操作: ";
}

int main() {
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

    // 连接到服务器
    clear_screen();
    std::cout << "正在连接聊天室服务器..." << std::endl;
    auto conn = client.connect("127.0.0.1", 8888);
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

                std::cout << " 正在登录..." << std::endl;
                auto ret = conn->call<bool>("user_login", current_username);
                if (ret.error_code() == 200 && ret.value()) {
                    std::cout << " \033[32m登录成功！\033[0m 欢迎 " << current_username << " 加入聊天室！" << std::endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    is_logged_in = true;
                    g_self_username = current_username;

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
            case 2: { // 查看在线用户
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
            case 3: { // 选择用户私聊
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
            case 4: { // 进入群聊
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
            case 5: { // 退出
                clear_screen();
                std::cout << "============= 退出程序 ===============" << std::endl;
                if (is_logged_in && !current_username.empty()) {
                    std::cout << " 正在为用户 " << current_username << " 下线..." << std::endl;
                    conn->notify("user_logout", current_username);
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
