#include <logger.hpp>
#include <mrpc/client.hpp>
#include <mrpc/coroutine.hpp>

#include <iostream>
#include <thread>
#include <chrono>
using namespace std::chrono_literals;
using namespace mrpc;

// CPU 密集型计算：斐波那契数列，模拟异步等待期间做其他工作
uint64_t fibonacci(int n) {
    if (n <= 1) return n;
    uint64_t a = 0, b = 1;
    for (int i = 2; i <= n; ++i) {
        uint64_t c = a + b;
        a = b;
        b = c;
    }
    return b;
}

task<void> test_single_coro_call() {
    // 1. 记录开始时间
    auto t1 = std::chrono::steady_clock::now();

    // 2. 建立连接 + 发起 coro_call（发送请求后立即返回 task_awaitable）
    //    服务端 test_mul 会 sleep(1s) 再返回结果
    auto conn = client::get().connect("127.0.0.1", 3333);
    if (!conn) {
        LOG_ERROR("连接失败!");
        co_return;
    }
    auto awaitable = conn->coro_call<uint32_t>("test_mul", 4, 4);

    auto t2 = std::chrono::steady_clock::now();

    // 3. 在 co_await 之前做 CPU 密集计算（模拟等待 RPC 期间处理其他任务）
    uint64_t fib = fibonacci(45);
    auto t3 = std::chrono::steady_clock::now();

    // 4. 等待 RPC 结果（协程挂起，不阻塞线程）
    auto result = co_await awaitable;
    auto t4 = std::chrono::steady_clock::now();

    // 5. 打印耗时分析
    auto call_setup = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    auto cpu_work   = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
    auto await_time = std::chrono::duration_cast<std::chrono::milliseconds>(t4 - t3).count();
    auto total      = std::chrono::duration_cast<std::chrono::milliseconds>(t4 - t1).count();

    LOG_INFO("========== 单次协程调用演示 ==========");
    LOG_INFO("fibonacci(45) = {}", fib);
    LOG_INFO("test_mul(4,4) = {}, err_code: {}", result.value(), result.error_code());
    LOG_INFO("调用建立时间: {} ms", call_setup);
    LOG_INFO("CPU 计算时间: {} ms", cpu_work);
    LOG_INFO("协程等待时间: {} ms", await_time);
    LOG_INFO("总耗时:       {} ms", total);
    LOG_INFO("-------------------------------------");
    LOG_INFO("同步模型估算: {}ms(计算) + 1000ms(RPC) = {}ms", cpu_work, cpu_work + 1000);
    LOG_INFO("协程模型实际: max({}ms, 1000ms) = {}ms", cpu_work, std::max(cpu_work, 1000L));
    LOG_INFO("结论: 协程让计算和 RPC 等待并行，节省了约 {}ms!", std::min(cpu_work, 1000L));
    LOG_INFO("=====================================");

    // 6. 通知主线程退出
    client::get().shutdown();
}

int main() {
    wlog::logger::get().init("logs/" PROJECT_NAME ".log");

    client::get().run();

    // 启动协程（在 main 线程执行到第一个 co_await 后挂起）
    // 必须持有 task 对象，否则析构时会销毁还在挂起的协程帧
    auto coro_task = test_single_coro_call();

    // 等待 shutdown 通知后退出
    client::get().wait_shutdown();
    wlog::logger::get().shutdown();
    return 0;
}
