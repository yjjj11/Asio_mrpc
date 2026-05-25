#pragma once

#include <mysql/mysql.h>
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <optional>

/// 消息结构体
struct Message {
    uint64_t seq_id;
    std::string from_user;
    std::string to_user;
    std::string msg;
    std::string created_at;
};
using Messages = std::vector<Message>;

/// 用户账户结构体
struct User {
    std::string username;
    std::string password_hash;
};

/// 好友请求结构体
struct FriendRequest {
    int id;
    std::string from_user;
    std::string to_user;
    std::string status;
    int64_t created_at;
};
using FriendRequests = std::vector<FriendRequest>;

/// 好友关系结构体
struct Friendship {
    std::string user_a;
    std::string user_b;
    int64_t created_at;
};

// ==================== MySQL 连接池 ====================

class MySQLPool {
    std::queue<MYSQL*> conns_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::string host_, user_, pass_, db_;
    uint16_t port_ = 3306;
public:
    ~MySQLPool() { close_all(); }

    bool init(const std::string& host, uint16_t port,
              const std::string& user, const std::string& pass,
              const std::string& db, int pool_size = 0) {
        host_ = host; port_ = port; user_ = user; pass_ = pass; db_ = db;
        if (pool_size <= 0)
            pool_size = std::max(1u, std::thread::hardware_concurrency());
        if (pool_size > 16) pool_size = 16;

        for (int i = 0; i < pool_size; i++) {
            MYSQL* conn = mysql_init(nullptr);
            if (!conn) return false;
            if (!mysql_real_connect(conn, host.c_str(), user.c_str(), pass.c_str(),
                                    db.c_str(), port, nullptr, 0)) {
                LOG_ERROR("MySQL connect failed: {}", mysql_error(conn));
                mysql_close(conn);
                close_all();
                return false;
            }
            mysql_set_character_set(conn, "utf8mb4");
            conns_.push(conn);
        }
        return true;
    }

    MYSQL* borrow() {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return !conns_.empty(); });
        MYSQL* conn = conns_.front();
        conns_.pop();
        lock.unlock();
        if (mysql_ping(conn)) {
            mysql_close(conn);
            conn = mysql_init(nullptr);
            if (!mysql_real_connect(conn, host_.c_str(), user_.c_str(), pass_.c_str(),
                                    db_.c_str(), port_, nullptr, 0)) {
                LOG_ERROR("MySQL reconnect failed: {}", mysql_error(conn));
            }
        }
        return conn;
    }

    void release(MYSQL* conn) {
        std::lock_guard<std::mutex> lock(mtx_);
        conns_.push(conn);
        cv_.notify_one();
    }

    void close_all() {
        std::lock_guard<std::mutex> lock(mtx_);
        while (!conns_.empty()) {
            mysql_close(conns_.front());
            conns_.pop();
        }
    }
};

// ==================== RAII 连接包装 ====================

struct AutoConn {
    MySQLPool& pool;
    MYSQL* conn;
    AutoConn(MySQLPool& p) : pool(p), conn(p.borrow()) {}
    ~AutoConn() { if (conn) pool.release(conn); }
    AutoConn(const AutoConn&) = delete;
    AutoConn& operator=(const AutoConn&) = delete;
    MYSQL* operator->() { return conn; }
    operator MYSQL*() { return conn; }
    bool ok() const { return conn != nullptr; }
};

// ==================== MySQL 查询结果 RAII ====================

class QueryResult {
    MYSQL_RES* res_ = nullptr;
public:
    QueryResult(MYSQL* conn, const std::string& sql) {
        if (mysql_query(conn, sql.c_str())) {
            LOG_ERROR("MySQL query error [{}]: {}", mysql_errno(conn), mysql_error(conn));
            return;
        }
        res_ = mysql_store_result(conn);
    }
    ~QueryResult() { if (res_) mysql_free_result(res_); }
    QueryResult(const QueryResult&) = delete;
    QueryResult& operator=(const QueryResult&) = delete;

