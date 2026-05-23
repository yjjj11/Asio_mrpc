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
#include <memory>
#include <cstring>

#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>

using namespace mrpc;

// ==================== 全局变量（与 client.cpp 一致） ====================

// 持久化会话消息存储: target_user -> vector<(sender, message, seq_id, ts)>
std::unordered_map<std::string, std::vector<std::tuple<std::string, std::string, uint64_t, std::string>>> g_chat_history_map;
std::mutex g_chat_map_mutex;
std::string g_current_chat_target;
bool g_in_chat = false;
bool g_in_group_chat = false;

// 群聊历史
std::vector<std::pair<std::string, std::string>> g_group_chat_history;
std::mutex g_group_mutex;

// 在线用户缓存
std::vector<std::string> g_online_users_cache;
uint64_t g_last_seq_id = 0;
std::mutex g_online_cache_mutex;
std::string g_self_username;
bool g_in_hub = false;
std::atomic<bool> g_server_offline{false};

// 每个会话的 min/max seq_id
std::unordered_map<std::string, uint64_t> g_conv_min_seq;
std::mutex g_conv_min_seq_mutex;
std::unordered_map<std::string, uint64_t> g_conv_max_seq;
std::mutex g_conv_max_seq_mutex;

// 拉取游标
std::unordered_map<std::string, uint64_t> g_pull_cursor;
std::mutex g_pull_cursor_mutex;

// 未读计数
std::unordered_map<std::string, size_t> g_unread_counts;
std::mutex g_unread_mutex;

// 好友列表
std::vector<std::string> g_friends_list;
std::mutex g_friends_mutex;

// 待处理好友请求 (id, from_user, created_at)
std::vector<std::tuple<int, std::string, int64_t>> g_pending_requests;
std::mutex g_req_mutex;

// RPC 连接
std::shared_ptr<connection> g_conn;

// 是否需要重绘（回调线程设 true，主循环消费）
std::atomic<bool> g_need_redraw{false};
std::atomic<bool> g_need_hub_redraw{false};
std::atomic<bool> g_need_chat_scroll{false};

// ==================== 前向声明 ====================
static bool is_online(const std::string& username);
static std::string fmt_ts(const std::string& ts_str);

// ==================== UI 状态 ====================

enum class AppState {
    CONNECTING,
    LOGIN,
    HUB,
    CHAT,
    GROUP_CHAT,
    FRIEND_MENU,
};

static AppState g_state = AppState::CONNECTING;
static char g_login_username[128] = {};
static char g_login_password[128] = {};
static char g_chat_input[4096] = {};
static std::string g_chat_target;
static bool g_chat_needs_init = false;
static bool g_chat_has_history = false;  // true = 已加载过更早消息

// 好友管理页 UI 状态
static char g_friend_search_keyword[128] = {};
static std::vector<std::string> g_friend_search_results;

// 第三方消息通知（在 chat 界面显示）
static std::string g_third_party_notify;
static std::mutex g_notify_mutex;

// 登录失败/注册反馈消息
static std::string g_login_feedback;
static std::string g_friend_feedback;
static double g_feedback_time = 0;

// 自动滚动到最新消息
static bool g_auto_scroll = true;

// 保存的 token（文件读取后异步验证）
static std::string g_saved_token;
static std::atomic<bool> g_auto_login_failed{false};

// ==================== 持久化函数 ====================

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

void save_cursors(const std::string& username) {
    std::ofstream f("cursor_" + username + ".txt");
    if (!f) return;
    std::lock_guard<std::mutex> lk(g_pull_cursor_mutex);
    for (const auto& [target, seq] : g_pull_cursor) {
        f << target << ":" << seq << "\n";
    }
}

std::string load_session_token() {
    std::ifstream f("session_token.txt");
    if (!f) return {};
    std::string line;
    if (!std::getline(f, line)) return {};
    auto pos = line.rfind(':');
    if (pos == std::string::npos) return {};
    return line.substr(pos + 1);
}

void save_session_token(const std::string& username, const std::string& token) {
    std::ofstream f("session_token.txt");
    if (!f) return;
    f << username << ":" << token << "\n";
}

void remove_session_token() {
    std::remove("session_token.txt");
}

// ==================== 辅助函数 ====================

static std::string fmt_ts(const std::string& ts_str) {
    if (ts_str.empty()) return "";
    std::time_t t = static_cast<std::time_t>(std::stoull(ts_str));
    std::tm* lt = std::localtime(&t);
    if (!lt) return "";
    std::ostringstream oss;
    oss << std::put_time(lt, "%m-%d %H:%M");
    return oss.str();
}

static bool is_online(const std::string& username) {
    std::lock_guard<std::mutex> lock(g_online_cache_mutex);
    return std::find(g_online_users_cache.begin(), g_online_users_cache.end(), username) != g_online_users_cache.end();
}

void fetch_unread_counts(std::shared_ptr<connection> conn, const std::string& username) {
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
    // Bug 4 fix: 也包括 chat_history_map 中已有的 partner
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
            g_unread_counts[partner] = 0;
        }
    }
}

// ==================== RPC 回调函数 ====================

int on_message(connection::cptr conn, const std::string& from_user, const std::string& message, uint64_t seq_id, const std::string& timestamp) {
    {
        std::lock_guard<std::mutex> lock(g_chat_map_mutex);
        g_chat_history_map[from_user].emplace_back(from_user, message, seq_id, timestamp);
        if (seq_id > g_last_seq_id) g_last_seq_id = seq_id;
        {
            std::lock_guard<std::mutex> lk(g_conv_min_seq_mutex);
            auto it = g_conv_min_seq.find(from_user);
            if (it == g_conv_min_seq.end() || seq_id < it->second)
                g_conv_min_seq[from_user] = seq_id;
        }
        {
            std::lock_guard<std::mutex> lk(g_conv_max_seq_mutex);
            auto it = g_conv_max_seq.find(from_user);
            if (it == g_conv_max_seq.end() || seq_id > it->second)
                g_conv_max_seq[from_user] = seq_id;
        }
        if (g_state == AppState::CHAT && from_user == g_chat_target) {
            g_auto_scroll = true;
        } else if (g_state == AppState::CHAT && from_user != g_chat_target) {
            std::lock_guard<std::mutex> nlk(g_notify_mutex);
            g_third_party_notify = from_user;
        }
        if (g_state == AppState::HUB) {
            g_need_hub_redraw = true;
        }
    }
    if (g_state == AppState::HUB || (g_state == AppState::CHAT && from_user != g_chat_target)) {
        std::lock_guard<std::mutex> lk(g_unread_mutex);
        g_unread_counts[from_user]++;
    }
    return 0;
}

int on_group_message(connection::cptr conn, const std::string& from_user, const std::string& message) {
    if (from_user == g_self_username) return 0;
    std::lock_guard<std::mutex> lock(g_group_mutex);
    g_group_chat_history.emplace_back(from_user, message);
    if (g_state == AppState::GROUP_CHAT) g_auto_scroll = true;
    return 0;
}

