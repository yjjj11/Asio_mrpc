#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <type_traits>

/// 通用线程池：接受任意可调用对象，异步执行
class TaskPool {
public:
    explicit TaskPool(size_t thread_count = 1)
        : stop_(false)
    {
        for (size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~TaskPool() {
        stop();
    }

    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;

    /// 投递任务，返回 std::future 获取返回值
    /// 用法: auto fut = pool.enqueue(func, arg1, arg2);
    template<typename Func, typename... Args>
    auto enqueue(Func&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<Func>, std::decay_t<Args>...>>
    {
        using return_type = std::invoke_result_t<std::decay_t<Func>, std::decay_t<Args>...>;
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<Func>(f), std::forward<Args>(args)...)
        );
        auto future = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mtx_);
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return future;
    }

    /// 投递无返回值任务（void 函数），不返回 future
    /// 用法: pool.post([] { do_something(); });
    template<typename Func, typename... Args>
    void post(Func&& f, Args&&... args) {
        auto bound = std::bind(std::forward<Func>(f), std::forward<Args>(args)...);
        {
            std::lock_guard<std::mutex> lock(mtx_);
            tasks_.emplace(std::move(bound));
        }
        cv_.notify_one();
    }

    /// 停止所有线程，等待已投递任务完成
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    size_t worker_count() const { return workers_.size(); }

private:
    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_;
};
