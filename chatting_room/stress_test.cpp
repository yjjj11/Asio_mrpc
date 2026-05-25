#include <logger.hpp>
#include <mrpc/client.hpp>
#include <mrpc/connection.hpp>
#include <spdlog/spdlog.h>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <asio.hpp>

using namespace mrpc;
namespace chrono = std::chrono;
using Clock = chrono::steady_clock;

// ==================== Per-Thread Stats ====================

struct ThreadStats {
    uint64_t ok = 0;
    uint64_t fail = 0;
    uint64_t total_us = 0;
    uint64_t count = 0;
    uint64_t buckets[9] = {0};

    void record(Clock::time_point start, Clock::time_point end) {
        auto us = chrono::duration_cast<chrono::microseconds>(end - start).count();
        total_us += us;
        ++count;
        ++ok;
        if (us < 500) buckets[0]++;
        else if (us < 1000) buckets[1]++;
        else if (us < 2000) buckets[2]++;
        else if (us < 5000) buckets[3]++;
        else if (us < 10000) buckets[4]++;
        else if (us < 20000) buckets[5]++;
        else if (us < 50000) buckets[6]++;
        else if (us < 100000) buckets[7]++;
        else buckets[8]++;
    }
};

struct AggregatedStats {
    uint64_t ok = 0, fail = 0, count = 0, total_us = 0;
    double avg_ms = 0;
    uint64_t buckets[9] = {0};
    double p50_ms = 0, p90_ms = 0, p99_ms = 0;
    static constexpr const char* BUCKET_NAMES[9] = {
        "<0.5ms", "0.5-1ms", "1-2ms", "2-5ms", "5-10ms",
        "10-20ms", "20-50ms", "50-100ms", ">100ms"
    };
    static constexpr uint64_t BUCKET_MAX_US[9] = {
        500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000
    };

    void merge_from(const std::vector<ThreadStats>& all_stats) {
        for (auto& s : all_stats) {
            ok += s.ok;
            fail += s.fail;
            count += s.count;
            total_us += s.total_us;
            for (int i = 0; i < 9; i++) buckets[i] += s.buckets[i];
        }
        if (count > 0) avg_ms = (double)total_us / count / 1000.0;
        uint64_t cum = 0;
        for (int i = 0; i < 9; i++) {
            cum += buckets[i];
            double pct = 100.0 * cum / count;
            if (p50_ms == 0 && pct >= 50.0) p50_ms = BUCKET_MAX_US[i] / 1000.0;
            if (p90_ms == 0 && pct >= 90.0) p90_ms = BUCKET_MAX_US[i] / 1000.0;
            if (p99_ms == 0 && pct >= 99.0) p99_ms = BUCKET_MAX_US[i] / 1000.0;
        }
        if (p50_ms == 0) p50_ms = avg_ms;
        if (p90_ms == 0) p90_ms = avg_ms;
        if (p99_ms == 0) p99_ms = avg_ms;
    }

    void print(const std::string& prefix) const {
        if (count == 0) { std::cout << prefix << "(no data)\n"; return; }
        std::cout << prefix << "Avg: " << std::fixed << std::setprecision(2) << avg_ms << " ms\n";
        uint64_t cum = 0;
        for (int i = 0; i < 9; i++) {
            cum += buckets[i];
            double pct = 100.0 * cum / count;
            std::cout << prefix << "  " << BUCKET_NAMES[i] << ": " << buckets[i]
                      << " (" << std::fixed << std::setprecision(1) << pct << "%)\n";
        }
        std::cout << prefix << "P50: " << p50_ms << " ms, P90: " << p90_ms
                  << " ms, P99: " << p99_ms << " ms\n";
    }
};

// ==================== Helpers ====================

struct Args {
    std::string test;
    int conns = 50;
    int payload = 1024;
    int duration = 15;
    int users = 500;
    int pairs = 50;
    uint16_t port = 8888;
    uint16_t echo_port = 7779;
    int pool = 1;
    uint16_t cross_send_port = 8881;
    uint16_t cross_recv_port = 8885;
};

