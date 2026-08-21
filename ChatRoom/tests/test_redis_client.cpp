#include "integration/redis_client.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

bool send_all(int fd, const std::string& data) {
    std::size_t offset = 0;

    while (offset < data.size()) {
        const ssize_t sent = ::send(
            fd,
            data.data() + offset,
            data.size() - offset,
            MSG_NOSIGNAL
        );

        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }

    return true;
}

bool read_exact(
    int fd,
    std::string& output,
    std::size_t count
) {
    output.clear();
    output.resize(count);
    std::size_t offset = 0;

    while (offset < count) {
        const ssize_t received = ::recv(
            fd,
            output.data() + offset,
            count - offset,
            0
        );

        if (received > 0) {
            offset += static_cast<std::size_t>(received);
        } else if (received < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }

    return true;
}

bool read_line(int fd, std::string& line) {
    line.clear();

    while (true) {
        char byte = '\0';
        const ssize_t received =
            ::recv(fd, &byte, 1, 0);

        if (received == 0) {
            return false;
        }

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        line.push_back(byte);

        if (line.size() >= 2U &&
            line[line.size() - 2U] == '\r' &&
            line.back() == '\n') {
            line.resize(line.size() - 2U);
            return true;
        }
    }
}

bool read_command(
    int fd,
    std::vector<std::string>& parts
) {
    std::string line;
    if (!read_line(fd, line) ||
        line.empty() ||
        line[0] != '*') {
        return false;
    }

    const int count = std::stoi(line.substr(1));
    parts.clear();
    parts.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        if (!read_line(fd, line) ||
            line.empty() ||
            line[0] != '$') {
            return false;
        }

        const std::size_t size =
            static_cast<std::size_t>(
                std::stoul(line.substr(1))
            );

        std::string payload;
        if (!read_exact(fd, payload, size)) {
            return false;
        }

        std::string crlf;
        if (!read_exact(fd, crlf, 2U) ||
            crlf != "\r\n") {
            return false;
        }

        parts.push_back(std::move(payload));
    }

    return true;
}

std::string upper(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(
            std::toupper(
                static_cast<unsigned char>(ch)
            )
        );
    }
    return value;
}

bool send_bulk(
    int fd,
    const std::optional<std::string>& value
) {
    if (!value) {
        return send_all(fd, "$-1\r\n");
    }

    return send_all(
        fd,
        "$" +
            std::to_string(value->size()) +
            "\r\n" +
            *value +
            "\r\n"
    );
}

class FakeRedisServer {
public:
    ~FakeRedisServer() {
        stop();
    }

    bool start() {
        listen_fd_ = ::socket(
            AF_INET,
            SOCK_STREAM,
            0
        );

        if (listen_fd_ < 0) {
            return false;
        }

        int reuse = 1;
        (void)::setsockopt(
            listen_fd_,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            sizeof(reuse)
        );

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr =
            htonl(INADDR_LOOPBACK);
        address.sin_port = htons(0);

        if (::bind(
                listen_fd_,
                reinterpret_cast<const sockaddr*>(
                    &address
                ),
                sizeof(address)
            ) != 0 ||
            ::listen(listen_fd_, 4) != 0) {
            return false;
        }

        socklen_t length = sizeof(address);
        if (::getsockname(
                listen_fd_,
                reinterpret_cast<sockaddr*>(
                    &address
                ),
                &length
            ) != 0) {
            return false;
        }

        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] {
            run();
        });
        return true;
    }

    unsigned int port() const {
        return port_;
    }

    void stop() {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }

        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    int listen_fd_ = -1;
    unsigned int port_ = 0;
    std::thread thread_;
    std::unordered_map<std::string, std::string> strings_;
    std::map<
        std::pair<std::string, std::string>,
        long long
    > hashes_;

    void run() {
        sockaddr_in peer{};
        socklen_t length = sizeof(peer);

        const int client_fd = ::accept(
            listen_fd_,
            reinterpret_cast<sockaddr*>(&peer),
            &length
        );

        if (client_fd < 0) {
            return;
        }

        std::vector<std::string> command;

        while (read_command(client_fd, command)) {
            if (command.empty()) {
                break;
            }

            const std::string name =
                upper(command[0]);

            if (name == "SELECT") {
                if (!send_all(client_fd, "+OK\r\n")) {
                    break;
                }
            } else if (name == "PING") {
                if (!send_all(client_fd, "+PONG\r\n")) {
                    break;
                }
            } else if (name == "SET" &&
                       command.size() >= 3U) {
                const std::string& key = command[1];

                if (strings_.count(key) != 0U) {
                    if (!send_all(client_fd, "$-1\r\n")) {
                        break;
                    }
                } else {
                    strings_[key] = command[2];
                    if (!send_all(client_fd, "+OK\r\n")) {
                        break;
                    }
                }
            } else if (name == "GET" &&
                       command.size() == 2U) {
                const auto iterator =
                    strings_.find(command[1]);

                if (!send_bulk(
                        client_fd,
                        iterator == strings_.end()
                            ? std::optional<std::string>{}
                            : std::optional<std::string>{
                                  iterator->second
                              }
                    )) {
                    break;
                }
            } else if (name == "EVAL" &&
                       command.size() >= 5U) {
                const std::string& key = command[3];
                const std::string& owner = command[4];
                const auto iterator = strings_.find(key);

                if (iterator == strings_.end() ||
                    iterator->second != owner) {
                    if (!send_all(client_fd, ":0\r\n")) {
                        break;
                    }
                } else if (command.size() == 5U) {
                    strings_.erase(iterator);
                    if (!send_all(client_fd, ":1\r\n")) {
                        break;
                    }
                } else {
                    if (!send_all(client_fd, ":1\r\n")) {
                        break;
                    }
                }
            } else if (name == "HINCRBY" &&
                       command.size() == 4U) {
                const auto key =
                    std::make_pair(
                        command[1],
                        command[2]
                    );

                long long& value = hashes_[key];
                value += std::stoll(command[3]);

                if (!send_all(
                        client_fd,
                        ":" +
                            std::to_string(value) +
                            "\r\n"
                    )) {
                    break;
                }
            } else if (name == "HSET" &&
                       command.size() == 4U) {
                hashes_[
                    std::make_pair(
                        command[1],
                        command[2]
                    )
                ] = std::stoll(command[3]);

                if (!send_all(client_fd, ":1\r\n")) {
                    break;
                }
            } else if (name == "EXPIRE") {
                if (!send_all(client_fd, ":1\r\n")) {
                    break;
                }
            } else if (name == "HMGET" &&
                       command.size() == 6U) {
                if (!send_all(client_fd, "*4\r\n")) {
                    break;
                }

                for (std::size_t index = 2U;
                     index < 6U;
                     ++index) {
                    const auto iterator =
                        hashes_.find(
                            std::make_pair(
                                command[1],
                                command[index]
                            )
                        );

                    if (iterator == hashes_.end()) {
                        if (!send_all(
                                client_fd,
                                "$-1\r\n"
                            )) {
                            break;
                        }
                    } else {
                        const std::string value =
                            std::to_string(
                                iterator->second
                            );

                        if (!send_bulk(
                                client_fd,
                                value
                            )) {
                            break;
                        }
                    }
                }
            } else {
                if (!send_all(
                        client_fd,
                        "-ERR unsupported fake command\r\n"
                    )) {
                    break;
                }
            }
        }

        ::close(client_fd);
    }
};

}  // namespace

