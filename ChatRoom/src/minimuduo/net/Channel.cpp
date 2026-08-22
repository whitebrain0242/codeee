#include "minimuduo/net/Channel.hpp"

#include "minimuduo/net/EventLoop.hpp"

#include <cassert>
#include <sys/epoll.h>
#include <utility>

namespace minimuduo::net {

Channel::Channel(EventLoop *loop, int fd)
    : loop_(loop), fd_(fd), events_(kNoneEvent), revents_(0), index_(-1),
      tied_(false) {}

Channel::~Channel() = default;
// 保证执行的时候上层换活着，再执行回调，避免访问野指针崩溃
void Channel::handleEvent() {
  if (!tied_) { // 如果是acceptor，不用依赖外部生命周期对象，没有风险
    handleEventWithGuard();
    return;
  }

  const std::shared_ptr<void> guard =
      tie_.lock(); // 强引用，如果已经被销毁就是空的
  if (guard) {
    handleEventWithGuard();
  }
}

void Channel::setReadCallback(EventCallback callback) {
  readCallback_ = std::move(callback);
}

void Channel::setWriteCallback(EventCallback callback) {
  writeCallback_ = std::move(callback);
}

void Channel::setCloseCallback(EventCallback callback) {
  closeCallback_ = std::move(callback);
}

void Channel::setErrorCallback(EventCallback callback) {
  errorCallback_ = std::move(callback);
}

void Channel::tie(const std::shared_ptr<void> &owner) {
  tie_ = owner; // 绑定一个外部对象用于生命周期保护（tcpconnection）
  tied_ = true;
}

int Channel::fd() const noexcept { return fd_; }

std::uint32_t Channel::events() const noexcept { return events_; }

void Channel::setRevents(std::uint32_t revents) noexcept { revents_ = revents; }

bool Channel::isNoneEvent() const noexcept {
  return events_ == kNoneEvent; // 没有监听任何事件
}

bool Channel::isWriting() const noexcept {
  return (events_ & kWriteEvent) != 0;
}

void Channel::enableReading() {
  events_ |= kReadEvent;
  update();
}

void Channel::disableReading() {
  events_ &= ~kReadEvent;
  update();
}

void Channel::enableWriting() {
  events_ |= kWriteEvent;
  update();
}

void Channel::disableWriting() {
  events_ &= ~kWriteEvent;
  update();
}

void Channel::disableAll() {
  events_ = kNoneEvent; // 关闭所有事件监听
  update();
}

void Channel::remove() {
  assert(isNoneEvent());
  loop_->removeChannel(this);
}

int Channel::index() const noexcept { return index_; }

void Channel::setIndex(int index) noexcept { index_ = index; }

EventLoop *Channel::ownerLoop() noexcept { return loop_; }

void Channel::update() { // epollctl改变感兴趣的事件
  loop_->updateChannel(this);
}

void Channel::handleEventWithGuard() {
  // 处理挂起事件：对端关闭连接且没有数据可读
  if ((revents_ & EPOLLHUP) != 0 && (revents_ & EPOLLIN) == 0) {
    if (closeCallback_) {
      closeCallback_();
    }
    return; // 一旦处理的了刮起就不再处理其他事件
  }
  // 处理错误事件
  if ((revents_ & EPOLLERR) != 0) {
    if (errorCallback_) {
      errorCallback_();
    }
  }
  // 处理读事件
  if ((revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) != 0) {
    if (readCallback_) {
      readCallback_();
    }
  }
  // 处理写事件
  if ((revents_ & EPOLLOUT) != 0) {
    if (writeCallback_) {
      writeCallback_();
    }
  }
}

} // namespace minimuduo::net
