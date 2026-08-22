#include "minimuduo/net/Poller.hpp"

#include "minimuduo/net/Channel.hpp"
#include "minimuduo/net/EventLoop.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/epoll.h>
#include <unistd.h>

namespace minimuduo::net {

namespace {

[[noreturn]] void throwSystemError(const char *operation) {
  throw std::runtime_error(std::string(operation) +
                           " failed: " + std::strerror(errno));
}

} // namespace

Poller::Poller(EventLoop *loop)
    : ownerLoop_(loop), epollFd_(::epoll_create1(EPOLL_CLOEXEC)), events_(64) {
  if (epollFd_ < 0) {
    throwSystemError("epoll_create1");
  }
}

Poller::~Poller() { ::close(epollFd_); }

void Poller::poll(std::chrono::milliseconds timeout,
                  ChannelList *activeChannels) {
  ownerLoop_->assertInLoopThread();

  const int eventCount =
      ::epoll_wait(epollFd_, events_.data(), static_cast<int>(events_.size()),
                   static_cast<int>(timeout.count()));

  if (eventCount > 0) {
    fillActiveChannels(eventCount, activeChannels);
    if (static_cast<std::size_t>(eventCount) == events_.size()) {
      events_.resize(events_.size() * 2U);
    }
    return;
  }

  if (eventCount < 0 && errno != EINTR) {
    throwSystemError("epoll_wait");
  }
}

void Poller::updateChannel(Channel *channel) {
  ownerLoop_->assertInLoopThread();

  const int index = channel->index();
  if (index == kNew || index == kDeleted) {
    channel->setIndex(kAdded); // 注册
    update(EPOLL_CTL_ADD, channel);
    return;
  }

  if (channel->isNoneEvent()) {
    update(EPOLL_CTL_DEL, channel);
    channel->setIndex(kDeleted);
  } else {
    update(EPOLL_CTL_MOD, channel);
  }
}

void Poller::removeChannel(Channel *channel) {
  ownerLoop_->assertInLoopThread();

  const int index = channel->index();
  if (index == kAdded) {
    update(EPOLL_CTL_DEL, channel);
  }
  channel->setIndex(kNew);
}

void Poller::fillActiveChannels(int eventCount,
                                ChannelList *activeChannels) const {
  for (int index = 0; index < eventCount; ++index) {
    auto *channel = static_cast<Channel *>(events_[index].data.ptr);
    channel->setRevents(events_[index].events);
    activeChannels->push_back(channel);
  }
}

void Poller::update(int operation, Channel *channel) {
  epoll_event event{};
  event.events = channel->events();
  event.data.ptr = channel;

  if (::epoll_ctl(epollFd_, operation, channel->fd(), &event) < 0) {
    if (operation == EPOLL_CTL_DEL && errno == ENOENT) {
      return;
    }
    throwSystemError("epoll_ctl");
  }
}

} // namespace minimuduo::net