int on_user_status_changed(connection::cptr conn, const std::string& username, bool online) {
    if (username == g_self_username || g_server_offline) return 0;
    {
        std::lock_guard<std::mutex> lock(g_online_cache_mutex);
        if (online) {
            if (std::find(g_online_users_cache.begin(), g_online_users_cache.end(), username) == g_online_users_cache.end())
                g_online_users_cache.push_back(username);
        } else {
            auto it = std::find(g_online_users_cache.begin(), g_online_users_cache.end(), username);
            if (it != g_online_users_cache.end()) g_online_users_cache.erase(it);
        }
    }
    if (g_state == AppState::HUB) g_need_hub_redraw = true;
    return 0;
}

int on_server_shutdown(connection::cptr conn) {
    g_server_offline = true;
    return 0;
}

int on_new_friend_request(connection::cptr conn, const std::string& from_user) {
    {
        std::lock_guard<std::mutex> lk(g_req_mutex);
        for (const auto& [id, f, _] : g_pending_requests) {
            if (f == from_user) return 0;
        }
        g_pending_requests.emplace_back(0, from_user, 0);
    }
    if (g_state == AppState::HUB) g_need_hub_redraw = true;
    return 0;
}

int on_friend_request_accepted(connection::cptr conn, const std::string& by_user) {
    auto ret = conn->call<std::vector<std::string>>("get_friends", g_self_username);
    if (ret.error_code() == 200) {
        std::lock_guard<std::mutex> lk(g_friends_mutex);
        g_friends_list = ret.value();
    }
    if (g_state == AppState::HUB) g_need_hub_redraw = true;
    return 0;
}

// ==================== UI 渲染函数 ====================

// ---- 连接中 ----
static void render_connecting() {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Connecting", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    float win_w = ImGui::GetWindowWidth();
    float win_h = ImGui::GetWindowHeight();

    // Async token auto-login
    static bool token_check_fired = false;
    if (!g_saved_token.empty() && !token_check_fired) {
        token_check_fired = true;
        std::string token_copy = g_saved_token;
        g_conn->async_call([token_copy](uint32_t ec, const std::string&, const nlohmann::json& data) {
            if (ec == 200 && !data.is_null()) {
                try {
                    std::string username = data.get<std::string>();
                    if (!username.empty()) {
                        g_self_username = username;
                        save_session_token(username, token_copy);
                        load_cursors(username);
                        g_conn->async_call([](uint32_t ec2, const std::string&, const nlohmann::json& data2) {
                            if (ec2 == 200 && !data2.is_null()) {
                                try {
                                    auto users = data2.get<std::vector<std::string>>();
                                    std::lock_guard<std::mutex> lock(g_online_cache_mutex);
                                    g_online_users_cache = std::move(users);
                                } catch (...) {}
                            }
                            g_state = AppState::HUB;
                        }, "get_online_users");
                        return;
                    }
                } catch (...) {}
            }
            g_auto_login_failed = true;
        }, "token_login", token_copy);
    }

    // Fallthrough: no token or check failed
    if (g_state == AppState::CONNECTING && (g_saved_token.empty() || g_auto_login_failed)) {
        g_state = AppState::LOGIN;
    }

    std::string text = g_saved_token.empty() ? "正在连接聊天室服务器..." : "正在验证登录状态...";
    float text_w = ImGui::CalcTextSize(text.c_str()).x;
    ImGui::SetCursorPos(ImVec2((win_w - text_w) * 0.5f, win_h * 0.4f));
    ImGui::Text("%s", text.c_str());

    ImGui::SetCursorPos(ImVec2((win_w - 80) * 0.5f, win_h * 0.4f + 40));
    if (ImGui::Button("取消", ImVec2(80, 30))) {
        SDL_Event q;
        q.type = SDL_QUIT;
        SDL_PushEvent(&q);
    }
    ImGui::End();
}

