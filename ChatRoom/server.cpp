#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

constexpr int kDefaultPort = 9000;
constexpr int kMaxEvents = 1024;
constexpr std::size_t kBufferSize = 4096;
constexpr std::size_t kMaxInputBuffer = 8192;
constexpr std::size_t kMaxChatMessage = 1000;
constexpr std::size_t kMaxNicknameLength = 20;



struct Client {
    //v2新增内容
    int fd=-1;
    //新增昵称
    std::string nickname;
    bool has_nickname=false;
    //当用户要退出时-》先把输出缓冲区中的数据发完，然后再关闭连接---处理QUIT
    bool close_after_write=false;

    std::string in_buffer;
    std::string out_buffer;
};
//v2新增命令结构体，第一个是命令名字，第二个是具体指令
struct Command{
    std::string name;
    std::string argument;
};//记得要加分号

static std::unordered_map<int, Client> clients;
//v2新增
//实现：使用toupper.。。
//目的：实现转大写操作
std::string to_upper_ascii(std::string text){
    for(char& c:text){
        c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

//v2新增解析命令函数
//实现：
//目的：用于删除字符串开头和结尾的空格/tab/\r------不同系统的换行格式不同需要处理
std::string trim(const std::string& text){
const auto begin=text.find_first_not_of("  \t\r");
    if(begin==std::string::npos)return "";

    const auto end=text.find_last_not_of(" \t\r");
    return text.substr(begin,end-begin+1);
}


//v2新增解析命令函数
//实现：
//目的：去掉首尾空格+提取第一个单词作为命令+把剩余内容作为参数
Command parse_command(const std::string& line){
    const std::string cleaned =trim(line);
    if(cleaned.empty())return {};
    const std::size_t separator=cleaned.find_first_of(" \t");
    if(separator==std::string::npos)return {to_upper_ascii(cleaned),""};
    return {to_upper_ascii(cleaned.substr(0,separator)),trim(cleaned.substr(separator+1))};
}


//v2新增临时用户名称
//实现：判断是否有昵称
//目的：创建临时用户昵称
std::string display_name(const Client& client){
    if(client.has_nickname)return client.nickname;
    return "guest-"+std::to_string(client.fd);
}

//v2新增解析命令函数
//实现：首先判断长度是1到20之间，其次只能游数字字母下划线构成，其他组成不通过
//目的：判断昵称是否合规

bool is_valid_nickname(const std::string& nickname){
    if(nickname.empty()||nickname.size()>kMaxNicknameLength)return false;
    for(char ch:nickname){
        const unsigned char value = static_cast<unsigned char>(ch);
        if(!std::isalnum(value)&&ch != '_')return false;
    }
    return true;
}
//v2新增
//实现：遍历
//目的：检查昵称唯一性，服务端不允许两个在线用户是相同的名字
bool nickname_in_use(const std::string& nickname,int except_fd){
    for(const auto& [fd,client]:clients){
        if(fd!=except_fd&&client.has_nickname&&client.nickname==nickname)
        return true;
    }
    return false;
}

int set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void update_epoll_events(int epoll_fd, int client_fd) {
    const auto it = clients.find(client_fd);
    if (it == clients.end()) {
        return;
    }

    epoll_event event{};
    event.data.fd = client_fd;
    event.events = EPOLLRDHUP;

    if (!it->second.close_after_write) {
        event.events |= EPOLLIN;
    }

    if (!it->second.out_buffer.empty()) {
        event.events |= EPOLLOUT;
    }

    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &event) == -1) {
        std::cerr << "epoll_ctl MOD failed, fd=" << client_fd
                  << ", error=" << std::strerror(errno) << '\n';
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

void broadcast_message(
    int epoll_fd,
    int sender_fd,
    const std::string& message,
    bool include_sender
) {
    for (auto& pair : clients) {
        int client_fd = pair.first;

        if (!include_sender && client_fd == sender_fd) {
            continue;
        }

        queue_message(epoll_fd, client_fd, message);
    }
}

void close_client(int epoll_fd, int client_fd,bool announce = true) {
    const auto it = clients.find(client_fd);
    if (it == clients.end()) {
        return;
    }

    const std::string name = display_name(it->second);

    std::cout << "client disconnected, fd=" << client_fd
              << ", name=" << name << '\n';

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
    close(client_fd);
    clients.erase(it);

    if (announce) {
        broadcast_message(
            epoll_fd,
            client_fd,
            "[system] " + name + " left the chat.\n",
            false
        );
    }
}
void send_help(int epoll_fd, int client_fd) {
    queue_message(
        epoll_fd,
        client_fd,
        "[system] commands:\n"
        "  HELP                show this help\n"
        "  NICK <name>         set or change nickname\n"
        "  SAY <message>       send a public message\n"
        "  WHO                 list online users\n"
        "  QUIT                leave the server\n"
    );
}

bool handle_command(
    int epoll_fd,
    int client_fd,
    const std::string& raw_line
) {
    auto it = clients.find(client_fd);
    if (it == clients.end()) {
        return false;
    }

    const Command command = parse_command(raw_line);

    if (command.name.empty()) {
        queue_message(epoll_fd, client_fd, "[error] empty command.\n");
        return true;
    }

    if (command.name == "HELP") {
        send_help(epoll_fd, client_fd);
        return true;
    }

    if (command.name == "NICK") {
        if (command.argument.find_first_of(" \t") != std::string::npos ||
            !is_valid_nickname(command.argument)) {
            queue_message(
                epoll_fd,
                client_fd,
                "[error] nickname must be 1-20 characters and contain only "
                "letters, digits, or underscore.\n"
            );
            return true;
        }

        if (nickname_in_use(command.argument, client_fd)) {
            queue_message(
                epoll_fd,
                client_fd,
                "[error] nickname is already in use.\n"
            );
            return true;
        }

        Client& client = it->second;
        const std::string old_name = display_name(client);

        if (client.has_nickname && client.nickname == command.argument) {
            queue_message(
                epoll_fd,
                client_fd,
                "[system] your nickname is already " + client.nickname + ".\n"
            );
            return true;
        }

        client.nickname = command.argument;
        client.has_nickname = true;

        broadcast_message(
            epoll_fd,
            client_fd,
            "[system] " + old_name + " is now known as " +
                client.nickname + ".\n",
            true
        );
        return true;
    }

    if (command.name == "SAY") {
        const Client& client = it->second;

        if (!client.has_nickname) {
            queue_message(
                epoll_fd,
                client_fd,
                "[error] set a nickname first: NICK <name>\n"
            );
            return true;
        }

        if (command.argument.empty()) {
            queue_message(
                epoll_fd,
                client_fd,
                "[error] usage: SAY <message>\n"
            );
            return true;
        }

        if (command.argument.size() > kMaxChatMessage) {
            queue_message(
                epoll_fd,
                client_fd,
                "[error] message is too long; maximum is " +
                    std::to_string(kMaxChatMessage) + " bytes.\n"
            );
            return true;
        }

        const std::string message =
            "[" + client.nickname + "] " + command.argument + "\n";

        std::cout << "chat from fd=" << client_fd
                  << ", name=" << client.nickname
                  << ": " << command.argument << '\n';

        broadcast_message(epoll_fd, client_fd, message, true);
        return true;
    }

    if (command.name == "WHO") {
        std::vector<std::string> names;
        names.reserve(clients.size());

        for (const auto& [fd, client] : clients) {
            (void)fd;
            names.push_back(display_name(client));
        }

        std::sort(names.begin(), names.end());

        std::string response =
            "[system] online users (" + std::to_string(names.size()) + "): ";

        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i != 0) {
                response += ", ";
            }
            response += names[i];
        }
        response += "\n";

        queue_message(epoll_fd, client_fd, response);
        return true;
    }

    if (command.name == "QUIT") {
        Client& client = it->second;
        client.close_after_write = true;
        queue_message(epoll_fd, client_fd, "[system] goodbye.\n");
        return false;
    }

    queue_message(
        epoll_fd,
        client_fd,
        "[error] unknown command. Type HELP to see available commands.\n"
    );
    return true;
}

