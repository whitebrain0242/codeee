#include "minimuduo/net/TcpConnection.hpp"

#include "minimuduo/net/Channel.hpp"
#include "minimuduo/net/EventLoop.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace minimuduo::net {

namespace {

std::string addressToText(
    const sockaddr_in& address
) {//将地址结构题转换成可读的字符串
    char ip[INET_ADDRSTRLEN]{};
    //二进制转淀粉十进制
    if (::inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip)) == nullptr) {
        return "unknown";
    }
    return std::string(ip) + ":" + std::to_string(ntohs(address.sin_port));
}

}  // namespace

TcpConnection::TcpConnection(
    EventLoop* loop,
    std::string name,
    int socketFd,
    const sockaddr_in& localAddress,
    const sockaddr_in& peerAddress,
    std::shared_ptr<TlsServerContext> tlsContext
)
    : loop_(loop),
      name_(std::move(name)),
      state_(State::kConnecting),
      socketFd_(socketFd),
      channel_(
          std::make_unique<Channel>(
              loop,
              socketFd
          )
      ),
      localAddress_(localAddress),
      peerAddress_(peerAddress),
      tlsContext_(std::move(tlsContext)) {
      //把channel的四个时间绑定到ctpconnection的处理函数上
      channel_->setReadCallback([this] { handleRead(); });
      channel_->setWriteCallback([this] { handleWrite(); });
      channel_->setCloseCallback([this] { handleClose(); });
      channel_->setErrorCallback([this] { handleError(); });
    //TLS初始化
    if (tlsContext_ != nullptr) {
    std::string error;
    ssl_ = tlsContext_->createSsl(socketFd_, error);
    if (!ssl_) {
        throw std::runtime_error("TLS SSL_new failed for " + name_ + ": " + error);
    }
}
}

TcpConnection::~TcpConnection() {
    ssl_.reset();
    ::close(socketFd_);
}
//getter
//状态与属性查询
EventLoop* TcpConnection::getLoop() const noexcept {
    return loop_;
}

const std::string& TcpConnection::name() const noexcept {
    return name_;
}

bool TcpConnection::connected() const noexcept {
    return state_.load() ==
        State::kConnected;
}

bool TcpConnection::tlsEnabled() const noexcept {
    return ssl_ != nullptr;
}

bool TcpConnection::tlsHandshakeComplete() const noexcept {
    return !tlsEnabled() || tlsHandshakeComplete_;
}

std::string TcpConnection::localAddressText() const {
    return addressToText(localAddress_);
}

std::string TcpConnection::peerAddressText() const {
    return addressToText(peerAddress_);
}
//返回TLS加密套件名称
std::string TcpConnection::tlsCipherName() const {
    if (!tlsHandshakeComplete_ || ssl_ == nullptr) return {};
    const char* cipher = SSL_get_cipher_name(ssl_.get());
    return cipher == nullptr ? std::string() : std::string(cipher);
}

void TcpConnection::send(//重载，无完成回调
    const std::string& message
) {
    send(message, {});
}

void TcpConnection::send(
    const std::string& message,
    SendCompleteCallback completion
) {
    if (state_.load() !=
        State::kConnected) {
        return;
    }

    if (loop_->isInLoopThread()) {
        sendInLoop(
            message,
            std::move(completion)
        );
        return;
    }

    const std::shared_ptr<TcpConnection>
        self =
            shared_from_this();

    loop_->runInLoop(
        [
            self,
            message,
            completion =
                std::move(completion)
        ]() mutable {
            self->sendInLoop(
                message,
                std::move(completion)
            );
        }
    );
}

void TcpConnection::shutdown() {
    State expected =
        State::kConnected;

    if (state_.compare_exchange_strong(
            expected,
            State::kDisconnecting
        )) {
        const std::shared_ptr<TcpConnection>
            self =
                shared_from_this();

        loop_->runInLoop(
            [self] {
                self->shutdownInLoop();
            }
        );
    }
}

void TcpConnection::forceClose() {
    const State state =
        state_.load();

    if (state != State::kConnected &&
        state != State::kDisconnecting) {
        return;
    }

    setState(State::kDisconnecting);

    const std::shared_ptr<TcpConnection>
        self =
            shared_from_this();

    loop_->queueInLoop(
        [self] {
            self->forceCloseInLoop();
        }
    );
}

