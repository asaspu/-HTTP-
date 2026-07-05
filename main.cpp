#include "coroutine.h"
#include "http_server.h"
#include "socket_utils.h"
#include <thread>
#include <vector>
#include <iostream>

struct ServerConfig {
    int port = 8080;
    int threads = std::thread::hardware_concurrency();
    std::string root = "./www";
    int keepalive = 5;
};

void worker_thread(int listen_fd, HttpServer* server) {
    Scheduler sched;

    int epfd = sched.get_epfd();
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    std::vector<struct epoll_event> events(1024);

    while (true) {
        int timeout = sched.has_ready() ? 0 : -1;
        int nfds = epoll_wait(epfd, events.data(), events.size(), timeout);

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == listen_fd) {
                while (true) {
                    int client = accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK);
                    if (client < 0) break;
                    server->on_accept(&sched, client);
                }
            } else {
                auto* c = static_cast<Coroutine*>(events[i].data.ptr);
                if (c) {
                    sched.coro_enqueue(c);  // 需要将该函数改为 public
                }
            }
        }

        sched.dispatch();  // 调度所有就绪协程（需添加该方法）
    }
}

int main() {
    ServerConfig config;

    int listen_fd = create_listen_socket(config.port);
    if (listen_fd < 0) {
        std::cerr << "Failed to create listen socket\n";
        return 1;
    }

    HttpServer server(config.root, config.keepalive);
    std::cout << "Server listening on port " << config.port << "\n";

    std::vector<std::thread> workers;
    for (int i = 0; i < config.threads; ++i) {
        workers.emplace_back(worker_thread, listen_fd, &server);
    }

    for (auto& t : workers) t.join();
    close(listen_fd);
    return 0;
}