void handle_client_read(int epoll_fd, int client_fd) {
    char buffer[kBufferSize];

    while (true) {
        const ssize_t received =
            recv(client_fd, buffer, sizeof(buffer), 0);

        if (received > 0) {
            auto it = clients.find(client_fd);
            if (it == clients.end()) {
                return;
            }

            Client& client = it->second;
            client.in_buffer.append(
                buffer,
                static_cast<std::size_t>(received)
            );

            if (client.in_buffer.size() > kMaxInputBuffer &&
                client.in_buffer.find('\n') == std::string::npos) {
                client.close_after_write = true;
                queue_message(
                    epoll_fd,
                    client_fd,
                    "[error] input line is too long; connection will close.\n"
                );
                return;
            }

            while (true) {
                const std::size_t newline = client.in_buffer.find('\n');
                if (newline == std::string::npos) {
                    break;
                }

                std::string line = client.in_buffer.substr(0, newline);
                client.in_buffer.erase(0, newline + 1);

                if (!handle_command(epoll_fd, client_fd, line)) {
                    return;
                }

                if (clients.find(client_fd) == clients.end()) {
                    return;
                }
            }
        } else if (received == 0) {
            close_client(epoll_fd, client_fd);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }

            std::cerr << "recv failed, fd=" << client_fd
                      << ", error=" << std::strerror(errno) << '\n';

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

    Client& client = it->second;

    while (!client.out_buffer.empty()) {
        const ssize_t sent = send(
            client_fd,
            client.out_buffer.data(),
            client.out_buffer.size(),
            MSG_NOSIGNAL
        );

        if (sent > 0) {
            client.out_buffer.erase(
                0,
                static_cast<std::size_t>(sent)
            );
        } else if (sent == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            std::cerr << "send failed, fd=" << client_fd
                      << ", error=" << std::strerror(errno) << '\n';

            close_client(epoll_fd, client_fd);
            return;
        }
    }

    if (client.out_buffer.empty() && client.close_after_write) {
        close_client(epoll_fd, client_fd);
        return;
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
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        std::cerr << "setsockopt failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        return -1;
    }

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
        std::cerr << "set_non_blocking failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

void accept_new_clients(int epoll_fd, int listen_fd) {
    while (true) {
        sockaddr_in client_address{};
        socklen_t client_length = sizeof(client_address);

        const int client_fd = accept(
            listen_fd,
            reinterpret_cast<sockaddr*>(&client_address),
            &client_length
        );

        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }

            std::cerr << "accept failed: "
                      << std::strerror(errno) << '\n';
            return;
        }

        if (set_non_blocking(client_fd) == -1) {
            std::cerr << "set_non_blocking client failed: "
                      << std::strerror(errno) << '\n';
            close(client_fd);
            continue;
        }

        epoll_event event{};
        event.data.fd = client_fd;
        event.events = EPOLLIN | EPOLLRDHUP;

        if (epoll_ctl(
                epoll_fd,
                EPOLL_CTL_ADD,
                client_fd,
                &event
            ) == -1) {
            std::cerr << "epoll_ctl ADD client failed: "
                      << std::strerror(errno) << '\n';
            close(client_fd);
            continue;
        }

        clients.emplace(
            client_fd,
            Client{client_fd, "", false, false, "", ""}
        );

        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(
            AF_INET,
            &client_address.sin_addr,
            ip,
            sizeof(ip)
        );

        const std::string guest_name =
            "guest-" + std::to_string(client_fd);

        std::cout << "new client connected, fd=" << client_fd
                  << ", ip=" << ip
                  << ", port=" << ntohs(client_address.sin_port)
                  << '\n';

        queue_message(
            epoll_fd,
            client_fd,
            "[system] welcome, you are temporarily " + guest_name + ".\n"
            "[system] set a nickname before chatting: NICK <name>\n"
        );
        send_help(epoll_fd, client_fd);

        broadcast_message(
            epoll_fd,
            client_fd,
            "[system] " + guest_name + " joined the chat.\n",
            false
        );
    }
}

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

