#pragma once
#include "client/client_state.hpp"
#include <string>

class SqliteClient;
class TlsClientTransport;

void print_local_help();

bool handle_local_numeric_command(
    TlsClientTransport &transport,
    int command_number,
    const std::string &arguments,
    ClientState &state,
    SqliteClient &cache
);
