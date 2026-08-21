#include "minimuduo/net/EventLoop.hpp"
#include "minimuduo/net/TcpConnection.hpp"
#include "minimuduo/net/TcpServer.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <netinet/in.h>
#include <set>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

int main() {
    constexpr int port = 19774;
    constexpr int client_count = 8;

    minimuduo::net::EventLoop main_loop;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);

    minimuduo::net::TcpServer server(
        &main_loop,
        address,
        "dist"
    );
    server.setThreadNum(4);

    std::mutex ids_mutex;
    std::set<std::thread::id> worker_ids;
    std::atomic<int> connected{0};

    server.setConnectionCallback(
        [&](const minimuduo::net::TcpConnectionPtr& connection) {
            if (!connection->connected()) {
                return;
            }

            {
                std::lock_guard<std::mutex> lock(ids_mutex);
                worker_ids.insert(std::this_thread::get_id());
            }

            connected.fetch_add(1);
            connection->send("ready\n");
        }
    );

    server.start();

    std::thread clients([&] {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100)
        );

        std::vector<int> sockets;
        sockets.reserve(client_count);

        for (int index = 0; index < client_count; ++index) {
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

            sockets.push_back(socket_fd);
        }

        for (int attempt = 0;
             attempt < 100 &&
             connected.load() < client_count;
             ++attempt) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(20)
            );
        }

        for (int socket_fd : sockets) {
            ::close(socket_fd);
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(100)
        );
        main_loop.quit();
    });

    main_loop.loop();
    clients.join();

    std::lock_guard<std::mutex> lock(ids_mutex);

    return connected.load() == client_count &&
                   worker_ids.size() == 4U
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