int main(int argc, char* argv[]) {
    int port = kDefaultPort;

    if (argc >= 2 && !parse_port(argv[1], port)) {
        std::cerr << "invalid port; expected 1-65535\n";
        return 1;
    }

    const int listen_fd = create_listen_socket(port);
    if (listen_fd == -1) {
        return 1;
    }

    const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) {
        std::cerr << "epoll_create1 failed: "
                  << std::strerror(errno) << '\n';
        close(listen_fd);
        return 1;
    }

    epoll_event listen_event{};
    listen_event.data.fd = listen_fd;
    listen_event.events = EPOLLIN;

    if (epoll_ctl(
            epoll_fd,
            EPOLL_CTL_ADD,
            listen_fd,
            &listen_event
        ) == -1) {
        std::cerr << "epoll_ctl ADD listen socket failed: "
                  << std::strerror(errno) << '\n';
        close(listen_fd);
        close(epoll_fd);
        return 1;
    }

    std::cout << "chat_server v2 started on port " << port << '\n';

    epoll_event events[kMaxEvents];

    while (true) {
        const int ready =
            epoll_wait(epoll_fd, events, kMaxEvents, -1);

        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            }

            std::cerr << "epoll_wait failed: "
                      << std::strerror(errno) << '\n';
            break;
        }

        for (int i = 0; i < ready; ++i) {
            const int fd = events[i].data.fd;
            const uint32_t event_mask = events[i].events;

            if (fd == listen_fd) {
                accept_new_clients(epoll_fd, listen_fd);
                continue;
            }

            if (event_mask & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                close_client(epoll_fd, fd);
                continue;
            }

            if (event_mask & EPOLLIN) {
                handle_client_read(epoll_fd, fd);
            }

            if (clients.find(fd) != clients.end() &&
                (event_mask & EPOLLOUT)) {
                handle_client_write(epoll_fd, fd);
            }
        }
    }

    for (const auto& [fd, client] : clients) {
        (void)client;
        close(fd);
    }

    close(listen_fd);
    close(epoll_fd);
    return 0;
}
