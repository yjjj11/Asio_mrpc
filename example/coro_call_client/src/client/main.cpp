#include <logger.hpp>
#include <mrpc/client.hpp>
#include <mrpc/coroutine.hpp>

#include <iostream>
#include <thread>
#include <chrono>
using namespace std::chrono_literals;
using namespace mrpc;

task<uint32_t> test_coro1(connection::cptr conn) {
    auto ret =  conn->coro_call<int>("test_add", 1, 1);  // 1+1
    auto ret1 =  conn->coro_call<int>("test_add", 2, 2); // 2+2
    auto ret2 =  conn->coro_call<int>("test_add", 4, 4); // 4+4
    double total=0;

    total += (co_await ret).value();
    total += (co_await ret1).value();
    total += (co_await ret2).value();

    LOG_DEBUG("coroutine final return: {}",total);
    co_return total; // 16
}
int main() {
    wlog::logger::get().init("logs/" PROJECT_NAME ".log");
    LOG_TRACE("main thread: {}", std::this_thread::get_id());

    client::get().run();

    auto conn = client::get().connect("127.0.0.1", 3333);
    if (conn == nullptr) return 1;

    test_coro1(conn);

    client::get().wait_shutdown();
	wlog::logger::get().shutdown();
    return 0;
}