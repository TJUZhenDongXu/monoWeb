#include "http_conn.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstdio>
#include <cerrno>

const std::unordered_map<std::string, HttpConn::HttpMethod> HttpConn::kMethodMap = {
    { "GET",  HttpConn::HttpMethod::GET  },
    { "POST", HttpConn::HttpMethod::POST },
    { "HEAD", HttpConn::HttpMethod::HEAD }
};

HttpConn::HttpConn()
    : sockfd_(-1),
      peer_addr_(),
      is_closed_(true),
      read_idx_(0),
      method_(HttpMethod::UNKNOWN),
      url_(),
      version_(),
      request_parsed_ok_(false),
      file_buf_(),
      write_buf_() {
}

HttpConn::~HttpConn() {
    Close();
}

bool HttpConn::SetNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    return true;
}

void HttpConn::Init(int sockfd, const sockaddr_in& peer_addr) {
    sockfd_ = sockfd;
    peer_addr_ = peer_addr;
    is_closed_ = false;

    // 重置状态
    read_idx_ = 0;
    method_ = HttpMethod::UNKNOWN;
    url_.clear();
    version_.clear();
    request_parsed_ok_ = false;
    file_buf_.clear();
    write_buf_.clear();

    // —— 不再把 sockfd_ 设为非阻塞 —— 
    // SetNonBlocking(sockfd_);    // ← 注释掉（或删除）这一行
}

void HttpConn::Close() {
    if (!is_closed_ && sockfd_ >= 0) {
        ::close(sockfd_);
        sockfd_ = -1;
        is_closed_ = true;
    }
}

void HttpConn::Process() {
    if (!Read()) {
        Close();
        return;
    }
    if (!ParseRequest()) {
        const char* bad_req =
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 15\r\n"
            "\r\n"
            "400 Bad Request";
        send(sockfd_, bad_req, strlen(bad_req), 0);
        Close();
        return;
    }
    WriteResponse();
    Close();
}

bool HttpConn::Read() {
    // 改为阻塞模式，此时 recv 在没有数据时会阻塞，直到浏览器发来第一行为止
    while (true) {
        ssize_t bytes_read = ::recv(sockfd_,
                                   read_buf_ + read_idx_,
                                   READ_BUFFER_SIZE - read_idx_,
                                   0);
        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 对于阻塞模式，一般不会走到这里，除非被信号打断
                continue;
            }
            return false;
        } else if (bytes_read == 0) {
            // 对方关闭
            return false;
        }
        read_idx_ += static_cast<size_t>(bytes_read);
        // 如果读满了，或者客户端一次性把完整请求发来，就跳出
        if (read_idx_ >= READ_BUFFER_SIZE) {
            break;
        }
        // 在阻塞模式下，如果没有填满缓冲区，通常说明已经读完这一批数据了
        // (浏览器通常会一次性把 GET 请求头发过来)。因此可跳出循环。
        break;
    }
    return true;
}

bool HttpConn::ParseRequest() {
    const std::string request(read_buf_, read_idx_);
    size_t line_end = request.find("\r\n");
    if (line_end == std::string::npos) {
        return false;
    }

    std::string request_line = request.substr(0, line_end);
    std::istringstream line_stream(request_line);

    std::string method_str, url, version_str;
    if (!(line_stream >> method_str >> url >> version_str)) {
        return false;
    }

    auto it = kMethodMap.find(method_str);
    if (it != kMethodMap.end()) {
        method_ = it->second;
    } else {
        method_ = HttpMethod::UNKNOWN;
    }
    if (method_ != HttpMethod::GET) {
        return false;
    }

    if (version_str != "HTTP/1.0" && version_str != "HTTP/1.1") {
        return false;
    }

    url_ = url;
    version_ = version_str;
    if (url_ == "/") {
        url_ = "/index.html";
    }

    request_parsed_ok_ = true;
    return true;
}

std::string HttpConn::GetMimeType(const std::string& path) {
    if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".html") == 0)   return "text/html";
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".htm") == 0)    return "text/html";
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".css") == 0)    return "text/css";
    if (path.size() >= 3 && path.compare(path.size() - 3, 3, ".js") == 0)     return "application/javascript";
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".jpg") == 0)    return "image/jpeg";
    if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".jpeg") == 0)   return "image/jpeg";
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".png") == 0)    return "image/png";
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".gif") == 0)    return "image/gif";
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".txt") == 0)    return "text/plain";
    return "application/octet-stream";
}

void HttpConn::WriteResponse() {
    if (!request_parsed_ok_) {
        return;
    }

    std::string full_path = "." + url_;
    struct stat st;
    if (stat(full_path.c_str(), &st) < 0 || S_ISDIR(st.st_mode)) {
        // 404
        const char* not_found =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 9\r\n"
            "\r\n"
            "Not Found";
        send(sockfd_, not_found, strlen(not_found), 0);
        return;
    }

    size_t file_size = static_cast<size_t>(st.st_size);
    file_buf_.resize(file_size);
    FILE* fp = fopen(full_path.c_str(), "rb");
    if (!fp) {
        // 500
        const char* err500 =
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 21\r\n"
            "\r\n"
            "Internal Server Error";
        send(sockfd_, err500, strlen(err500), 0);
        return;
    }
    fread(file_buf_.data(), 1, file_size, fp);
    fclose(fp);

    std::ostringstream header;
    header << "HTTP/1.1 200 OK\r\n";
    header << "Content-Type: " << GetMimeType(full_path) << "\r\n";
    header << "Content-Length: " << file_size << "\r\n";
    header << "Connection: close\r\n";
    header << "\r\n";

    write_buf_ = header.str();
    send(sockfd_, write_buf_.c_str(), write_buf_.size(), 0);
    send(sockfd_, file_buf_.data(), file_size, 0);
}