void TcpConnection::setConnectionCallback(
    ConnectionCallback callback
) {
    connectionCallback_ =
        std::move(callback);
}

void TcpConnection::setMessageCallback(
    MessageCallback callback
) {
    messageCallback_ =
        std::move(callback);
}

void TcpConnection::setWriteCompleteCallback(
    WriteCompleteCallback callback
) {
    writeCompleteCallback_ =
        std::move(callback);
}

void TcpConnection::setCloseCallback(
    CloseCallback callback
) {
    closeCallback_ =
        std::move(callback);
}

void TcpConnection::setContext(
    std::any context
) {
    context_ =
        std::move(context);
}

const std::any&
TcpConnection::getContext() const noexcept {
    return context_;
}

std::any*
TcpConnection::getMutableContext() noexcept {
    return &context_;
}

void TcpConnection::connectEstablished() {
    loop_->assertInLoopThread();

    setState(State::kConnected);
    channel_->tie(
        shared_from_this()
    );
    channel_->enableReading();

    if (tlsEnabled()) {
        driveTlsHandshake();
    } else {
        notifyApplicationEstablished();
    }
}

void TcpConnection::connectDestroyed() {
    loop_->assertInLoopThread();

    const State state =
        state_.load();

    if (state == State::kConnected ||
        state == State::kDisconnecting) {
        setState(State::kDisconnected);
        channel_->disableAll();

        if (applicationEstablished_ &&
            connectionCallback_) {
            connectionCallback_(
                shared_from_this()
            );
        }
    }

    channel_->remove();
}

void TcpConnection::setState(
    State state
) noexcept {
    state_.store(state);
}

void TcpConnection::handleRead() {
    loop_->assertInLoopThread();

    if (tlsEnabled()) {
        if (!tlsHandshakeComplete_) {
            driveTlsHandshake();
            return;
        }

        handleTlsRead();

        if (tlsWriteBlockedOnRead_ &&
            state_.load() ==
                State::kConnected) {
            tlsWriteBlockedOnRead_ = false;
            flushTlsOutput();
        }

        return;
    }

    handlePlainRead();
}

void TcpConnection::handleWrite() {
    loop_->assertInLoopThread();

    if (tlsEnabled()) {
        if (!tlsHandshakeComplete_) {
            driveTlsHandshake();
            return;
        }

        if (tlsReadBlockedOnWrite_) {
            tlsReadBlockedOnWrite_ = false;
            handleTlsRead();

            if (state_.load() !=
                State::kConnected) {
                return;
            }
        }

        flushTlsOutput();
        return;
    }

    handlePlainWrite();
}

void TcpConnection::handlePlainRead() {
    int savedErrno = 0;

    const ssize_t bytesRead =
        inputBuffer_.readFd(
            socketFd_,
            &savedErrno
        );

    if (bytesRead > 0) {
        if (messageCallback_) {
            messageCallback_(
                shared_from_this(),
                &inputBuffer_
            );
        }
        return;
    }

    if (bytesRead == 0) {
        handleClose();
        return;
    }

    errno = savedErrno;

    if (errno != EAGAIN &&
        errno != EWOULDBLOCK &&
        errno != EINTR) {
        handleError();
        handleClose();
    }
}

void TcpConnection::handlePlainWrite() {
    if (!channel_->isWriting()) {
        return;
    }

    int savedErrno = 0;

    const ssize_t bytesWritten =
        outputBuffer_.writeFd(
            socketFd_,
            &savedErrno
        );

    if (bytesWritten > 0) {
        outputBuffer_.retrieve(
            static_cast<std::size_t>(
                bytesWritten
            )
        );

        if (outputBuffer_.readableBytes() ==
            0U) {
            channel_->disableWriting();
            finishOutputCompletions();

            if (state_.load() ==
                State::kDisconnecting) {
                shutdownInLoop();
            }
        }

        return;
    }

    errno = savedErrno;

    if (errno != EAGAIN &&
        errno != EWOULDBLOCK) {
        handleError();
    }
}

