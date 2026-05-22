#ifndef MRPC_CLIENT_HPP
#define MRPC_CLIENT_HPP

#pragma once

#include <asio.hpp>
#include "router.hpp"
#include "connection.hpp"
namespace mrpc {
using namespace asio::ip;
class connection;

/**
 *  client for global
 */
class client final : private asio::noncopyable {
  public:
    /**
     * singleton for client
     */
    static client& get() {
        static client obj;
        return obj;
    }

    /**
     * export router object
     */
    mrpc::router& router() {
        return router_;
    }

    std::shared_ptr<connection> connect(const std::string& host, uint16_t port,
                                        std::time_t timeout = connection::DEFAULT_TIMEOUT) {
        std::string key = host + ":" + std::to_string(port);

        // 复用已有连接
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = connection_cache_.find(key);
            if (it != connection_cache_.end()) {
                if (it->second->has_connected()) {
                    LOG_DEBUG("复用连接: {}", key);
                    return it->second;
                }
                LOG_WARN("缓存连接已失效，清理: {}", key);
                connection_cache_.erase(it);
            }
        }

        auto conn = std::make_shared<connection>(asio::ip::tcp::socket(get_iocontext()), router_);
        if (conn == nullptr) return nullptr;
        if (!conn->connect(host, port, timeout)) return nullptr;

        LOG_DEBUG("新建连接: {}", key);
        conn->start_heartbeat(10); // 客户端心跳保活
        // 缓存新连接
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            connection_cache_[key] = conn;
        }
        return conn;
    }

    std::shared_ptr<connection> async_connect(const std::string& host, uint16_t port) {
        auto conn = std::make_shared<connection>(asio::ip::tcp::socket(get_iocontext()), router_);
        if (conn == nullptr) return nullptr;
        conn->async_connect(host, port);
        return conn;
    }

    /**
     *  use one thread per iocontex, and auto runing
     *
     * @param io_count io_context pool size, default is double cpu count
     * @param thread_per_io thread count per io_context
     */
    void run(std::size_t io_count = 0,
             std::size_t thread_per_io = 1) {
        if (is_running_) return; // prevent call repeated.
        if (io_count < 1) {
            io_count = std::thread::hardware_concurrency() * 2;
        }
		iocs_.clear();
        //创建线程池
		for (std::size_t i = 0; i < io_count; ++i) {
			auto ioc = std::make_shared<asio::io_context>();
			iocs_.push_back(ioc);
            // assign a work, or io will stop
            workds_.emplace_back(std::make_shared<asio::io_context::work>(*ioc));
            for (std::size_t i = 0; i < thread_per_io; ++i) {
                thread_pool_.emplace_back([ioc]() {
                    ioc->run();
                });
            }
        }
        is_running_ = true;
        LOG_INFO("client runing ...");
    }

        /**
     *  shutdown all services and threads
     */
    void shutdown() {
        // 先释放所有缓存连接（停止心跳，断开 shared_ptr 循环依赖）
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            for (auto& [key, conn] : connection_cache_) {
                conn->stop_heartbeat();
            }
            connection_cache_.clear();
        }
        for (auto& ioc : iocs_) {
            ioc->stop();
        }
    }

    /**
     * wait all server and thread stoped
     */
    void wait_shutdown() {
        for (auto& thread : thread_pool_) {
            thread.join();
        }
    }

  private:
    client() {}
    ~client() {
        shutdown();
    }

    asio::io_context& get_iocontext() {
        // round-robin
        if (iocs_.size() < 2) {
            return *(iocs_.at(0));
        }
        ++next_ioc_index_;
        if (next_ioc_index_ >= iocs_.size()) {
            next_ioc_index_ = 1; // the first io_context only for accept
        }
        auto& ioc = iocs_[next_ioc_index_];
        return *ioc;
    }

  private:
	mrpc::router router_;
    std::atomic_bool is_running_ = false;                   // check is running, prevent multiple call run functions
    std::atomic_uint64_t next_ioc_index_ = 0;               // use atomic ensure thread safe
    std::vector<std::shared_ptr<asio::io_context>> iocs_;   // io pool
    std::vector<std::thread> thread_pool_;                  // thread pool
    std::vector<std::shared_ptr<asio::io_context::work>> workds_;

    // 连接缓存: "ip:port" -> connection
    std::unordered_map<std::string, std::shared_ptr<connection>> connection_cache_;
    std::mutex cache_mutex_;
};
} // namespace mrpc

#endif // MRPC_CLIENT_HPP

