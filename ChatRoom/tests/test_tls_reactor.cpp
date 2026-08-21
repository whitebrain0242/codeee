#include "config.hpp"

#include "minimuduo/net/Buffer.hpp"
#include "minimuduo/net/EventLoop.hpp"
#include "minimuduo/net/TcpConnection.hpp"
#include "minimuduo/net/TcpServer.hpp"
#include "minimuduo/net/TlsContext.hpp"

#include <arpa/inet.h>
#include <openssl/ssl.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

int main(
    int argc,
    char* argv[]
) {
    if (argc != 4) {
        std::cerr
            << "usage: test_tls_reactor "
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

    auto server_tls =
        std::make_shared<
            minimuduo::net::TlsServerContext
        >();

    minimuduo::net::TlsClientContext
        client_tls;

    std::string error;

    if (!server_tls->initialize(
            server_config,
            error
        ) ||
        !client_tls.initialize(
            client_config,
            error
        )) {
        std::cerr
            << error
            << '\n';
        return EXIT_FAILURE;
    }

    // Fixed loopback test port in the same spirit as the existing reactor
    // smoke tests. This test validates TLS inside TcpConnection/SubReactor.
    constexpr unsigned short port =
        19883;

    sockaddr_in listen_address{};
    listen_address.sin_family =
        AF_INET;
    listen_address.sin_addr.s_addr =
        htonl(INADDR_LOOPBACK);
    listen_address.sin_port =
        htons(port);

    minimuduo::net::EventLoop
        main_loop;

    minimuduo::net::TcpServer server(
        &main_loop,
        listen_address,
        "tls-reactor"
    );

    server.setThreadNum(1);
    server.setTlsContext(
        server_tls
    );

    server.setMessageCallback(
        [](
            const minimuduo::net::
                TcpConnectionPtr& connection,
            minimuduo::net::Buffer* buffer
        ) {
            const std::string message =
                buffer->retrieveAllAsString();

            connection->send(
                "echo:" +
                message
            );
        }
    );

    server.start();

    bool client_ok = false;

    std::thread client_thread(
        [&] {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(
                    150
                )
            );

            const int fd =
                ::socket(
                    AF_INET,
                    SOCK_STREAM,
                    0
                );

            if (fd < 0) {
                main_loop.quit();
                return;
            }

            sockaddr_in address{};
            address.sin_family =
                AF_INET;
            address.sin_addr.s_addr =
                htonl(INADDR_LOOPBACK);
            address.sin_port =
                htons(port);

            if (::connect(
                    fd,
                    reinterpret_cast<
                        const sockaddr*
                    >(&address),
                    sizeof(address)
                ) != 0) {
                ::close(fd);
                main_loop.quit();
                return;
            }

            std::string local_error;

            minimuduo::net::SslPtr ssl =
                client_tls.createSsl(
                    fd,
                    "127.0.0.1",
                    local_error
                );

            if (!ssl ||
                !client_tls.connectBlocking(
                    ssl.get(),
                    local_error
                )) {
                std::cerr
                    << local_error
                    << '\n';
                ::close(fd);
                main_loop.quit();
                return;
            }

            const std::string request =
                "reactor-tls";

            if (SSL_write(
                    ssl.get(),
                    request.data(),
                    static_cast<int>(
                        request.size()
                    )
                ) <= 0) {
                ::close(fd);
                main_loop.quit();
                return;
            }

            char response[128]{};

            const int count =
                SSL_read(
                    ssl.get(),
                    response,
                    sizeof(response)
                );

            client_ok =
                count > 0 &&
                std::string(
                    response,
                    static_cast<std::size_t>(
                        count
                    )
                ) ==
                    "echo:reactor-tls";

            (void)SSL_shutdown(
                ssl.get()
            );

            ssl.reset();
            ::close(fd);

            main_loop.quit();
        }
    );

    main_loop.loop();

    client_thread.join();

    if (!client_ok) {
        std::cerr
            << "TLS Reactor echo failed\n";
        return EXIT_FAILURE;
    }

    std::cout
        << "TLS Reactor echo passed\n";

    return EXIT_SUCCESS;
}
