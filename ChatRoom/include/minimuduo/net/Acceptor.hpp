#pragma once

#include "minimuduo/net/NonCopyable.hpp"

#include <functional>
#include <memory>
#include <netinet/in.h>

namespace minimuduo::net {

class Channel;
class EventLoop;

class Acceptor final : private NonCopyable {
public:
    using NewConnectionCallback =
        std::function<void(int socketFd, const sockaddr_in& peerAddress)>;

    Acceptor(EventLoop* loop, const sockaddr_in& listenAddress);
    ~Acceptor();

    void setNewConnectionCallback(NewConnectionCallback callback);
    void listen();
    bool listening() const noexcept;

private:
    void handleRead();

    EventLoop* loop_;
    int acceptSocket_;//监听socket
    std::unique_ptr<Channel> acceptChannel_;//处理读事件
    NewConnectionCallback newConnectionCallback_;//回调，处理新连接
    bool listening_;//是否开始监听
};

}  // namespace minimuduo::net