static Args parse_args(int argc, char* argv[]) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto eq = arg.find('=');
        if (eq == std::string::npos) {
            a.test = arg;
            continue;
        }
        auto key = arg.substr(0, eq);
        auto val = arg.substr(eq + 1);
        if (key == "--test") a.test = val;
        else if (key == "--conns") a.conns = std::stoi(val);
        else if (key == "--payload") a.payload = std::stoi(val);
        else if (key == "--duration") a.duration = std::stoi(val);
        else if (key == "--users") a.users = std::stoi(val);
        else if (key == "--pairs") a.pairs = std::stoi(val);
        else if (key == "--port") a.port = static_cast<uint16_t>(std::stoi(val));
        else if (key == "--echo-port") a.echo_port = static_cast<uint16_t>(std::stoi(val));
        else if (key == "--pool") a.pool = std::stoi(val);
        else if (key == "--cross-send-port") a.cross_send_port = static_cast<uint16_t>(std::stoi(val));
        else if (key == "--cross-recv-port") a.cross_recv_port = static_cast<uint16_t>(std::stoi(val));
    }
    return a;
}

/// 创建短连接（用于注册等一次性操作），返回前已关闭
static bool do_admin_call(const std::string& host, uint16_t port,
                          const std::function<void(std::shared_ptr<connection>)>& fn) {
    asio::io_context ioc;
    auto work = std::make_shared<asio::io_context::work>(ioc);
    std::thread ioc_thr([&ioc] { ioc.run(); });

    asio::ip::tcp::socket sock(ioc);
    std::error_code ec;
    sock.open(asio::ip::tcp::v4(), ec);
    if (ec) { ioc.stop(); ioc_thr.join(); return false; }

    auto conn = std::make_shared<connection>(std::move(sock), mrpc::client::get().router());
    if (!conn->connect(host, port, 3000)) {
        ioc.stop(); ioc_thr.join(); return false;
    }
    conn->start_heartbeat(10);
    fn(std::move(conn));
    // conn shared_ptr goes out of scope here, connection destroyed
    ioc.stop();
    ioc_thr.join();
    return true;
}

// ==================== Test 1: Echo QPS ====================

static void test_echo_qps(const Args& args) {
    std::cout << "\n========== Echo QPS Test ==========\n";
    std::cout << "Connections: " << args.conns
              << ", Payload: " << args.payload << " B"
              << ", Duration: " << args.duration << " s\n\n";

    mrpc::client::get().run();
    auto& router = mrpc::client::get().router();

    std::atomic<bool> running{true};
    std::string payload(args.payload, 'A');
    std::vector<ThreadStats> all_stats(args.conns);
    std::vector<std::thread> workers;

    for (int i = 0; i < args.conns; i++) {
        workers.emplace_back([&router, &payload, &running, &all_stats, &args, i]() {
            // 每个线程创建独立 io_context + socket
            asio::io_context ioc;
            auto work = std::make_shared<asio::io_context::work>(ioc);
            std::thread ioc_thr([&ioc] { ioc.run(); });

            asio::ip::tcp::socket sock(ioc);
            std::error_code ec;
            sock.open(asio::ip::tcp::v4(), ec);
            if (ec) { all_stats[i].fail++; ioc.stop(); ioc_thr.join(); return; }

            auto conn = std::make_shared<connection>(std::move(sock), router);
            if (!conn->connect("127.0.0.1", args.echo_port, 3000)) {
                all_stats[i].fail++;
                ioc.stop(); ioc_thr.join(); return;
            }
            conn->start_heartbeat(10);

            auto& stat = all_stats[i];
            while (running.load(std::memory_order_acquire)) {
                auto start = Clock::now();
                auto ret = conn->call<std::string>("echo", payload);
                auto end = Clock::now();
                if (ret.error_code() == 200 && ret.value() == payload) {
                    stat.record(start, end);
                } else {
                    stat.fail++;
                }
            }

            // Cleanup this thread's resources
            conn->stop_heartbeat();
            ioc.stop();
            ioc_thr.join();
            conn->close();
        });
    }

    std::this_thread::sleep_for(chrono::seconds(args.duration));
    running.store(false, std::memory_order_release);
    for (auto& w : workers) if (w.joinable()) w.join();

    AggregatedStats agg;
    agg.merge_from(all_stats);
    double qps = agg.count / (double)args.duration;
    std::cout << "Results: " << agg.count << " requests in " << args.duration << " s\n";
    std::cout << "QPS: " << std::fixed << std::setprecision(0) << qps << "\n";
    std::cout << "OK: " << agg.ok << ", Fail: " << agg.fail << "\n";
    agg.print("Latency: ");
}

