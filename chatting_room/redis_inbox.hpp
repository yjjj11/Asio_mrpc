#pragma once

#include <hiredis/hiredis.h>
#include <string>
#include <cstdint>
#include <mutex>

/// Redis 封装：仅用于 seq_id 生成和 token 管理（不再存储会话消息）
class RedisInbox {
public:
    RedisInbox() = default;
    ~RedisInbox() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (ctx_) redisFree(ctx_);
    }

    RedisInbox(const RedisInbox&) = delete;
    RedisInbox& operator=(const RedisInbox&) = delete;

    bool connect(const std::string& host = "127.0.0.1", int port = 6379) {
        std::lock_guard<std::mutex> lock(mtx_);
        ctx_ = redisConnect(host.c_str(), port);
        if (!ctx_ || ctx_->err) {
            if (ctx_) { redisFree(ctx_); ctx_ = nullptr; }
            return false;
        }
        const char* av[] = {"PING"}; size_t al[] = {4};
        auto* r = (redisReply*)redisCommandArgv(ctx_, 1, av, al);
        if (!r) { redisFree(ctx_); ctx_ = nullptr; return false; }
        freeReplyObject(r);
        return true;
    }

    /// 每个聊天对独立的 seq_id，key = seq:<sorted_a>:<sorted_b>
    uint64_t seq_id(const std::string& sender, const std::string& receiver) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::string key;
        if (sender < receiver)
            key = "seq:" + sender + ":" + receiver;
        else
            key = "seq:" + receiver + ":" + sender;

        const char* av[] = {"INCR", key.c_str()};
        size_t al[] = {4, key.size()};
        auto* r = (redisReply*)redisCommandArgv(ctx_, 2, av, al);
        if (!r || r->type != REDIS_REPLY_INTEGER) {
            if (r) freeReplyObject(r);
            return 0;
        }
        uint64_t seq = static_cast<uint64_t>(r->integer);
        freeReplyObject(r);
        return seq;
    }

    /// SETEX token:<token> ttl <username>
    bool save_token(const std::string& token, const std::string& username, int ttl = 1800) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::string key = "token:" + token;
        std::string ttl_str = std::to_string(ttl);
        const char* av[] = {"SETEX", key.c_str(), ttl_str.c_str(), username.c_str()};
        size_t al[] = {5, key.size(), ttl_str.size(), username.size()};
        auto* r = (redisReply*)redisCommandArgv(ctx_, 4, av, al);
        if (!r) return false;
        freeReplyObject(r);
        return true;
    }

    /// GET token:<token> → username or ""
    std::string verify_token(const std::string& token) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::string key = "token:" + token;
        const char* av[] = {"GET", key.c_str()};
        size_t al[] = {3, key.size()};
        auto* r = (redisReply*)redisCommandArgv(ctx_, 2, av, al);
        if (!r || r->type != REDIS_REPLY_STRING) {
            if (r) freeReplyObject(r);
            return {};
        }
        std::string username(r->str);
        freeReplyObject(r);
        return username;
    }

    /// DEL token:<token>
    bool delete_token(const std::string& token) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::string key = "token:" + token;
        const char* av[] = {"DEL", key.c_str()};
        size_t al[] = {3, key.size()};
        auto* r = (redisReply*)redisCommandArgv(ctx_, 2, av, al);
        if (!r) return false;
        freeReplyObject(r);
        return true;
    }

private:
    redisContext* ctx_ = nullptr;
    std::mutex mtx_;
};
