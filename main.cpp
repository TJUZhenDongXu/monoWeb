// 文件名：main.cpp
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <unordered_map>

#include "epoller/epoller.h"
#include "http/http_conn.h"

// 最大并发监听事件数
static const int MAX_EVENTS = 1024;

/**
 * 将 fd 设置为非阻塞。
 * @return 0 成功，-1 失败（并设置 errno）。
 */
int setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    // 允许端口快速重用
    int on = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));

    sockaddr_in serv_addr;
    std::memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_port        = htons(port);
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&serv_addr), sizeof(serv_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return EXIT_FAILURE;
    }
    if (listen(listen_fd, 128) < 0) {
        perror("listen");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    // 将监听 socket 设为非阻塞
    if (setNonBlocking(listen_fd) < 0) {
        perror("setNonBlocking");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    // 创建 Epoller 实例
    Epoller epoller(MAX_EVENTS);
    // 注册 listen_fd，监听可读事件，使用边缘触发（EPOLLET）
    epoller.addFd(listen_fd, EPOLLIN | EPOLLET);

    // 存储 fd->HttpConn* 的映射，方便事件到来时找到对应对象
    std::unordered_map<int, HttpConn*> conn_map;

    std::cout << "Server running on port " << port << std::endl;

    // 主循环：调用 epoll_wait，分发事件
    while (true) {
        int eventCnt = epoller.wait(-1);  // 阻塞等待
        if (eventCnt < 0) {
            // 忽略 EINTR
            continue;
        }
        auto* events = epoller.getEvents();
        for (int i = 0; i < eventCnt; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == listen_fd) {
                // 监听套接字有事件 ⇒ 新连接到来
                while (true) {
                    sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(listen_fd,
                                           reinterpret_cast<sockaddr*>(&client_addr),
                                           &client_len);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // 已经没有新的连接，跳出 accept 循环
                            break;
                        } else {
                            perror("accept");
                            break;
                        }
                    }

                    // 将新连接置为非阻塞
                    if (setNonBlocking(client_fd) < 0) {
                        perror("setNonBlocking");
                        close(client_fd);
                        continue;
                    }
                    // 将 client_fd 注册到 epoll，监听可读、挂断事件，使用边缘触发
                    epoller.addFd(client_fd, EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLHUP);

                    // 为每个连接创建 HttpConn 并保存到 map
                    HttpConn* conn = new HttpConn();
                    conn->Init(client_fd, client_addr);
                    conn_map[client_fd] = conn;

                    std::cout << "Accepted: "
                              << inet_ntoa(client_addr.sin_addr)
                              << ":" << ntohs(client_addr.sin_port)
                              << ", fd=" << client_fd << std::endl;
                }
            } else {
                // 已建立连接上的 I/O 事件
                // 先检查是否是对端挂断或错误
                if (ev & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    // 对端断开或错误，清理
                    auto it = conn_map.find(fd);
                    if (it != conn_map.end()) {
                        it->second->Close();
                        delete it->second;
                        conn_map.erase(it);
                    }
                    epoller.delFd(fd);
                    continue;
                }
                // 如果是可读事件
                if (ev & EPOLLIN) {
                    auto it = conn_map.find(fd);
                    if (it != conn_map.end()) {
                        HttpConn* conn = it->second;
                        // 目前仍然使用同步方式：Process 包含 Read/Parse/Write，并在末尾 Close
                        epoller.delFd(fd);
                        conn->Process();

                        // 处理完毕后，关闭并释放掉连接
                        delete conn;
                        conn_map.erase(it);
                    }
                }
                // 如果未来要支持可写事件（EPOLLOUT），可以在这里加上判断并调用 conn->OnWrite()
            }
        }
    }

    // 虽然一般不会到这里
    close(listen_fd);
    return 0;
}
