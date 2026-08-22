#pragma once

#include "tls_config.hpp"
#include "minimuduo/net/NonCopyable.hpp"

#include <openssl/ssl.h>

#include <memory>
#include <string>
//SSL上下文层:封装SSL对象,对底层SSL的调用
namespace minimuduo::net {

struct SslDeleter {
    void operator()(SSL* ssl) const noexcept;
};

using SslPtr = std::unique_ptr<SSL, SslDeleter>;

class TlsServerContext final : private NonCopyable {
public:
    TlsServerContext() = default;
    ~TlsServerContext();

    //创建ctx,TLS版本，证书，私钥加载，检查
    bool initialize(
        const TlsServerConfig& config,
        std::string& error
    );

    SslPtr createSsl(//服务器创建SSL对象
        int socketFd,
        std::string& error
    ) const;

    bool initialized() const noexcept;//判断是否已初始化

private:
    SSL_CTX* context_ = nullptr;
};

class TlsClientContext final : private NonCopyable {
public:
    TlsClientContext() = default;
    ~TlsClientContext();

    bool initialize(
        const TlsClientConfig& config,
        std::string& error
    );

    SslPtr createSsl(
        int socketFd,
        const std::string& peerIdentity,
        std::string& error
    ) const;

    bool connectBlocking(
        SSL* ssl,
        std::string& error
    ) const;

    bool initialized() const noexcept;

private:
    SSL_CTX* context_ = nullptr;
    bool verify_peer_ = true;
};

std::string openssl_error_text(
    const std::string& operation
);

}  // namespace minimuduo::net
