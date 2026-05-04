#include <logger.hpp>
#include <mrpc/client.hpp>
#include <iostream>
#include <string>
#include <limits>
#include <mutex>
#include <vector>

using namespace mrpc;

// 全局：聊天历史记录和同步锁
std::vector<std::pair<std::string, std::string>> g_chat_history;
std::mutex g_chat_mutex;
std::string g_current_chat_target;
bool g_in_chat = false;
bool g_in_group_chat = false;

// 全局：群聊历史
std::vector<std::pair<std::string, std::string>> g_group_chat_history;
std::mutex g_group_mutex;

// 清屏
void clear_screen() {
    system("clear");
}

// 按任意键继续
void press_any_key() {
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

    for (const auto& msg : g_chat_history) {
        if (msg.first == target_name) {
            std::cout << "\033[34m" << target_name << "\033[0m: " << msg.second << std::endl;
        } else if (msg.first == "__me__") {
            std::cout << "\033[32m我\033[0m: " << msg.second << std::endl;
        }
    }

    std::cout << "----------------------------------------" << std::endl;
    std::cout << " 输入消息 (输入 'quit' 退出): " << std::endl;
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

// 接收私聊消息的RPC回调
int on_message(connection::cptr conn, const std::string& from_user, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_chat_mutex);
    g_chat_history.emplace_back(from_user, message);

    if (g_in_chat && from_user == g_current_chat_target) {
        redraw_chat_ui(g_current_chat_target);
    }
    return 0;
}

// 接收群聊消息的RPC回调
int on_group_message(connection::cptr conn, const std::string& from_user, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_group_mutex);
    g_group_chat_history.emplace_back(from_user, message);

    if (g_in_group_chat) {
        redraw_group_chat_ui();
    }
    return 0;
}

// 聊天界面 (推送模式)
void chat_room(std::shared_ptr<connection> conn, const std::string& my_name, const std::string& target_name) {
    std::string input;

    {
        std::lock_guard<std::mutex> lock(g_chat_mutex);
        g_current_chat_target = target_name;
        g_in_chat = true;
        g_chat_history.clear();
    }

    // 清除输入缓冲区
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    while (true) {
        // 重绘界面
        {
            std::lock_guard<std::mutex> lock(g_chat_mutex);
            redraw_chat_ui(target_name);
        }

        std::getline(std::cin, input);

        if (input == "quit" || input == "exit") {
            break;
        }

        if (!input.empty()) {
            auto send_ret = conn->call<bool>("send_message", my_name, target_name, input);
            if (send_ret.error_code() == 200 && send_ret.value()) {
                // 发送成功，本地记录
                std::lock_guard<std::mutex> lock(g_chat_mutex);
                g_chat_history.emplace_back("__me__", input);
            } else {
                std::cout << "\r\033[2K\033[31m[发送失败] 用户可能已离线\033[0m" << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(800));
            }
        }
    }

    // 退出聊天
    std::lock_guard<std::mutex> lock(g_chat_mutex);
    g_in_chat = false;
    g_current_chat_target.clear();
}

// 群聊界面
void group_chat_room(std::shared_ptr<connection> conn, const std::string& my_name) {
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
            auto send_ret = conn->call<3000, bool>("send_group_message", my_name, input);
            if (send_ret.error_code() == 200 && send_ret.value()) {
                std::lock_guard<std::mutex> lock(g_group_mutex);
                g_group_chat_history.emplace_back("__me__", input);
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_group_mutex);
    g_in_group_chat = false;
}

void print_menu(const std::string& username, bool logged_in) {
    std::cout << "========================================" << std::endl;
    std::cout << "           聊天室客户端                 " << std::endl;
    std::cout << "========================================" << std::endl;
    if (logged_in) {
        std::cout << " 当前用户: " << username << " [在线]" << std::endl;
    } else {
        std::cout << " 状态: 未登录" << std::endl;
    }
    std::cout << "========================================" << std::endl;
    std::cout << "  1. 用户登录" << std::endl;
    std::cout << "  2. 查看在线用户" << std::endl;
    std::cout << "  3. 选择用户私聊" << std::endl;
    std::cout << "  4. 进入群聊" << std::endl;
    std::cout << "  5. 退出程序" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << " 请选择操作: ";
}

int main() {
    wlog::logger::get().init("logs/chatting_room_client.log");

    auto& client = mrpc::client::get();
    client.run();

    // 注册接收消息的回调函数到客户端的 router
    client.router().reg_handle("on_message", on_message);
    client.router().reg_handle("on_group_message", on_group_message);

    // 连接到服务器
    clear_screen();
    std::cout << "正在连接聊天室服务器..." << std::endl;
    auto conn = client.connect("127.0.0.1", 8888);
    if (!conn) {
        std::cerr << "连接服务器失败！请确认服务器已启动。" << std::endl;
        return 1;
    }

    std::string current_username;
    bool is_logged_in = false;

    while (true) {
        clear_screen();
        print_menu(current_username, is_logged_in);

        int choice;
        std::cin >> choice;

        switch (choice) {
            case 1: { // 登录
                clear_screen();
                std::cout << "=============== 用户登录 ===============" << std::endl;
                if (is_logged_in) {
                    std::cout << " 您已经登录了！当前用户: " << current_username << std::endl;
                } else {
                    std::cout << " 请输入用户名: ";
                    std::cin >> current_username;

                    std::cout << " 正在登录..." << std::endl;
                    auto ret = conn->call<bool>("user_login", current_username);
                    if (ret.error_code() == 200 && ret.value()) {
                        std::cout << " \033[32m登录成功！\033[0m 欢迎 " << current_username << " 加入聊天室！" << std::endl;
                        is_logged_in = true;
                    } else {
                        std::cout << " \033[31m登录失败！\033[0m 用户名可能已存在" << std::endl;
                        current_username.clear();
                    }
                }
                press_any_key();
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
                if (choice > 0 && choice <= (int)other_users.size()) {
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
                    auto ret = conn->call<bool>("user_logout", current_username);
                    if (ret.error_code() == 200 && ret.value()) {
                        std::cout << " \033[32m下线成功！\033[0m" << std::endl;
                    } else {
                        std::cout << " \033[33m下线提醒: " << ret.error_msg() << "\033[0m" << std::endl;
                    }
                }
                std::cout << " 正在断开连接..." << std::endl;
                client.shutdown();
                clear_screen();
                std::cout << "========================================" << std::endl;
                std::cout << "           感谢使用聊天室               " << std::endl;
                std::cout << "               再见！                   " << std::endl;
                std::cout << "========================================" << std::endl;
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