// ==================== Test 1b: Echo QPS using client::connect (cached, shared) ====================

static void test_echo_qps_shared(const Args& args) {
    std::cout << "\n========== Echo QPS Test (Shared Connection) ==========\n";
    std::cout << "Threads: " << args.conns
              << ", Payload: " << args.payload << " B"
              << ", Duration: " << args.duration << " s\n\n";

    auto& client = mrpc::client::get();
    client.run();
    auto& router = client.router();

    // 建立一条共享连接
    asio::io_context shared_ioc;
    auto shared_work = std::make_shared<asio::io_context::work>(shared_ioc);
    std::thread shared_ioc_thr([&shared_ioc] { shared_ioc.run(); });
    asio::ip::tcp::socket shared_sock(shared_ioc);
    std::error_code ec;
    shared_sock.open(asio::ip::tcp::v4(), ec);
    if (ec) { shared_ioc.stop(); shared_ioc_thr.join(); return; }
    auto conn = std::make_shared<connection>(std::move(shared_sock), router);
    if (!conn->connect("127.0.0.1", args.echo_port, 3000)) {
        shared_ioc.stop(); shared_ioc_thr.join(); return;
    }
    conn->start_heartbeat(10);

    std::atomic<bool> running{true};
    std::string payload(args.payload, 'A');
    std::vector<ThreadStats> all_stats(args.conns);
    std::vector<std::thread> workers;

    for (int i = 0; i < args.conns; i++) {
        workers.emplace_back([conn, &payload, &running, &all_stats, i]() {
            auto& stat = all_stats[i];
            while (running.load(std::memory_order_acquire)) {
                auto start = Clock::now();
                auto ret = conn->call<std::string>("echo", payload);
                auto end = Clock::now();
                if (ret.error_code() == 200 && ret.value() == payload) {
                    stat.record(start, end);
                } else {
                    stat.fail++;
                }
            }
        });
    }

    std::this_thread::sleep_for(chrono::seconds(args.duration));
    running.store(false, std::memory_order_release);
    for (auto& w : workers) if (w.joinable()) w.join();

    conn->stop_heartbeat();
    conn->close();
    shared_ioc.stop();
    shared_ioc_thr.join();

    AggregatedStats agg;
    agg.merge_from(all_stats);
    double qps = agg.count / (double)args.duration;
    std::cout << "Results: " << agg.count << " requests in " << args.duration << " s\n";
    std::cout << "QPS: " << std::fixed << std::setprecision(0) << qps << "\n";
    std::cout << "OK: " << agg.ok << ", Fail: " << agg.fail << "\n";
    agg.print("Latency: ");
}

// ==================== Test 2: Max Online Users ====================

