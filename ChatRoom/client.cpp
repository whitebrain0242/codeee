#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

static constexpr int BUFFER_SIZE = 4096;

int connect_to_server(const std::string& ip, int port) {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        std::cerr << "socket failed: " << strerror(errno) << std::endl;
        return -1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "invalid ip address" << std::endl;
        close(sock_fd);
        return -1;
    }

    if (connect(sock_fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == -1) {
        std::cerr << "connect failed: " << strerror(errno) << std::endl;
        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

int main(int argc, char* argv[]) {
    std::string ip = "127.0.0.1";
    int port = 9000;

    if (argc >= 2) {
        ip = argv[1];
    }

    if (argc >= 3) {
        port = std::stoi(argv[2]);
    }

    int sock_fd = connect_to_server(ip, port);
    if (sock_fd == -1) {
        return 1;
    }

    std::cout << "connected to server " << ip << ":" << port << std::endl;
    std::cout << "type message and press Enter. type /quit to exit." << std::endl;

    pollfd fds[2];

    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;

    fds[1].fd = sock_fd;
    fds[1].events = POLLIN;

    char buffer[BUFFER_SIZE];

    while (true) {
        int ret = poll(fds, 2, -1);

        if (ret == -1) {
            if (errno == EINTR) {
                continue;
            }

            std::cerr << "poll failed: " << strerror(errno) << std::endl;
            break;
        }

        if (fds[0].revents & POLLIN) {
            std::string line;
            if (!std::getline(std::cin, line)) {
                break;
            }

            if (line == "/quit") {
                break;
            }

            line += "\n";

            ssize_t n = send(sock_fd, line.data(), line.size(), 0);
            if (n == -1) {
                std::cerr << "send failed: " << strerror(errno) << std::endl;
                break;
            }
        }

        if (fds[1].revents & POLLIN) {
            ssize_t n = recv(sock_fd, buffer, sizeof(buffer) - 1, 0);

            if (n > 0) {
                buffer[n] = '\0';
                std::cout << buffer;
            } else if (n == 0) {
                std::cout << "server closed connection" << std::endl;
                break;
            } else {
                std::cerr << "recv failed: " << strerror(errno) << std::endl;
                break;
            }
        }

        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::cout << "connection closed" << std::endl;
            break;
        }
    }

    close(sock_fd);

    return 0;
}
