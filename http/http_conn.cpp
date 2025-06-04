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
      parse_state_(ParseState::REQUEST_LINE),
      method_(HttpMethod::UNKNOWN),
      url_(),
      version_(),
      request_parsed_ok_(false),
      write_buf_(),
      write_idx_(0),
      file_buf_(),
      file_idx_(0),
      keep_alive_(false),
      headers_() {
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
    write_idx_ = 0;
    file_idx_ = 0;

    parse_state_ = ParseState::REQUEST_LINE;

    method_ = HttpMethod::UNKNOWN;
    url_.clear();
    version_.clear();
    request_parsed_ok_ = false;
    file_buf_.clear();
    write_buf_.clear();
    headers_.clear();
    keep_alive_ = false;

    SetNonBlocking(sockfd_);   
}

void HttpConn::Close() {
    if (!is_closed_ && sockfd_ >= 0) {
        ::close(sockfd_);
        sockfd_ = -1;
        is_closed_ = true;
    }
}

// void HttpConn::Process() {
//     if (!Read()) {
//         Close();
//         return;
//     }
//     if (!ParseRequest()) {
//         const char* bad_req =
//             "HTTP/1.1 400 Bad Request\r\n"
//             "Content-Type: text/plain\r\n"
//             "Content-Length: 15\r\n"
//             "\r\n"
//             "400 Bad Request";
//         send(sockfd_, bad_req, strlen(bad_req), 0);
//         Close();
//         return;
//     }
//     WriteResponse();
//     Close();
// }

bool HttpConn::OnRead(){
    while (true){
        ssize_t bytes_read = ::recv(sockfd_,
                            read_buf_ + read_idx_,
                            READ_BUFFER_SIZE - read_idx_,
                            0); 
        if (bytes_read == 0) {
            // 对端已经主动关闭（FIN），本连接不再可读
            return false;
        }
        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 暂时没有更多数据可读，跳出循环
                break;
            } else {
                // 其他错误（如 ECONNRESET 等），直接关闭
                return false;
            }
        }
        read_idx_ += static_cast<size_t>(bytes_read);
        if (read_idx_ >= READ_BUFFER_SIZE) {
            // 缓冲区已经满了，不要继续往下读，以防 overflow
            break;
        }
    }
    
    std::string request(read_buf_, read_idx_);
    size_t pos = 0;

    if(parse_state_ == ParseState::REQUEST_LINE){
        auto line_end = request.find("\r\n");
        if(line_end == std::string::npos){
            return false;
        }
        std::string request_line = request.substr(0, line_end);
        if(!parseRequestLine(request_line)){
            perror("parseRequestLine failed");
            return false;
        }
        pos = line_end + 2;
        parse_state_ = ParseState::HEADERS;
    }

    if(parse_state_ == ParseState::HEADERS){
        auto header_end = request.find("\r\n\r\n");
        if(header_end == std:: string::npos){
            return false;
        }
        size_t header_line_start = pos;
        while(header_line_start < header_end){
            auto line_end = request.find("\r\n", header_line_start);
            std::string header_line = request.substr(header_line_start, line_end - header_line_start);
            if(!parseHeaders(header_line)){
                return false;
            }
            header_line_start = line_end + 2;
        }
        parse_state_ = ParseState::FINISH;
    }

    if(parse_state_ == ParseState::FINISH){
        auto it = headers_.find("Connection");
        if (it != headers_.end() &&
            (it->second == "keep-alive" || it->second == "Keep-Alive")) {
            keep_alive_ = true;
        }
        prepareResponse();

        return true;
    }
    return false;
}

