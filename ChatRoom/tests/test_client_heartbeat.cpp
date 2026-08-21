#include "client/client_heartbeat.hpp"
#include "client/tls_client_transport.hpp"
#include "config.hpp"
#include "minimuduo/net/TlsContext.hpp"

#include <arpa/inet.h>
#include <openssl/ssl.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
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
            << "usage: test_client_heartbeat "
               "<server.crt> <server.key> <ca.crt>\n";
        return EXIT_FAILURE;
    }

    TlsServerConfig server_config;
    server_config.certificate_file =
        argv[1];
    server_config.private_key_file =
        argv[2];

    minimuduo::net::TlsServerContext
        server_tls;

    std::string error;

    if (!server_tls.initialize(
            server_config,
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
    address.sin_family =
        AF_INET;
    address.sin_addr.s_addr =
        htonl(INADDR_LOOPBACK);
    address.sin_port =
        htons(0);

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

    socklen_t length =
        sizeof(address);

    if (::getsockname(
            listen_fd,
            reinterpret_cast<
                sockaddr*
            >(&address),
            &length
        ) != 0) {
        ::close(listen_fd);
        return EXIT_FAILURE;
    }

    const int port =
        ntohs(
            address.sin_port
        );

    std::atomic<bool>
        server_received_ping{false};

    std::thread server_thread(
        [&] {
            const int client_fd =
                ::accept(
                    listen_fd,
                    nullptr,
                    nullptr
                );

            if (client_fd < 0) {
                return;
            }

            std::string local_error;

            minimuduo::net::SslPtr ssl =
                server_tls.createSsl(
                    client_fd,
                    local_error
                );

            if (!ssl ||
                SSL_accept(
                    ssl.get()
                ) != 1) {
                ::close(client_fd);
                return;
            }

            char buffer[128]{};

            const int received =
                SSL_read(
                    ssl.get(),
                    buffer,
                    sizeof(buffer)
                );

            if (received <= 0) {
                ::close(client_fd);
                return;
            }

            const std::string ping(
                buffer,
                static_cast<std::size_t>(
                    received
                )
            );

            if (ping.rfind(
                    "PING ",
                    0
                ) != 0 ||
                ping.back() != '\n') {
                ::close(client_fd);
                return;
            }

            const std::string nonce =
                ping.substr(
                    5U,
                    ping.size() - 6U
                );

            server_received_ping.store(
                !nonce.empty()
            );

            const std::string pong =
                "PONG " +
                nonce +
                "\n";

            (void)SSL_write(
                ssl.get(),
                pong.data(),
                static_cast<int>(
                    pong.size()
                )
            );

            (void)SSL_shutdown(
                ssl.get()
            );

            ssl.reset();
            ::close(client_fd);
        }
    );

    TlsClientConfig client_config;
    client_config.verify_peer =
        true;
    client_config.ca_file =
        argv[3];

    TlsClientTransport transport;

    if (!transport.connect(
            "127.0.0.1",
            port,
            client_config,
            error
        )) {
        std::cerr
            << error
            << '\n';

        ::close(listen_fd);
        server_thread.join();
        return EXIT_FAILURE;
    }

    ClientHeartbeat heartbeat(
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(2000)
    );

    if (!heartbeat.tick(
            transport,
            error
        )) {
        std::cerr
            << "client failed to send PING: "
            << error
            << '\n';

        transport.shutdown();
        ::close(listen_fd);
        server_thread.join();
        return EXIT_FAILURE;
    }

    char buffer[128]{};

    const TransportReadResult result =
        transport.receive(
            buffer,
            sizeof(buffer)
        );

    if (result.status !=
        TransportReadStatus::Data) {
        std::cerr
            << "client did not receive PONG\n";

        transport.shutdown();
        ::close(listen_fd);
        server_thread.join();
        return EXIT_FAILURE;
    }

    std::string line(
        buffer,
        result.bytes
    );

    if (!line.empty() &&
        line.back() == '\n') {
        line.pop_back();
    }

    const bool consumed =
        heartbeat.consume_protocol_line(
            line,
            transport
        );

    const bool alive =
        !heartbeat.timed_out();

    transport.shutdown();
    ::close(listen_fd);
    server_thread.join();

    return
        consumed &&
        alive &&
        server_received_ping.load()
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
}
