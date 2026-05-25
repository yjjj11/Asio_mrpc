#pragma once

#include <cstdint>
#include <chrono>
#include <mutex>
#include <string>
#include <cstdlib>

/// Snowflake ID 生成器
/// 41-bit timestamp(ms) + 10-bit node_id + 12-bit sequence = 63 bits
/// 自定义纪元: 2024-01-01 00:00:00 UTC
class Snowflake {
    static constexpr uint64_t EPOCH_MS = 1704067200000ULL;  // 2024-01-01
    static constexpr uint64_t NODE_BITS = 10;
    static constexpr uint64_t SEQ_BITS  = 12;
    static constexpr uint64_t MAX_SEQ   = (1ULL << SEQ_BITS) - 1;  // 4095

    uint64_t node_id_;
    uint64_t last_ms_ = 0;
    uint64_t seq_     = 0;
    std::mutex mtx_;

    static uint64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

public:
    /// 从节点 ID 字符串（如 "node-1"）构造，数字后缀作为 node_id
    explicit Snowflake(const std::string& node_id_str)
        : node_id_(parse_node_id(node_id_str)) {}

    /// 直接指定 node_id
    explicit Snowflake(uint64_t node_id) : node_id_(node_id) {}

    /// 生成下一个 ID，线程安全
    uint64_t next_id() {
        std::lock_guard<std::mutex> lock(mtx_);
        uint64_t now = now_ms() - EPOCH_MS;

        if (now == last_ms_) {
            seq_ = (seq_ + 1) & MAX_SEQ;
            if (seq_ == 0) {
                // 当前毫秒用尽，等待下一毫秒
                while (now == last_ms_) {
                    now = now_ms() - EPOCH_MS;
                }
            }
        } else {
            seq_ = 0;
            last_ms_ = now;
        }

        return (now << (NODE_BITS + SEQ_BITS)) | (node_id_ << SEQ_BITS) | seq_;
    }

private:
    static uint64_t parse_node_id(const std::string& s) {
        // 从字符串末尾提取数字后缀: "node-1" -> 1, "server-42" -> 42
        auto pos = s.find_last_of('-');
        if (pos == std::string::npos) return 0;
        uint64_t id = static_cast<uint64_t>(std::atoll(s.c_str() + pos + 1));
        return id & ((1ULL << NODE_BITS) - 1);  // 限制在 10 bit 内
    }
};
