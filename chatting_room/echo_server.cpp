#include <logger.hpp>
#include <mrpc/server.hpp>
#include <iostream>

using namespace mrpc;

int main() {
    wlog::logger::get().init("logs/echo_server.log");

    auto& svr = server::get();
    svr.set_ip_port("0.0.0.0", 7779);
    svr.run();

    svr.reg_func("echo", [](const std::string& msg) -> std::string {
        return msg;
    });

    if (!svr.accept()) {
        std::cerr << "Echo server failed to bind port 7779" << std::endl;
        return 1;
    }
    LOG_INFO("Echo server running on port 7779");
    std::cout << "Echo server running on port 7779" << std::endl;

    svr.wait_shutdown();
    return 0;
}
