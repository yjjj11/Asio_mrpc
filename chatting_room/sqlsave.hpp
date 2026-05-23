#pragma once

#include <sqlite_orm/sqlite_orm.h>
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <mutex>

/// 持久化消息结构体，对应 sqlite messages 表
struct Message {
    uint64_t seq_id;
    std::string from_user;
    std::string to_user;
    std::string msg;
    std::string created_at;
};

/// 用户账户结构体，对应 sqlite users 表
struct User {
    std::string username;
    std::string password_hash;
};

/// 好友请求结构体
struct FriendRequest {
    int id;
    std::string from_user;
    std::string to_user;
    std::string status;  // "pending", "accepted", "rejected"
    int64_t created_at;
};

/// 好友关系结构体
struct Friendship {
    std::string user_a;  // 字典序小者
    std::string user_b;  // 字典序大者
    int64_t created_at;
};

/// 构建 sqlite_orm storage（分离用于类型推导）
inline auto make_message_storage(const std::string& path) {
    using namespace sqlite_orm;
    return make_storage(path,
        make_table("messages",
            make_column("seq_id", &Message::seq_id, primary_key()),
            make_column("from_user", &Message::from_user),
            make_column("to_user", &Message::to_user),
            make_column("msg", &Message::msg),
            make_column("created_at", &Message::created_at)
        ),
        make_table("users",
            make_column("username", &User::username, primary_key()),
            make_column("password_hash", &User::password_hash)
        ),
        make_table("friend_requests",
            make_column("id", &FriendRequest::id, primary_key().autoincrement()),
            make_column("from_user", &FriendRequest::from_user),
            make_column("to_user", &FriendRequest::to_user),
            make_column("status", &FriendRequest::status),
            make_column("created_at", &FriendRequest::created_at)
        ),
        make_table("friends",
            make_column("user_a", &Friendship::user_a),
            make_column("user_b", &Friendship::user_b),
            make_column("created_at", &Friendship::created_at),
            primary_key(&Friendship::user_a, &Friendship::user_b)
        )
    );
}

/// SQLite 持久化封装：存储超过 10 条的历史聊天消息
class SqliteSaver {
    using Storage = decltype(make_message_storage(""));
    std::unique_ptr<Storage> storage_;
    std::mutex mtx_;  // 保护 SQLite 连接（任务池线程 + RPC 线程可能同时访问）
public:
    bool init(const std::string& db_path) {
        auto st = make_message_storage(db_path);
        st.sync_schema();
        std::lock_guard<std::mutex> lock(mtx_);
        storage_ = std::make_unique<Storage>(std::move(st));
        return true;
    }

    /// 批量写入（INSERT OR REPLACE，按 seq_id 主键去重）
    void save(const std::vector<Message>& msgs) {
        if (msgs.empty()) return;
        std::lock_guard<std::mutex> lock(mtx_);
        storage_->transaction([&] {
            for (const auto& m : msgs) {
                storage_->replace(m);
            }
            return true;
        });
    }

    /// 查询两个用户之间的历史消息（翻页用）
    /// before_seq_id = 0 表示从最新开始，返回按 seq_id DESC（最新在前）
    std::vector<Message> load(const std::string& user_a,
                              const std::string& user_b,
                              uint64_t before_seq_id = 0,
                              size_t max_count = 20) {
        using namespace sqlite_orm;
        std::lock_guard<std::mutex> lock(mtx_);
        if (before_seq_id > 0) {
            return storage_->get_all<Message>(
                where(
                    ((c(&Message::from_user) == user_a && c(&Message::to_user) == user_b) ||
                     (c(&Message::from_user) == user_b && c(&Message::to_user) == user_a)) &&
                    c(&Message::seq_id) < before_seq_id
                ),
                order_by(&Message::seq_id).desc(),
                limit(static_cast<int>(max_count))
            );
        }
        return storage_->get_all<Message>(
            where(
                (c(&Message::from_user) == user_a && c(&Message::to_user) == user_b) ||
                (c(&Message::from_user) == user_b && c(&Message::to_user) == user_a)
            ),
            order_by(&Message::seq_id).desc(),
            limit(static_cast<int>(max_count))
        );
    }

