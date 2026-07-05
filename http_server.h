#pragma once
#include "coroutine.h"
#include <string>
#include <cstddef>

class HttpServer {
public:
    /**
     * @brief 构造HTTP服务器
     * @param root_dir 静态文件根目录
     * @param keepalive_timeout Keep-Alive超时时间(秒)
     */
    HttpServer(const std::string& root_dir, int keepalive_timeout);

    /**
     * @brief 新连接到达时调用，由主事件循环在accept后执行
     * @param sched 当前线程的协程调度器
     * @param client_fd 客户端socket文件描述符
     */
    void on_accept(Scheduler* sched, int client_fd);

private:
    /**
     * @brief 单个HTTP连接的协程处理主函数
     * @param sched 当前线程的协程调度器
     * @param client_fd 客户端socket文件描述符
     */
    void handle_connection(Scheduler* sched, int client_fd);

    /**
     * @brief 协程友好的读操作，数据未就绪时自动挂起协程
     * @param fd 要读取的文件描述符
     * @param buf 接收缓冲区
     * @param n 缓冲区大小
     * @return 成功返回读取字节数，0表示连接关闭，-1表示错误
     */
    ssize_t coro_read(int fd, void* buf, size_t n);

    /**
     * @brief 协程友好的写操作，写缓冲区满时自动挂起协程
     * @param fd 要写入的文件描述符
     * @param buf 发送缓冲区
     * @param n 要发送的字节数
     * @return 成功返回发送的总字节数，-1表示错误
     */
    ssize_t coro_write_all(int fd, const void* buf, size_t n);

    /**
     * @brief 发送标准HTTP响应
     */
    void send_response(int fd, int status_code,
                       const std::string& status_msg,
                       const std::string& content_type,
                       const std::string& body);

    /**
     * @brief 发送HTTP错误响应
     */
    void send_error(int fd, int status_code, const std::string& msg);

    /**
     * @brief 使用sendfile零拷贝发送静态文件
     * @param fd 客户端socket
     * @param path 本地文件路径
     */
    void send_file(int fd, const std::string& path);

    /**
     * @brief 解析HTTP请求行
     * @return 解析成功返回true，失败返回false
     */
    bool parse_request_line(const char* buf, std::string& method,
                            std::string& path, std::string& version);

    /**
     * @brief 检查请求头是否要求关闭连接
     * @return 要求关闭返回true，否则返回false
     */
    bool should_close_connection(const char* buf);

    /**
     * @brief 根据文件扩展名获取MIME类型
     */
    static std::string get_mime_type(const std::string& path);

    std::string root_dir_;      // 静态文件根目录
    int keepalive_timeout_;     // Keep-Alive超时时间(秒)
};
