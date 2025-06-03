#ifndef EPOLLER_H
#define EPOLLER_H

#include <sys/epoll.h>
#include <vector>

/**
 * Epoller:
 *   一个简单的 epoll 封装类。支持添加、修改、删除 fd，
 *   并通过 wait() 获取就绪事件列表。
 */
class Epoller {
public:
    /**
     * @param max_events 要一次性处理的最大就绪事件数。
     */
    explicit Epoller(int max_events = 1024);
    ~Epoller();

    /**
     * 将 fd 加入 epoll 集合，监听事件由 events 指定（如 EPOLLIN | EPOLLET）。
     * @return true 成功，false 失败（会打印 perror）。
     */
    bool addFd(int fd, uint32_t events);

    /**
     * 修改已经在 epoll 中的 fd，切换它的监听事件到新的 events。
     * @return true 成功，false 失败（会打印 perror）。
     */
    bool modFd(int fd, uint32_t events);

    /**
     * 将 fd 从 epoll 中删除（通常在连接关闭时调用）。
     */
    bool delFd(int fd);

    /**
     * 等待 I/O 就绪。timeout_ms 单位毫秒，-1 表示无限等待。
     * @return 就绪的文件描述符数量（>=0），若出错返回 -1（会打印 perror，除非 errno==EINTR）。
     */
    int wait(int timeout_ms = -1);

    /**
     * 返回内部存储的就绪事件数组首地址。数组长度为构造时传入的 max_events。
     * wait() 之后，前 [0, n) 个元素就是就绪事件。
     */
    struct epoll_event* getEvents();

private:
    int epoll_fd_;               // epoll_create1 创建的文件描述符
    int max_events_;             // 最大就绪事件数
    std::vector<epoll_event> events_;  // 存储 epoll_wait 返回事件的数组
};

#endif // EPOLLER_H