void TcpConnection::driveTlsHandshake() {
    loop_->assertInLoopThread();

    if (ssl_ == nullptr ||
        tlsHandshakeComplete_ ||
        state_.load() ==
            State::kDisconnected) {
        return;
    }

    ERR_clear_error();

    const int result =
        SSL_accept(ssl_.get());

    if (result == 1) {
        tlsHandshakeComplete_ = true;
        tlsWriteBlockedOnRead_ = false;
        tlsReadBlockedOnWrite_ = false;

        if (channel_->isWriting() &&
            outputBuffer_.readableBytes() ==
                0U) {
            channel_->disableWriting();
        }

        notifyApplicationEstablished();

        if (outputBuffer_.readableBytes() >
            0U) {
            flushTlsOutput();
        }

        if (SSL_pending(ssl_.get()) > 0 &&
            state_.load() ==
                State::kConnected) {
            handleTlsRead();
        }

        return;
    }

    const int sslError =
        SSL_get_error(
            ssl_.get(),
            result
        );

    if (sslError ==
        SSL_ERROR_WANT_READ) {
        channel_->enableReading();

        if (channel_->isWriting() &&
            outputBuffer_.readableBytes() ==
                0U) {
            channel_->disableWriting();
        }

        return;
    }

    if (sslError ==
        SSL_ERROR_WANT_WRITE) {
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
        return;
    }

    if (sslError ==
            SSL_ERROR_ZERO_RETURN ||
        sslError ==
            SSL_ERROR_SYSCALL) {
        handleClose();
        return;
    }

    handleError();
    handleClose();
}

void TcpConnection::handleTlsRead() {
    loop_->assertInLoopThread();

    if (!tlsHandshakeComplete_ ||
        ssl_ == nullptr) {
        return;
    }

    bool receivedApplicationData = false;

    while (true) {
        char buffer[16 * 1024]{};

        ERR_clear_error();

        const int result =
            SSL_read(
                ssl_.get(),
                buffer,
                static_cast<int>(
                    sizeof(buffer)
                )
            );

        if (result > 0) {
            inputBuffer_.append(
                buffer,
                static_cast<std::size_t>(
                    result
                )
            );
            receivedApplicationData = true;
            continue;
        }

        const int sslError =
            SSL_get_error(
                ssl_.get(),
                result
            );

        if (sslError ==
            SSL_ERROR_WANT_READ) {
            break;
        }

        if (sslError ==
            SSL_ERROR_WANT_WRITE) {
            tlsReadBlockedOnWrite_ = true;

            if (!channel_->isWriting()) {
                channel_->enableWriting();
            }

            break;
        }

        if (sslError ==
            SSL_ERROR_ZERO_RETURN) {
            handleClose();
            return;
        }

        if (sslError ==
            SSL_ERROR_SYSCALL &&
            errno == EINTR) {
            continue;
        }

        handleError();
        handleClose();
        return;
    }

    if (receivedApplicationData &&
        messageCallback_ &&
        state_.load() ==
            State::kConnected) {
        messageCallback_(
            shared_from_this(),
            &inputBuffer_
        );
    }
}

void TcpConnection::flushTlsOutput() {
    loop_->assertInLoopThread();

    if (!tlsHandshakeComplete_ ||
        ssl_ == nullptr ||
        state_.load() ==
            State::kDisconnected) {
        return;
    }

    while (outputBuffer_.readableBytes() >
           0U) {
        const std::size_t available =
            outputBuffer_.readableBytes();

        const int writeSize =
            static_cast<int>(
                std::min<std::size_t>(
                    available,
                    static_cast<std::size_t>(
                        INT_MAX
                    )
                )
            );

        ERR_clear_error();

        const int result =
            SSL_write(
                ssl_.get(),
                outputBuffer_.peek(),
                writeSize
            );

        if (result > 0) {
            outputBuffer_.retrieve(
                static_cast<std::size_t>(
                    result
                )
            );
            continue;
        }

        const int sslError =
            SSL_get_error(
                ssl_.get(),
                result
            );

        if (sslError ==
            SSL_ERROR_WANT_WRITE) {
            if (!channel_->isWriting()) {
                channel_->enableWriting();
            }
            return;
        }

        if (sslError ==
            SSL_ERROR_WANT_READ) {
            tlsWriteBlockedOnRead_ = true;

            if (channel_->isWriting()) {
                channel_->disableWriting();
            }

            channel_->enableReading();
            return;
        }

        if (sslError ==
            SSL_ERROR_SYSCALL &&
            errno == EINTR) {
            continue;
        }

        handleError();
        handleClose();
        return;
    }

    tlsWriteBlockedOnRead_ = false;

    if (channel_->isWriting() &&
        !tlsReadBlockedOnWrite_) {
        channel_->disableWriting();
    }

    finishOutputCompletions();

    if (state_.load() ==
        State::kDisconnecting) {
        shutdownInLoop();
    }
}

