#include <logger.hpp>
#include <mrpc/server.hpp>
using namespace mrpc;
void exception_callback(connection::cptr conn, int error,
         msg_id_t id, const std::string& buffer){
             if (error == mrpc::not_implemented) {
                conn->async_call([](uint32_t err_code, const std::string& err_msg, const nlohmann::json& ret){
                    LOG_DEBUG("query_funcname response: {}", ret.dump());
                }, "query_funcname", id.msg_id);
    } 
}
int test_mul(connection::cptr conn,int i, int j) {
    LOG_DEBUG("recv test mul: {} * {}", i, j);
    return i * j;
}
int test_add(connection::cptr conn,int i, int j) {
    LOG_DEBUG("recv test add: {} + {}", i, j);
    return i + j;
}
auto query_name(connection::cptr conn, uint64_t msg_id) {
        auto funcname = conn->router().query_msg_name(msg_id);
        LOG_DEBUG("remote query message name: {}-{}", funcname, msg_id);
        return std::make_tuple(msg_id, funcname);
}
class Echo{
    public:
        int echo(connection::cptr conn, int i) {
            LOG_DEBUG("recv echo: {}", i);
            return i;
        }
};
int main() {
    wlog::logger::get().init("logs/" PROJECT_NAME ".log");
    auto& server = server::get();
    server.set_ip_port("127.0.0.1", 3333);
    server.set_server_name("test_server");
    server.run();
    server.register_to_Zk();

    server.reg_func("test_add", test_add);
    //非成员函数无指定工作线程
    server.reg_func("test_add", test_add);
    //双向调试函数
    server.reg_func("query_funcname", query_name);
    // 非类函数投递到工作线程执行
    server.reg_func("test_mul", test_mul , std::weak_ptr<asio::io_context>(server.get_work_context()));
    //注册类函数
    server.reg_func("echo", &Echo::echo, std::make_shared<Echo>());

    //设置异常处理（找不到的情况）
    server.router().set_exception_callback(exception_callback);
    
    server.accept();//启动监听上下文

    server.wait_shutdown();
    wlog::logger::get().shutdown();
    return 0;
}