static void test_max_online(const Args& args) {
    std::cout << "\n========== Max Online Users Test ==========\n";
    std::cout << "Target users: " << args.users << "\n\n";

    // Phase 1: Register all users
    std::cout << "Phase 1: Registering " << args.users << " users... ";
    std::cout.flush();
    do_admin_call("127.0.0.1", args.port, [&](std::shared_ptr<connection> conn) {
        for (int i = 0; i < args.users; i++) {
            conn->call<bool>("register_user", "stress_u" + std::to_string(i), "pass");
        }
    });
    std::cout << "done\n";

    // Phase 2: Connect & login
    std::cout << "Phase 2: Connecting & logging in...\n";
    std::atomic<int> login_ok{0};
    std::atomic<int> login_fail{0};
    std::atomic<bool> running{true};
    std::vector<std::thread> workers;
    // We use a blocking barrier: each worker holds its connection on the stack
    // and the worker's io_context lives for the whole duration.
    // hold_conns[i] = conn is done inside the lambda — but we release it BEFORE
    // destroying the io_context to avoid socket outliving io_context.
    using Barrier = std::shared_ptr<std::promise<void>>;
    std::vector<Barrier> barriers(args.users);
    std::vector<std::shared_ptr<connection>> hold_conns(args.users);

    auto& router = mrpc::client::get().router();
    auto start_time = Clock::now();
    for (int i = 0; i < args.users; i++) {
        barriers[i] = std::make_shared<std::promise<void>>();
        workers.emplace_back([&router, &login_ok, &login_fail, &hold_conns, &running, &args, i, barrier = barriers[i]]() {
            auto name = "stress_u" + std::to_string(i);

            asio::io_context ioc;
            auto work = std::make_shared<asio::io_context::work>(ioc);
            std::thread ioc_thr([&ioc] { ioc.run(); });

            asio::ip::tcp::socket sock(ioc);
            std::error_code ec;
            sock.open(asio::ip::tcp::v4(), ec);
            if (ec) { login_fail.fetch_add(1); ioc.stop(); ioc_thr.join(); barrier->set_value(); return; }

            auto conn = std::make_shared<connection>(std::move(sock), router);
            if (!conn->connect("127.0.0.1", args.port, 5000)) {
                login_fail.fetch_add(1);
                ioc.stop(); ioc_thr.join(); barrier->set_value(); return;
            }
            conn->start_heartbeat(10);

            auto ret = conn->call<std::string>("user_login", name, "pass");
            if (ret.error_code() == 200 && !ret.value().empty()) {
                login_ok.fetch_add(1);
                hold_conns[i] = conn;
            } else {
                login_fail.fetch_add(1);
            }
            barrier->set_value();

            while (running.load(std::memory_order_acquire))
                std::this_thread::sleep_for(chrono::seconds(1));

            // CRITICAL: Release the shared_ptr BEFORE destroying the io_context.
            // The socket's service references the io_context's internal reactor — if
            // the io_context dies first, socket destruction = use-after-free.
            hold_conns[i].reset();
            conn->stop_heartbeat();
            conn->close();
            ioc.stop();
            ioc_thr.join();
        });
    }

    // Wait for all to connect
    for (auto& b : barriers) b->get_future().wait();
    auto elapsed = chrono::duration_cast<chrono::duration<double>>(Clock::now() - start_time).count();

    int hold = std::min(10, std::max(3, args.duration));
    std::cout << login_ok.load() << "/" << args.users << " logged in (" << elapsed << " s)\n";
    std::cout << "Holding for " << hold << " s to verify stability... ";
    std::cout.flush();
    std::this_thread::sleep_for(chrono::seconds(hold));
    std::cout << "done\n";

    running.store(false, std::memory_order_release);
    for (auto& w : workers) if (w.joinable()) w.join();

    std::cout << "\nResults:\n";
    std::cout << "  Target: " << args.users << " users\n";
    std::cout << "  Logged in: " << login_ok.load() << "\n";
    std::cout << "  Failed: " << login_fail.load() << "\n";
    std::cout << "  Connection rate: " << std::fixed << std::setprecision(0)
              << login_ok.load() / elapsed << " conn/s\n";
}

// ==================== Test 3: Message Throughput ====================

