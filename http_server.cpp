#include "http_server.h"
#include "socket_utils.h"
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <algorithm>

// ==================== 构造函数 ====================
HttpServer::HttpServer(const std::string& root_dir, int keepalive_timeout)
    : root_dir_(root_dir), keepalive_timeout_(keepalive_timeout) {
}

// ==================== 新连接入口 ====================
void HttpServer::on_accept(Scheduler* sched, int client_fd) {
    // 为每个新连接创建一个独立协程
    sched->spawn([this, sched, client_fd]() {
        handle_connection(sched, client_fd);
    });
}

// ==================== 连接处理协程 ====================
void HttpServer::handle_connection(Scheduler* sched, int client_fd) {
    Coroutine* self = Coroutine::current;  // 获取当前协程控制块

    // ---------- 创建非阻塞超时timerfd ----------
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (tfd < 0) {
        close(client_fd);
        return;
    }

    // 将timerfd加入epoll，关联到当前协程
    {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLONESHOT;
        ev.data.ptr = self;
        epoll_ctl(sched->get_epfd(), EPOLL_CTL_ADD, tfd, &ev);
    }

    char buf[8192];  // 8KB请求缓冲区

    // ===== Keep-Alive主循环：处理同一个连接上的多个请求 =====
    while (true) {
        // ---------- 检测Keep-Alive超时 ----------
        uint64_t exp_cnt = 0;
        ssize_t rd = read(tfd, &exp_cnt, sizeof(exp_cnt));
        if (rd > 0 && exp_cnt > 0) {
            // 超时，退出循环关闭连接
            break;
        }

        // ---------- 重置超时定时器 ----------
        struct itimerspec its;
        its.it_value.tv_sec = keepalive_timeout_;
        its.it_value.tv_nsec = 0;
        its.it_interval.tv_sec = 0;
        its.it_interval.tv_nsec = 0;
        timerfd_settime(tfd, 0, &its, nullptr);

        // ---------- 读取HTTP请求 ----------
        ssize_t n = coro_read(client_fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;  // 连接关闭、超时或出错
        buf[n] = '\0';

        // ---------- 解析请求行 ----------
        std::string method, path, version;
        if (!parse_request_line(buf, method, path, version)) {
            send_error(client_fd, 400, "Bad Request");
            if (should_close_connection(buf)) break;
            continue;
        }

        // ---------- 路由：仅支持GET和HEAD方法 ----------
        if (method != "GET" && method != "HEAD") {
            send_error(client_fd, 405, "Method Not Allowed");
            if (should_close_connection(buf)) break;
            continue;
        }

        // ---------- 构建完整文件路径 ----------
        std::string filepath;
        if (root_dir_.back() == '/') {
            filepath = root_dir_ + path.substr(1);
        } else {
            filepath = root_dir_ + path;
        }
        
        // 根路径自动映射到index.html
        if (filepath.back() == '/') {
            filepath += "index.html";
        }

        // ---------- 发送静态文件 ----------
        send_file(client_fd, filepath);

        // ---------- 检查是否需要关闭连接 ----------
        if (should_close_connection(buf)) break;
    }

    // ---------- 资源清理 ----------
    epoll_ctl(sched->get_epfd(), EPOLL_CTL_DEL, tfd, nullptr);
    close(tfd);
    close(client_fd);
}

// ==================== 协程版read ====================
ssize_t HttpServer::coro_read(int fd, void* buf, size_t n) {
    while (true) {
        ssize_t ret = recv(fd, buf, n, 0);
        if (ret > 0) return ret;
        if (ret == 0) return 0;  // 对端关闭连接
        
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 数据未就绪，挂起当前协程，等待EPOLLIN事件
            Coroutine::current->scheduler->yield(fd, EPOLLIN);
        } else {
            return -1;  // 其他错误
        }
    }
}

// ==================== 协程版write_all ====================
ssize_t HttpServer::coro_write_all(int fd, const void* buf, size_t n) {
    size_t sent = 0;
    const char* ptr = static_cast<const char*>(buf);
    
    while (sent < n) {
        ssize_t ret = send(fd, ptr + sent, n - sent, 0);
        if (ret > 0) {
            sent += ret;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 写缓冲区满，挂起等待EPOLLOUT事件
            Coroutine::current->scheduler->yield(fd, EPOLLOUT);
        } else {
            return -1;
        }
    }
    return sent;
}

// ==================== 发送HTTP响应 ====================
void HttpServer::send_response(int fd, int status_code,
                               const std::string& status_msg,
                               const std::string& content_type,
                               const std::string& body) {
    char header[512];
    int len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        status_code, status_msg.c_str(),
        content_type.c_str(), body.size());
    
    coro_write_all(fd, header, len);
    if (!body.empty()) {
        coro_write_all(fd, body.data(), body.size());
    }
}

void HttpServer::send_error(int fd, int status_code, const std::string& msg) {
    std::string body = "<html><body><h1>" + std::to_string(status_code) +
                       " " + msg + "</h1></body></html>";
    send_response(fd, status_code, msg, "text/html", body);
}

// ==================== 零拷贝发送静态文件 ====================
void HttpServer::send_file(int fd, const std::string& path) {
    int file_fd = open(path.c_str(), O_RDONLY);
    if (file_fd < 0) {
        send_error(fd, 404, "Not Found");
        return;
    }

    struct stat st;
    fstat(file_fd, &st);
    off_t file_size = st.st_size;

    // 发送HTTP响应头
    std::string mime = get_mime_type(path);
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        mime.c_str(), file_size);
    coro_write_all(fd, header, header_len);

    // 使用sendfile零拷贝发送文件内容
    off_t offset = 0;
    while (offset < file_size) {
        ssize_t sent = sendfile(fd, file_fd, &offset, file_size - offset);
        if (sent > 0) {
            continue;  // 发送成功，继续
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 发送缓冲区满，挂起协程等待可写
            Coroutine::current->scheduler->yield(fd, EPOLLOUT);
        } else {
            break;  // 真正错误，退出
        }
    }
    close(file_fd);
}

// ==================== HTTP请求解析 ====================
bool HttpServer::parse_request_line(const char* buf, std::string& method,
                                    std::string& path, std::string& version) {
    char m[16] = {0}, p[512] = {0}, v[16] = {0};
    if (sscanf(buf, "%15s %511s %15s", m, p, v) != 3) {
        return false;
    }
    method = m;
    path = p;
    version = v;

    // 防止目录遍历攻击
    if (path.find("..") != std::string::npos) {
        return false;
    }
    return true;
}

bool HttpServer::should_close_connection(const char* buf) {
    // 不区分大小写查找Connection头部
    const char* conn = strcasestr(buf, "Connection:");
    if (conn) {
        conn += 11;
        while (*conn == ' ') conn++;
        if (strncasecmp(conn, "close", 5) == 0) {
            return true;
        }
    }
    return false;
}

// ==================== MIME类型映射 ====================
std::string HttpServer::get_mime_type(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";

    std::string ext = path.substr(dot);
    // 扩展名转小写
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".css")  return "text/css";
    if (ext == ".js")   return "application/javascript";
    if (ext == ".json") return "application/json";
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif")  return "image/gif";
    if (ext == ".svg")  return "image/svg+xml";
    if (ext == ".ico")  return "image/x-icon";
    if (ext == ".txt")  return "text/plain";
    if (ext == ".xml")  return "application/xml";
    if (ext == ".pdf")  return "application/pdf";
    if (ext == ".zip")  return "application/zip";
    if (ext == ".mp4")  return "video/mp4";
    if (ext == ".webm") return "video/webm";
    return "application/octet-stream";
}