    bool ok() const { return res_ != nullptr; }
    MYSQL_ROW fetch() { return res_ ? mysql_fetch_row(res_) : nullptr; }
    my_ulonglong num_rows() const { return res_ ? mysql_num_rows(res_) : 0; }
};

// ==================== MySQL 持久化封装 ====================

class MySqlSaver {
    MySQLPool pool_;

    static std::string escape(MYSQL* conn, const std::string& s) {
        if (s.empty()) return std::string(2, '\'');
        std::vector<char> buf(s.size() * 2 + 1);
        unsigned long len = mysql_real_escape_string(conn, buf.data(), s.data(), s.size());
        return "'" + std::string(buf.data(), len) + "'";
    }

    static std::string q(const std::string& s) { return "'" + s + "'"; }

    static uint64_t to_u64(const char* s) { return s ? strtoull(s, nullptr, 10) : 0; }
    static int64_t to_i64(const char* s) { return s ? strtoll(s, nullptr, 10) : 0; }
    static int to_i(const char* s) { return s ? atoi(s) : 0; }

    /// 返回字典序的（小, 大）对，用于 friends 表
    static std::pair<std::string, std::string> friend_pair(const std::string& a, const std::string& b) {
        if (a < b) return {a, b};
        return {b, a};
    }

    bool exec(MYSQL* conn, const std::string& sql) {
        if (mysql_query(conn, sql.c_str())) {
            LOG_ERROR("MySQL exec error [{}]: {} sql: {}", mysql_errno(conn), mysql_error(conn), sql);
            return false;
        }
        return true;
    }

    Message row_to_message(MYSQL_ROW row) {
        return {to_u64(row[0]), row[1] ? row[1] : "", row[2] ? row[2] : "",
                row[3] ? row[3] : "", row[4] ? row[4] : ""};
    }

public:
    bool init(const std::string& host, uint16_t port,
              const std::string& user, const std::string& pass,
              const std::string& db, int pool_size = 0) {
        if (!pool_.init(host, port, user, pass, db, pool_size))
            return false;

        // 自动创建表
        AutoConn ac(pool_);
        const char* schema[] = {
            "CREATE TABLE IF NOT EXISTS messages ("
            "seq_id BIGINT UNSIGNED NOT NULL,"
            "from_user VARCHAR(255) NOT NULL,"
            "to_user VARCHAR(255) NOT NULL,"
            "msg TEXT NOT NULL,"
            "created_at VARCHAR(64) NOT NULL,"
            "PRIMARY KEY (seq_id, from_user, to_user)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

            "CREATE TABLE IF NOT EXISTS users ("
            "username VARCHAR(255) NOT NULL PRIMARY KEY,"
            "password_hash VARCHAR(255) NOT NULL"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

            "CREATE TABLE IF NOT EXISTS friend_requests ("
            "id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
            "from_user VARCHAR(255) NOT NULL,"
            "to_user VARCHAR(255) NOT NULL,"
            "status VARCHAR(32) NOT NULL DEFAULT 'pending',"
            "created_at BIGINT NOT NULL,"
            "INDEX idx_fr_to (to_user),"
            "INDEX idx_fr_from (from_user)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

            "CREATE TABLE IF NOT EXISTS friends ("
            "user_a VARCHAR(255) NOT NULL,"
            "user_b VARCHAR(255) NOT NULL,"
            "created_at BIGINT NOT NULL,"
            "PRIMARY KEY (user_a, user_b),"
            "INDEX idx_fb (user_b)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
        };
        for (auto& s : schema) {
            if (!exec(ac, s)) return false;
        }

        // 常用查询索引（静默处理已存在的情况）
        const char* indexes[] = {
            "ALTER TABLE messages ADD INDEX idx_conv (from_user, to_user, seq_id)",
        };
        for (auto& s : indexes) {
            if (mysql_query(ac, s) && mysql_errno(ac) != 1061) {  // 1061 = duplicate key
                LOG_ERROR("MySQL index error [{}]: {}", mysql_errno(ac), mysql_error(ac));
            }
        }

        LOG_INFO("MySQL schema initialized");
        return true;
    }