static void test_message_throughput(const Args& args) {
    std::cout << "\n========== Message Throughput Test ==========\n";
    std::cout << "Pairs: " << args.pairs
              << ", Payload: " << args.payload << " B"
              << ", Duration: " << args.duration << " s\n\n";

    auto& router = mrpc::client::get().router();

    int total_users = args.pairs * 2;
    std::cout << "Phase 1: Registering " << total_users << " users... ";
    std::cout.flush();
    do_admin_call("127.0.0.1", args.port, [&](std::shared_ptr<connection> conn) {
        for (int i = 0; i < total_users; i++) {
            conn->call<bool>("register_user", "msg_u" + std::to_string(i), "pass");
        }
    });
    std::cout << "done\n";

    std::atomic<bool> running{true};
    std::vector<ThreadStats> all_stats(args.pairs);
    std::vector<std::thread> workers;
    std::string message(args.payload, 'X');

    auto start_time = Clock::now();
    for (int p = 0; p < args.pairs; p++) {
        workers.emplace_back([&router, &running, &all_stats, &message, &args, p]() {
            auto from = "msg_u" + std::to_string(p * 2);
            auto to   = "msg_u" + std::to_string(p * 2 + 1);

            asio::io_context ioc;
            auto work = std::make_shared<asio::io_context::work>(ioc);
            std::thread ioc_thr([&ioc] { ioc.run(); });

            // 创建连接池，每条连接分别登录，Nginx least_conn 分配到不同后端
            int pool = std::max(1, args.pool);
            std::vector<std::shared_ptr<connection>> conns;
            conns.reserve(pool);
            for (int c = 0; c < pool; c++) {
                asio::ip::tcp::socket sock(ioc);
                std::error_code ec;
                sock.open(asio::ip::tcp::v4(), ec);
                if (ec) { all_stats[p].fail++; ioc.stop(); ioc_thr.join(); return; }

                auto conn = std::make_shared<connection>(std::move(sock), router);
                if (!conn->connect("127.0.0.1", args.port, 5000)) {
                    all_stats[p].fail++; ioc.stop(); ioc_thr.join(); return;
                }
                conn->start_heartbeat(10);

                auto login = conn->call<std::string>("user_login", from, "pass");
                if (login.error_code() != 200 || login.value().empty()) {
                    all_stats[p].fail++; ioc.stop(); ioc_thr.join(); return;
                }
                conns.push_back(std::move(conn));
            }

            auto& stat = all_stats[p];
            uint64_t msg_count = 0;
            while (running.load(std::memory_order_acquire)) {
                auto& conn = conns[msg_count % pool];
                auto start = Clock::now();
                auto ret = conn->call<uint64_t>("send_message", from, to, message);
                auto end = Clock::now();
                if (ret.error_code() == 200 && ret.value() > 0) {
                    stat.record(start, end);
                } else {
                    stat.fail++;
                }
                ++msg_count;
            }

            for (auto& c : conns) {
                c->stop_heartbeat();
                c->close();
            }
            ioc.stop();
            ioc_thr.join();
        });
    }

    std::this_thread::sleep_for(chrono::seconds(args.duration));
    running.store(false, std::memory_order_release);
    for (auto& w : workers) if (w.joinable()) w.join();
    auto elapsed = chrono::duration_cast<chrono::duration<double>>(Clock::now() - start_time).count();

    AggregatedStats agg;
    agg.merge_from(all_stats);
    double qps = agg.count / elapsed;
    std::cout << "\nResults: " << agg.count << " messages in " << std::fixed << std::setprecision(1)
              << elapsed << " s\n";
    std::cout << "QPS: " << std::fixed << std::setprecision(0) << qps << "\n";
    std::cout << "OK: " << agg.ok << ", Fail: " << agg.fail << "\n";
    agg.print("Latency: ");
}

// ==================== Test 3b: Message Throughput (Cross-Node) ====================

