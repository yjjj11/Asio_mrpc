#ifndef ZOOKEEPERUTIL_HPP
#define ZOOKEEPERUTIL_HPP

#include <string>
#include <zookeeper/zookeeper.h>
#include <semaphore.h>
#include <iostream>
#include <cstdlib>
#include <mutex>
#include <condition_variable>
#include <optional>
// 全局watcher：仅处理会话连接事件
void global_watcher(zhandle_t* zh, int type, int state, const char* path, void* watcherCtx);

// 异步操作的回调上下文：存储操作状态、结果数据、信号量（用于阻塞等待）
struct ZkAsyncCtx {
    int rc;                     // 操作返回码（ZOK为成功）
    std::string data;           // 存储getData的结果数据
    sem_t sem;                  // 阻塞等待的信号量
    char path_buf[128];         // 存储create的节点路径
    int path_buf_len;           // path_buf长度
    char data_buf[64];          // 存储getData的数据缓冲区
    int data_buf_len;           // data_buf长度
    ZkAsyncCtx() : rc(-1), path_buf_len(128), data_buf_len(64) {
        sem_init(&sem, 0, 0);   // 信号量初始值0
    }
    ~ZkAsyncCtx() {
        sem_destroy(&sem);      // 销毁信号量
    }
};

class Zookeeperutil
{
public:
    Zookeeperutil();
    ~Zookeeperutil();
    void start();
    void create(std::string path, std::string data, int state);
    std::optional<std::pair<std::string,uint64_t>> getData(std::string path);

    void set_ip_port(const std::string& ip, uint64_t port) {
        m_zk_ip = ip;
        m_zk_port = std::to_string(port);
    }

private:
    zhandle_t* m_handle;  // zk客户端句柄
    // ZK地址写死，可直接修改
     std::string m_zk_ip = "127.0.0.1";
     std::string m_zk_port = "2181";
};


// 全局watcher实现：仅处理会话连接事件
void global_watcher(zhandle_t* zh, int type, int state, const char* path, void* watcherCtx) {
    if (type == ZOO_SESSION_EVENT) {
        if (state == ZOO_CONNECTED_STATE) {
            // 从上下文中取出信号量
            sem_t* conn_sem = static_cast<sem_t*>(watcherCtx);
            sem_post(conn_sem); // 释放信号量，唤醒start
            std::cout << "ZK session connected success!" << std::endl;
        } else if (state == ZOO_EXPIRED_SESSION_STATE) {
            std::cerr << "ZK session expired, need re-init!" << std::endl;
        }
    }
}

// 异步exists回调：判断节点是否存在
void aexists_completion(int rc, const struct Stat* stat, const void* ctx) {
    ZkAsyncCtx* async_ctx = const_cast<ZkAsyncCtx*>(static_cast<const ZkAsyncCtx*>(ctx));
    async_ctx->rc = rc;
    sem_post(&async_ctx->sem); // 释放信号量，唤醒等待的主线程
}

// 异步create回调：节点创建完成
void acreate_completion(int rc, const char* path, const void* ctx) {
    ZkAsyncCtx* async_ctx = const_cast<ZkAsyncCtx*>(static_cast<const ZkAsyncCtx*>(ctx));
    async_ctx->rc = rc;
    if (rc == ZOK && path) {
        snprintf(async_ctx->path_buf, async_ctx->path_buf_len, "%s", path);
    }
    sem_post(&async_ctx->sem);
}

// 异步get回调：获取节点数据完成
void aget_completion(int rc, const char* data, int data_len, const struct Stat* stat, const void* ctx) {
    ZkAsyncCtx* async_ctx = const_cast<ZkAsyncCtx*>(static_cast<const ZkAsyncCtx*>(ctx));
    async_ctx->rc = rc;
    if (rc == ZOK && data && data_len > 0) {
        async_ctx->data = std::string(data, data_len); // 按实际长度存储数据
    }
    sem_post(&async_ctx->sem);
}

// 构造函数
Zookeeperutil::Zookeeperutil() : m_handle(nullptr) {}

