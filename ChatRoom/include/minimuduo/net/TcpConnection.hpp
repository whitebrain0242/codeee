#pragma once

#include "minimuduo/net/Buffer.hpp"
#include "minimuduo/net/Callbacks.hpp"
#include "minimuduo/net/NonCopyable.hpp"
#include "minimuduo/net/TlsContext.hpp"

#include <any>
#include <atomic>
#include <functional>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <vector>

namespace minimuduo::net {

class Channel;
class EventLoop;

class TcpConnection final
    : private NonCopyable,
      public std::enable_shared_from_this<TcpConnection> {
public:
    using SendCompleteCallback =
        std::function<void()>;

    TcpConnection(
        EventLoop* loop,
        std::string name,
        int socketFd,
        const sockaddr_in& localAddress,
        const sockaddr_in& peerAddress,
        std::shared_ptr<TlsServerContext> tlsContext = {}
    );

    ~TcpConnection();

    EventLoop* getLoop() const noexcept;
    const std::string& name() const noexcept;
    bool connected() const noexcept;
    bool tlsEnabled() const noexcept;
    bool tlsHandshakeComplete() const noexcept;

    std::string localAddressText() const;
    std::string peerAddressText() const;
    std::string tlsCipherName() const;

    void send(const std::string& message);

    void send(
        const std::string& message,
        SendCompleteCallback completion
    );

    void shutdown();
    void forceClose();

    void setConnectionCallback(
        ConnectionCallback callback
    );

    void setMessageCallback(
        MessageCallback callback
    );

    void setWriteCompleteCallback(
        WriteCompleteCallback callback
    );

    void setCloseCallback(
        CloseCallback callback
    );

    void setContext(std::any context);
    const std::any& getContext() const noexcept;
    std::any* getMutableContext() noexcept;

    void connectEstablished();
    void connectDestroyed();

private:
    enum class State {//四种状态
        kDisconnected,//未连接
        kConnecting,//正在建立连接
        kConnected,//已连接，可读写
        kDisconnecting,//正在主动关闭，等待对端确认
    };

    void setState(State state) noexcept;

    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();

    void handlePlainRead();
    void handlePlainWrite();

    void driveTlsHandshake();
    void handleTlsRead();
    void flushTlsOutput();

    void notifyApplicationEstablished();//通知上层连接已经就绪
    void finishOutputCompletions();//完成所有带发送回调

    void sendInLoop(
        std::string message,
        SendCompleteCallback completion
    );

    void shutdownInLoop();
    void forceCloseInLoop();

    EventLoop* loop_;
    const std::string name_;
    std::atomic<State> state_;//确保切换进程是安全的
    int socketFd_;
    std::unique_ptr<Channel> channel_;
    const sockaddr_in localAddress_;//本地地址
    const sockaddr_in peerAddress_;//对端地址

    std::shared_ptr<TlsServerContext> tlsContext_;
    SslPtr ssl_;
    //握手状态和流量控制标志
    bool tlsHandshakeComplete_ = false;
    bool applicationEstablished_ = false;
    bool tlsWriteBlockedOnRead_ = false;
    bool tlsReadBlockedOnWrite_ = false;
    //用户回调
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    CloseCallback closeCallback_;

    Buffer inputBuffer_;
    Buffer outputBuffer_;
    std::vector<SendCompleteCallback>
        pendingSendCompletions_;
    std::any context_;//自定义上下文
};

}  // namespace minimuduo::net