// ---- 登录/注册 ----
static void render_login() {
    auto& io = ImGui::GetIO();
    float win_w = io.DisplaySize.x;
    float win_h = io.DisplaySize.y;
    float card_w = 480;
    float card_h = 520;
    float card_x = (win_w - card_w) * 0.5f;
    float card_y = (win_h - card_h) * 0.35f;

    // 背景遮罩
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), ImVec2(win_w, win_h),
        IM_COL32(230, 230, 230, 255));

    // 卡片阴影
    for (int i = 4; i >= 0; --i) {
        float o = i * 2.0f;
        bg->AddRectFilled(ImVec2(card_x - o, card_y - o), ImVec2(card_x + card_w + o, card_y + card_h + o),
            IM_COL32(0, 0, 0, 10 - i * 2), 12.0f);
    }

    // 卡片背景（白色）
    bg->AddRectFilled(ImVec2(card_x, card_y), ImVec2(card_x + card_w, card_y + card_h),
        IM_COL32(255, 255, 255, 255), 12.0f);

    // 卡片顶部绿色条
    bg->AddRectFilled(ImVec2(card_x, card_y), ImVec2(card_x + card_w, card_y + 120),
        IM_COL32(18, 160, 77, 255), 12.0f);
    bg->AddRectFilled(ImVec2(card_x, card_y + 112), ImVec2(card_x + card_w, card_y + 120),
        IM_COL32(18, 160, 77, 255));

    // 标题
    const char* title = "聊天室";
    float title_w = ImGui::CalcTextSize(title).x;
    bg->AddText(ImGui::GetFont(), 28.0f,
        ImVec2(card_x + (card_w - title_w) * 0.5f, card_y + 28),
        IM_COL32(255, 255, 255, 255), title);

    const char* sub = "输入账号密码登录或注册";
    float sub_w = ImGui::CalcTextSize(sub).x;
    bg->AddText(ImGui::GetFont(), 13.0f,
        ImVec2(card_x + (card_w - sub_w) * 0.5f, card_y + 66),
        IM_COL32(220, 255, 220, 255), sub);

    // ---- ImGui 控件（覆盖在卡片上） ----
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Login", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBackground);

    float cx = card_x + 40;
    float iw = card_w - 80;
    float cy = card_y + 150;

    ImGui::SetCursorPos(ImVec2(cx, cy));
    {
        static bool first_time = true;
        if (first_time && g_login_username[0] == '\0') {
            ImGui::SetKeyboardFocusHere();
            first_time = false;
        }
        if (g_login_username[0] != '\0') first_time = true;
    }
    ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1), "用户名");
    ImGui::SetCursorPos(ImVec2(cx, cy + 32));
    ImGui::PushItemWidth(iw);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.97f, 0.97f, 0.97f, 1));
    ImGui::InputText("##username", g_login_username, sizeof(g_login_username));
    ImGui::PopStyleColor();
    ImGui::PopItemWidth();

    ImGui::SetCursorPos(ImVec2(cx, cy + 110));
    ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1), "密码");
    ImGui::SetCursorPos(ImVec2(cx, cy + 145));
    ImGui::PushItemWidth(iw);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.97f, 0.97f, 0.97f, 1));
    ImGui::InputText("##password", g_login_password, sizeof(g_login_password), ImGuiInputTextFlags_Password);
    ImGui::PopStyleColor();
    ImGui::PopItemWidth();

    ImGui::SetCursorPos(ImVec2(cx, cy + 218));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.09f, 0.62f, 0.30f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.12f, 0.72f, 0.35f, 1));
    if (ImGui::Button("登  录", ImVec2(iw, 52))) {
        std::string username(g_login_username);
        std::string password(g_login_password);
        if (!username.empty() && !password.empty()) {
            try {
                auto ret = g_conn->call<std::string>("user_login", username, password);
                if (ret.error_code() == 200 && !ret.value().empty()) {
                    save_session_token(username, ret.value());
                    g_self_username = username;
                    load_cursors(username);
                    auto init_ret = g_conn->call<std::vector<std::string>>("get_online_users");
                    if (init_ret.error_code() == 200) {
                        std::lock_guard<std::mutex> lock(g_online_cache_mutex);
                        g_online_users_cache = init_ret.value();
                    }
                    g_state = AppState::HUB;
                    g_login_feedback.clear();
                    g_login_username[0] = '\0';
                    g_login_password[0] = '\0';
                } else {
                    g_login_feedback = "登录失败！用户名或密码错误";
                    g_feedback_time = ImGui::GetTime();
                }
            } catch (const std::exception& e) {
                g_login_feedback = std::string("登录失败: ") + e.what();
                g_feedback_time = ImGui::GetTime();
            }
        }
    }
    ImGui::PopStyleColor(2);

    ImGui::SetCursorPos(ImVec2(cx, cy + 288));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.95f, 0.95f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.09f, 0.62f, 0.30f, 1));
    if (ImGui::Button("注  册", ImVec2(iw, 52))) {
        std::string username(g_login_username);
        std::string password(g_login_password);
        if (!username.empty() && !password.empty()) {
            try {
                auto ret = g_conn->call<bool>("register_user", username, password);
                if (ret.error_code() == 200 && ret.value()) {
                    g_login_feedback = "注册成功！请登录";
                } else {
                    g_login_feedback = "注册失败！用户名可能已存在";
                }
            } catch (const std::exception& e) {
                g_login_feedback = std::string("注册失败: ") + e.what();
            }
            g_feedback_time = ImGui::GetTime();
        }
    }
    ImGui::PopStyleColor(3);

    // 反馈消息
    if (!g_login_feedback.empty()) {
        double elapsed = ImGui::GetTime() - g_feedback_time;
        if (elapsed < 5.0) {
            ImGui::SetCursorPos(ImVec2(cx, cy + 360));
            float fw = ImGui::CalcTextSize(g_login_feedback.c_str()).x;
            ImGui::SetCursorPosX(cx + (iw - fw) * 0.5f);
            ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.1f, 1), "%s", g_login_feedback.c_str());
        } else {
            g_login_feedback.clear();
        }
    }

    ImGui::End();
}

// 头像颜色生成
static const ImU32 avatar_colors[] = {
    IM_COL32(68, 114, 196, 255), IM_COL32(237, 125, 49, 255),
    IM_COL32(165, 165, 165, 255), IM_COL32(255, 192, 0, 255),
    IM_COL32(91, 155, 213, 255), IM_COL32(112, 173, 71, 255),
    IM_COL32(158, 72, 14, 255), IM_COL32(142, 68, 173, 255),
};
static ImU32 avatar_color(const std::string& name) {
    size_t h = std::hash<std::string>{}(name);
    return avatar_colors[h % 8];
}

// 绘制圆形头像（首字符）
static void draw_avatar(ImDrawList* dl, const ImVec2& pos, float radius, const std::string& name) {
    dl->AddCircleFilled(pos, radius, avatar_color(name), 32);
    if (!name.empty()) {
        char c = (char)std::toupper((unsigned char)name[0]);
        std::string label(1, c);
        float font_size = radius * 1.2f;
        float tw = ImGui::CalcTextSize(label.c_str()).x;
        dl->AddText(ImGui::GetFont(), font_size,
            ImVec2(pos.x - tw * 0.5f, pos.y - font_size * 0.4f),
            IM_COL32(255, 255, 255, 255), label.c_str());
    }
}

// 绘制红点未读徽标
static void draw_badge(ImDrawList* dl, const ImVec2& center, size_t count) {
    if (count == 0 && false) return; // 保留但忽略
    std::string badge_text = count > 99 ? "99+" : std::to_string(count);
    float tw = ImGui::CalcTextSize(badge_text.c_str()).x;
    float bw = std::max(tw + 12, 18.0f);
    float bh = 18.0f;
    ImVec2 bmin(center.x - bw * 0.5f, center.y - bh * 0.5f);
    ImVec2 bmax(center.x + bw * 0.5f, center.y + bh * 0.5f);
    dl->AddRectFilled(bmin, bmax, IM_COL32(255, 60, 60, 255), bh * 0.5f);
    dl->AddText(ImGui::GetFont(), 11.0f,
        ImVec2(center.x - tw * 0.5f, center.y - 6),
        IM_COL32(255, 255, 255, 255), badge_text.c_str());
}