    /// seq_id > after_seq_id, ORDER BY seq_id ASC, LIMIT max_count
    std::vector<Message> load_after(const std::string& user_a,
                                    const std::string& user_b,
                                    uint64_t after_seq_id,
                                    size_t max_count = 50) {
        using namespace sqlite_orm;
        std::lock_guard<std::mutex> lock(mtx_);
        return storage_->get_all<Message>(
            where(
                ((c(&Message::from_user) == user_a && c(&Message::to_user) == user_b) ||
                 (c(&Message::from_user) == user_b && c(&Message::to_user) == user_a)) &&
                c(&Message::seq_id) > after_seq_id
            ),
            order_by(&Message::seq_id).asc(),
            limit(static_cast<int>(max_count))
        );
    }

    /// 取会话最新消息（不含 seq_id 过滤），ORDER BY seq_id DESC, LIMIT max_count
    std::vector<Message> load_latest(const std::string& user_a,
                                     const std::string& user_b,
                                     size_t max_count = 50) {
        using namespace sqlite_orm;
        std::lock_guard<std::mutex> lock(mtx_);
        return storage_->get_all<Message>(
            where(
                (c(&Message::from_user) == user_a && c(&Message::to_user) == user_b) ||
                (c(&Message::from_user) == user_b && c(&Message::to_user) == user_a)
            ),
            order_by(&Message::seq_id).desc(),
            limit(static_cast<int>(max_count))
        );
    }

    /// 注册用户：插入 users 表
    bool register_user(const std::string& username, const std::string& password) {
        using namespace sqlite_orm;
        std::lock_guard<std::mutex> lock(mtx_);
        auto existing = storage_->get_all<User>(
            where(c(&User::username) == username)
        );
        if (!existing.empty()) return false;

        storage_->replace(User{username, password});
        LOG_INFO("新用户注册: {}", username);
        return true;
    }

    /// 验证用户密码
    bool verify_user(const std::string& username, const std::string& password) {
        using namespace sqlite_orm;
        std::lock_guard<std::mutex> lock(mtx_);
        auto users = storage_->get_all<User>(
            where(c(&User::username) == username)
        );
        return !users.empty() && users[0].password_hash == password;
    }

    /// 检查用户是否存在
    bool user_exists(const std::string& username) {
        using namespace sqlite_orm;
        std::lock_guard<std::mutex> lock(mtx_);
        auto users = storage_->get_all<User>(
            where(c(&User::username) == username)
        );
        return !users.empty();
    }

    // ========== 好友系统 ==========

    /// 搜索用户（关键字模糊匹配）
    std::vector<std::string> search_users(const std::string& keyword, const std::string& self, size_t max_count = 20) {
        using namespace sqlite_orm;
        std::lock_guard<std::mutex> lock(mtx_);
        auto users = storage_->get_all<User>(
            where(like(&User::username, "%" + keyword + "%") and
                  c(&User::username) != self),
            limit(static_cast<int>(max_count))
        );
        std::vector<std::string> result;
        result.reserve(users.size());
        for (auto& u : users) result.push_back(std::move(u.username));
        return result;
    }