static void test_message_throughput_crossnode(const Args& args) {
    std::cout << "\n========== Message Throughput Test (Cross-Node) ==========\n";
    std::cout << "Pairs: " << args.pairs
              << ", Payload: " << args.payload << " B"
              << ", Duration: " << args.duration << " s\n";
    std::cout << "Architecture: sender -> node-" << (args.cross_send_port - 8880)
              << " (" << args.cross_send_port << "), receiver -> node-"
              << (args.cross_recv_port - 8880) << " (" << args.cross_recv_port
              << "), every message forwarded via cross-node RPC\n\n";

    auto& router = mrpc::client::get().router();

    int total_users = args.pairs * 2;
    std::cout << "Phase 1: Registering " << total_users << " users... ";
    std::cout.flush();
    do_admin_call("127.0.0.1", args.cross_send_port, [&](std::shared_ptr<connection> conn) {
        for (int i = 0; i < total_users; i++) {
            conn->call<bool>("register_user", "msg_u" + std::to_string(i), "pass");
        }
    });
    std::cout << "done\n";

    std::atomic<bool> running{true};
    std::vector<ThreadStats> all_stats(args.pairs);
    std::vector<std::thread> workers;
    std::string message(args.payload, 'X');

    auto start_time = Clock::now();
    for (int p = 0; p < args.pairs; p++) {
        workers.emplace_back([&router, &running, &all_stats, &message, &args, p]() {
            auto from = "msg_u" + std::to_string(p * 2);
            auto to   = "msg_u" + std::to_string(p * 2 + 1);

            asio::io_context ioc;
            auto work = std::make_shared<asio::io_context::work>(ioc);
            std::thread ioc_thr([&ioc] { ioc.run(); });

            // Connection 1: sender -> send_port (e.g. node-1)
            asio::ip::tcp::socket send_sock(ioc);
            std::error_code ec;
            send_sock.open(asio::ip::tcp::v4(), ec);
            if (ec) { all_stats[p].fail++; ioc.stop(); ioc_thr.join(); return; }
            auto send_conn = std::make_shared<connection>(std::move(send_sock), router);
            if (!send_conn->connect("127.0.0.1", args.cross_send_port, 5000)) {
                all_stats[p].fail++; ioc.stop(); ioc_thr.join(); return;
            }
            send_conn->start_heartbeat(10);
            auto login = send_conn->call<std::string>("user_login", from, "pass");
            if (login.error_code() != 200 || login.value().empty()) {
                all_stats[p].fail++; ioc.stop(); ioc_thr.join(); return;
            }

            // Connection 2: receiver -> recv_port (e.g. node-5) — keeps target online on another node
            asio::ip::tcp::socket recv_sock(ioc);
            recv_sock.open(asio::ip::tcp::v4(), ec);
            if (ec) { all_stats[p].fail++; ioc.stop(); ioc_thr.join(); return; }
            auto recv_conn = std::make_shared<connection>(std::move(recv_sock), router);
            if (!recv_conn->connect("127.0.0.1", args.cross_recv_port, 5000)) {
                all_stats[p].fail++; ioc.stop(); ioc_thr.join(); return;
            }
            recv_conn->start_heartbeat(10);
            auto recv_login = recv_conn->call<std::string>("user_login", to, "pass");
            if (recv_login.error_code() != 200 || recv_login.value().empty()) {
                all_stats[p].fail++; ioc.stop(); ioc_thr.join(); return;
            }

            // Send messages via send_conn — each triggers cross-node RPC forwarding
            auto& stat = all_stats[p];
            uint64_t msg_count = 0;
            while (running.load(std::memory_order_acquire)) {
                auto start = Clock::now();
                auto ret = send_conn->call<uint64_t>("send_message", from, to, message);
                auto end = Clock::now();
                if (ret.error_code() == 200 && ret.value() > 0) {
                    stat.record(start, end);
                } else {
                    stat.fail++;
                }
                ++msg_count;
            }

            send_conn->stop_heartbeat();
            send_conn->close();
            recv_conn->stop_heartbeat();
            recv_conn->close();
            ioc.stop();
            ioc_thr.join();
        });
    }

    std::this_thread::sleep_for(chrono::seconds(args.duration));
    running.store(false, std::memory_order_release);
    for (auto& w : workers) if (w.joinable()) w.join();
    auto elapsed = chrono::duration_cast<chrono::duration<double>>(Clock::now() - start_time).count();

    AggregatedStats agg;
    agg.merge_from(all_stats);
    double qps = agg.count / elapsed;
    std::cout << "\nResults: " << agg.count << " messages in " << std::fixed << std::setprecision(1)
              << elapsed << " s\n";
    std::cout << "QPS: " << std::fixed << std::setprecision(0) << qps << "\n";
    std::cout << "OK: " << agg.ok << ", Fail: " << agg.fail << "\n";
    agg.print("Latency: ");
}

// ==================== Test 1c: Echo QPS (Reactor Mode) ====================

