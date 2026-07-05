#include "coroutine.h"
#include <iostream>
#include <cassert>
#include <unistd.h>
#include <sys/epoll.h>

thread_local Coroutine* Coroutine::current = nullptr;

Scheduler::Scheduler() {
    epfd_ = epoll_create1(0);
    assert(epfd_ >= 0);
    events_.resize(1024);
}

Scheduler::~Scheduler() {
    close(epfd_);
}

void Scheduler::spawn(std::function<void()> func) {
    auto* c = new Coroutine(this, std::move(func));
    getcontext(&c->ctx);
    c->ctx.uc_stack.ss_sp = c->stack.data();
    c->ctx.uc_stack.ss_size = c->stack.size();
    c->ctx.uc_link = &main_ctx_;
    makecontext(&c->ctx, coro_entry, 0);
    coro_enqueue(c);
}

void Scheduler::yield(int fd, int epoll_events) {
    Coroutine* curr = Coroutine::current;
    if (!curr) return;

    epoll_event ev{};
    ev.events = epoll_events | EPOLLONESHOT;
    ev.data.ptr = curr;
    epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);

    if (epoll_events & EPOLLIN) curr->state = CoroState::WAIT_READ;
    else if (epoll_events & EPOLLOUT) curr->state = CoroState::WAIT_WRITE;

    swapcontext(&curr->ctx, &main_ctx_);
}

// 新增：协程入队实现（现在是 public）
void Scheduler::coro_enqueue(Coroutine* c) {
    if (!ready_head_) {
        ready_head_ = ready_tail_ = c;
    } else {
        ready_tail_->next = c;
        ready_tail_ = c;
    }
    c->state = CoroState::READY;
    c->next = nullptr;
}

Coroutine* Scheduler::coro_dequeue() {
    if (!ready_head_) return nullptr;
    Coroutine* c = ready_head_;
    ready_head_ = ready_head_->next;
    if (!ready_head_) ready_tail_ = nullptr;
    c->next = nullptr;
    return c;
}

// 新增：判断是否有就绪协程
bool Scheduler::has_ready() const {
    return ready_head_ != nullptr;
}

// 新增：调度并执行所有就绪协程
void Scheduler::dispatch() {
    while (Coroutine* c = coro_dequeue()) {
        Coroutine::current = c;
        swapcontext(&main_ctx_, &c->ctx);
        Coroutine::current = nullptr;

        if (c->state == CoroState::DEAD) {
            delete c;
        }
    }
}

void Scheduler::run(int listen_fd) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd, &ev);

    while (true) {
        int timeout = has_ready() ? 0 : -1;
        int nfds = epoll_wait(epfd_, events_.data(), events_.size(), timeout);

        for (int i = 0; i < nfds; ++i) {
            epoll_event& e = events_[i];
            if (e.data.fd == listen_fd) {
                // 这里可以放 accept 逻辑，也可以交给外部 worker_thread 处理
            } else {
                auto* c = static_cast<Coroutine*>(e.data.ptr);
                if (c) {
                    epoll_ctl(epfd_, EPOLL_CTL_DEL, c->fd, nullptr);
                    coro_enqueue(c);
                }
            }
        }

        dispatch();
    }
}

void Scheduler::coro_entry() {
    Coroutine* c = Coroutine::current;
    if (c && c->func) {
        c->func();
    }
    if (c) {
        c->state = CoroState::DEAD;
    }
    swapcontext(&c->ctx, &c->scheduler->main_ctx_);
}
