#include "client/tls_client_transport.hpp"

#include "minimuduo/net/SocketOptions.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/bio.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <climits>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>

TlsClientTransport::~TlsClientTransport() {
    shutdown();
}

bool TlsClientTransport::connect(
    const std::string& ip,
    int port,
    const TlsClientConfig& config,
    std::string& error
) {
    shutdown();

    if (!tls_context_.initialize(
            config,
            error
        )) {
        return false;
    }

    socket_fd_ =
        ::socket(
            AF_INET,
            SOCK_STREAM,
            0
        );

    if (socket_fd_ < 0) {
        error =
            "socket failed: " +
            std::string(
                std::strerror(errno)
            );
        return false;
    }

    sockaddr_in address{};
    address.sin_family =
        AF_INET;

    address.sin_port =
        htons(
            static_cast<std::uint16_t>(
                port
            )
        );

    if (::inet_pton(
            AF_INET,
            ip.c_str(),
            &address.sin_addr
        ) != 1) {
        error =
            "invalid IPv4 address";
        shutdown();
        return false;
    }

    if (::connect(
            socket_fd_,
            reinterpret_cast<
                sockaddr*
            >(&address),
            sizeof(address)
        ) != 0) {
        error =
            "connect failed: " +
            std::string(
                std::strerror(errno)
            );

        shutdown();
        return false;
    }

    std::string keepalive_error;

    // Best effort: the client-originated PING/server PONG probe is transport-only,
    // while SO_KEEPALIVE is the kernel-level fallback.
    (void)minimuduo::net::
        configureTcpKeepAlive(
            socket_fd_,
            60,
            15,
            3,
            keepalive_error
        );

    peer_identity_ =
        config.server_name.empty()
            ? ip
            : config.server_name;

    ssl_ =
        tls_context_.createSsl(
            socket_fd_,
            peer_identity_,
            error
        );

    if (!ssl_) {
        shutdown();
        return false;
    }

    if (!tls_context_.connectBlocking(
            ssl_.get(),
            error
        )) {
        shutdown();
        return false;
    }

    connected_ = true;
    return true;
}

bool TlsClientTransport::send(
    const std::string& data,
    std::string& error
) {
    if (!connected_ ||
        !ssl_) {
        error =
            "TLS transport is not connected";
        return false;
    }

    std::size_t offset = 0U;

    while (offset < data.size()) {
        const int write_size =
            static_cast<int>(
                std::min<std::size_t>(
                    data.size() - offset,
                    static_cast<std::size_t>(
                        INT_MAX
                    )
                )
            );

        ERR_clear_error();

        const int sent =
            SSL_write(
                ssl_.get(),
                data.data() + offset,
                write_size
            );

        if (sent > 0) {
            offset +=
                static_cast<std::size_t>(
                    sent
                );
            continue;
        }

        const int ssl_error =
            SSL_get_error(
                ssl_.get(),
                sent
            );

        if (ssl_error ==
                SSL_ERROR_WANT_READ ||
            ssl_error ==
                SSL_ERROR_WANT_WRITE) {
            continue;
        }

        error =
            minimuduo::net::
                openssl_error_text(
                    "SSL_write"
                );

        return false;
    }

    return true;
}

bool TlsClientTransport::zero_copy_send_available() const noexcept {
#if defined(__linux__) && \
    OPENSSL_VERSION_NUMBER >= 0x30000000L && \
    !defined(OPENSSL_NO_KTLS)
    if (!connected_ || !ssl_) {
        return false;
    }

    BIO* write_bio =
        SSL_get_wbio(ssl_.get());

    return
        write_bio != nullptr &&
        BIO_get_ktls_send(write_bio) != 0;
#else
    return false;
#endif
}

