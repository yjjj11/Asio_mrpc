#include <logger.hpp>
#include <mrpc/client.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <bitset>
using namespace std::chrono_literals;
using namespace mrpc;

int main() {
    wlog::logger::get().init("logs/" PROJECT_NAME ".log");
    auto& client = client::get();
    client.run();
        
    auto ret = client.call<uint32_t>("test_add", 11, 12);
    if (ret.error_code() == mrpc::ok) {
        std::cout << "return: " << ret.value() << std::endl;
    } else {
        std::cout << "return error: " << ret.error_msg() << std::endl;
    }
    
    client.call<void>("other_echo", "hello other_server");
   
    client.wait_shutdown();
    wlog::logger::get().shutdown();
    return 0;
}