#include "minimuduo/net/TlsContext.hpp"

#include <openssl/err.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>

#include <array>
#include <sstream>

namespace minimuduo::net {

namespace {

bool isIpLiteral(
    const std::string& value
) {
    std::array<unsigned char, 16> bytes{};

    return ::inet_pton(
               AF_INET,
               value.c_str(),
               bytes.data()
           ) == 1 ||
           ::inet_pton(
               AF_INET6,
               value.c_str(),
               bytes.data()
           ) == 1;
}

bool configureCommonContext(
    SSL_CTX* context,
    std::string& error
) {
    if (context == nullptr) {
        error = "SSL_CTX is null";
        return false;
    }

    if (SSL_CTX_set_min_proto_version(
            context,
            TLS1_2_VERSION
        ) != 1) {
        error = openssl_error_text(
            "SSL_CTX_set_min_proto_version"
        );
        return false;
    }

#ifdef TLS1_3_VERSION
    if (SSL_CTX_set_max_proto_version(
            context,
            TLS1_3_VERSION
        ) != 1) {
        error = openssl_error_text(
            "SSL_CTX_set_max_proto_version"
        );
        return false;
    }
#endif

    SSL_CTX_set_options(
        context,
        SSL_OP_NO_COMPRESSION
    );

    return true;
}

}  // namespace

void SslDeleter::operator()(
    SSL* ssl
) const noexcept {
    if (ssl != nullptr) {
        SSL_free(ssl);
    }
}

TlsServerContext::~TlsServerContext() {
    if (context_ != nullptr) {
        SSL_CTX_free(context_);
        context_ = nullptr;
    }
}

bool TlsServerContext::initialize(
    const TlsServerConfig& config,
    std::string& error
) {
    if (context_ != nullptr) {
        SSL_CTX_free(context_);
        context_ = nullptr;
    }

    if (!config.enabled) {
        error =
            "TLS server config has enabled=false; "
            "v8.4 requires TLS";
        return false;
    }

    context_ =
        SSL_CTX_new(TLS_server_method());

    if (context_ == nullptr) {
        error = openssl_error_text(
            "SSL_CTX_new(TLS_server_method)"
        );
        return false;
    }

    if (!configureCommonContext(
            context_,
            error
        )) {
        SSL_CTX_free(context_);
        context_ = nullptr;
        return false;
    }

    if (SSL_CTX_use_certificate_chain_file(
            context_,
            config.certificate_file.c_str()
        ) != 1) {
        error = openssl_error_text(
            "SSL_CTX_use_certificate_chain_file"
        );
        SSL_CTX_free(context_);
        context_ = nullptr;
        return false;
    }

    if (SSL_CTX_use_PrivateKey_file(
            context_,
            config.private_key_file.c_str(),
            SSL_FILETYPE_PEM
        ) != 1) {
        error = openssl_error_text(
            "SSL_CTX_use_PrivateKey_file"
        );
        SSL_CTX_free(context_);
        context_ = nullptr;
        return false;
    }

    if (SSL_CTX_check_private_key(
            context_
        ) != 1) {
        error = openssl_error_text(
            "SSL_CTX_check_private_key"
        );
        SSL_CTX_free(context_);
        context_ = nullptr;
        return false;
    }

    SSL_CTX_set_verify(
        context_,
        SSL_VERIFY_NONE,
        nullptr
    );

    return true;
}

SslPtr TlsServerContext::createSsl(
    int socketFd,
    std::string& error
) const {
    if (context_ == nullptr) {
        error =
            "TLS server context is not initialized";
        return {};
    }

    SslPtr ssl(
        SSL_new(context_)
    );

    if (!ssl) {
        error =
            openssl_error_text("SSL_new");
        return {};
    }

    if (SSL_set_fd(
            ssl.get(),
            socketFd
        ) != 1) {
        error =
            openssl_error_text("SSL_set_fd");
        return {};
    }

    SSL_set_accept_state(ssl.get());
    return ssl;
}

bool TlsServerContext::initialized() const noexcept {
    return context_ != nullptr;
}

TlsClientContext::~TlsClientContext() {
    if (context_ != nullptr) {
        SSL_CTX_free(context_);
        context_ = nullptr;
    }
}

bool TlsClientContext::initialize(
    const TlsClientConfig& config,
    std::string& error
) {
    if (context_ != nullptr) {
        SSL_CTX_free(context_);
        context_ = nullptr;
    }

    if (!config.enabled) {
        error =
            "TLS client config has enabled=false; "
            "v8.4 requires TLS";
        return false;
    }

    context_ =
        SSL_CTX_new(TLS_client_method());

    if (context_ == nullptr) {
        error = openssl_error_text(
            "SSL_CTX_new(TLS_client_method)"
        );
        return false;
    }

    if (!configureCommonContext(
            context_,
            error
        )) {
        SSL_CTX_free(context_);
        context_ = nullptr;
        return false;
    }

    verify_peer_ = config.verify_peer;

    if (verify_peer_) {
        SSL_CTX_set_verify(
            context_,
            SSL_VERIFY_PEER,
            nullptr
        );

        if (config.ca_file.empty()) {
            error =
                "TLS client verify_peer=true "
                "requires ca_file";
            SSL_CTX_free(context_);
            context_ = nullptr;
            return false;
        }

        if (SSL_CTX_load_verify_locations(
                context_,
                config.ca_file.c_str(),
                nullptr
            ) != 1) {
            error = openssl_error_text(
                "SSL_CTX_load_verify_locations"
            );
            SSL_CTX_free(context_);
            context_ = nullptr;
            return false;
        }
    } else {
        SSL_CTX_set_verify(
            context_,
            SSL_VERIFY_NONE,
            nullptr
        );
    }

    return true;
}

SslPtr TlsClientContext::createSsl(
    int socketFd,
    const std::string& peerIdentity,
    std::string& error
) const {
    if (context_ == nullptr) {
        error =
            "TLS client context is not initialized";
        return {};
    }

    SslPtr ssl(
        SSL_new(context_)
    );

    if (!ssl) {
        error =
            openssl_error_text("SSL_new");
        return {};
    }

    if (SSL_set_fd(
            ssl.get(),
            socketFd
        ) != 1) {
        error =
            openssl_error_text("SSL_set_fd");
        return {};
    }

    if (verify_peer_) {
        if (peerIdentity.empty()) {
            error =
                "TLS peer identity cannot be empty "
                "when verification is enabled";
            return {};
        }

        X509_VERIFY_PARAM* parameters =
            SSL_get0_param(ssl.get());

        if (parameters == nullptr) {
            error =
                "SSL_get0_param returned null";
            return {};
        }

        if (isIpLiteral(peerIdentity)) {
            if (X509_VERIFY_PARAM_set1_ip_asc(
                    parameters,
                    peerIdentity.c_str()
                ) != 1) {
                error =
                    "X509_VERIFY_PARAM_set1_ip_asc failed";
                return {};
            }
        } else {
            if (SSL_set1_host(
                    ssl.get(),
                    peerIdentity.c_str()
                ) != 1) {
                error =
                    "SSL_set1_host failed";
                return {};
            }

            if (SSL_set_tlsext_host_name(
                    ssl.get(),
                    peerIdentity.c_str()
                ) != 1) {
                error =
                    openssl_error_text(
                        "SSL_set_tlsext_host_name"
                    );
                return {};
            }
        }
    }

    SSL_set_connect_state(ssl.get());
    return ssl;
}

bool TlsClientContext::connectBlocking(
    SSL* ssl,
    std::string& error
) const {
    if (ssl == nullptr) {
        error = "SSL is null";
        return false;
    }

    if (SSL_connect(ssl) != 1) {
        error =
            openssl_error_text("SSL_connect");
        return false;
    }

    if (verify_peer_ &&
        SSL_get_verify_result(ssl) != X509_V_OK) {
        error =
            "TLS peer certificate verification failed: " +
            std::string(
                X509_verify_cert_error_string(
                    SSL_get_verify_result(ssl)
                )
            );
        return false;
    }

    return true;
}

bool TlsClientContext::initialized() const noexcept {
    return context_ != nullptr;
}

std::string openssl_error_text(
    const std::string& operation
) {
    std::ostringstream output;
    output << operation;

    bool found = false;

    while (true) {
        const unsigned long code =
            ERR_get_error();

        if (code == 0UL) {
            break;
        }

        char buffer[256]{};
        ERR_error_string_n(
            code,
            buffer,
            sizeof(buffer)
        );

        output
            << (found ? " | " : ": ")
            << buffer;
        found = true;
    }

    if (!found) {
        output << ": no OpenSSL error detail";
    }

    return output.str();
}

}  // namespace minimuduo::net
