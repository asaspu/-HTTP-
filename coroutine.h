#pragma once
#include <ucontext.h>
#include <functional>
#include <memory>
#include <vector>
#include <sys/epoll.h>

constexpr size_t CORO_STACK_SIZE = 64 * 1024;   // 64KB

class Scheduler;  // 前向声明

// 协程状态
enum class CoroState {
    DEAD,
    READY,
    WAIT_READ,
    WAIT_WRITE,
    TIMEOUT
};

// 协程控制块
struct Coroutine {
    ucontext_t ctx;
    std::vector<char> stack;            // 独立栈
    CoroState state = CoroState::READY;
    int fd = -1;                        // 关联的 socket 或 timerfd
    std::function<void()> func;         // 协程体
    Scheduler* scheduler = nullptr;     // 所属调度器
    Coroutine* next = nullptr;          // 调度链表

    // 用于在 swapcontext 前临时保存当前协程指针
    static thread_local Coroutine* current;

    Coroutine(Scheduler* sched, std::function<void()> f)
        : stack(CORO_STACK_SIZE), func(std::move(f)), scheduler(sched) {}
};

// 协程调度器（每个工作线程一个实例）
class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    // 创建新协程并加入就绪队列
    void spawn(std::function<void()> func);

    // 当前协程让出 CPU，等待 fd 上的 events 事件
    void yield(int fd, int epoll_events);

    // 启动事件循环（永不返回）
    void run(int listen_fd);
    
    int get_epfd() const { return epfd_; }

    // --- 新增/修改的部分 ---
    // 1. 暴露协程入队接口，供外部使用
    void coro_enqueue(Coroutine* c);

    // 2. 判断是否有就绪协程
    bool has_ready() const;

    // 3. 调度并执行所有就绪协程
    void dispatch();
    // ----------------------

private:
    Coroutine* coro_dequeue();
    static void coro_entry();   // 协程入口函数

    int epfd_;
    ucontext_t main_ctx_;       // 主协程上下文
    Coroutine* ready_head_ = nullptr;
    Coroutine* ready_tail_ = nullptr;
    std::vector<struct epoll_event> events_;
};
