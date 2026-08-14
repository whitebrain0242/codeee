#pragma once

#include "minimuduo/net/NonCopyable.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace minimuduo::net {

class Channel;
class Poller;

class EventLoop final : private NonCopyable {
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    void loop();
    void quit();

    void runInLoop(Functor callback);
    void queueInLoop(Functor callback);

    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);

    bool isInLoopThread() const noexcept;
    void assertInLoopThread() const;

private:
    void wakeup();
    void handleWakeupRead();
    void doPendingFunctors();

    using ChannelList = std::vector<Channel*>;

    std::atomic<bool> looping_;//是否进入事件循环
    std::atomic<bool> quit_;//退出标志
    std::atomic<bool> callingPendingFunctors_;//表示当前未执行待处理任务队列

    const std::thread::id threadId_;//当前线程ID

    std::unique_ptr<Poller> poller_;//创建Poller对象
    int wakeupFd_;
    std::unique_ptr<Channel> wakeupChannel_;
    ChannelList activeChannels_;

    std::mutex mutex_;
    std::vector<Functor> pendingFunctors_;
};

}  // namespace minimuduo::net