// ---- HUB 主界面 ----
static void render_hub() {
    static bool hub_inited = false;
    if (!hub_inited) {
        try {
            {
                auto ret = g_conn->call<std::vector<std::string>>("get_friends", g_self_username);
                if (ret.error_code() == 200) {
                    std::lock_guard<std::mutex> lk(g_friends_mutex);
                    g_friends_list = ret.value();
                }
            }
            {
                auto ret = g_conn->call<std::vector<std::tuple<int, std::string, int64_t>>>("get_pending_requests", g_self_username);
                if (ret.error_code() == 200) {
                    std::lock_guard<std::mutex> lk(g_req_mutex);
                    g_pending_requests = ret.value();
                }
            }
            fetch_unread_counts(g_conn, g_self_username);
        } catch (const std::exception&) {
            // 初始化失败，下一次进入 hub 会重试
        }
        hub_inited = true;
    }

    // 构建联系人列表
    std::set<std::string> friend_set;
    std::vector<std::string> friends_online, friends_offline, online_non_friends;
    {
        std::lock_guard<std::mutex> lk(g_friends_mutex);
        for (const auto& f : g_friends_list) friend_set.insert(f);
    }
    {
        std::lock_guard<std::mutex> lock(g_online_cache_mutex);
        for (const auto& u : g_online_users_cache) {
            if (u == g_self_username) continue;
            if (friend_set.find(u) != friend_set.end())
                friends_online.push_back(u);
            else
                online_non_friends.push_back(u);
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_friends_mutex);
        for (const auto& f : g_friends_list) {
            if (!is_online(f)) friends_offline.push_back(f);
        }
    }

    // 未读计数
    size_t total_unread = 0;
    {
        std::lock_guard<std::mutex> lk(g_unread_mutex);
        for (const auto& [_, c] : g_unread_counts) total_unread += c;
    }

    const auto& disp = ImGui::GetIO().DisplaySize;

    // ---- 顶部绿色栏 ----
    {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(disp.x, 52), IM_COL32(18, 160, 77, 255));
        dl->AddText(ImGui::GetFont(), 18.0f, ImVec2(16, 14), IM_COL32(255, 255, 255, 255), "聊天室");
        // 用户名 + 在线状态
        std::string status_text = g_self_username + "  ·  在线";
        float stw = ImGui::CalcTextSize(status_text.c_str()).x;
        dl->AddText(ImGui::GetFont(), 13.0f, ImVec2(disp.x - stw - 16, 16),
            IM_COL32(200, 255, 210, 255), status_text.c_str());
        // 绿色圆点
        dl->AddCircleFilled(ImVec2(disp.x - stw - 22, 26), 4, IM_COL32(140, 255, 160, 255), 16);
    }

    // 占位顶部
    // 根窗口 — 必须包裹所有子窗口
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(disp);
    ImGui::Begin("HubRoot", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ImGui::SetCursorPos(ImVec2(0, 52));

    // ---- 左侧联系人面板 ----
    float left_w = 340;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 1));
    ImGui::BeginChild("LeftPanel", ImVec2(left_w, disp.y - 52 - 52),
        false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    // 好友请求提醒横幅
    {
        std::lock_guard<std::mutex> lk(g_req_mutex);
        if (!g_pending_requests.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.95f, 0.85f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.92f, 0.78f, 1.0f));
            std::string req_text = "你有 " + std::to_string(g_pending_requests.size()) + " 条好友请求，点击处理";
            if (ImGui::Button(req_text.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 32))) {
                g_state = AppState::FRIEND_MENU;
            }
            ImGui::PopStyleColor(2);
            ImGui::Separator();
        }
    }

    // 绘制联系人项
    auto draw_user_entry = [&](const std::string& name, bool online, bool is_friend) {
        ImGui::PushID(name.c_str());

        // 计算这一行的高度约为 56px
        float row_h = 60.0f;
        ImVec2 item_min = ImGui::GetCursorScreenPos();
        ImVec2 item_max(item_min.x + ImGui::GetContentRegionAvail().x, item_min.y + row_h);

        // hover 高亮
        bool hovered = ImGui::IsMouseHoveringRect(item_min, item_max);
        if (hovered) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(item_min, item_max, IM_COL32(235, 235, 235, 255), 0);
        }

        // 点击检测
        if (hovered && ImGui::IsMouseClicked(0)) {
            std::lock_guard<std::mutex> lk(g_unread_mutex);
            g_unread_counts.erase(name);
            g_chat_target = name;
            g_chat_needs_init = true;
            g_auto_scroll = true;
            g_third_party_notify.clear();
            g_state = AppState::CHAT;
            ImGui::PopID();
            return;
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        float cy = item_min.y + row_h * 0.5f;

        // 圆形头像
        draw_avatar(dl, ImVec2(item_min.x + 28, cy), 20, name);

        // 用户名
        dl->AddText(ImGui::GetFont(), 15.0f,
            ImVec2(item_min.x + 58, cy - 12),
            online ? IM_COL32(30, 30, 30, 255) : IM_COL32(130, 130, 130, 255),
            name.c_str());

        // 在线/离线小文字
        dl->AddText(ImGui::GetFont(), 11.0f,
            ImVec2(item_min.x + 58, cy + 6),
            online ? IM_COL32(70, 170, 70, 255) : IM_COL32(160, 160, 160, 255),
            online ? "在线" : "离线");

        // 未读徽标
        size_t unread = 0;
        {
            std::lock_guard<std::mutex> lk(g_unread_mutex);
            auto uit = g_unread_counts.find(name);
            if (uit != g_unread_counts.end()) unread = uit->second;
        }
        if (unread > 0) {
            draw_badge(dl, ImVec2(item_max.x - 28, cy), unread);
        } else if (unread == 0) {
            auto uit = g_unread_counts.find(name);
            if (uit != g_unread_counts.end()) {
                dl->AddCircleFilled(ImVec2(item_max.x - 22, cy), 5, IM_COL32(255, 60, 60, 255), 16);
            }
        }

        ImGui::SetCursorPosY(item_min.y + row_h);
        ImGui::Dummy(ImVec2(1, 1));
        ImGui::PopID();
    };

    // 分组标题
    auto draw_section_title = [](const char* title, ImU32 color = IM_COL32(140, 140, 140, 255)) {
        float pos_y = ImGui::GetCursorPosY();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImGui::GetFont(), 12.0f,
            ImVec2(ImGui::GetCursorScreenPos().x + 12, pos_y + 4),
            color, title);
        ImGui::SetCursorPosY(pos_y + 24);
    };

    bool has_friends = !friends_online.empty() || !friends_offline.empty();
    bool has_others = !online_non_friends.empty();

    if (!has_friends && !has_others) {
        ImGui::SetCursorPosY(40);
        float tw = ImGui::CalcTextSize("暂无联系人，输入 f 搜索好友").x;
        ImGui::SetCursorPosX((left_w - tw) * 0.5f);
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "暂无联系人，好友管理搜索好友");
    } else {
        draw_section_title("好友");
        for (const auto& name : friends_online) draw_user_entry(name, true, true);
        for (const auto& name : friends_offline) draw_user_entry(name, false, true);

        if (!online_non_friends.empty()) {
            ImGui::Separator();
            draw_section_title("在线用户");
            for (const auto& name : online_non_friends) draw_user_entry(name, true, false);
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(); // ChildBg

    // ---- 右侧内容区 ----
    ImGui::SameLine();
    ImGui::BeginChild("RightPanel", ImVec2(0, -52), false);
    {
        float rw = ImGui::GetWindowWidth();
        float rh = ImGui::GetWindowHeight();
        ImGui::SetCursorPos(ImVec2((rw - 200) * 0.5f, rh * 0.4f));
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "选择一个联系人开始聊天");
    }
    ImGui::EndChild();

    // ---- 底部工具栏 ----
    ImGui::SetCursorPos(ImVec2(0, disp.y - 52));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.97f, 0.97f, 0.97f, 1));
    ImGui::BeginChild("BottomBar", ImVec2(0, 52), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::SetCursorPos(ImVec2(12, 10));
    if (ImGui::Button("好友管理", ImVec2(100, 32))) {
        g_state = AppState::FRIEND_MENU;
    }
    ImGui::SameLine();
    if (ImGui::Button("群聊", ImVec2(70, 32))) {
        g_state = AppState::GROUP_CHAT;
        g_auto_scroll = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("刷新", ImVec2(70, 32))) {
        g_conn->async_call([](uint32_t ec, const std::string&, const nlohmann::json& data) {
            if (ec == 200 && !data.is_null()) {
                try {
                    auto users = data.get<std::vector<std::string>>();
                    std::lock_guard<std::mutex> lock(g_online_cache_mutex);
                    g_online_users_cache = std::move(users);
                } catch (...) {}
            }
        }, "get_online_users");
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 110);
    if (ImGui::Button("退出登录", ImVec2(90, 32))) {
        save_cursors(g_self_username);
        g_conn->notify("user_logout", g_self_username, load_session_token());
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
        g_chat_history_map.clear();
        g_conv_min_seq.clear();
        g_conv_max_seq.clear();
        g_pull_cursor.clear();
        g_self_username.clear();
        hub_inited = false;
        g_state = AppState::LOGIN;
    }
    ImGui::EndChild();

    ImGui::End();
}