static void test_echo_qps_reactor(const Args& args, bool raw = false) {
    std::cout << "\n========== Echo QPS Test (Reactor Mode" << (raw ? " + RAW" : "") << ") ==========\n";
    std::cout << "Connections: " << args.conns
              << ", Payload: " << args.payload << " B"
              << ", Duration: " << args.duration << " s\n";

    unsigned io_threads = std::thread::hardware_concurrency();
    std::cout << "Architecture: 1 shared io_context + " << io_threads
              << " threads, " << args.conns << " connections multiplexed"
              << (raw ? ", RAW format (no json)" : ", MSGPACK format") << "\n\n";

    mrpc::client::get().run();
    auto& router = mrpc::client::get().router();

    // Shared io_context + thread pool for reactor-style I/O
    asio::io_context shared_ioc;
    auto shared_work = std::make_shared<asio::io_context::work>(shared_ioc);
    std::vector<std::thread> io_threads_vec;
    for (unsigned i = 0; i < io_threads; i++) {
        io_threads_vec.emplace_back([&shared_ioc] { shared_ioc.run(); });
    }

    std::atomic<bool> running{true};
    std::string payload(args.payload, 'A');
    std::vector<ThreadStats> all_stats(args.conns);
    std::vector<std::thread> workers;

    for (int i = 0; i < args.conns; i++) {
        workers.emplace_back([&router, &shared_ioc, &payload, &running, &all_stats, &args, i, raw]() {
            // All connections bound to the shared io_context
            asio::ip::tcp::socket sock(shared_ioc);
            std::error_code ec;
            sock.open(asio::ip::tcp::v4(), ec);
            if (ec) { all_stats[i].fail++; return; }

            auto conn = std::make_shared<connection>(std::move(sock), router);
            if (!conn->connect("127.0.0.1", args.echo_port, 3000)) {
                all_stats[i].fail++; return;
            }
            conn->start_heartbeat(10);

            auto& stat = all_stats[i];
            while (running.load(std::memory_order_acquire)) {
                auto start = Clock::now();
                auto ret = raw
                    ? conn->call<std::string, MSG_FMT_RAW>("echo", payload)
                    : conn->call<std::string>("echo", payload);
                auto end = Clock::now();
                if (ret.error_code() == 200 && ret.value() == payload) {
                    stat.record(start, end);
                } else {
                    stat.fail++;
                }
            }

            conn->stop_heartbeat();
            conn->close();
        });
    }

    std::this_thread::sleep_for(chrono::seconds(args.duration));
    running.store(false, std::memory_order_release);
    for (auto& w : workers) if (w.joinable()) w.join();

    shared_work.reset();
    shared_ioc.stop();
    for (auto& t : io_threads_vec) if (t.joinable()) t.join();

    AggregatedStats agg;
    agg.merge_from(all_stats);
    double qps = agg.count / (double)args.duration;
    std::cout << "Results: " << agg.count << " requests in " << args.duration << " s\n";
    std::cout << "QPS: " << std::fixed << std::setprecision(0) << qps << "\n";
    std::cout << "OK: " << agg.ok << ", Fail: " << agg.fail << "\n";
    agg.print("Latency: ");
}

// ==================== Test 1d: Echo QPS (RAW format, no nlohmann) ====================

static void test_echo_qps_raw(const Args& args) {
    std::cout << "\n========== Echo QPS Test (RAW Format) ==========\n";
    std::cout << "Connections: " << args.conns
              << ", Payload: " << args.payload << " B"
              << ", Duration: " << args.duration << " s\n";
    std::cout << "Wire format: raw bytes, no nlohmann/json serialization\n\n";

    mrpc::client::get().run();
    auto& router = mrpc::client::get().router();

    std::atomic<bool> running{true};
    std::string payload(args.payload, 'A');
    std::vector<ThreadStats> all_stats(args.conns);
    std::vector<std::thread> workers;

    for (int i = 0; i < args.conns; i++) {
        workers.emplace_back([&router, &payload, &running, &all_stats, &args, i]() {
            asio::io_context ioc;
            auto work = std::make_shared<asio::io_context::work>(ioc);
            std::thread ioc_thr([&ioc] { ioc.run(); });

            asio::ip::tcp::socket sock(ioc);
            std::error_code ec;
            sock.open(asio::ip::tcp::v4(), ec);
            if (ec) { all_stats[i].fail++; ioc.stop(); ioc_thr.join(); return; }

            auto conn = std::make_shared<connection>(std::move(sock), router);
            if (!conn->connect("127.0.0.1", args.echo_port, 3000)) {
                all_stats[i].fail++;
                ioc.stop(); ioc_thr.join(); return;
            }
            conn->start_heartbeat(10);

            auto& stat = all_stats[i];
            while (running.load(std::memory_order_acquire)) {
                auto start = Clock::now();
                // Use MSG_FMT_RAW to bypass nlohmann/json serialization
                auto ret = conn->call<std::string, MSG_FMT_RAW>("echo", payload);
                auto end = Clock::now();
                if (ret.error_code() == 200 && ret.value() == payload) {
                    stat.record(start, end);
                } else {
                    stat.fail++;
                }
            }

            conn->stop_heartbeat();
            ioc.stop();
            ioc_thr.join();
            conn->close();
        });
    }

    std::this_thread::sleep_for(chrono::seconds(args.duration));
    running.store(false, std::memory_order_release);
    for (auto& w : workers) if (w.joinable()) w.join();

    AggregatedStats agg;
    agg.merge_from(all_stats);
    double qps = agg.count / (double)args.duration;
    std::cout << "Results: " << agg.count << " requests in " << args.duration << " s\n";
    std::cout << "QPS: " << std::fixed << std::setprecision(0) << qps << "\n";
    std::cout << "OK: " << agg.ok << ", Fail: " << agg.fail << "\n";
    agg.print("Latency: ");
}

