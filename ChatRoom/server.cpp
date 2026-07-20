#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>

static constexpr int MAX_EVENTS = 1024;
static constexpr int BUFFER_SIZE = 4096;

struct Client {
    int fd;
    std::string in_buffer;
    std::string out_buffer;
};

static std::unordered_map<int, Client> clients;

int set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void close_client(int epoll_fd, int client_fd) {
    std::cout << "client disconnected, fd = " << client_fd << std::endl;

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
    close(client_fd);
    clients.erase(client_fd);
}

void update_epoll_events(int epoll_fd, int client_fd) {
    epoll_event event{};
    event.data.fd = client_fd;
    event.events = EPOLLIN | EPOLLRDHUP;

    auto it = clients.find(client_fd);
    if (it != clients.end() && !it->second.out_buffer.empty()) {
        event.events |= EPOLLOUT;
    }

    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &event) == -1) {
        std::cerr << "epoll_ctl mod failed: " << strerror(errno) << std::endl;
    }
}

void queue_message(int epoll_fd, int client_fd, const std::string& message) {
    auto it = clients.find(client_fd);
    if (it == clients.end()) {
        return;
    }

    it->second.out_buffer += message;
    update_epoll_events(epoll_fd, client_fd);
}

void handle_client_read(int epoll_fd, int client_fd) {
    char buffer[BUFFER_SIZE];

    while (true) {
        ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);

        if (n > 0) {
            Client& client = clients[client_fd];
            client.in_buffer.append(buffer, n);

            while (true) {
                size_t pos = client.in_buffer.find('\n');
                if (pos == std::string::npos) {
                    break;
                }

                std::string line = client.in_buffer.substr(0, pos + 1);
                client.in_buffer.erase(0, pos + 1);

                std::cout << "recv from fd " << client_fd << ": " << line;

                std::string reply = "[server echo] " + line;
                queue_message(epoll_fd, client_fd, reply);
            }
        } else if (n == 0) {
            close_client(epoll_fd, client_fd);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            std::cerr << "recv failed: " << strerror(errno) << std::endl;
            close_client(epoll_fd, client_fd);
            return;
        }
    }
}

void handle_client_write(int epoll_fd, int client_fd) {
    auto it = clients.find(client_fd);
    if (it == clients.end()) {
        return;
    }

    std::string& out = it->second.out_buffer;

    while (!out.empty()) {
        ssize_t n = send(client_fd, out.data(), out.size(), 0);

        if (n > 0) {
            out.erase(0, n);
        } else if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            std::cerr << "send failed: " << strerror(errno) << std::endl;
            close_client(epoll_fd, client_fd);
            return;
        }
    }

    update_epoll_events(epoll_fd, client_fd);
}

int create_listen_socket(int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        std::cerr << "socket failed: " << strerror(errno) << std::endl;
        return -1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
        std::cerr << "bind failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, SOMAXCONN) == -1) {
        std::cerr << "listen failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        return -1;
    }

    if (set_non_blocking(listen_fd) == -1) {
        std::cerr << "set_non_blocking failed" << std::endl;
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

void accept_new_clients(int epoll_fd, int listen_fd) {
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(
            listen_fd,
            reinterpret_cast<sockaddr*>(&client_addr),
            &client_len
        );

        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            std::cerr << "accept failed: " << strerror(errno) << std::endl;
            break;
        }

        if (set_non_blocking(client_fd) == -1) {
            std::cerr << "set_non_blocking client failed" << std::endl;
            close(client_fd);
            continue;
        }

        epoll_event event{};
        event.data.fd = client_fd;
        event.events = EPOLLIN | EPOLLRDHUP;

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) == -1) {
            std::cerr << "epoll_ctl add client failed: " << strerror(errno) << std::endl;
            close(client_fd);
            continue;
        }

        clients[client_fd] = Client{client_fd, "", ""};

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

        std::cout << "new client connected, fd = " << client_fd
                  << ", ip = " << ip
                  << ", port = " << ntohs(client_addr.sin_port)
                  << std::endl;

        queue_message(epoll_fd, client_fd, "Welcome to chatroom_v0.\n");
    }
}

int main(int argc, char* argv[]) {
    int port = 9000;

    if (argc >= 2) {
        port = std::stoi(argv[1]);
    }

    int listen_fd = create_listen_socket(port);
    if (listen_fd == -1) {
        return 1;
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        std::cerr << "epoll_create1 failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }

    epoll_event listen_event{};
    listen_event.data.fd = listen_fd;
    listen_event.events = EPOLLIN;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &listen_event) == -1) {
        std::cerr << "epoll_ctl add listen_fd failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        close(epoll_fd);
        return 1;
    }

    std::cout << "chat_server started on port " << port << std::endl;

    epoll_event events[MAX_EVENTS];

    while (true) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }

            std::cerr << "epoll_wait failed: " << strerror(errno) << std::endl;
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == listen_fd) {
                accept_new_clients(epoll_fd, listen_fd);
                continue;
            }

            if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                close_client(epoll_fd, fd);
                continue;
            }

            if (ev & EPOLLIN) {
                handle_client_read(epoll_fd, fd);
            }

            if (ev & EPOLLOUT) {
                handle_client_write(epoll_fd, fd);
            }
        }
    }

    close(listen_fd);
    close(epoll_fd);

    return 0;
}
