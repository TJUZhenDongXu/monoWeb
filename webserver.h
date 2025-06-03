#ifndef MINI_WEBSERVER_WEBSERVER_H_      
#define MINI_WEBSERVER_WEBSERVER_H_

#include <cstdint>               // uint16_t
#include <unordered_map>         // unordered_map
#include <memory>                // shared_ptr / weak_ptr

#include "threadpool.h"          // ThreadPool 声明
#include "http_conn.h"           // HttpConn 声明

// 如果你只想前向声明而不包含完整头文件，可用：
// class ThreadPool;
// class HttpConn;
// 但随后实现文件里仍需 #include 真正的头文件。

class WebServer
{
public:
    explicit WebServer(uint16_t port = 8080, std::size_t workers = 8);
    ~WebServer();

    // 禁止拷贝，允许移动（可选）
    // WebServer(const WebServer&)            = delete;
    // WebServer& operator=(const WebServer&) = delete;
    // WebServer(WebServer&&)                 = default;
    // WebServer& operator=(WebServer&&)      = default;

    void Run();                // 阻塞主循环

private:
    void HandleAccept();                      // 处理 listenfd 上的 EPOLLIN
    void HandleEvent(int fd, uint32_t ev);    // 处理普通连接事件
    int  SetNonBlock(int fd);

private:
    uint16_t port_{8080};
    int listenfd_{-1};
    int epfd_{-1};

    ThreadPool pool_;      // 线程池
    std::unordered_map<int, std::weak_ptr<HttpConn>> conns_; // fd → weak_ptr
};

#endif  
