#pragma once

#include "minimuduo/net/NonCopyable.hpp"

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace minimuduo::net {

class EventLoop;

class EventLoopThread final : private NonCopyable {
public:
    explicit EventLoopThread(std::string name);
    ~EventLoopThread();

    EventLoop* startLoop();

private:
    void threadFunction();

    EventLoop* loop_;
    bool exiting_;//退出标志
    std::string name_;//线程名字--为了调试和日志
    std::thread thread_;//线程实体
    std::mutex mutex_;
    std::condition_variable condition_;//条件变量
};

}  // namespace minimuduo::net
