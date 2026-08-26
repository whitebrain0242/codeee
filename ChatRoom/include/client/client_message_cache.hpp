#pragma once

#include "client/client_state.hpp"
#include "integration/sqlite_client.hpp"

#include <string>

bool parse_private_message_line(
    const std::string& line,
    const std::string& active_username,
    LocalPrivateMessage& message
);

bool parse_group_message_line(
    const std::string& line,
    const std::string& active_username,
    LocalGroupMessage& message
);

void cache_server_message(
    const std::string& line,
    const ClientState& state,
    SqliteClient& cache
);

bool display_chat_message_line(
    const std::string& line,
    const ClientState& state
);

