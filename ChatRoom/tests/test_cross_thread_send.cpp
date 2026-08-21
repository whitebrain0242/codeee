#include "minimuduo/net/EventLoop.hpp"
#include "minimuduo/net/TcpConnection.hpp"
#include "minimuduo/net/TcpServer.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

int main() {
    constexpr int port = 19775;

    minimuduo::net::EventLoop main_loop;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);

    minimuduo::net::TcpServer server(
        &main_loop,
        address,
        "cross"
    );
    server.setThreadNum(1);

    std::mutex mutex;
    std::condition_variable condition;
    minimuduo::net::TcpConnectionPtr captured;

    server.setConnectionCallback(
        [&](const minimuduo::net::TcpConnectionPtr& connection) {
            if (!connection->connected()) {
                return;
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                captured = connection;
            }
            condition.notify_one();
        }
    );

    server.start();

    bool received_expected = false;
    std::atomic<bool> completion_called{false};

    std::thread client([&] {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100)
        );

        const int socket_fd =
            ::socket(AF_INET, SOCK_STREAM, 0);

        if (socket_fd < 0) {
            main_loop.quit();
            return;
        }

        sockaddr_in server_address{};
        server_address.sin_family = AF_INET;
        server_address.sin_port = htons(port);
        (void)::inet_pton(
            AF_INET,
            "127.0.0.1",
            &server_address.sin_addr
        );

        if (::connect(
                socket_fd,
                reinterpret_cast<const sockaddr*>(
                    &server_address
                ),
                sizeof(server_address)
            ) != 0) {
            ::close(socket_fd);
            main_loop.quit();
            return;
        }

        minimuduo::net::TcpConnectionPtr connection;

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!condition.wait_for(
                    lock,
                    std::chrono::seconds(2),
                    [&] {
                        return captured != nullptr;
                    }
                )) {
                ::close(socket_fd);
                main_loop.quit();
                return;
            }

            connection = captured;
        }

        // This thread is neither the MainReactor nor the connection's
        // SubReactor. TcpConnection::send must queue into the owner loop
        // and wake its epoll through eventfd.
        connection->send(
            "cross-thread-ok\n",
            [&] {
                completion_called.store(true);
            }
        );

        char buffer[64]{};
        const ssize_t received =
            ::recv(
                socket_fd,
                buffer,
                sizeof(buffer),
                0
            );

        if (received > 0) {
            received_expected =
                std::string(
                    buffer,
                    static_cast<std::size_t>(received)
                ) == "cross-thread-ok\n";
        }

        for (int attempt = 0;
             attempt < 50 && !completion_called.load();
             ++attempt) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(10)
            );
        }

        ::close(socket_fd);

        {
            std::lock_guard<std::mutex> lock(mutex);
            captured.reset();
        }
        connection.reset();

        std::this_thread::sleep_for(
            std::chrono::milliseconds(100)
        );
        main_loop.quit();
    });

    main_loop.loop();
    client.join();

    return received_expected && completion_called.load()
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
