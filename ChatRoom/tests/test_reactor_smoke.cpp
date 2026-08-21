#include "minimuduo/net/Buffer.hpp"
#include "minimuduo/net/EventLoop.hpp"
#include "minimuduo/net/TcpConnection.hpp"
#include "minimuduo/net/TcpServer.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

int main() {
    constexpr int port = 19773;

    minimuduo::net::EventLoop mainLoop;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);

    minimuduo::net::TcpServer server(
        &mainLoop,
        address,
        "reactor-smoke");
    server.setThreadNum(2);

    server.setConnectionCallback(
        [](const minimuduo::net::TcpConnectionPtr&) {});

    server.setMessageCallback(
        [](const minimuduo::net::TcpConnectionPtr& connection,
           minimuduo::net::Buffer* buffer) {
            connection->send(buffer->retrieveAllAsString());
        });

    server.start();

    std::atomic<bool> passed{false};

    std::thread client([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const int socketFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (socketFd < 0) {
            mainLoop.quit();
            return;
        }

        sockaddr_in serverAddress{};
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_port = htons(port);
        (void)::inet_pton(
            AF_INET,
            "127.0.0.1",
            &serverAddress.sin_addr);

        bool connected = false;
        for (int attempt = 0; attempt < 20 && !connected; ++attempt) {
            if (::connect(
                    socketFd,
                    reinterpret_cast<const sockaddr*>(&serverAddress),
                    sizeof(serverAddress)) == 0) {
                connected = true;
            } else {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(50));
            }
        }

        if (connected) {
            static constexpr char payload[] = "reactor-ok\n";
            (void)::send(
                socketFd,
                payload,
                sizeof(payload) - 1U,
                MSG_NOSIGNAL);

            char response[64]{};
            const ssize_t received =
                ::recv(socketFd, response, sizeof(response), 0);

            passed.store(
                received == static_cast<ssize_t>(sizeof(payload) - 1U) &&
                std::string(
                    response,
                    static_cast<std::size_t>(received)) ==
                    payload);
        }

        ::close(socketFd);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        mainLoop.quit();
    });

    mainLoop.loop();
    client.join();

    return passed.load() ? EXIT_SUCCESS : EXIT_FAILURE;
}
