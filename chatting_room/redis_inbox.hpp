#pragma once

#include <hiredis/hiredis.h>
#include <string>
#include <vector>
#include <tuple>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <mutex>

/// Redis 封装：会话 ZSET (conv:A:B) 存储最近 N 条消息
/// 编码格式 "from|to|msg"（三字段），按 seq_id 排序
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

    /// INCR 获取全局递增 seq_id
    uint64_t next_seq_id() {
        std::lock_guard<std::mutex> lock(mtx_);
        const char* av[] = {"INCR", "global:msg_seq"};
        size_t al[] = {4, 14};
        auto* r = (redisReply*)redisCommandArgv(ctx_, 2, av, al);
        if (!r || r->type != REDIS_REPLY_INTEGER) {
            if (r) freeReplyObject(r);
            return 0;
        }
        uint64_t seq = static_cast<uint64_t>(r->integer);
        freeReplyObject(r);
        return seq;
    }

    /// ZADD conv:<sorted_pair> seq_id "from|to|msg"
    /// 返回 true 表示写入成功
    bool push_conv(const std::string& user_a, const std::string& user_b,
                   uint64_t seq_id, const std::string& from,
                   const std::string& to, const std::string& msg) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto encoded = from + "|" + to + "|" + msg;
        auto seq_str = std::to_string(seq_id);
        std::string key = conv_key(user_a, user_b);
        const char* av[] = {"ZADD", key.c_str(), seq_str.c_str(), encoded.c_str()};
        size_t al[] = {4, key.size(), seq_str.size(), encoded.size()};
        auto* r = (redisReply*)redisCommandArgv(ctx_, 4, av, al);
        if (!r) return false;
        freeReplyObject(r);
        return true;
    }

    /// 如果 conv ZSET 超过 keep 条，弹出最旧的并返回
    /// 返回 vector<(seq_id, from, to, msg)>
    std::vector<std::tuple<uint64_t, std::string, std::string, std::string>> trim_conv(
        const std::string& user_a, const std::string& user_b, size_t keep = 10)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<std::tuple<uint64_t, std::string, std::string, std::string>> result;
        std::string key = conv_key(user_a, user_b);

        // ZCARD 检查大小
        const char* card_av[] = {"ZCARD", key.c_str()};
        size_t card_al[] = {5, key.size()};
        auto* card_r = (redisReply*)redisCommandArgv(ctx_, 2, card_av, card_al);
        if (!card_r || card_r->type != REDIS_REPLY_INTEGER) {
            if (card_r) freeReplyObject(card_r);
            return result;
        }
        long long count = card_r->integer;
        freeReplyObject(card_r);

        if (count <= static_cast<long long>(keep)) return result;
        long long remove_cnt = count - keep;

        // ZRANGE 0 remove_cnt-1 获取最旧的成员
        std::string rcnt = std::to_string(remove_cnt - 1);
        const char* range_av[] = {"ZRANGE", key.c_str(), "0", rcnt.c_str(), "WITHSCORES"};
        size_t range_al[] = {6, key.size(), 1, rcnt.size(), 10};
        auto* range_r = (redisReply*)redisCommandArgv(ctx_, 5, range_av, range_al);
        if (!range_r || range_r->type != REDIS_REPLY_ARRAY) {
            if (range_r) freeReplyObject(range_r);
            return result;
        }

        // 解码被移除的消息
        for (size_t i = 0; i + 1 < range_r->elements; i += 2) {
            if (range_r->element[i]->type != REDIS_REPLY_STRING) continue;
            if (range_r->element[i + 1]->type != REDIS_REPLY_STRING) continue;
            std::string encoded(range_r->element[i]->str);
            auto fpos = encoded.find('|');
            if (fpos == std::string::npos) continue;
            auto tpos = encoded.find('|', fpos + 1);
            if (tpos == std::string::npos) continue;
            std::string from = encoded.substr(0, fpos);
            std::string to   = encoded.substr(fpos + 1, tpos - fpos - 1);
            std::string msg  = encoded.substr(tpos + 1);
            uint64_t sid = strtoull(range_r->element[i + 1]->str, nullptr, 10);
            result.emplace_back(sid, std::move(from), std::move(to), std::move(msg));
        }
        freeReplyObject(range_r);

        // ZREMRANGEBYRANK 0 remove_cnt-1 删除最旧的消息
        std::string rcnt2 = std::to_string(remove_cnt - 1);
        const char* rem_av[] = {"ZREMRANGEBYRANK", key.c_str(), "0", rcnt2.c_str()};
        size_t rem_al[] = {15, key.size(), 1, rcnt2.size()};
        auto* rem_r = (redisReply*)redisCommandArgv(ctx_, 4, rem_av, rem_al);
        if (rem_r) freeReplyObject(rem_r);

        return result;
    }

    /// ZRANGE conv ZSET 取最近 limit 条（升序，seq_id 从小到大）
    std::vector<std::tuple<uint64_t, std::string, std::string, std::string>> pull_recent(
        const std::string& user_a, const std::string& user_b, size_t limit = 10)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<std::tuple<uint64_t, std::string, std::string, std::string>> result;
        std::string key = conv_key(user_a, user_b);
        std::string start = "-" + std::to_string(limit);
        const char* end = "-1";

        // ZRANGE key -limit -1 WITHSCORES 取最高分的 limit 条，升序排列
        const char* av[] = {"ZRANGE", key.c_str(), start.c_str(), end, "WITHSCORES"};
        size_t al[] = {6, key.size(), start.size(), 2, 10};
        auto* r = (redisReply*)redisCommandArgv(ctx_, 5, av, al);
        if (!r || r->type != REDIS_REPLY_ARRAY) {
            if (r) freeReplyObject(r);
            return result;
        }
        for (size_t i = 0; i + 1 < r->elements; i += 2) {
            if (r->element[i]->type != REDIS_REPLY_STRING) continue;
            if (r->element[i + 1]->type != REDIS_REPLY_STRING) continue;
            std::string encoded(r->element[i]->str);
            auto fpos = encoded.find('|');
            if (fpos == std::string::npos) continue;
            auto tpos = encoded.find('|', fpos + 1);
            if (tpos == std::string::npos) continue;
            std::string from = encoded.substr(0, fpos);
            std::string to   = encoded.substr(fpos + 1, tpos - fpos - 1);
            std::string msg  = encoded.substr(tpos + 1);
            uint64_t sid = strtoull(r->element[i + 1]->str, nullptr, 10);
            result.emplace_back(sid, std::move(from), std::move(to), std::move(msg));
        }
        freeReplyObject(r);
        return result;
    }

    /// ZRANGEBYSCORE key (after_seq +inf WITHSCORES LIMIT 0 limit
    /// 返回 seq > after_seq 的消息，ASC 升序
    std::vector<std::tuple<uint64_t, std::string, std::string, std::string>> pull_after(
        const std::string& user_a, const std::string& user_b,
        uint64_t after_seq, size_t limit = 50)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<std::tuple<uint64_t, std::string, std::string, std::string>> result;
        std::string key = conv_key(user_a, user_b);
        std::string after_str = "(" + std::to_string(after_seq);
        std::string limit_str = std::to_string(limit);

        const char* av[] = {"ZRANGEBYSCORE", key.c_str(), after_str.c_str(), "+inf", "WITHSCORES", "LIMIT", "0", limit_str.c_str()};
        size_t al[] = {13, key.size(), after_str.size(), 4, 10, 5, 1, limit_str.size()};
        auto* r = (redisReply*)redisCommandArgv(ctx_, 8, av, al);
        if (!r || r->type != REDIS_REPLY_ARRAY) {
            if (r) freeReplyObject(r);
            return result;
        }
        for (size_t i = 0; i + 1 < r->elements; i += 2) {
            if (r->element[i]->type != REDIS_REPLY_STRING) continue;
            if (r->element[i + 1]->type != REDIS_REPLY_STRING) continue;
            std::string encoded(r->element[i]->str);
            auto fpos = encoded.find('|');
            if (fpos == std::string::npos) continue;
            auto tpos = encoded.find('|', fpos + 1);
            if (tpos == std::string::npos) continue;
            std::string from = encoded.substr(0, fpos);
            std::string to   = encoded.substr(fpos + 1, tpos - fpos - 1);
            std::string msg  = encoded.substr(tpos + 1);
            uint64_t sid = strtoull(r->element[i + 1]->str, nullptr, 10);
            result.emplace_back(sid, std::move(from), std::move(to), std::move(msg));
        }
        freeReplyObject(r);
        return result;
    }

    /// ZREVRANGEBYSCORE key (before_seq -inf WITHSCORES LIMIT 0 limit
    /// 返回 seq <= before_seq 的消息，DESC 降序（最新在前）
    std::vector<std::tuple<uint64_t, std::string, std::string, std::string>> pull_before(
        const std::string& user_a, const std::string& user_b,
        uint64_t before_seq, size_t limit = 10)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<std::tuple<uint64_t, std::string, std::string, std::string>> result;
        std::string key = conv_key(user_a, user_b);
        std::string before_str = std::to_string(before_seq);
        std::string limit_str = std::to_string(limit);

        const char* av[] = {"ZREVRANGEBYSCORE", key.c_str(), before_str.c_str(), "-inf", "WITHSCORES", "LIMIT", "0", limit_str.c_str()};
        size_t al[] = {17, key.size(), before_str.size(), 4, 10, 5, 1, limit_str.size()};
        auto* r = (redisReply*)redisCommandArgv(ctx_, 8, av, al);
        if (!r || r->type != REDIS_REPLY_ARRAY) {
            if (r) freeReplyObject(r);
            return result;
        }
        for (size_t i = 0; i + 1 < r->elements; i += 2) {
            if (r->element[i]->type != REDIS_REPLY_STRING) continue;
            if (r->element[i + 1]->type != REDIS_REPLY_STRING) continue;
            std::string encoded(r->element[i]->str);
            auto fpos = encoded.find('|');
            if (fpos == std::string::npos) continue;
            auto tpos = encoded.find('|', fpos + 1);
            if (tpos == std::string::npos) continue;
            std::string from = encoded.substr(0, fpos);
            std::string to   = encoded.substr(fpos + 1, tpos - fpos - 1);
            std::string msg  = encoded.substr(tpos + 1);
            uint64_t sid = strtoull(r->element[i + 1]->str, nullptr, 10);
            result.emplace_back(sid, std::move(from), std::move(to), std::move(msg));
        }
        freeReplyObject(r);
        return result;
    }

private:
    redisContext* ctx_ = nullptr;
    std::mutex mtx_;

    /// 生成有序会话 key：conv:<user_a>:<user_b>（字典序）
    static std::string conv_key(const std::string& a, const std::string& b) {
        if (a < b) return "conv:" + a + ":" + b;
        return "conv:" + b + ":" + a;
    }
};
