#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * HttpConn:
 *   一个最简单的 GET-only 静态文件服务器连接处理类。
 *   使用流程：
 *     1. 在 accept() 后调用 Init(sockfd, 客户端地址)，并设置 sockfd 为非阻塞；
 *     2. Process() 会阻塞读取一次请求，解析请求行，仅支持 GET；
 *     3. 根据 URL 读取静态文件，打包 HTTP 响应并发送，最后关闭连接。
 *
 *  如果后续你要扩展成 epoll + 读写分离模型，可以把 Read/Write 分离，
 *  但目前仅做最简单的“同步阻塞”实现。
 */
class HttpConn {
public:
    // 单次 Read 缓冲区大小
    static constexpr int READ_BUFFER_SIZE = 4096;

    HttpConn();
    ~HttpConn();

    /// accept() 之后，第一个使用它，用来设置 fd、清理状态，并把 fd 设为非阻塞
    void Init(int sockfd, const sockaddr_in& peer_addr);

    /// 立即关闭连接
    void Close();

    /// “业务入口”：调用 Read()、ParseRequest()、WriteResponse()，并在末尾 Close()
    void Process();

    // 仅支持 GET, POST, HEAD；但我们这里只真正处理 GET，其他返回 400/405
    enum class HttpMethod {
        GET,
        POST,
        HEAD,
        UNKNOWN
    };

private:
    /// 循环从 sockfd_ 读取数据到 read_buf_，直到返回 EAGAIN（非阻塞）
    bool Read();

    /// 只解析第一行：“<METHOD> <URL> <VERSION>\r\n”
    bool ParseRequest();

    /// 如果解析成功，根据 URL 在本地读取静态文件，拼装 HTTP 响应并 send
    void WriteResponse();

    /// 根据文件后缀返回对应的 MIME
    static std::string GetMimeType(const std::string& path);

    /// 将 fd 设为非阻塞
    static bool SetNonBlocking(int fd);

private:
    int                          sockfd_{-1};          // 客户端 fd
    sockaddr_in                  peer_addr_;           // 客户端地址
    bool                         is_closed_{true};     // 是否已经关闭

    // ———— 读部分 ————
    char                         read_buf_[READ_BUFFER_SIZE]; // 一次 recv 的缓冲区
    size_t                       read_idx_{0};         // read_buf_ 中已读入的字节数

    // ———— 解析结果 ————
    HttpMethod                   method_{HttpMethod::UNKNOWN};
    static const std::unordered_map<std::string, HttpMethod> kMethodMap;
    std::string                  url_;                 // 客户端 GET 的路径, e.g. "/index.html"
    std::string                  version_;             // HTTP/1.1
    bool                         request_parsed_ok_{false};

    // ———— 写部分 ————
    std::vector<char>            file_buf_;            // 已读取完的文件二进制内容
    std::string                  write_buf_;           // 拼装好的 HTTP 响应头
};

#endif // HTTP_CONN_H