// ==================== Main ====================

int main(int argc, char* argv[]) {
    wlog::logger::get().init("logs/stress_test.log");
    spdlog::set_level(spdlog::level::err);  // 压测只输出 error 级别日志

    // 注册哑 handler 避免服务端推送通知时报错
    auto& rtr = mrpc::client::get().router();
    rtr.reg_handle("on_user_status_changed",
        [](const std::shared_ptr<connection>&, const std::string&, bool) { return true; });
    rtr.reg_handle("on_message",
        [](const std::shared_ptr<connection>&, const std::string&, const std::string&, uint64_t, const std::string&) { return true; });

    auto args = parse_args(argc, argv);
    if (args.test.empty()) {
        std::cerr << "Usage: " << argv[0] << " --test=<echo|echo_shared|echo_reactor|echo_reactor_raw|echo_raw|online|message|message_cross> [options]\n";
        std::cerr << "  echo:            --conns=N --payload=SIZE --duration=SEC (one ioc per conn)\n";
        std::cerr << "  echo_shared:     --threads=N --payload=SIZE --duration=SEC (shared conn)\n";
        std::cerr << "  echo_reactor:    --conns=N --payload=SIZE --duration=SEC (reactor multiplexing)\n";
        std::cerr << "  echo_reactor_raw:--conns=N --payload=SIZE --duration=SEC (reactor + RAW)\n";
        std::cerr << "  echo_raw:        --conns=N --payload=SIZE --duration=SEC (RAW format, no json)\n";
        std::cerr << "  online:          --users=N\n";
        std::cerr << "  message:         --pairs=N --payload=SIZE --duration=SEC --port=N --pool=N\n";
        std::cerr << "  message_cross:   --pairs=N --payload=SIZE --duration=SEC (cross-node RPC)\n";
        std::cerr << "                   --cross-send-port=N --cross-recv-port=N (default 8881/8885)\n";
        return 1;
    }

    if (args.test == "echo") {
        test_echo_qps(args);
    } else if (args.test == "echo_shared") {
        test_echo_qps_shared(args);
    } else if (args.test == "echo_reactor") {
        test_echo_qps_reactor(args);
    } else if (args.test == "echo_reactor_raw") {
        test_echo_qps_reactor(args, true);
    } else if (args.test == "echo_raw") {
        test_echo_qps_raw(args);
    } else if (args.test == "online") {
        test_max_online(args);
    } else if (args.test == "message") {
        test_message_throughput(args);
    } else if (args.test == "message_cross") {
        test_message_throughput_crossnode(args);
    } else {
        std::cerr << "Unknown test: " << args.test << "\n";
        return 1;
    }

    // Graceful shutdown: stop io_contexts and join threads
    // before static destruction to avoid "terminate" from ~thread
    auto& client = mrpc::client::get();
    client.shutdown();
    client.wait_shutdown();

    std::cout << "\nTest complete.\n";
    return 0;
}
