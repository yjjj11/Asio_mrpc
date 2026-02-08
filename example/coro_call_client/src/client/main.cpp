#include <logger.hpp>
#include <mrpc/client.hpp>
#include <mrpc/coroutine.hpp>

#include <iostream>
#include <thread>
#include <chrono>
using namespace std::chrono_literals;
using namespace mrpc;

task<uint32_t> test_coro1() {
    auto starts = std::chrono::steady_clock::now();
    auto total = co_await client::get().coro_call<uint32_t>("test_mul", 4, 4);
    auto ends = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(ends - starts);
    LOG_DEBUG("\ncoroutine1 total time: {} ms\n", duration.count());

    co_return total.value(); // 16
}
task<uint32_t> test_coro2() {
    auto starts = std::chrono::steady_clock::now();
    auto total = co_await client::get().coro_call<uint32_t>("test_mul", 4, 4);
     total = co_await client::get().coro_call<uint32_t>("test_mul", 4, 4);
     total = co_await client::get().coro_call<uint32_t>("test_mul", 4, 4);    
    auto ends = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(ends - starts);
    LOG_DEBUG("\ncoroutine2 total time: {} ms\n", duration.count());

     co_return total.value(); // 16
}
int main() {
    wlog::logger::get().init("logs/" PROJECT_NAME ".log");
    LOG_TRACE("main thread: {}", std::this_thread::get_id());

    client::get().run();

    // auto conn = client::get().connect("127.0.0.1", 3333);
    // if (conn == nullptr) return 1;

    // auto starts = std::chrono::steady_clock::now();
    // test_coro1(conn);
    // test_coro1(conn);
    // test_coro1(conn);
    // auto ends = std::chrono::steady_clock::now();
    // auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(ends - starts);
    // LOG_DEBUG("\ncoroutine total time: {} ms\n", duration.count());


    auto starts = std::chrono::steady_clock::now();
    test_coro2();
    auto total = client::get().call<uint32_t>("test_mul", 4, 4);
    total = client::get().call<uint32_t>("test_mul", 4, 4);
    total = client::get().call<uint32_t>("test_mul", 4, 4);
    auto ends = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(ends - starts);
    LOG_DEBUG("\ncoroutine total time: {} ms\n", duration.count());

    client::get().wait_shutdown();
	wlog::logger::get().shutdown();
    return 0;
}