void TcpConnection::notifyApplicationEstablished() {
    if (applicationEstablished_) {
        return;
    }

    applicationEstablished_ = true;

    if (connectionCallback_) {
        connectionCallback_(
            shared_from_this()
        );
    }
}

void TcpConnection::finishOutputCompletions() {
    if (writeCompleteCallback_) {
        const std::shared_ptr<TcpConnection>
            self =
                shared_from_this();

        loop_->queueInLoop(
            [
                self,
                callback =
                    writeCompleteCallback_
            ] {
                callback(self);
            }
        );
    }

    std::vector<SendCompleteCallback>
        completions;

    completions.swap(
        pendingSendCompletions_
    );

    for (SendCompleteCallback& completion :
         completions) {
        if (completion) {
            loop_->queueInLoop(
                std::move(completion)
            );
        }
    }
}

void TcpConnection::handleClose() {
    loop_->assertInLoopThread();

    const State state =
        state_.load();

    if (state == State::kDisconnected) {
        return;
    }

    setState(State::kDisconnected);
    channel_->disableAll();

    const std::shared_ptr<TcpConnection>
        guard =
            shared_from_this();

    if (applicationEstablished_ &&
        connectionCallback_) {
        connectionCallback_(guard);
    }

    if (closeCallback_) {
        closeCallback_(guard);
    }
}

void TcpConnection::handleError() {
    int socketError = 0;
    socklen_t length =
        sizeof(socketError);

    if (::getsockopt(
            socketFd_,
            SOL_SOCKET,
            SO_ERROR,
            &socketError,
            &length
        ) < 0) {
        socketError = errno;
    }

    (void)socketError;
}

void TcpConnection::sendInLoop(
    std::string message,
    SendCompleteCallback completion
) {
    loop_->assertInLoopThread();

    if (state_.load() ==
        State::kDisconnected) {
        return;
    }

    if (tlsEnabled()) {
        outputBuffer_.append(message);

        if (completion) {
            pendingSendCompletions_.push_back(
                std::move(completion)
            );
        }

        if (tlsHandshakeComplete_) {
            flushTlsOutput();
        }

        return;
    }

    ssize_t bytesWritten = 0;
    std::size_t remaining =
        message.size();
    bool faultError = false;

    if (!channel_->isWriting() &&
        outputBuffer_.readableBytes() ==
            0U) {
        bytesWritten = ::send(
            socketFd_,
            message.data(),
            message.size(),
            MSG_NOSIGNAL
        );

        if (bytesWritten >= 0) {
            remaining =
                message.size() -
                static_cast<std::size_t>(
                    bytesWritten
                );

            if (remaining == 0U) {
                if (writeCompleteCallback_) {
                    const std::shared_ptr<
                        TcpConnection
                    > self =
                        shared_from_this();

                    loop_->queueInLoop(
                        [
                            self,
                            callback =
                                writeCompleteCallback_
                        ] {
                            callback(self);
                        }
                    );
                }

                if (completion) {
                    loop_->queueInLoop(
                        std::move(completion)
                    );
                }
            }
        } else {
            bytesWritten = 0;

            if (errno != EAGAIN &&
                errno != EWOULDBLOCK) {
                if (errno == EPIPE ||
                    errno == ECONNRESET) {
                    faultError = true;
                }
            }
        }
    }

    if (!faultError &&
        remaining > 0U) {
        outputBuffer_.append(
            message.data() +
                static_cast<std::size_t>(
                    bytesWritten
                ),
            remaining
        );

        if (completion) {
            pendingSendCompletions_.push_back(
                std::move(completion)
            );
        }

        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
    }
}

void TcpConnection::shutdownInLoop() {
    loop_->assertInLoopThread();

    if (channel_->isWriting() ||
        outputBuffer_.readableBytes() >
            0U) {
        return;
    }

    if (ssl_ != nullptr &&
        tlsHandshakeComplete_) {
        ERR_clear_error();
        (void)SSL_shutdown(ssl_.get());
    }

    (void)::shutdown(
        socketFd_,
        SHUT_WR
    );
}

void TcpConnection::forceCloseInLoop() {
    loop_->assertInLoopThread();

    if (state_.load() ==
            State::kConnected ||
        state_.load() ==
            State::kDisconnecting) {
        handleClose();
    }
}

}  // namespace minimuduo::net