    /// 发送好友请求：返回 true 表示成功，false 表示已存在 pending 请求或已是好友
    bool send_friend_request(const std::string& from, const std::string& to) {
        using namespace sqlite_orm;
        std::lock_guard<std::mutex> lock(mtx_);

        // 检查是否已是好友
        auto [a, b] = friend_pair(from, to);
        auto existing_friends = storage_->get_all<Friendship>(
            where(c(&Friendship::user_a) == a and c(&Friendship::user_b) == b)
        );
        if (!existing_friends.empty()) return false;

        // 检查是否有 pending 请求已存在
        auto existing_reqs = storage_->get_all<FriendRequest>(
            where(c(&FriendRequest::from_user) == from and
                  c(&FriendRequest::to_user) == to and
                  c(&FriendRequest::status) == "pending")
        );
        if (!existing_reqs.empty()) return false;

        // 检查反向是否有 pending 请求（对方已经发过）
        auto reverse_reqs = storage_->get_all<FriendRequest>(
            where(c(&FriendRequest::from_user) == to and
                  c(&FriendRequest::to_user) == from and
                  c(&FriendRequest::status) == "pending")
        );
        if (!reverse_reqs.empty()) {
            // 对方已经发过请求，自动接受
            for (auto& r : reverse_reqs) {
                r.status = "accepted";
                storage_->update(r);
            }
            storage_->replace(Friendship{a, b, std::time(nullptr)});
            return true;
        }

        storage_->insert(FriendRequest{0, from, to, "pending", std::time(nullptr)});
        return true;
    }

    /// 获取待处理的好友请求（别人发给我的）
    std::vector<FriendRequest> get_pending_requests(const std::string& username) {
        using namespace sqlite_orm;
        std::lock_guard<std::mutex> lock(mtx_);
        return storage_->get_all<FriendRequest>(
            where(c(&FriendRequest::to_user) == username and
                  c(&FriendRequest::status) == "pending"),
            order_by(&FriendRequest::created_at).desc()
        );
    }

    /// 获取已发送但未处理的请求
    std::vector<FriendRequest> get_sent_requests(const std::string& username) {
        using namespace sqlite_orm;
        std::lock_guard<std::mutex> lock(mtx_);
        return storage_->get_all<FriendRequest>(
            where(c(&FriendRequest::from_user) == username and
                  c(&FriendRequest::status) == "pending"),
            order_by(&FriendRequest::created_at).desc()
        );
    }

    /// 处理好友请求
    bool handle_friend_request(int request_id, bool accept) {
        using namespace sqlite_orm;
        std::lock_guard<std::mutex> lock(mtx_);

        auto requests = storage_->get_all<FriendRequest>(
            where(c(&FriendRequest::id) == request_id and
                  c(&FriendRequest::status) == "pending")
        );
        if (requests.empty()) return false;
        auto& req = requests[0];

        if (accept) {
            req.status = "accepted";
            storage_->update(req);
            auto [a, b] = friend_pair(req.from_user, req.to_user);
            storage_->replace(Friendship{a, b, std::time(nullptr)});
        } else {
            req.status = "rejected";
            storage_->update(req);
        }
        return true;
    }

    /// 获取好友列表
    std::vector<std::string> get_friends(const std::string& username) {
        using namespace sqlite_orm;
        std::lock_guard<std::mutex> lock(mtx_);
        auto rows = storage_->get_all<Friendship>(
            where(c(&Friendship::user_a) == username or
                  c(&Friendship::user_b) == username)
        );
        std::vector<std::string> result;
        result.reserve(rows.size());
        for (auto& f : rows) {
            if (f.user_a == username)
                result.push_back(std::move(f.user_b));
            else
                result.push_back(std::move(f.user_a));
        }
        return result;
    }

    /// 获取存储引用（供 server.cpp 需要直接查询时使用）
    Storage& get_storage() { return *storage_; }

    /// 检查是否已是好友
    bool is_friend(const std::string& a, const std::string& b) {
        using namespace sqlite_orm;
        std::lock_guard<std::mutex> lock(mtx_);
        auto [sa, sb] = friend_pair(a, b);
        auto rows = storage_->get_all<Friendship>(
            where(c(&Friendship::user_a) == sa and c(&Friendship::user_b) == sb)
        );
        return !rows.empty();
    }

private:
    /// 返回字典序的（小, 大）对，用于 friends 表
    static std::pair<std::string, std::string> friend_pair(const std::string& a, const std::string& b) {
        if (a < b) return {a, b};
        return {b, a};
    }
};
