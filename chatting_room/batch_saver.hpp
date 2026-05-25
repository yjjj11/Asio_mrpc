#pragma once

#include "mysql_save.hpp"
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>

/// 消息批量写入器：攒批后异步 flush 到 MySQL
class MessageBatchSaver {
    MySqlSaver* db_ = nullptr;
    std::vector<Message> batch_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::thread flush_thread_;
    bool stop_ = false;

    static constexpr size_t kBatchSize = 50;
    static constexpr auto kFlushInterval = std::chrono::milliseconds(100);

    void flush_loop() {
        while (true) {
            std::vector<Message> msgs;
            {
                std::unique_lock lock(mtx_);
                cv_.wait_for(lock, kFlushInterval, [this] {
                    return stop_ || batch_.size() >= kBatchSize;
                });
                if (batch_.empty() && !stop_) continue;
                msgs.swap(batch_);
            }
            if (!msgs.empty()) {
                db_->save(msgs);
                msgs.clear();
            }
            if (stop_) break;
        }
        // Final flush
        std::vector<Message> remaining;
        {
            std::lock_guard lock(mtx_);
            remaining.swap(batch_);
        }
        if (!remaining.empty()) {
            db_->save(remaining);
        }
    }

public:
    MessageBatchSaver() = default;

    void start(MySqlSaver* db) {
        db_ = db;
        stop_ = false;
        flush_thread_ = std::thread(&MessageBatchSaver::flush_loop, this);
    }

    void stop() {
        {
            std::lock_guard lock(mtx_);
            stop_ = true;
        }
        cv_.notify_one();
        if (flush_thread_.joinable())
            flush_thread_.join();
    }

    ~MessageBatchSaver() {
        if (flush_thread_.joinable()) {
            stop();
        }
    }

    void push(Message msg) {
        std::lock_guard lock(mtx_);
        batch_.push_back(std::move(msg));
        if (batch_.size() >= kBatchSize)
            cv_.notify_one();
    }

    MessageBatchSaver(const MessageBatchSaver&) = delete;
    MessageBatchSaver& operator=(const MessageBatchSaver&) = delete;
};