    // ========== 消息 ==========

    void save(const std::vector<Message>& msgs) {
        if (msgs.empty()) return;
        AutoConn ac(pool_);
        if (!exec(ac, "BEGIN")) return;

        std::string sql = "INSERT INTO messages VALUES ";
        for (size_t i = 0; i < msgs.size(); i++) {
            if (i > 0) sql += ",";
            sql += "(" + std::to_string(msgs[i].seq_id) + ","
                 + escape(ac, msgs[i].from_user) + ","
                 + escape(ac, msgs[i].to_user) + ","
                 + escape(ac, msgs[i].msg) + ","
                 + escape(ac, msgs[i].created_at) + ")";
        }
        sql += " ON DUPLICATE KEY UPDATE msg=VALUES(msg), created_at=VALUES(created_at)";
        if (!exec(ac, sql))
            exec(ac, "ROLLBACK");
        else
            exec(ac, "COMMIT");
    }

    Messages load(const std::string& user_a, const std::string& user_b,
                  uint64_t before_seq_id = 0, size_t max_count = 20) {
        AutoConn ac(pool_);
        std::string sql = "SELECT seq_id,from_user,to_user,msg,created_at FROM messages WHERE "
            "((from_user=" + q(user_a) + " AND to_user=" + q(user_b) + ")"
            " OR (from_user=" + q(user_b) + " AND to_user=" + q(user_a) + "))";
        if (before_seq_id > 0)
            sql += " AND seq_id < " + std::to_string(before_seq_id);
        sql += " ORDER BY seq_id DESC LIMIT " + std::to_string(max_count);

        QueryResult qr(ac, sql);
        if (!qr.ok()) return {};
        Messages result;
        while (auto row = qr.fetch())
            result.push_back(row_to_message(row));
        return result;
    }

    Messages load_after(const std::string& user_a, const std::string& user_b,
                        uint64_t after_seq_id, size_t max_count = 50) {
        AutoConn ac(pool_);
        std::string sql = "SELECT seq_id,from_user,to_user,msg,created_at FROM messages WHERE "
            "((from_user=" + q(user_a) + " AND to_user=" + q(user_b) + ")"
            " OR (from_user=" + q(user_b) + " AND to_user=" + q(user_a) + "))"
            " AND seq_id > " + std::to_string(after_seq_id)
            + " ORDER BY seq_id ASC LIMIT " + std::to_string(max_count);

        QueryResult qr(ac, sql);
        if (!qr.ok()) return {};
        Messages result;
        while (auto row = qr.fetch())
            result.push_back(row_to_message(row));
        return result;
    }

    Messages load_latest(const std::string& user_a, const std::string& user_b,
                         size_t max_count = 50) {
        AutoConn ac(pool_);
        std::string sql = "SELECT seq_id,from_user,to_user,msg,created_at FROM messages WHERE "
            "((from_user=" + q(user_a) + " AND to_user=" + q(user_b) + ")"
            " OR (from_user=" + q(user_b) + " AND to_user=" + q(user_a) + "))"
            " ORDER BY seq_id DESC LIMIT " + std::to_string(max_count);

        QueryResult qr(ac, sql);
        if (!qr.ok()) return {};
        Messages result;
        while (auto row = qr.fetch())
            result.push_back(row_to_message(row));
        return result;
    }

    // ========== 用户 ==========

    bool register_user(const std::string& username, const std::string& password) {
        AutoConn ac(pool_);
        // 检查已存在
        QueryResult qr(ac, "SELECT username FROM users WHERE username=" + q(username));
        if (qr.ok() && qr.num_rows() > 0) return false;
        if (!exec(ac, "INSERT INTO users VALUES (" + q(username) + "," + q(password) + ")"))
            return false;
        LOG_INFO("MySQL: new user registered: {}", username);
        return true;
    }

