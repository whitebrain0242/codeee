#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

constexpr int kDefaultPort = 9000;
constexpr std::size_t kBufferSize = 4096;

int connect_to_server(const std::string& ip, int port) {
    const int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
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
//v1:新增send_all函数
bool send_all(int socket_fd, const std::string& data) {
    std::size_t total_sent = 0;

    while (total_sent < data.size()) {
        const ssize_t sent = send(
            socket_fd,
            data.data() + total_sent,
            data.size() - total_sent,
            MSG_NOSIGNAL
        );

        if (sent > 0) {
            total_sent += static_cast<std::size_t>(sent);
            continue;
        }

        if (sent == -1 && errno == EINTR) {
            continue;
        }

        std::cerr << "send failed: "
                  << std::strerror(errno) << '\n';
        return false;
    }

    return true;
}

//v2新增自定义端口函数

bool parse_port(const char* text, int& port) {
    try {
        const int value = std::stoi(text);
        if (value < 1 || value > 65535) {
            return false;
        }
        port = value;
        return true;
    } catch (...) {
        return false;
    }
}

std::string first_word_upper(const std::string& line){
    const std::size_t begin =line.find_first_not_of(" \t");
    if(begin==std::string::npos)return "";
    const std::size_t end=line.find_first_of(" \t",begin);
    std::string word=line.substr(begin,end-begin);
    for(char&ch:word){
        if(ch>='a'&&ch<='z')ch = static_cast<char>(ch - 'a' + 'A');
    } 
    return word;
}


int main(int argc, char* argv[]) {
    std::string ip = "127.0.0.1";
    int port = kDefaultPort;

    if (argc >= 2) {
        ip = argv[1];
    }

    if (argc >= 3 && !parse_port(argv[2], port)) {
        std::cerr << "invalid port; expected 1-65535\n";
        return 1;
    }

    const int sock_fd = connect_to_server(ip, port);
    if (sock_fd == -1) {
        return 1;
    }

    std::cout << "connected to server " << ip << ":" << port << std::endl;
    std::cout << "enter HELP to view commands.\n";

    pollfd descriptors[2]{};
    descriptors[0].fd = STDIN_FILENO;
    descriptors[0].events = POLLIN;
    descriptors[1].fd = sock_fd;
    descriptors[1].events = POLLIN;

    bool waiting_for_server_close = false;
    char buffer[kBufferSize];

    while (true) {
        int ret = poll(descriptors, 2, -1);

        if (ret == -1) {
            if (errno == EINTR) {
                continue;
            }

            std::cerr << "poll failed: " << strerror(errno) << std::endl;
            break;
        }

        if ((descriptors[0].revents & POLLIN)&&!waiting_for_server_close) {
            std::string line;
            if (!std::getline(std::cin, line)) {
                line = "QUIT";
                waiting_for_server_close = true;
                descriptors[0].fd = -1;
            }

            if (!send_all(sock_fd, line + "\n")) {
                break;
            }

            if (first_word_upper(line) == "QUIT") {
                waiting_for_server_close = true;
                descriptors[0].fd = -1;
            }
        }

        if (descriptors[1].revents & POLLIN) {
            ssize_t n = recv(sock_fd, buffer, sizeof(buffer) - 1, 0);

            if (n > 0) {
                std::cout.write(buffer, n);
                std::cout.flush();
            } else if (n == 0) {
                std::cout << "server closed connection" << std::endl;
                break;
            } else if (errno != EINTR) {
                std::cerr << "recv failed: "
                          << std::strerror(errno) << '\n';
                break;
            }
        }

        if (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::cout << "connection closed" << std::endl;
            break;
        }
    }

    close(sock_fd);

    return 0;
}
