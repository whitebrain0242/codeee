#include "chat_server.hpp"
#include "in_memory_friend_repository.hpp"
#include "in_memory_user_repository.hpp"
#include "protocol.hpp"

#include <iostream>

int main(int argc, char* argv[]) {
    int port = 19321;

    if (argc >= 2) {
        if (!chat::parse_port(argv[1], port)) {
            std::cerr
                << "invalid test port"
                << std::endl;
            return 1;
        }
    }

    chat::test::InMemoryUserRepository
        user_repository;

    chat::test::InMemoryFriendRepository
        friend_repository;

    std::string error;

    if (!user_repository.initialize(error)) {
        std::cerr << error << std::endl;
        return 1;
    }

    if (!friend_repository.initialize(error)) {
        std::cerr << error << std::endl;
        return 1;
    }

    chat::ChatServer server(
        port,
        user_repository,
        friend_repository
    );

    if (!server.initialize()) {
        return 1;
    }

    return server.run();
}
