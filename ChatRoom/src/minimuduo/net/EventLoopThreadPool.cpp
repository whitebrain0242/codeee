#include "minimuduo/net/EventLoopThreadPool.hpp"

#include "minimuduo/net/EventLoop.hpp"
#include "minimuduo/net/EventLoopThread.hpp"

#include <stdexcept>
#include <utility>

namespace minimuduo::net {

EventLoopThreadPool::EventLoopThreadPool(EventLoop *baseLoop, std::string name)
    : baseLoop_(baseLoop), name_(std::move(name)), started_(false),
      threadCount_(0), next_(0) {}

EventLoopThreadPool::~EventLoopThreadPool() = default;

void EventLoopThreadPool::setThreadNum(int threadCount) {
  if (started_) {
    throw std::logic_error("cannot change thread count after start");
  }
  if (threadCount < 0) {
    throw std::invalid_argument("thread count cannot be negative");
  }
  threadCount_ = threadCount;
}

void EventLoopThreadPool::start() {
  baseLoop_->assertInLoopThread();
  if (started_) {
    return;
  }

  started_ = true;
  for (int index = 0; index < threadCount_; ++index) {
    auto thread = std::make_unique<EventLoopThread>(name_ + "-io-" +
                                                    std::to_string(index));
    loops_.push_back(thread->startLoop());
    threads_.push_back(std::move(thread));
  }
}

EventLoop *EventLoopThreadPool::getNextLoop() {
  baseLoop_->assertInLoopThread();

  if (loops_.empty()) {
    return baseLoop_;
  }

  EventLoop *loop = loops_[next_];
  next_ = (next_ + 1U) % loops_.size(); // next自增并取模
  return loop;
}

const std::vector<EventLoop *> &
EventLoopThreadPool::getAllLoops() const noexcept {
  return loops_;
}

} // namespace minimuduo::net
