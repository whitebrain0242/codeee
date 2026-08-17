#include "minimuduo/net/TcpServer.hpp"

#include "minimuduo/net/Acceptor.hpp"
#include "minimuduo/net/EventLoop.hpp"
#include "minimuduo/net/EventLoopThreadPool.hpp"
#include "minimuduo/net/TcpConnection.hpp"
#include "minimuduo/net/TlsContext.hpp"
#include "minimuduo/net/SocketOptions.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace minimuduo::net {

TcpServer::TcpServer(
    EventLoop* loop,
    const sockaddr_in& listenAddress,
    std::string name)
    : loop_(loop),
      name_(std::move(name)),
      acceptor_(std::make_unique<Acceptor>(loop, listenAddress)),
      threadPool_(std::make_unique<EventLoopThreadPool>(loop, name_)),
      started_(false),
      nextConnectionId_(1) {
    acceptor_->setNewConnectionCallback(
        [this](int socketFd, const sockaddr_in& peerAddress) {
            newConnection(socketFd, peerAddress);
        });
}

TcpServer::~TcpServer() {
    loop_->assertInLoopThread();

    for (auto& item : connections_) {
        TcpConnectionPtr connection = item.second;
        item.second.reset();
        connection->getLoop()->runInLoop(
            [connection] { connection->connectDestroyed(); });
    }
}

void TcpServer::setThreadNum(int threadCount) {
    threadPool_->setThreadNum(threadCount);
}
//使用原子变量 started_ 确保只能在服务器启动前设置 TLS 上下文
void TcpServer::setTlsContext(std::shared_ptr<TlsServerContext> tlsContext) {
    if (started_.load()) {
        throw std::logic_error("setTlsContext must be called before start");
    }
    tlsContext_ = std::move(tlsContext);
}

void TcpServer::setConnectionCallback(ConnectionCallback callback) {
    connectionCallback_ = std::move(callback);
}

void TcpServer::setMessageCallback(MessageCallback callback) {
    messageCallback_ = std::move(callback);
}

void TcpServer::setWriteCompleteCallback(WriteCompleteCallback callback) {
    writeCompleteCallback_ = std::move(callback);
}

void TcpServer::start() {
    bool expected = false;
    //使用 compare_exchange_strong 原子地将 started_ 从 false 改为 true，若已经为 true 则直接返回（防止重复启动）。
    if (!started_.compare_exchange_strong(expected, true)) {
        return;
    }
    //将实际启动操作 runInLoop 到主 loop 线程中执行
    loop_->runInLoop([this] {
        threadPool_->start();
        if (!acceptor_->listening()) {
            acceptor_->listen();
        }
    });
}

void TcpServer::newConnection(int socketFd, const sockaddr_in& peerAddress) {
    loop_->assertInLoopThread();  //确保在主线程

    EventLoop* ioLoop = threadPool_->getNextLoop();  //轮询分配工作线程

    // 配置 TCP Keep-Alive（60秒空闲，15秒探测间隔，3次重试）
    std::string keepaliveError;
    if (!configureTcpKeepAlive(socketFd, 60, 15, 3, keepaliveError)) {
        std::cerr << "TCP keepalive configuration failed: " << keepaliveError << '\n';
    }

    // 获取本端地址（getsockname）
    sockaddr_in localAddress{};
    socklen_t addressLength = sizeof(localAddress);
    if (::getsockname(socketFd, reinterpret_cast<sockaddr*>(&localAddress), &addressLength) < 0) {
        ::close(socketFd);
        throw std::runtime_error("getsockname failed: " + std::string(std::strerror(errno)));
    }

    // 生成连接名称（如 "myServer-conn-1"）
    const std::string connectionName = name_ + "-conn-" + std::to_string(nextConnectionId_++);

    // 创建 TcpConnection 对象（传入 ioLoop、名称、fd、地址、TLS上下文）
    TcpConnectionPtr connection = std::make_shared<TcpConnection>(
        ioLoop, connectionName, socketFd, localAddress, peerAddress, tlsContext_);

    connections_[connectionName] = connection;  // 加入活动连接映射

    // 设置各种回调（从 TcpServer 拷贝给 connection）
    connection->setConnectionCallback(connectionCallback_);
    connection->setMessageCallback(messageCallback_);
    connection->setWriteCompleteCallback(writeCompleteCallback_);
    // 设置关闭回调，当连接关闭时调用 TcpServer::removeConnection
    connection->setCloseCallback(
        [this](const TcpConnectionPtr& closedConnection) {
            removeConnection(closedConnection);
        });

    // 将连接建立（注册事件）投递到工作线程执行
    ioLoop->runInLoop([connection] { connection->connectEstablished(); });
}

void TcpServer::removeConnection(const TcpConnectionPtr& connection) {
    loop_->runInLoop([this, connection] { removeConnectionInLoop(connection); });
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& connection) {
    loop_->assertInLoopThread();

    const std::size_t erased = connections_.erase(connection->name());
    if (erased == 0) {
        return;
    }

    EventLoop* ioLoop = connection->getLoop();
    ioLoop->queueInLoop([connection] { connection->connectDestroyed(); });
}

}  // namespace minimuduo::net
