#include "protocol.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
    constexpr std::size_t kBufferSize = 4096;
    int connect_to_server(const std::string &ip, int port)
    {
        const int socket_fd =
        socket(AF_INET, SOCK_STREAM, 0);

    if (socket_fd == -1) {
        std::cerr
            << "socket failed: "
            << std::strerror(errno)
            << std::endl;
        return -1;
    }

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_port =
        htons(static_cast<std::uint16_t>(port));

    if (
        inet_pton(
            AF_INET,
            ip.c_str(),
            &server_address.sin_addr
        ) != 1
    ) {
        std::cerr
            << "invalid IPv4 address"
            << std::endl;
        close(socket_fd);
        return -1;
    }

    if (
        connect(
            socket_fd,
            reinterpret_cast<sockaddr*>(
                &server_address
            ),
            sizeof(server_address)
        ) == -1
    ) {
        std::cerr
            << "connect failed: "
            << std::strerror(errno)
            << std::endl;
        close(socket_fd);
        return -1;
    }

    return socket_fd;
    }
    // v1:新增send_all函数
    bool send_all(int socket_fd, const std::string &data)
    {
        std::size_t total_sent = 0;

        while (total_sent < data.size())
        {
            const ssize_t sent = send(
                socket_fd,
                data.data() + total_sent,
                data.size() - total_sent,
                MSG_NOSIGNAL);

            if (sent > 0)
            {
                total_sent += static_cast<std::size_t>(sent);
                continue;
            }

            if (sent == -1 && errno == EINTR)
            {
                continue;
            }

            std::cerr << "send failed: "
                      << std::strerror(errno) << '\n';
            return false;
        }

        return true;
    }
}


int main(int argc, char *argv[])
{
    std::string ip = "127.0.0.1";
    int port = 9000;

    if (argc >= 2)
    {
        ip = argv[1];
    }

    if (argc >= 3 && !chat::parse_port(argv[2], port))
    {
        std::cerr << "invalid port; expected 1-65535\n";
        return 1;
    }

    const int socket_fd =
        connect_to_server(ip, port);

    if (socket_fd == -1) {
        return 1;
    }

    std::cout
        << "connected to "
        << ip
        << ":"
        << port
        << std::endl;

    std::cout
        << "type HELP to view commands."
        << std::endl;

    pollfd descriptors[2]{};

    descriptors[0].fd = STDIN_FILENO;
    descriptors[0].events = POLLIN;

    descriptors[1].fd = socket_fd;
    descriptors[1].events = POLLIN;

    bool waiting_for_server_close = false;
    char buffer[kBufferSize];

    while (true) {
        const int result =
            poll(descriptors, 2, -1);

        if (result == -1) {
            if (errno == EINTR) {
                continue;
            }

            std::cerr
                << "poll failed: "
                << std::strerror(errno)
                << std::endl;
            break;
        }

        if (
            !waiting_for_server_close &&
            (descriptors[0].revents & POLLIN) != 0
        ) {
            std::string line;

            if (!std::getline(std::cin, line)) {
                break;
            }

            const chat::Command command =
                chat::parse_command(line);

            line += "\n";

            if (!send_all(socket_fd, line)) {
                break;
            }

            if (command.name == "QUIT") {
                waiting_for_server_close = true;
                descriptors[0].fd = -1;
            }
        }

        if (
            (descriptors[1].revents & POLLIN) != 0
        ) {
            const ssize_t received =
                recv(
                    socket_fd,
                    buffer,
                    sizeof(buffer) - 1,
                    0
                );

            if (received > 0) {
                buffer[received] = '\0';
                std::cout << buffer;
                std::cout.flush();
            } else if (received == 0) {
                std::cout
                    << "server closed connection"
                    << std::endl;
                break;
            } else if (errno != EINTR) {
                std::cerr
                    << "recv failed: "
                    << std::strerror(errno)
                    << std::endl;
                break;
            }
        }

        if (
            (
                descriptors[1].revents &
                (POLLERR | POLLHUP | POLLNVAL)
            ) != 0
        ) {
            std::cout
                << "connection closed"
                << std::endl;
            break;
        }
    }

    close(socket_fd);
    return 0;
}
