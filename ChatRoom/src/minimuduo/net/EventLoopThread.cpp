#include "minimuduo/net/EventLoopThread.hpp"

#include "minimuduo/net/EventLoop.hpp"

#include <utility>

#if defined(__linux__)
#include <pthread.h>
#endif
//如何在一个独立线程中运行事件循环，并确保主线程能安全地拿到该循环的指针
namespace minimuduo::net {

EventLoopThread::EventLoopThread(std::string name)
    : loop_(nullptr),
      exiting_(false),
      name_(std::move(name)) {}

EventLoopThread::~EventLoopThread() {
    exiting_ = true;

    EventLoop* loop = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop = loop_;
    }

    if (loop != nullptr) {
        loop->quit();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

EventLoop* EventLoopThread::startLoop() {
    thread_ = std::thread([this] { threadFunction(); });

    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return loop_ != nullptr; });
    return loop_;
}

void EventLoopThread::threadFunction() {
    if (!name_.empty()) {
        const std::string shortName = name_.substr(0, 15);
        (void)::pthread_setname_np(::pthread_self(), shortName.c_str());
    }

    EventLoop loop;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = &loop;
        condition_.notify_one();
    }

    loop.loop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = nullptr;
    }
}

}  // namespace minimuduo::net
