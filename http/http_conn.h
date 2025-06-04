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

    /**
     * @brief 当epoll通知这个fd可读的时候，调用OnRead
     * 
     * @return true 表示完整读到请求，需要切换到写事件
     * @return false 还没读完或者出错，需要继续等待可读或者关闭
     */
    bool OnRead();

    /**
     * @brief epoll通知这个fd可写的是偶，调用OnWrite
     * 
     * @return true 响应全部发送完毕，可以安全地关闭连接
     * @return false 
     */
    bool OnWrite();

    // 仅支持 GET, POST, HEAD；但我们这里只真正处理 GET，其他返回 400/405
    enum class HttpMethod {
        GET,
        POST,
        HEAD,
        UNKNOWN
    };

    //解析状态机
    enum class ParseState{
        REQUEST_LINE,
        HEADERS,
        BODY,
        FINISH
    };

private:
    bool parseRequestLine(const std::string &line);
    bool parseHeaders(const std::string& header_line);

    void prepareResponse();
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

    //存储解析出来的http header
    std::unordered_map<std::string, std::string> headers_;
    //状态参数
    ParseState parse_state_{ParseState::REQUEST_LINE};
    // ———— 写部分 ————
    std::vector<char>            file_buf_;            // 已读取完的文件二进制内容
    size_t file_idx_{0};

    std::string                  write_buf_;           // 拼装好的 HTTP 响应头
    size_t write_idx_{0};                              //发了多少字节

    bool keep_alive_{false};
};

#endif // HTTP_CONN_H
