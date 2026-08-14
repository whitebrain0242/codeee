#pragma once

#include "minimuduo/net/NonCopyable.hpp"

#include <memory>
#include <string>
#include <vector>

namespace minimuduo::net {

class EventLoop;
class EventLoopThread;

class EventLoopThreadPool final : private NonCopyable {
public:
    EventLoopThreadPool(EventLoop* baseLoop, std::string name);
    ~EventLoopThreadPool();

    void setThreadNum(int threadCount);
    void start();

    EventLoop* getNextLoop();
    const std::vector<EventLoop*>& getAllLoops() const noexcept;

private:
    EventLoop* baseLoop_;//主事件循环
    std::string name_;//线程池名称前缀-用来给线程命名
    bool started_;//启动与否
    int threadCount_;//子线程数量
    std::size_t next_;//轮询：分配下一个eVentLoop索引
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};

}  // namespace minimuduo::net
