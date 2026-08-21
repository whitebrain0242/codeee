#include "chat_server.hpp"
#include "minimuduo/net/TcpConnection.hpp"

#include "protocol.hpp"

#include <charconv>
#include <cstdint>
#include <string>

namespace {

bool parse_heartbeat_nonce(
    const std::string& text,
    std::uint64_t& value
) {
    const std::string cleaned =
        trim(text);

    if (cleaned.empty()) {
        return false;
    }

    const char* begin =
        cleaned.data();

    const char* end =
        begin + cleaned.size();

    const auto result =
        std::from_chars(
            begin,
            end,
            value
        );

    return
        result.ec == std::errc() &&
        result.ptr == end;
}

}  // namespace

void ChatServer::handle_ping(
    const TcpConnectionPtr& connection,
    const std::string& arguments
) {
    std::uint64_t nonce = 0;

    if (!parse_heartbeat_nonce(
            arguments,
            nonce
        )) {
        // PING/PONG are protocol-reserved. Invalid heartbeat frames are
        // ignored instead of exposing them as normal user commands.
        return;
    }

    connection->send(
        "PONG " +
        std::to_string(nonce) +
        "\n"
    );
}

