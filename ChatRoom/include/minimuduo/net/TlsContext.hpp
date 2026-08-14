#pragma once

#include "tls_config.hpp"
#include "minimuduo/net/NonCopyable.hpp"

#include <openssl/ssl.h>

#include <memory>
#include <string>

namespace minimuduo::net {

struct SslDeleter {
    void operator()(SSL* ssl) const noexcept;
};

using SslPtr = std::unique_ptr<SSL, SslDeleter>;

class TlsServerContext final : private NonCopyable {
public:
    TlsServerContext() = default;
    ~TlsServerContext();

    bool initialize(
        const TlsServerConfig& config,
        std::string& error
    );

    SslPtr createSsl(
        int socketFd,
        std::string& error
    ) const;

    bool initialized() const noexcept;

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