    bool verify_user(const std::string& username, const std::string& password) {
        AutoConn ac(pool_);
        QueryResult qr(ac, "SELECT password_hash FROM users WHERE username=" + q(username));
        if (!qr.ok()) return false;
        auto row = qr.fetch();
        return row && row[0] && password == row[0];
    }

    bool user_exists(const std::string& username) {
        AutoConn ac(pool_);
        QueryResult qr(ac, "SELECT 1 FROM users WHERE username=" + q(username));
        return qr.ok() && qr.num_rows() > 0;
    }

    // ========== 好友 ==========

    std::vector<std::string> search_users(const std::string& keyword, const std::string& self,
                                          size_t max_count = 20) {
        AutoConn ac(pool_);
        std::vector<char> buf(keyword.size() * 2 + 1);
        unsigned long len = mysql_real_escape_string(ac, buf.data(), keyword.data(), keyword.size());
        std::string safe_key(buf.data(), len);
        QueryResult qr(ac, "SELECT username FROM users WHERE username LIKE '%"
                       + safe_key + "%' AND username !=" + q(self)
                       + " LIMIT " + std::to_string(max_count));
        if (!qr.ok()) return {};
        std::vector<std::string> result;
        while (auto row = qr.fetch())
            if (row[0]) result.emplace_back(row[0]);
        return result;
    }

    bool send_friend_request(const std::string& from, const std::string& to) {
        AutoConn ac(pool_);
        auto [a, b] = friend_pair(from, to);

        // 检查是否已是好友
        QueryResult fr(ac, "SELECT 1 FROM friends WHERE user_a=" + q(a) + " AND user_b=" + q(b));
        if (fr.ok() && fr.num_rows() > 0) return false;

        // 检查已有 pending 请求
        QueryResult pr(ac, "SELECT 1 FROM friend_requests WHERE from_user=" + q(from)
                       + " AND to_user=" + q(to) + " AND status='pending'");
        if (pr.ok() && pr.num_rows() > 0) return false;

        // 反向请求存在？自动接受
        QueryResult rr(ac, "SELECT id FROM friend_requests WHERE from_user=" + q(to)
                       + " AND to_user=" + q(from) + " AND status='pending' LIMIT 1");
        if (rr.ok() && rr.num_rows() > 0) {
            if (!exec(ac, "UPDATE friend_requests SET status='accepted' WHERE from_user=" + q(to)
                      + " AND to_user=" + q(from) + " AND status='pending'"))
                return false;
            return exec(ac, "INSERT INTO friends VALUES (" + q(a) + "," + q(b) + ","
                        + std::to_string(std::time(nullptr)) + ")");
        }

        return exec(ac, "INSERT INTO friend_requests (from_user,to_user,status,created_at) VALUES ("
                    + q(from) + "," + q(to) + ",'pending'," + std::to_string(std::time(nullptr)) + ")");
    }

    FriendRequests get_pending_requests(const std::string& username) {
        AutoConn ac(pool_);
        QueryResult qr(ac, "SELECT id,from_user,to_user,status,created_at FROM friend_requests "
                       "WHERE to_user=" + q(username) + " AND status='pending' ORDER BY created_at DESC");
        if (!qr.ok()) return {};
        FriendRequests result;
        while (auto row = qr.fetch())
            result.push_back({to_i(row[0]), row[1] ? row[1] : "", row[2] ? row[2] : "",
                              row[3] ? row[3] : "", to_i64(row[4])});
        return result;
    }

