#include "minimuduo/net/TlsContext.hpp"

#include <arpa/inet.h>
#include <openssl/ssl.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

bool send_all_ssl(
    SSL* ssl,
    const std::string& message
) {
    std::size_t offset = 0U;

    while (offset < message.size()) {
        const int result =
            SSL_write(
                ssl,
                message.data() + offset,
                static_cast<int>(
                    message.size() - offset
                )
            );

        if (result <= 0) {
            return false;
        }

        offset +=
            static_cast<std::size_t>(
                result
            );
    }

    return true;
}

}  // namespace

int main(
    int argc,
    char* argv[]
) {
    if (argc != 4) {
        std::cerr
            << "usage: test_tls_context "
               "<server.crt> <server.key> <ca.crt>\n";
        return EXIT_FAILURE;
    }

    TlsServerConfig server_config;
    server_config.certificate_file =
        argv[1];
    server_config.private_key_file =
        argv[2];

    TlsClientConfig client_config;
    client_config.ca_file =
        argv[3];
    client_config.verify_peer =
        true;

    minimuduo::net::TlsServerContext
        server_context;

    minimuduo::net::TlsClientContext
        client_context;

    std::string error;

    if (!server_context.initialize(
            server_config,
            error
        ) ||
        !client_context.initialize(
            client_config,
            error
        )) {
        std::cerr
            << error
            << '\n';
        return EXIT_FAILURE;
    }

    const int listen_fd =
        ::socket(
            AF_INET,
            SOCK_STREAM,
            0
        );

    if (listen_fd < 0) {
        return EXIT_FAILURE;
    }

    int reuse = 1;

    (void)::setsockopt(
        listen_fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse,
        sizeof(reuse)
    );

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr =
        htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);

    if (::bind(
            listen_fd,
            reinterpret_cast<
                const sockaddr*
            >(&address),
            sizeof(address)
        ) != 0 ||
        ::listen(
            listen_fd,
            4
        ) != 0) {
        ::close(listen_fd);
        return EXIT_FAILURE;
    }

    socklen_t address_length =
        sizeof(address);

    if (::getsockname(
            listen_fd,
            reinterpret_cast<
                sockaddr*
            >(&address),
            &address_length
        ) != 0) {
        ::close(listen_fd);
        return EXIT_FAILURE;
    }

    const unsigned int port =
        ntohs(address.sin_port);

    std::atomic<bool>
        server_ok{false};

    std::thread server_thread(
        [&] {
            sockaddr_in peer{};
            socklen_t peer_length =
                sizeof(peer);

            const int client_fd =
                ::accept(
                    listen_fd,
                    reinterpret_cast<
                        sockaddr*
                    >(&peer),
                    &peer_length
                );

            if (client_fd < 0) {
                return;
            }

            std::string local_error;

            minimuduo::net::SslPtr
                server_ssl =
                    server_context.createSsl(
                        client_fd,
                        local_error
                    );

            if (!server_ssl ||
                SSL_accept(
                    server_ssl.get()
                ) != 1) {
                ::close(client_fd);
                return;
            }

            char buffer[64]{};

            const int received =
                SSL_read(
                    server_ssl.get(),
                    buffer,
                    sizeof(buffer)
                );

            if (received <= 0 ||
                std::string(
                    buffer,
                    static_cast<std::size_t>(
                        received
                    )
                ) != "hello-tls") {
                ::close(client_fd);
                return;
            }

            server_ok.store(
                send_all_ssl(
                    server_ssl.get(),
                    "tls-ok"
                )
            );

            (void)SSL_shutdown(
                server_ssl.get()
            );

            server_ssl.reset();
            ::close(client_fd);
        }
    );

    const int client_fd =
        ::socket(
            AF_INET,
            SOCK_STREAM,
            0
        );

    if (client_fd < 0) {
        ::close(listen_fd);
        server_thread.join();
        return EXIT_FAILURE;
    }

    if (::connect(
            client_fd,
            reinterpret_cast<
                const sockaddr*
            >(&address),
            sizeof(address)
        ) != 0) {
        ::close(client_fd);
        ::close(listen_fd);
        server_thread.join();
        return EXIT_FAILURE;
    }

    minimuduo::net::SslPtr
        client_ssl =
            client_context.createSsl(
                client_fd,
                "127.0.0.1",
                error
            );

    if (!client_ssl ||
        !client_context.connectBlocking(
            client_ssl.get(),
            error
        )) {
        std::cerr
            << error
            << '\n';
        ::close(client_fd);
        ::close(listen_fd);
        server_thread.join();
        return EXIT_FAILURE;
    }

    if (!send_all_ssl(
            client_ssl.get(),
            "hello-tls"
        )) {
        ::close(client_fd);
        ::close(listen_fd);
        server_thread.join();
        return EXIT_FAILURE;
    }

    char response[64]{};

    const int response_size =
        SSL_read(
            client_ssl.get(),
            response,
            sizeof(response)
        );

    const bool client_ok =
        response_size > 0 &&
        std::string(
            response,
            static_cast<std::size_t>(
                response_size
            )
        ) == "tls-ok" &&
        SSL_get_verify_result(
            client_ssl.get()
        ) == X509_V_OK;

    (void)SSL_shutdown(
        client_ssl.get()
    );

    client_ssl.reset();
    ::close(client_fd);
    ::close(listen_fd);

    server_thread.join();

    if (!client_ok ||
        !server_ok.load()) {
        std::cerr
            << "TLS verified exchange failed\n";
        return EXIT_FAILURE;
    }

    std::cout
        << "TLS context verified exchange passed on port "
        << port
        << '\n';

    return EXIT_SUCCESS;
}