// 辅助函数：绘制消息气泡
static void draw_bubble(const std::string& sender, const std::string& text, const std::string& ts_str) {
    float avail_w = ImGui::GetContentRegionAvail().x;
    float max_bubble_w = avail_w * 0.72f;
    float padding_x = 14.0f;
    float padding_y = 10.0f;
    float corner_radius = 10.0f;

    bool is_me = (sender == "__me__");
    bool is_system = (sender == "__system__");

    // 时间戳居中
    if (!ts_str.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1));
        float ts_w = ImGui::CalcTextSize(ts_str.c_str()).x;
        ImGui::SetCursorPosX((avail_w - ts_w) * 0.5f);
        ImGui::Text("%s", ts_str.c_str());
        ImGui::PopStyleColor();
    }

    if (is_system) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.7f, 0.1f, 1));
        float tw = ImGui::CalcTextSize(text.c_str()).x;
        ImGui::SetCursorPosX((avail_w - tw) * 0.5f);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
        return;
    }

    // 计算文本尺寸（换行）
    float wrap_width = max_bubble_w - padding_x * 2;
    ImVec2 text_size = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, wrap_width, text.c_str());

    float bubble_w = std::min(text_size.x + padding_x * 2, max_bubble_w);
    float bubble_h = text_size.y + padding_y * 2;

    ImVec2 cursor_screen = ImGui::GetCursorScreenPos();
    float bubble_x = is_me ? (cursor_screen.x + avail_w - bubble_w) : cursor_screen.x;

    // 画背景圆角矩形
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 rect_min(bubble_x, cursor_screen.y);
    ImVec2 rect_max(bubble_x + bubble_w, cursor_screen.y + bubble_h);
    ImU32 bg_col = is_me ? IM_COL32(76, 143, 252, 255) : IM_COL32(245, 245, 245, 255);
    dl->AddRectFilled(rect_min, rect_max, bg_col, corner_radius);

    // 文本（白色 for me，黑色 for other）
    ImVec2 text_pos(bubble_x + padding_x, cursor_screen.y + padding_y);
    ImU32 txt_col = is_me ? IM_COL32(255, 255, 255, 255) : IM_COL32(30, 30, 30, 255);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), text_pos, txt_col, text.c_str(), NULL, wrap_width, NULL);

    // 预留空间
    ImGui::Dummy(ImVec2(avail_w, bubble_h + 4));
}

