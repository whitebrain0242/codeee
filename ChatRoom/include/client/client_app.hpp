#pragma once

#include "client/client_heartbeat.hpp"
#include "client/client_state.hpp"
#include "client/tls_client_transport.hpp"
#include "integration/sqlite_client.hpp"

#include <filesystem>
#include <string>

struct ClientAppConfig {
    std::string host =
        "127.0.0.1";

    int port = 9000;

    std::string sqlite_path =
        "chat_client.db";

    std::filesystem::path download_root =
        "downloads";

    std::string tls_config_path =
        "config/tls_client.conf";
};

class ClientApp {
public:
    int run(
        const ClientAppConfig& config
    );

private:
    bool initialize(
        const ClientAppConfig& config
    );

    bool read_tls_available();

    void consume_complete_lines();

    void process_server_line(
        const std::string& line
    );

    void remember_login_attempt(
        const std::string& line
    );

    SqliteClient cache_;
    TlsClientTransport transport_;
    ClientHeartbeat heartbeat_;
    ClientState state_;

    std::string server_buffer_;
};