    FriendRequests get_sent_requests(const std::string& username) {
        AutoConn ac(pool_);
        QueryResult qr(ac, "SELECT id,from_user,to_user,status,created_at FROM friend_requests "
                       "WHERE from_user=" + q(username) + " AND status='pending' ORDER BY created_at DESC");
        if (!qr.ok()) return {};
        FriendRequests result;
        while (auto row = qr.fetch())
            result.push_back({to_i(row[0]), row[1] ? row[1] : "", row[2] ? row[2] : "",
                              row[3] ? row[3] : "", to_i64(row[4])});
        return result;
    }

    bool handle_friend_request(int request_id, bool accept) {
        AutoConn ac(pool_);

        // 检查请求存在且 pending
        QueryResult qr(ac, "SELECT from_user,to_user FROM friend_requests "
                       "WHERE id=" + std::to_string(request_id) + " AND status='pending'");
        if (!qr.ok() || qr.num_rows() == 0) return false;
        auto row = qr.fetch();
        if (!row || !row[0] || !row[1]) return false;
        std::string from = row[0], to = row[1];

        if (accept) {
            if (!exec(ac, "UPDATE friend_requests SET status='accepted' WHERE id="
                      + std::to_string(request_id)))
                return false;
            auto [a, b] = friend_pair(from, to);
            return exec(ac, "INSERT IGNORE INTO friends VALUES (" + q(a) + "," + q(b) + ","
                        + std::to_string(std::time(nullptr)) + ")");
        } else {
            return exec(ac, "UPDATE friend_requests SET status='rejected' WHERE id="
                        + std::to_string(request_id));
        }
    }

    std::vector<std::string> get_friends(const std::string& username) {
        AutoConn ac(pool_);
        QueryResult qr(ac, "SELECT user_a,user_b,created_at FROM friends "
                       "WHERE user_a=" + q(username) + " OR user_b=" + q(username));
        if (!qr.ok()) return {};
        std::vector<std::string> result;
        while (auto row = qr.fetch()) {
            if (!row[0] || !row[1]) continue;
            if (username == row[0]) result.emplace_back(row[1]);
            else result.emplace_back(row[0]);
        }
        return result;
    }

    bool is_friend(const std::string& a, const std::string& b) {
        auto [sa, sb] = friend_pair(a, b);
        AutoConn ac(pool_);
        QueryResult qr(ac, "SELECT 1 FROM friends WHERE user_a=" + q(sa) + " AND user_b=" + q(sb));
        return qr.ok() && qr.num_rows() > 0;
    }

    // ========== 新增辅助方法（替代直接 get_storage()） ==========

    /// 按 ID 查询好友请求
    std::optional<FriendRequest> get_friend_request(int request_id) {
        AutoConn ac(pool_);
        QueryResult qr(ac, "SELECT id,from_user,to_user,status,created_at FROM friend_requests "
                       "WHERE id=" + std::to_string(request_id));
        if (!qr.ok()) return std::nullopt;
        auto row = qr.fetch();
        if (!row) return std::nullopt;
        return FriendRequest{to_i(row[0]), row[1] ? row[1] : "", row[2] ? row[2] : "",
                             row[3] ? row[3] : "", to_i64(row[4])};
    }

    /// 批量查询未读信息（含最新消息发送者）
    std::vector<std::tuple<std::string, uint64_t, std::string>>
    get_unread_info(const std::string& username,
                    const std::vector<std::string>& partners) {
        AutoConn ac(pool_);
        std::vector<std::tuple<std::string, uint64_t, std::string>> result;
        for (const auto& p : partners) {
            QueryResult qr(ac,
                "SELECT seq_id,from_user FROM messages WHERE "
                "((from_user=" + q(username) + " AND to_user=" + q(p) + ")"
                " OR (from_user=" + q(p) + " AND to_user=" + q(username) + "))"
                " ORDER BY seq_id DESC LIMIT 1");
            if (!qr.ok()) continue;
            auto row = qr.fetch();
            if (row && row[0] && row[1])
                result.emplace_back(p, to_u64(row[0]), row[1]);
        }
        return result;
    }
};