// ---- 私聊界面 ----
static void render_chat() {
    // 首次进入 — 同步消息
    if (g_chat_needs_init) {
        g_chat_needs_init = false;
        g_chat_has_history = false;

        // 取游标
        uint64_t after_seq = 0;
        {
            std::lock_guard<std::mutex> lk(g_pull_cursor_mutex);
            auto it = g_pull_cursor.find(g_chat_target);
            if (it != g_pull_cursor.end()) after_seq = it->second;
        }

        // 加载消息（SQLite 作为唯一数据源）
        {
            auto ret = g_conn->call<std::vector<std::tuple<uint64_t, std::string, std::string, std::string, std::string>>>(
                "sync_messages", g_self_username, g_chat_target, after_seq, (size_t)20);
            if (ret.error_code() == 200 && !ret.value().empty()) {
                std::lock_guard<std::mutex> lock(g_chat_map_mutex);
                for (auto& [seq_id, from, to, msg, ts] : ret.value()) {
                    if (from == g_self_username)
                        g_chat_history_map[g_chat_target].emplace_back("__me__", msg, seq_id, ts);
                    else
                        g_chat_history_map[g_chat_target].emplace_back(from, msg, seq_id, ts);
                    if (seq_id > g_conv_max_seq[g_chat_target])
                        g_conv_max_seq[g_chat_target] = seq_id;
                    if (seq_id > g_last_seq_id) g_last_seq_id = seq_id;
                }
                uint64_t max_seen = std::get<0>(ret.value().back());
                {
                    std::lock_guard<std::mutex> lk(g_pull_cursor_mutex);
                    auto it = g_pull_cursor.find(g_chat_target);
                    if (it == g_pull_cursor.end() || max_seen > it->second)
                        g_pull_cursor[g_chat_target] = max_seen;
                }
            }
        }

        // 按 seq_id 排序消息
        {
            std::lock_guard<std::mutex> lock(g_chat_map_mutex);
            auto& msgs = g_chat_history_map[g_chat_target];
            std::sort(msgs.begin(), msgs.end(), [](const auto& a, const auto& b) {
                return std::get<2>(a) < std::get<2>(b);
            });
        }
    }

    const auto& disp = ImGui::GetIO().DisplaySize;

    // ---- 顶部绿色栏 ----
    {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(disp.x, 50), IM_COL32(18, 160, 77, 255));
        // 返回按钮（文字）
        dl->AddText(ImGui::GetFont(), 15.0f, ImVec2(16, 14), IM_COL32(220, 255, 220, 255), "< 返回");
        // 点击检测返回
        bool back_hovered = ImGui::IsMouseHoveringRect(ImVec2(0, 0), ImVec2(80, 50));
        if (back_hovered && ImGui::IsMouseClicked(0)) {
            g_chat_history_map[g_chat_target].clear();
            g_chat_target.clear();
            g_chat_input[0] = '\0';
            g_state = AppState::HUB;
            return;
        }
        // 标题
        std::string title = "与 " + g_chat_target + " 聊天中";
        float tw = ImGui::CalcTextSize(title.c_str()).x;
        dl->AddText(ImGui::GetFont(), 17.0f, ImVec2((disp.x - tw) * 0.5f, 14),
            IM_COL32(255, 255, 255, 255), title.c_str());
    }

    // 根窗口包裹子窗口
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(disp);
    ImGui::Begin("ChatRoot", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ImGui::SetCursorPos(ImVec2(0, 50));

    // ---- 消息区域 ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.92f, 0.92f, 0.91f, 1));
    ImGui::BeginChild("Messages", ImVec2(0, -(64 + 4)), false,
        ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::PopStyleColor();

    // 加载更早消息
    {
        uint64_t before_seq = 0;
        {
            std::lock_guard<std::mutex> lk(g_conv_min_seq_mutex);
            auto it = g_conv_min_seq.find(g_chat_target);
            if (it != g_conv_min_seq.end() && it->second > 0)
                before_seq = it->second;
        }
        if (before_seq > 0 && !g_chat_has_history) {
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - 140) * 0.5f);
            if (ImGui::Button("加载更早消息", ImVec2(140, 28))) {
                auto hret = g_conn->call<std::vector<std::tuple<uint64_t, std::string, std::string, std::string, std::string>>>(
                    "sync_history", g_self_username, g_chat_target, before_seq, (size_t)10);
                if (hret.error_code() == 200 && !hret.value().empty()) {
                    std::lock_guard<std::mutex> lock(g_chat_map_mutex);
                    uint64_t new_min = UINT64_MAX;
                    for (auto it = hret.value().rbegin(); it != hret.value().rend(); ++it) {
                        auto& [seq_id, from, to, msg, ts] = *it;
                        auto pos = g_chat_history_map[g_chat_target].begin();
                        if (from == g_self_username)
                            g_chat_history_map[g_chat_target].emplace(pos, "__me__", msg, seq_id, ts);
                        else
                            g_chat_history_map[g_chat_target].emplace(pos, from, msg, seq_id, ts);
                        if (seq_id < new_min) new_min = seq_id;
                    }
                    std::lock_guard<std::mutex> lk(g_conv_min_seq_mutex);
                    g_conv_min_seq[g_chat_target] = new_min;
                } else {
                    g_chat_has_history = true;
                }
            }
            ImGui::Separator();
        } else if (before_seq == 0) {
            g_chat_has_history = true;
        }
    }

    // 第三方消息通知
    {
        std::lock_guard<std::mutex> nlk(g_notify_mutex);
        if (!g_third_party_notify.empty()) {
            std::string notify = "来自 " + g_third_party_notify + " 的新消息";
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.5f, 0.1f, 1));
            float ntw = ImGui::CalcTextSize(notify.c_str()).x;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ntw) * 0.5f);
            ImGui::TextWrapped("%s", notify.c_str());
            ImGui::PopStyleColor();
        }
    }

    // 消息气泡
    {
        std::lock_guard<std::mutex> lock(g_chat_map_mutex);
        auto it = g_chat_history_map.find(g_chat_target);
        if (it != g_chat_history_map.end()) {
            std::unordered_set<uint64_t> seen;
            for (const auto& msg : it->second) {
                uint64_t sid = std::get<2>(msg);
                if (sid > 0 && !seen.insert(sid).second) continue;
                const auto& sender = std::get<0>(msg);
                const auto& text = std::get<1>(msg);
                std::string ts_str = fmt_ts(std::get<3>(msg));
                draw_bubble(sender, text, ts_str);
            }
        }
    }

    // 自动滚动到底部
    if (g_auto_scroll) {
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) {
            ImGui::SetScrollHereY(1.0f);
        }
        g_auto_scroll = false;
    }

    ImGui::EndChild();

    // ---- 输入区域 ----
    ImGui::SetCursorPos(ImVec2(0, disp.y - 64));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.97f, 0.97f, 0.97f, 1));
    ImGui::BeginChild("ChatInput", ImVec2(0, 64), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(12, 8));
    float input_w = ImGui::GetContentRegionAvail().x - 90;
    ImGui::PushItemWidth(input_w);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1, 1, 1, 1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1, 1, 1, 1));
    bool enter_pressed = ImGui::InputText("##input", g_chat_input, sizeof(g_chat_input),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor(2);
    ImGui::PopItemWidth();

    ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionAvail().x - 72, 8));
    if (ImGui::Button("发送", ImVec2(66, 36)) || enter_pressed) {
        std::string msg(g_chat_input);
        g_chat_input[0] = '\0';
        if (!msg.empty() && !g_server_offline) {
            auto send_ret = g_conn->call<uint64_t>("send_message", g_self_username, g_chat_target, msg);
            uint64_t send_seq_id = (send_ret.error_code() == 200) ? send_ret.value() : 0;
            auto local_ts = std::to_string(std::time(nullptr));
            {
                std::lock_guard<std::mutex> lock(g_chat_map_mutex);
                g_chat_history_map[g_chat_target].emplace_back("__me__", msg, send_seq_id, local_ts);
                if (send_seq_id > 0) {
                    if (send_seq_id > g_last_seq_id) g_last_seq_id = send_seq_id;
                    {
                        std::lock_guard<std::mutex> lk(g_conv_max_seq_mutex);
                        auto it = g_conv_max_seq.find(g_chat_target);
                        if (it == g_conv_max_seq.end() || send_seq_id > it->second)
                            g_conv_max_seq[g_chat_target] = send_seq_id;
                    }
                    {
                        std::lock_guard<std::mutex> lk(g_conv_min_seq_mutex);
                        auto it = g_conv_min_seq.find(g_chat_target);
                        if (it == g_conv_min_seq.end() || send_seq_id < it->second)
                            g_conv_min_seq[g_chat_target] = send_seq_id;
                    }
                    {
                        std::lock_guard<std::mutex> lk(g_pull_cursor_mutex);
                        auto it = g_pull_cursor.find(g_chat_target);
                        if (it == g_pull_cursor.end() || send_seq_id > it->second)
                            g_pull_cursor[g_chat_target] = send_seq_id;
                    }
                } else {
                    g_chat_history_map[g_chat_target].emplace_back("__system__", "【对方用户不存在，消息发送失败】", 0, local_ts);
                }
            }
            g_auto_scroll = true;
            // 焦点回到输入框
            ImGui::SetKeyboardFocusHere(-1);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---- 群聊界面 ----
static void render_group_chat() {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("GroupChat", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    // 顶部
    ImGui::BeginChild("GCTop", ImVec2(0, 40), false);
    if (ImGui::Button("<- 返回", ImVec2(70, 30))) {
        g_state = AppState::HUB;
        ImGui::EndChild();
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.8f, 1), "  群聊");
    ImGui::EndChild();
    ImGui::Separator();

    // 消息列表
    ImGui::BeginChild("GCMessages", ImVec2(0, -(50 + 4)), false);

    {
        std::lock_guard<std::mutex> lock(g_group_mutex);
        for (const auto& msg : g_group_chat_history) {
            if (msg.first == "__me__") {
                float tw = ImGui::CalcTextSize(msg.second.c_str()).x;
                ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - tw);
                ImGui::TextColored(ImVec4(0, 0.8f, 0.2f, 1), "%s", ("我: " + msg.second).c_str());
            } else {
                ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.8f, 1), "%s", msg.first.c_str());
                ImGui::SameLine();
                ImGui::TextWrapped("%s", msg.second.c_str());
            }
        }
    }

    if (g_auto_scroll) {
        ImGui::SetScrollHereY(1.0f);
        g_auto_scroll = false;
    }

    ImGui::EndChild();
    ImGui::Separator();

    // 输入
    ImGui::BeginChild("GCInput", ImVec2(0, 0), false);
    float input_w = ImGui::GetContentRegionAvail().x - 80;
    ImGui::PushItemWidth(input_w);
    bool enter = ImGui::InputText("##gcinput", g_chat_input, sizeof(g_chat_input),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("发送", ImVec2(70, 0)) || enter) {
        std::string msg(g_chat_input);
        g_chat_input[0] = '\0';
        if (!msg.empty() && !g_server_offline) {
            g_conn->notify("send_group_message", g_self_username, msg);
            {
                std::lock_guard<std::mutex> lock(g_group_mutex);
                g_group_chat_history.emplace_back("__me__", msg);
            }
            g_auto_scroll = true;
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

// ---- 好友管理 ----
static void render_friend_menu() {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("FriendMenu", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    float win_w = ImGui::GetWindowWidth();
    ImGui::BeginChild("FMTop", ImVec2(0, 40), false);
    if (ImGui::Button("<- 返回", ImVec2(70, 30))) {
        g_state = AppState::HUB;
        g_friend_feedback.clear();
        ImGui::EndChild();
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.3f, 0.6f, 1.0f, 1), "  好友管理");
    ImGui::EndChild();
    ImGui::Separator();

    // === 搜索用户 ===
    ImGui::BeginChild("FMSearch", ImVec2(0, 0), true);
    ImGui::Text("搜索用户");
    ImGui::PushItemWidth(win_w * 0.4f);
    ImGui::InputText("##search", g_friend_search_keyword, sizeof(g_friend_search_keyword));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("搜索", ImVec2(60, 0))) {
        std::string kw(g_friend_search_keyword);
        if (!kw.empty()) {
            auto ret = g_conn->call<std::vector<std::string>>("search_users", kw, g_self_username);
            if (ret.error_code() == 200) {
                g_friend_search_results = ret.value();
            } else {
                g_friend_search_results.clear();
            }
        }
    }

    if (!g_friend_search_results.empty()) {
        ImGui::Separator();
        for (const auto& user : g_friend_search_results) {
            ImGui::Text("%s", user.c_str());
            ImGui::SameLine(win_w * 0.5f);
            std::string btn = "发送好友请求##" + user;
            if (ImGui::Button(btn.c_str(), ImVec2(120, 24))) {
                auto ret = g_conn->call<bool>("send_friend_request", g_self_username, user);
                if (ret.error_code() == 200 && ret.value()) {
                    g_friend_feedback = "好友请求已发送给 " + user;
                } else {
                    g_friend_feedback = "发送失败：请求已存在或已是好友";
                }
            }
            ImGui::SameLine(win_w * 0.6f);
            if (is_online(user))
                ImGui::TextColored(ImVec4(0, 0.8f, 0, 1), "在线");
            else
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "离线");
        }
    }

    // === 待处理请求 ===
    ImGui::Separator();
    ImGui::Text("待处理的好友请求");
    {
        // 刷新
        auto ret = g_conn->call<std::vector<std::tuple<int, std::string, int64_t>>>("get_pending_requests", g_self_username);
        if (ret.error_code() == 200) {
            std::lock_guard<std::mutex> lk(g_req_mutex);
            g_pending_requests = ret.value();
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_req_mutex);
        if (g_pending_requests.empty()) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "  暂无");
        } else {
            for (auto& [id, from, ts] : g_pending_requests) {
                ImGui::Text("  %s", from.c_str());
                ImGui::SameLine(win_w * 0.5f);
                ImGui::PushID(id);
                if (ImGui::Button("接受", ImVec2(60, 24))) {
                    auto ret = g_conn->call<bool>("handle_friend_request", id, true);
                    if (ret.error_code() == 200 && ret.value()) {
                        g_friend_feedback = "已添加 " + from + " 为好友！";
                        // 刷新好友列表
                        auto f_ret = g_conn->call<std::vector<std::string>>("get_friends", g_self_username);
                        if (f_ret.error_code() == 200) {
                            std::lock_guard<std::mutex> fk(g_friends_mutex);
                            g_friends_list = f_ret.value();
                        }
                    }
                }
                ImGui::SameLine(win_w * 0.5f + 70);
                if (ImGui::Button("拒绝", ImVec2(60, 24))) {
                    g_conn->call<bool>("handle_friend_request", id, false);
                }
                ImGui::PopID();
            }
        }
    }

    // === 已发送的请求 ===
    ImGui::Separator();
    ImGui::Text("已发送的请求");
    {
        auto ret = g_conn->call<std::vector<std::tuple<int, std::string, int64_t>>>("get_sent_requests", g_self_username);
        if (ret.error_code() == 200 && !ret.value().empty()) {
            for (auto& [id, to, ts] : ret.value()) {
                ImGui::Text("  → %s  [等待接受]", to.c_str());
            }
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "  暂无");
        }
    }

    // === 反馈 ===
    if (!g_friend_feedback.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1, 0.8f, 0, 1), "%s", g_friend_feedback.c_str());
        g_friend_feedback.clear();
    }

    ImGui::EndChild();
    ImGui::End();
}

