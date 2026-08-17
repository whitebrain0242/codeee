#pragma once

#include "minimuduo/net/Callbacks.hpp"
#include "minimuduo/net/NonCopyable.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <string>

namespace minimuduo::net {

class Acceptor;
class EventLoop;
class EventLoopThreadPool;
class TlsServerContext;

class TcpServer final : private NonCopyable {
public:
    TcpServer(
        EventLoop* loop,
        const sockaddr_in& listenAddress,
        std::string name);
    ~TcpServer();

    void setThreadNum(int threadCount);
    //启用TLS传输加密，传入TLS配置--TLS握手
    void setTlsContext(
        std::shared_ptr<TlsServerContext> tlsContext
    );
    void setConnectionCallback(ConnectionCallback callback);//连接建立和关闭
    void setMessageCallback(MessageCallback callback);//受到消息
    void setWriteCompleteCallback(WriteCompleteCallback callback);//数据发送完成

    void start();

private:
    void newConnection(int socketFd, const sockaddr_in& peerAddress);//处理新的连接
    void removeConnection(const TcpConnectionPtr& connection);//断开一个连接
    void removeConnectionInLoop(const TcpConnectionPtr& connection);//真正删除连接

    EventLoop* loop_;
    const std::string name_;//服务器名称
    std::unique_ptr<Acceptor> acceptor_;
    std::unique_ptr<EventLoopThreadPool> threadPool_;
    std::shared_ptr<TlsServerContext> tlsContext_;

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;

    std::atomic<bool> started_;
    int nextConnectionId_;//防止重复启动
    std::map<std::string, TcpConnectionPtr> connections_;//所有活动连接
};

}  // namespace minimuduo::net