bool HttpConn::OnWrite(){
    while(write_idx_ < write_buf_.size()){
        ssize_t bytes_sent = ::send(sockfd_, write_buf_.data() + write_idx_,
                                    write_buf_.size() - write_idx_, 0);
        if (bytes_sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 发送缓冲已满，下次 EPOLLOUT 再继续
                return false;
            }
            // 其他错误，直接让调用者关闭连接
            return true;
        }
        write_idx_ += static_cast<size_t>(bytes_sent);
    }

    size_t file_size = file_buf_.size();
    while(file_idx_ < file_size){
        ssize_t bytes_sent = ::send(
            sockfd_,
            file_buf_.data() + file_idx_,
            file_size - file_idx_,
            0
        );
        if (bytes_sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 发送缓冲已满，下次 EPOLLOUT 再继续
                return false;
            }
            // 其他错误，通知上层关闭
            return true;
        }
        file_idx_ += static_cast<size_t>(bytes_sent);
    }

    if(keep_alive_){
        read_idx_ = 0;
        write_idx_ = 0;
        file_idx_ = 0;
        write_buf_.clear();
        file_buf_.clear();
        parse_state_ = ParseState::REQUEST_LINE;
        headers_.clear();
        request_parsed_ok_ = false;
        keep_alive_ = false; // 下次如果 header 再带 keep-alive，会再置 true

        // 通知上层：本次不用关闭连接，改回监听 EPOLLIN
        return false;
    }
    return true;
}


bool HttpConn::parseRequestLine(const std::string &line){
    std::istringstream line_stream(line);

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

    if (method_ != HttpMethod::GET && method_ != HttpMethod::HEAD) {
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

bool HttpConn::parseHeaders(const std::string& header_line){
    auto colon = header_line.find(':');
    if(colon == std::string::npos){
        return false;
    }
    std::string key = header_line.substr(0, colon);
    size_t value_start = colon + 1;
    while (value_start < header_line.size() && header_line[value_start] == ' '){
        ++ value_start;
    }
    std::string value = header_line.substr(value_start);

    headers_[key] = value;
    return true;
}

void HttpConn::prepareResponse(){
    if (!request_parsed_ok_) {
        return;
    }

    std::string full_path = "." + url_;
    struct stat st;
    if (stat(full_path.c_str(), &st) < 0 || S_ISDIR(st.st_mode)) {
        // 404
        std::ostringstream header;
        header << "HTTP/1.1 404 Not Found\r\n";
        header << "Content-Type: text/plain\r\n";
        header << "Content-Length: 9\r\n";
        header << "Connection: close\r\n";
        header << "\r\n";
        header << "Not Found";
        write_buf_ = header.str();
        file_buf_.clear();
        return;
    }

    size_t file_size = static_cast<size_t>(st.st_size);
    FILE* fp = fopen(full_path.c_str(), "rb");

    if (!fp) {
        // 500
        std::ostringstream header;
        header << "HTTP/1.1 500 Internal Server Error\r\n";
        header << "Content-Type: text/plain\r\n";
        header << "Content-Length: 21\r\n";
        header << "Connection: close\r\n";
        header << "\r\n";
        header << "Internal Server Error";
        write_buf_ = header.str();
        file_buf_.clear();
        return;
    }

    file_buf_.resize(file_size);
    size_t n = fread(file_buf_.data(), 1, file_size, fp);
    fclose(fp);

    if(n != file_size){
        std::ostringstream header;
        header << "HTTP/1.1 500 Internal Server Error\r\n";
        header << "Content-Type: text/plain\r\n";
        header << "Content-Length: 21\r\n";
        header << "Connection: close\r\n";
        header << "\r\n";
        header << "Internal Server Error";
        write_buf_ = header.str();
        file_buf_.clear();
        return;
    }

    std::ostringstream header;
    header << "HTTP/1.1 200 OK\r\n";
    header << "Content-Type: " << GetMimeType(full_path) << "\r\n";
    header << "Content-Length: " << file_size << "\r\n";
    if (keep_alive_) {
        header << "Connection: keep-alive\r\n";
    } else {
        header << "Connection: close\r\n";
    }
    header << "\r\n";
    write_buf_ = header.str();
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
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".mp4") == 0)    return "video/mp4";
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".txt") == 0)    return "text/plain";
    return "application/octet-stream";
}

