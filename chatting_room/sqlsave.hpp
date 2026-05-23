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
};
