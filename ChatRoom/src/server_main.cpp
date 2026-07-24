#include "chat_server.hpp"
#include "protocol.hpp"

#include <iostream>

namespace{
    constexpr int kDefaultPort = 9000; 
}
int main(int argc, char *argv[])
{
    int port = kDefaultPort;

    if (argc >= 2 && !chat::parse_port(argv[1], port))
    {
        std::cerr << "invalid port; expected 1-65535\n";
        return 1;
    }
    chat::ChatServer server(port);

    if (!server.initialize()) {
        return 1;
    }

    return server.run();
}
