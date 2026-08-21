#pragma once

#include "client/client_state.hpp"

#include <string>

class SqliteClient;
class TlsClientTransport;

void print_local_help();

bool handle_local_command(
    TlsClientTransport& transport,
    const std::string& line,
    ClientState& state,
    SqliteClient& cache
);
