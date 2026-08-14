#include "minimuduo/net/EventLoop.hpp"

#include "minimuduo/net/Channel.hpp"
#include "minimuduo/net/Poller.hpp"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

namespace minimuduo::net {

namespace {

int createEventFd() {
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);//用来跨线程唤醒，本质是一个计数器
    if (fd < 0) {
        throw std::runtime_error(
            std::string("eventfd failed: ") + std::strerror(errno));
    }
    return fd;
}

}  // namespace

EventLoop::EventLoop()
    : looping_(false),
      quit_(false),
      callingPendingFunctors_(false),
      threadId_(std::this_thread::get_id()),
      poller_(std::make_unique<Poller>(this)),
      wakeupFd_(createEventFd()),
      wakeupChannel_(std::make_unique<Channel>(this, wakeupFd_)) {
    wakeupChannel_->setReadCallback([this] { handleWakeupRead(); });
    wakeupChannel_->enableReading();
}

EventLoop::~EventLoop() {
    assertInLoopThread();
    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
    ::close(wakeupFd_);
}

void EventLoop::loop() {
    assertInLoopThread();

    bool expected = false;
    if (!looping_.compare_exchange_strong(expected, true)) {
        throw std::logic_error("EventLoop::loop called while already looping");
    }

    quit_.store(false);
    while (!quit_.load()) {
        activeChannels_.clear();
        poller_->poll(std::chrono::milliseconds(10000), &activeChannels_);

        for (Channel* channel : activeChannels_) {
            channel->handleEvent();
        }

        doPendingFunctors();
    }

    looping_.store(false);
}

void EventLoop::quit() {
    quit_.store(true);
    if (!isInLoopThread()) {
        wakeup();
    }
}

void EventLoop::runInLoop(Functor callback) {
    if (isInLoopThread()) {
        callback();
    } else {
        queueInLoop(std::move(callback));
    }
}

void EventLoop::queueInLoop(Functor callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingFunctors_.push_back(std::move(callback));
    }

    if (!isInLoopThread() || callingPendingFunctors_.load()) {
        wakeup();
    }
}

void EventLoop::updateChannel(Channel* channel) {
    assert(channel->ownerLoop() == this);
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel) {
    assert(channel->ownerLoop() == this);
    poller_->removeChannel(channel);
}

bool EventLoop::isInLoopThread() const noexcept {
    return threadId_ == std::this_thread::get_id();
}

void EventLoop::assertInLoopThread() const {
    if (!isInLoopThread()) {
        throw std::logic_error("EventLoop used from a foreign thread");
    }
}

void EventLoop::wakeup() {
    const std::uint64_t one = 1;
    const ssize_t written = ::write(wakeupFd_, &one, sizeof(one));
    if (written < 0 && errno != EAGAIN) {
        throw std::runtime_error(
            std::string("eventfd write failed: ") + std::strerror(errno));
    }
}

void EventLoop::handleWakeupRead() {
    std::uint64_t one = 0;
    const ssize_t readBytes = ::read(wakeupFd_, &one, sizeof(one));
    if (readBytes < 0 && errno != EAGAIN) {
        throw std::runtime_error(
            std::string("eventfd read failed: ") + std::strerror(errno));
    }
}

void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPendingFunctors_.store(true);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    }

    for (const Functor& functor : functors) {
        functor();
    }

    callingPendingFunctors_.store(false);
}

}  // namespace minimuduo::net