// ==================== main ====================

int main(int argc, char* argv[]) {
    uint16_t port = 8877;
    if (argc > 1) port = static_cast<uint16_t>(std::stoi(argv[1]));

    wlog::logger::get().init("logs/chatting_room_client_gui.log");
#ifdef _LOG_CONSOLE
    spdlog::default_logger()->sinks().pop_back();
#endif

    auto& client = mrpc::client::get();
    client.run();

    client.router().reg_handle("on_message", on_message);
    client.router().reg_handle("on_group_message", on_group_message);
    client.router().reg_handle("on_user_status_changed", on_user_status_changed);
    client.router().reg_handle("on_server_shutdown", on_server_shutdown);
    client.router().reg_handle("on_new_friend_request", on_new_friend_request);
    client.router().reg_handle("on_friend_request_accepted", on_friend_request_accepted);

    g_conn = client.connect("127.0.0.1", port);
    if (!g_conn) {
        std::cerr << "连接服务器失败！请确认服务器已启动。" << std::endl;
        return 1;
    }
    g_conn->start_heartbeat(10);
    g_conn->set_closed_callback([](const std::shared_ptr<connection>&) {
        g_server_offline = true;
    });

    // ---- SDL2 + ImGui 初始化 ----
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL_Init 失败: " << SDL_GetError() << std::endl;
        return 1;
    }

    // OpenGL 3.3
    const char* glsl_version = "#version 330";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow("聊天室",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!window) {
        std::cerr << "SDL_CreateWindow 失败: " << SDL_GetError() << std::endl;
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);
    SDL_StartTextInput();
    SDL_EventState(SDL_TEXTINPUT, SDL_ENABLE);
    SDL_EventState(SDL_KEYDOWN, SDL_ENABLE);  // 启用文本输入（中文 IME 需要）

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // 不保存 imgui.ini

    // ========== 自定义配色方案（仿微信风格） ==========
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.ChildRounding = 6;
    style.FrameRounding = 6;
    style.PopupRounding = 6;
    style.ScrollbarRounding = 4;
    style.GrabRounding = 4;
    style.TabRounding = 4;

    style.WindowBorderSize = 0;
    style.ChildBorderSize = 0;
    style.FrameBorderSize = 1;
    style.PopupBorderSize = 0;

    style.FramePadding = ImVec2(22, 16);
    style.ItemSpacing = ImVec2(8, 6);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.WindowPadding = ImVec2(0, 0);
    style.ScrollbarSize = 6;

    // 主色：微信绿
    ImVec4 green_primary   = ImVec4(0.09f, 0.62f, 0.30f, 1.0f); // #18a04d
    ImVec4 green_dark      = ImVec4(0.07f, 0.52f, 0.25f, 1.0f);
    ImVec4 green_light     = ImVec4(0.12f, 0.72f, 0.35f, 1.0f);
    ImVec4 bg_main         = ImVec4(0.93f, 0.93f, 0.92f, 1.0f); // #ededeb
    ImVec4 bg_panel        = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 text_dark       = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    ImVec4 text_gray       = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    ImVec4 header_bg       = ImVec4(0.93f, 0.93f, 0.93f, 1.0f);

    style.Colors[ImGuiCol_Text]                  = text_dark;
    style.Colors[ImGuiCol_TextDisabled]           = text_gray;
    style.Colors[ImGuiCol_WindowBg]              = bg_main;
    style.Colors[ImGuiCol_ChildBg]               = bg_panel;
    style.Colors[ImGuiCol_PopupBg]               = ImVec4(1, 1, 1, 0.95f);
    style.Colors[ImGuiCol_Border]                = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_FrameBg]               = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive]         = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    style.Colors[ImGuiCol_TitleBg]               = green_primary;
    style.Colors[ImGuiCol_TitleBgActive]         = green_dark;
    style.Colors[ImGuiCol_TitleBgCollapsed]      = green_primary;
    style.Colors[ImGuiCol_MenuBarBg]             = header_bg;
    style.Colors[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    style.Colors[ImGuiCol_CheckMark]             = green_primary;
    style.Colors[ImGuiCol_SliderGrab]            = green_primary;
    style.Colors[ImGuiCol_SliderGrabActive]      = green_light;
    style.Colors[ImGuiCol_Button]                = green_primary;
    style.Colors[ImGuiCol_ButtonHovered]         = green_light;
    style.Colors[ImGuiCol_ButtonActive]          = green_dark;
    style.Colors[ImGuiCol_Header]                = ImVec4(0.90f, 0.95f, 0.90f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered]         = ImVec4(0.80f, 0.90f, 0.80f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive]          = ImVec4(0.70f, 0.85f, 0.70f, 1.0f);
    style.Colors[ImGuiCol_Separator]             = ImVec4(0.88f, 0.88f, 0.88f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive]       = ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip]            = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_ResizeGripActive]      = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_Tab]                   = bg_panel;
    style.Colors[ImGuiCol_TabHovered]            = ImVec4(0.90f, 0.95f, 0.90f, 1.0f);
    style.Colors[ImGuiCol_TabActive]             = green_primary;
    style.Colors[ImGuiCol_TabUnfocused]          = bg_panel;
    style.Colors[ImGuiCol_TabUnfocusedActive]    = green_primary;
    style.Colors[ImGuiCol_DragDropTarget]        = green_primary;
    style.Colors[ImGuiCol_NavHighlight]          = green_primary;
    style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0, 0, 0, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.35f);

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // 设置中文字体（DroidSansFallbackFull 只有 CJK，需要叠在基字上）
    {
        io.Fonts->Clear();

        ImFontConfig latin_cfg;
        latin_cfg.OversampleH = 1;
        latin_cfg.OversampleV = 1;
        ImFont* latin = io.Fonts->AddFontFromFileTTF(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 20.0f, &latin_cfg,
            io.Fonts->GetGlyphRangesDefault());

        // 叠 CJK（MergeMode 会把 CJK 字形合入主字体纹理）
        ImFontConfig cjk_cfg;
        cjk_cfg.MergeMode = true;
        cjk_cfg.OversampleH = 1;
        cjk_cfg.OversampleV = 1;
        io.Fonts->AddFontFromFileTTF(
            "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", 20.0f, &cjk_cfg,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());

        if (latin)
            io.FontDefault = latin;
        else
            io.Fonts->AddFontDefault();

        io.Fonts->Build();
    }

    // ---- 尝试 token 自动登录（异步验证，在 render_connecting 中进行） ----
    {
        std::ifstream sf("session_token.txt");
        if (sf) {
            std::string line;
            if (std::getline(sf, line)) {
                auto pos = line.rfind(':');
                if (pos != std::string::npos) {
                    g_saved_token = line.substr(pos + 1);
                }
            }
        }
    }

    // ---- 主循环 ----
    bool done = false;
    Uint32 last_frame = SDL_GetTicks();

    while (!done) {
        // 事件处理（必须在帧率限制之前，否则会丢事件）
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) done = true;
        }

        // 帧率限制 ~60fps
        Uint32 now = SDL_GetTicks();
        if (now - last_frame < 16) {
            SDL_Delay(1);
            continue;
        }
        last_frame = now;

        // 服务器断开时切换到 LOGIN
        if (g_server_offline && g_state == AppState::HUB) {
            g_state = AppState::LOGIN;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // 渲染当前状态页面
        switch (g_state) {
            case AppState::CONNECTING:
                render_connecting();
                break;
            case AppState::LOGIN:
                render_login();
                break;
            case AppState::HUB:
                render_hub();
                break;
            case AppState::CHAT:
                render_chat();
                break;
            case AppState::GROUP_CHAT:
                render_group_chat();
                break;
            case AppState::FRIEND_MENU:
                render_friend_menu();
                break;
        }

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.95f, 0.95f, 0.95f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    // ---- 清理 ----
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (!g_self_username.empty()) {
        save_cursors(g_self_username);
        g_conn->notify("user_logout", g_self_username, load_session_token());
        remove_session_token();
    }

    client.shutdown();
    client.wait_shutdown();
    wlog::logger::get().shutdown();

    return 0;
}
