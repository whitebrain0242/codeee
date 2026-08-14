#include "minimuduo/net/Acceptor.hpp"

#include "minimuduo/net/Channel.hpp"
#include "minimuduo/net/EventLoop.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace minimuduo::net {

namespace {

[[noreturn]] void throwSocketError(const char* operation) {
    throw std::runtime_error(
        std::string(operation) + " failed: " + std::strerror(errno));
}

}  // namespace

Acceptor::Acceptor(EventLoop* loop, const sockaddr_in& listenAddress)
    : loop_(loop),
      acceptSocket_(::socket(
          AF_INET,
          SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
          0)),
      acceptChannel_(nullptr),
      listening_(false) {
    if (acceptSocket_ < 0) {
        throwSocketError("socket");
    }

    int enabled = 1;
    if (::setsockopt(
            acceptSocket_,
            SOL_SOCKET,
            SO_REUSEADDR,
            &enabled,
            sizeof(enabled)) < 0) {
        ::close(acceptSocket_);
        throwSocketError("setsockopt(SO_REUSEADDR)");
    }

    if (::bind(
            acceptSocket_,
            reinterpret_cast<const sockaddr*>(&listenAddress),
            sizeof(listenAddress)) < 0) {
        ::close(acceptSocket_);
        throwSocketError("bind");
    }

    acceptChannel_ = std::make_unique<Channel>(loop_, acceptSocket_);
    acceptChannel_->setReadCallback([this] { handleRead(); });
}

Acceptor::~Acceptor() {
    loop_->assertInLoopThread();
    if (listening_) {
        acceptChannel_->disableAll();
        acceptChannel_->remove();
    }
    ::close(acceptSocket_);
}

void Acceptor::setNewConnectionCallback(NewConnectionCallback callback) {
    newConnectionCallback_ = std::move(callback);
}

void Acceptor::listen() {
    loop_->assertInLoopThread();
    if (listening_) {
        return;
    }

    if (::listen(acceptSocket_, SOMAXCONN) < 0) {
        throwSocketError("listen");
    }

    listening_ = true;
    acceptChannel_->enableReading();
}

bool Acceptor::listening() const noexcept {
    return listening_;
}

void Acceptor::handleRead() {
    loop_->assertInLoopThread();

    while (true) {
        sockaddr_in peerAddress{};
        socklen_t addressLength = sizeof(peerAddress);

        const int socketFd = ::accept4(
            acceptSocket_,
            reinterpret_cast<sockaddr*>(&peerAddress),
            &addressLength,
            SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (socketFd >= 0) {
            if (newConnectionCallback_) {
                newConnectionCallback_(socketFd, peerAddress);
            } else {
                ::close(socketFd);
            }
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        throwSocketError("accept4");
    }
}

}  // namespace minimuduo::net
