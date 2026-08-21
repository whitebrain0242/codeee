#pragma once

#include "minimuduo/net/Buffer.hpp"
#include "minimuduo/net/Callbacks.hpp"
#include "minimuduo/net/NonCopyable.hpp"
#include "minimuduo/net/TlsContext.hpp"

#include <any>
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <utility>
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

    void sendFileFrame(
        std::string header,
        const std::filesystem::path& path,
        std::uint64_t offset,
        std::uint64_t byteCount,
        SendCompleteCallback completion = {}
    );

    bool zeroCopyFileSendAvailable() const noexcept;

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
    struct PendingFileSend;

    enum class State {
        kDisconnected,
        kConnecting,
        kConnected,
        kDisconnecting,
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

    void notifyApplicationEstablished();
    void finishOutputCompletions();

    void sendInLoop(
        std::string message,
        SendCompleteCallback completion
    );

    void sendFileFrameInLoop(
        std::string header,
        std::filesystem::path path,
        std::uint64_t offset,
        std::uint64_t byteCount,
        SendCompleteCallback completion
    );

    void startFileFrame(
        PendingFileSend pending
    );

    void flushFileSend();
    void finishActiveFileSend();
    void drainDeferredSends();
    void startNextQueuedFileFrame();

    void shutdownInLoop();
    void forceCloseInLoop();

    EventLoop* loop_;
    const std::string name_;
    std::atomic<State> state_;
    int socketFd_;
    std::unique_ptr<Channel> channel_;
    const sockaddr_in localAddress_;
    const sockaddr_in peerAddress_;

    std::shared_ptr<TlsServerContext> tlsContext_;
    SslPtr ssl_;
    bool tlsHandshakeComplete_ = false;
    bool applicationEstablished_ = false;
    bool tlsWriteBlockedOnRead_ = false;
    bool tlsReadBlockedOnWrite_ = false;

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    CloseCallback closeCallback_;

    Buffer inputBuffer_;
    Buffer outputBuffer_;
    std::vector<SendCompleteCallback>
        pendingSendCompletions_;

    struct PendingFileSend {
        int fd = -1;
        std::string header;
        std::filesystem::path path;
        std::uint64_t offset = 0U;
        std::uint64_t remaining = 0U;
        SendCompleteCallback completion;
    };

    std::optional<PendingFileSend>
        activeFileSend_;

    std::deque<PendingFileSend>
        queuedFileFrames_;

    std::deque<
        std::pair<
            std::string,
            SendCompleteCallback
        >
    > deferredSends_;

    std::any context_;
};

}  // namespace minimuduo::net