bool TlsClientTransport::send_file(
    const std::filesystem::path& path,
    std::uint64_t offset,
    std::uint64_t byte_count,
    bool& used_zero_copy,
    std::string& error
) {
    used_zero_copy = false;

    if (!connected_ || !ssl_) {
        error =
            "TLS transport is not connected";
        return false;
    }

    const int fd =
        ::open(
            path.c_str(),
            O_RDONLY | O_CLOEXEC
        );

    if (fd < 0) {
        error =
            "cannot open file for upload: " +
            std::string(std::strerror(errno));
        return false;
    }

    struct FdGuard {
        int fd = -1;
        ~FdGuard() {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    } guard{fd};

#if defined(__linux__) && \
    OPENSSL_VERSION_NUMBER >= 0x30000000L && \
    !defined(OPENSSL_NO_KTLS)
    if (zero_copy_send_available()) {
        std::uint64_t sent_total = 0U;

        while (sent_total < byte_count) {
            const std::size_t request =
                static_cast<std::size_t>(
                    std::min<std::uint64_t>(
                        byte_count - sent_total,
                        1024ULL * 1024ULL * 1024ULL
                    )
                );

            ERR_clear_error();

            const int sent =
                SSL_sendfile(
                    ssl_.get(),
                    fd,
                    static_cast<off_t>(
                        offset + sent_total
                    ),
                    request,
                    0
                );

            if (sent > 0) {
                sent_total +=
                    static_cast<std::uint64_t>(
                        sent
                    );
                continue;
            }

            const int ssl_error =
                SSL_get_error(
                    ssl_.get(),
                    sent
                );

            if (ssl_error ==
                    SSL_ERROR_WANT_READ ||
                ssl_error ==
                    SSL_ERROR_WANT_WRITE) {
                continue;
            }

            error =
                minimuduo::net::
                    openssl_error_text(
                        "SSL_sendfile"
                    );
            return false;
        }

        used_zero_copy = true;
        return true;
    }
#endif

    // Safe fallback for kernels/OpenSSL builds without kTLS. This still sends
    // raw binary bytes (no text expansion) and uses a bounded buffer.
    std::array<char, 256 * 1024>
        buffer{};

    std::uint64_t sent_total = 0U;

    while (sent_total < byte_count) {
        const std::size_t request =
            static_cast<std::size_t>(
                std::min<std::uint64_t>(
                    byte_count - sent_total,
                    buffer.size()
                )
            );

        const ssize_t read_count =
            ::pread(
                fd,
                buffer.data(),
                request,
                static_cast<off_t>(
                    offset + sent_total
                )
            );

        if (read_count <= 0) {
            error =
                read_count == 0
                    ? "file became shorter during upload"
                    : "pread failed: " +
                        std::string(
                            std::strerror(errno)
                        );
            return false;
        }

        std::size_t written = 0U;

        while (written <
               static_cast<std::size_t>(
                   read_count
               )) {
            ERR_clear_error();

            const int result =
                SSL_write(
                    ssl_.get(),
                    buffer.data() + written,
                    static_cast<int>(
                        static_cast<std::size_t>(
                            read_count
                        ) -
                        written
                    )
                );

            if (result > 0) {
                written +=
                    static_cast<std::size_t>(
                        result
                    );
                continue;
            }

            const int ssl_error =
                SSL_get_error(
                    ssl_.get(),
                    result
                );

            if (ssl_error ==
                    SSL_ERROR_WANT_READ ||
                ssl_error ==
                    SSL_ERROR_WANT_WRITE) {
                continue;
            }

            error =
                minimuduo::net::
                    openssl_error_text(
                        "SSL_write(file)"
                    );
            return false;
        }

        sent_total +=
            static_cast<std::uint64_t>(
                read_count
            );
    }

    return true;
}


TransportReadResult
TlsClientTransport::receive(
    char* buffer,
    std::size_t capacity
) {
    TransportReadResult result;

    if (!connected_ ||
        !ssl_ ||
        buffer == nullptr ||
        capacity == 0U) {
        result.status =
            TransportReadStatus::Error;

        result.error =
            "invalid TLS receive state";

        return result;
    }

    ERR_clear_error();

    const int received =
        SSL_read(
            ssl_.get(),
            buffer,
            static_cast<int>(
                std::min<std::size_t>(
                    capacity,
                    static_cast<std::size_t>(
                        INT_MAX
                    )
                )
            )
        );

    if (received > 0) {
        result.status =
            TransportReadStatus::Data;

        result.bytes =
            static_cast<std::size_t>(
                received
            );

        return result;
    }

    const int ssl_error =
        SSL_get_error(
            ssl_.get(),
            received
        );

    if (ssl_error ==
        SSL_ERROR_ZERO_RETURN) {
        connected_ = false;

        result.status =
            TransportReadStatus::Closed;

        return result;
    }

    if (ssl_error ==
            SSL_ERROR_WANT_READ ||
        ssl_error ==
            SSL_ERROR_WANT_WRITE) {
        result.status =
            TransportReadStatus::Retry;

        return result;
    }

    result.status =
        TransportReadStatus::Error;

    result.error =
        minimuduo::net::
            openssl_error_text(
                "SSL_read"
            );

    return result;
}

int TlsClientTransport::fd() const noexcept {
    return socket_fd_;
}

int TlsClientTransport::pending() const noexcept {
    if (!ssl_) {
        return 0;
    }

    return
        SSL_pending(
            ssl_.get()
        );
}

std::string
TlsClientTransport::tls_version() const {
    if (!ssl_) {
        return {};
    }

    const char* value =
        SSL_get_version(
            ssl_.get()
        );

    return value == nullptr
        ? std::string()
        : std::string(value);
}

std::string
TlsClientTransport::cipher_name() const {
    if (!ssl_) {
        return {};
    }

    const char* value =
        SSL_get_cipher_name(
            ssl_.get()
        );

    return value == nullptr
        ? std::string()
        : std::string(value);
}

const std::string&
TlsClientTransport::peer_identity()
    const noexcept {
    return peer_identity_;
}

bool TlsClientTransport::connected()
    const noexcept {
    return connected_;
}

void TlsClientTransport::shutdown() {
    if (ssl_) {
        ERR_clear_error();
        (void)SSL_shutdown(
            ssl_.get()
        );

        ssl_.reset();
    }

    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }

    connected_ = false;
    peer_identity_.clear();
}