// 析构函数
Zookeeperutil::~Zookeeperutil() {
    if (m_handle != nullptr) {
        zookeeper_close(m_handle);
        m_handle = nullptr;
        std::cout << "ZK handle closed!" << std::endl;
    }
}
// 初始化ZK连接
void Zookeeperutil::start() {
    std::string connstr = m_zk_ip + ":" + m_zk_port;
    // 局部信号量，用于等待连接成功
    sem_t conn_sem;
    sem_init(&conn_sem, 0, 0);

    // 初始化ZK句柄：多线程版本
    m_handle = zookeeper_init(
        connstr.c_str(),
        global_watcher,
        30000,
        nullptr,
        &conn_sem, // 将信号量作为上下文传入
        0
    );
    if (m_handle == nullptr) {
        std::cerr << "zookeeper_init error! connect to: " << connstr << std::endl;
        sem_destroy(&conn_sem);
        exit(EXIT_FAILURE);
    }

    // 等待watcher连接成功
    sem_wait(&conn_sem);
    sem_destroy(&conn_sem);
    std::cout << "zookeeper_init success! connect to: " << connstr << std::endl;
}
// 创建节点：支持自动创建多级父节点
void Zookeeperutil::create(std::string path, std::string data, int state) {
    if (m_handle == nullptr) {
        std::cerr << "ZK handle is null, call start first!" << std::endl;
        exit(EXIT_FAILURE);
    }

    // 第一步：递归创建所有父节点
    size_t pos = 1;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        std::string parent_path = path.substr(0, pos);
        ZkAsyncCtx ctx;
        // 判断父节点是否存在
        zoo_aexists(m_handle, parent_path.c_str(), 0, aexists_completion, &ctx);
        sem_wait(&ctx.sem);
        if (ctx.rc == ZNONODE) {
            // 创建父节点（持久节点，因为临时节点不能有子节点）
            zoo_acreate(
                m_handle,
                parent_path.c_str(),
                "",
                0,
                &ZOO_OPEN_ACL_UNSAFE,
                ZOO_PERSISTENT,
                acreate_completion,
                &ctx
            );
            sem_wait(&ctx.sem);
            if (ctx.rc != ZOK) {
                std::cerr << "create parent node failure, rc: " << ctx.rc << ", path: " << parent_path << std::endl;
                exit(EXIT_FAILURE);
            }
        }
    }

    // 第二步：创建目标节点
    ZkAsyncCtx ctx;
    zoo_aexists(m_handle, path.c_str(), 0, aexists_completion, &ctx);
    sem_wait(&ctx.sem);

    if (ctx.rc == ZNONODE) {
        zoo_acreate(
            m_handle,
            path.c_str(),
            data.c_str(),
            data.size(),
            &ZOO_OPEN_ACL_UNSAFE,
            state,
            acreate_completion,
            &ctx
        );
        sem_wait(&ctx.sem);

        if (ctx.rc == ZOK) {
            LOG_INFO("ZK function node create success, path: [{}]", path);
        } else {
            LOG_ERROR("znode create failure, rc: [{}], path: [{}]", ctx.rc, path);
            exit(EXIT_FAILURE);
        }
    }
}

// 获取节点数据：异步实现，对外同步调用（返回值/用法和原有一致）
std::optional<std::pair<std::string,uint64_t>> Zookeeperutil::getData(std::string path) {
    if (m_handle == nullptr) {
        std::cerr << "ZK handle is null, call start first!" << std::endl;
        return std::nullopt;
    }
    ZkAsyncCtx ctx;
    // 异步获取节点数据
    zoo_aget(
        m_handle,
        path.c_str(),
        0,
        aget_completion,
        &ctx
    );
    sem_wait(&ctx.sem); // 阻塞等待get回调完成

    if (ctx.rc != ZOK) {
        std::cerr << "get znode error, rc: " << ctx.rc << ", path: " << path << std::endl;
        return std::nullopt;
    } else {
        // 解析IP:PORT格式
        size_t pos = ctx.data.find(':');
        if (pos == std::string::npos) {
            std::cerr << "invalid data format, no ':' found: " << ctx.data << std::endl;
            return std::nullopt;
        }
        std::string ip = ctx.data.substr(0, pos);
        uint64_t port = std::stoull(ctx.data.substr(pos + 1));
        return std::make_pair(ip,port); // 返回回调中存储的结果数据
    }
}

#endif  // ZOOKEEPERUTIL_HPP