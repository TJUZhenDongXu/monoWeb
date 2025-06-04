// 文件：main.cpp
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
    // 注册 listen_fd，监听可读事件，使用边缘触发（EPOLLIN | EPOLLET）
    epoller.addFd(listen_fd, EPOLLIN | EPOLLET);

    // 存储 fd->HttpConn* 的映射，方便事件到来时找到对应对象
    std::unordered_map<int, HttpConn*> conn_map;

    std::cout << "Server running on port " << port << std::endl;

    // 主循环：调用 epoll_wait，分发事件
    while (true) {
        int eventCnt = epoller.wait(-1);  // 阻塞等待
        if (eventCnt < 0) {
            // 如果是 EINTR，可以忽略；否则打印错误
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        auto* events = epoller.getEvents();
        for (int i = 0; i < eventCnt; ++i) {
            int fd  = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == listen_fd) {
                // ——— accept 新连接 ———
                while (true) {
                    sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(listen_fd,
                                           reinterpret_cast<sockaddr*>(&client_addr),
                                           &client_len);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // 已经没有新的连接了，跳出 accept 循环
                            break;
                        } else {
                            perror("accept");
                            break;
                        }
                    }

                    // 将新连接设为非阻塞
                    if (setNonBlocking(client_fd) < 0) {
                        perror("setNonBlocking");
                        close(client_fd);
                        continue;
                    }

                    // 新连接注册到 epoll，监听可读 + 掉线 (EPOLLIN | EPOLLET | EPOLLRDHUP)
                    epoller.addFd(client_fd, EPOLLIN | EPOLLET | EPOLLRDHUP);

                    // 为每个连接创建 HttpConn 并保存到 conn_map
                    HttpConn* conn = new HttpConn();
                    conn->Init(client_fd, client_addr);
                    conn_map[client_fd] = conn;

                    std::cout << "Accepted: "
                              << inet_ntoa(client_addr.sin_addr)
                              << ":" << ntohs(client_addr.sin_port)
                              << ", fd=" << client_fd << std::endl;
                }
            } else {
                // ——— 已建立连接上的 I/O 事件 ———

                // 1) 首先检测是否挂断或错误
                if (ev & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    // 对端断开或出错，清理
                    epoller.delFd(fd);
                    auto it = conn_map.find(fd);
                    if (it != conn_map.end()) {
                        it->second->Close();
                        delete it->second;
                        conn_map.erase(it);
                    }
                    continue;
                }

                // 2) 如果可读事件到来，就调用 OnRead()
                if (ev & EPOLLIN) {
                    auto it = conn_map.find(fd);
                    if (it == conn_map.end()) continue;
                    HttpConn* conn = it->second;

                    bool needWrite = conn->OnRead();
                    if (needWrite) {
                        // 说明 OnRead() 已经 parse 完请求并生成好响应，
                        // 需要把该 fd 从“只读”切换到“可写”
                        epoller.modFd(fd, EPOLLOUT | EPOLLET | EPOLLRDHUP);
                    }
                }

                // 3) 如果可写事件到来，就调用 OnWrite()
                if (ev & EPOLLOUT) {
                    auto it = conn_map.find(fd);
                    if (it == conn_map.end()) continue;
                    HttpConn* conn = it->second;

                    bool closeConn = conn->OnWrite();
                    if (closeConn) {
                        // 响应完全发送，关闭并删除
                        epoller.delFd(fd);
                        conn->Close();
                        delete conn;
                        conn_map.erase(it);
                    } else {
                        // 说明要保持长连接或是 send 缓冲满暂时没发完
                        // 如果是 keep-alive 模式，OnWrite() 已经帮忙重置状态
                        // 将它改回监听“可读”
                        epoller.modFd(fd, EPOLLIN | EPOLLET | EPOLLRDHUP);
                    }
                }
            }
        }
    }

    // 清理——虽然通常不会执行到这里
    close(listen_fd);
    return 0;
}