int main() {
    FakeRedisServer server;

    if (!server.start()) {
        std::cerr << "failed to start fake Redis server\n";
        return EXIT_FAILURE;
    }

    {
        RedisConfig config;
        config.host = "127.0.0.1";
        config.port = server.port();
        config.database = 0;
        config.connect_timeout_ms = 1000;
        config.key_prefix = "testchat";

        RedisClient redis;
        std::string error;

        if (!redis.connect(config, error) ||
            !redis.ping(error)) {
            std::cerr << "connect/ping failed: " << error << '\n';
            return EXIT_FAILURE;
        }

        bool claimed = false;
        if (!redis.claim_presence(
                "alice",
                "server-a",
                120,
                claimed,
                error
            ) ||
            !claimed) {
            std::cerr << "claim failed: " << error << '\n';
            return EXIT_FAILURE;
        }

        if (!redis.claim_presence(
                "alice",
                "server-b",
                120,
                claimed,
                error
            ) ||
            claimed) {
            std::cerr << "duplicate claim semantics failed\n";
            return EXIT_FAILURE;
        }

        std::optional<std::string> owner;
        if (!redis.presence_owner(
                "alice",
                owner,
                error
            ) ||
            !owner ||
            *owner != "server-a") {
            std::cerr << "presence owner failed: " << error << '\n';
            return EXIT_FAILURE;
        }

        bool refreshed = false;
        if (!redis.refresh_presence_if_owned(
                "alice",
                "server-a",
                120,
                refreshed,
                error
            ) ||
            !refreshed) {
            std::cerr << "presence refresh failed: " << error << '\n';
            return EXIT_FAILURE;
        }

        std::int64_t result = 0;
        if (!redis.adjust_unread(
                "alice",
                "private",
                2,
                result,
                error
            ) ||
            result != 2 ||
            !redis.adjust_unread(
                "alice",
                "group",
                1,
                result,
                error
            ) ||
            result != 1 ||
            !redis.adjust_unread(
                "alice",
                "private_file",
                3,
                result,
                error
            ) ||
            result != 3 ||
            !redis.adjust_unread(
                "alice",
                "group_file",
                4,
                result,
                error
            ) ||
            result != 4) {
            std::cerr << "unread increment failed: " << error << '\n';
            return EXIT_FAILURE;
        }

        RedisUnreadCounts counts;
        if (!redis.unread_counts(
                "alice",
                counts,
                error
            ) ||
            counts.private_messages != 2 ||
            counts.group_messages != 1 ||
            counts.private_files != 3 ||
            counts.group_files != 4) {
            std::cerr << "unread lookup failed: " << error << '\n';
            return EXIT_FAILURE;
        }

        if (!redis.adjust_unread(
                "alice",
                "private",
                -1,
                result,
                error
            ) ||
            result != 1) {
            std::cerr << "unread decrement failed: " << error << '\n';
            return EXIT_FAILURE;
        }

        bool removed = false;
        if (!redis.remove_presence_if_owned(
                "alice",
                "server-b",
                removed,
                error
            ) ||
            removed) {
            std::cerr << "wrong-owner removal failed\n";
            return EXIT_FAILURE;
        }

        if (!redis.remove_presence_if_owned(
                "alice",
                "server-a",
                removed,
                error
            ) ||
            !removed) {
            std::cerr << "owner removal failed: " << error << '\n';
            return EXIT_FAILURE;
        }

        if (!redis.presence_owner(
                "alice",
                owner,
                error
            ) ||
            owner) {
            std::cerr << "presence removal verification failed\n";
            return EXIT_FAILURE;
        }
    }

    server.stop();
    std::cout << "redis client fake-server tests passed\n";
    return EXIT_SUCCESS;
}
