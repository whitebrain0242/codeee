#include "client/client_app.hpp"

#include "client/client_common.hpp"
#include "client/client_file_transfer.hpp"
#include "client/client_local_commands.hpp"
#include "client/client_message_cache.hpp"
#include "config.hpp"
#include "protocol.hpp"

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

int ClientApp::run(
    const ClientAppConfig& config
) {
    if (!initialize(config)) {
        return 1;
    }

    std::cout
        << "TLS connected to "
        << config.host
        << ":"
        << config.port
        << '\n'
        << "[tls] version: "
        << transport_.tls_version()
        << ", cipher: "
        << transport_.cipher_name()
        << '\n'
        << "[tls] verified peer identity: "
        << transport_.peer_identity()
        << '\n'
        << "[heartbeat] client sends PING every 20s; "
           "server only replies PONG; timeout 60s.\n"
        << "[local] SQLite cache: "
        << cache_.database_path()
        << '\n'
        << "[local] download root: "
        << state_.download_root.string()
        << '\n'
        << "[local] Type LOCAL_HELP for local/file commands.\n";

    pollfd descriptors[2]{};

    descriptors[0].fd =
        STDIN_FILENO;

    descriptors[0].events =
        POLLIN;

    descriptors[1].fd =
        transport_.fd();

    descriptors[1].events =
        POLLIN;

    bool waiting_for_server_close =
        false;

    bool running =
        true;

    while (running) {
        if (transport_.pending() > 0) {
            if (!read_tls_available()) {
                break;
            }
        }

        const int result =
            ::poll(
                descriptors,
                2,
                1000
            );

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            std::cerr
                << "poll failed: "
                << std::strerror(errno)
                << '\n';

            break;
        }

        if (!waiting_for_server_close &&
            (
                descriptors[0].revents &
                POLLIN
            )) {
            std::string line;

            if (!std::getline(
                    std::cin,
                    line
                )) {
                break;
            }

            if (handle_local_command(
                    transport_,
                    line,
                    state_,
                    cache_
                )) {
                continue;
            }

            remember_login_attempt(
                line
            );

            const bool quitting =
                trim(line) ==
                "QUIT";

            std::string send_error;

            if (!transport_.send(
                    line + "\n",
                    send_error
                )) {
                std::cerr
                    << "TLS send failed: "
                    << send_error
                    << '\n';

                break;
            }

            if (quitting) {
                waiting_for_server_close =
                    true;

                descriptors[0].fd =
                    -1;
            }
        }

        if (descriptors[1].revents &
            POLLIN) {
            if (!read_tls_available()) {
                running = false;
            }
        }

        if (descriptors[1].revents &
            (
                POLLERR |
                POLLHUP |
                POLLNVAL
            )) {
            if (transport_.pending() > 0) {
                (void)read_tls_available();
            }

            break;
        }

        std::string heartbeat_error;

        if (!heartbeat_.tick(
                transport_,
                heartbeat_error
            )) {
            std::cerr
                << "[heartbeat] "
                << heartbeat_error
                << "; closing connection.\n";
            break;
        }

    }

    if (!server_buffer_.empty()) {
        process_server_line(
            server_buffer_
        );
    }

    preserve_partial_downloads(
        state_
    );

    transport_.shutdown();

    return 0;
}

bool ClientApp::initialize(
    const ClientAppConfig& config
) {
    std::string error;

    if (!cache_.open(
            config.sqlite_path,
            error
        )) {
        std::cerr
            << "SQLite open failed: "
            << error
            << '\n';

        return false;
    }

    TlsClientConfig tls_config;

    if (!load_tls_client_config(
            config.tls_config_path,
            tls_config,
            error
        )) {
        std::cerr
            << "TLS client config failed: "
            << error
            << '\n';

        return false;
    }

    if (!transport_.connect(
            config.host,
            config.port,
            tls_config,
            error
        )) {
        std::cerr
            << "TLS connection failed: "
            << error
            << '\n';

        return false;
    }

    state_.download_root =
        config.download_root;

    return true;
}

bool ClientApp::read_tls_available() {
    char buffer[kClientBufferSize]{};

    while (true) {
        const TransportReadResult result =
            transport_.receive(
                buffer,
                sizeof(buffer)
            );

        if (result.status ==
            TransportReadStatus::Data) {
            server_buffer_.append(
                buffer,
                result.bytes
            );

            consume_complete_lines();

            if (transport_.pending() <= 0) {
                return true;
            }

            continue;
        }

        if (result.status ==
            TransportReadStatus::Retry) {
            return true;
        }

        if (result.status ==
            TransportReadStatus::Closed) {
            return false;
        }

        std::cerr
            << "TLS receive failed: "
            << result.error
            << '\n';

        return false;
    }
}

void ClientApp::consume_complete_lines() {
    while (true) {
        if (state_.binary_download
                .remaining_bytes > 0U) {
            if (server_buffer_.empty()) {
                break;
            }

            if (!consume_file_binary_payload(
                    transport_,
                    server_buffer_,
                    state_
                )) {
                std::cerr
                    << "[file error] failed to persist "
                       "raw binary download payload.\n";
                break;
            }

            continue;
        }

        const std::size_t newline =
            server_buffer_.find('\n');

        if (newline ==
            std::string::npos) {
            break;
        }

        std::string line =
            server_buffer_.substr(
                0,
                newline
            );

        server_buffer_.erase(
            0,
            newline + 1U
        );

        if (!line.empty() &&
            line.back() == '\r') {
            line.pop_back();
        }

        process_server_line(
            line
        );
    }

    std::cout.flush();
}

void ClientApp::process_server_line(
    const std::string& line
) {
    if (heartbeat_.consume_protocol_line(
            line,
            transport_
        )) {
        return;
    }

    if (handle_file_protocol_line(
            transport_,
            line,
            state_,
            cache_
        )) {
        return;
    }

    std::cout
        << line
        << '\n';

    if (starts_with(
            line,
            "[system] login successful."
        ) &&
        !state_
             .pending_login_username
             .empty()) {
        state_.active_username =
            state_
                .pending_login_username;

        state_
            .pending_login_username
            .clear();

        std::cout
            << "[local] SQLite cache account: "
            << state_.active_username
            << '\n';

        (void)load_and_resume_pending_uploads(
            transport_,
            state_,
            cache_
        );
    }

    cache_server_message(
        line,
        state_,
        cache_
    );

    if (starts_with(
            line,
            "[system] logout successful."
        )) {
        state_.active_username.clear();
        state_.pending_uploads.clear();
        state_.upload_queue.clear();
        state_.active_upload_token.clear();
        state_.downloads.clear();
    }
}

void ClientApp::remember_login_attempt(
    const std::string& line
) {
    const Command command =
        parse_command(line);

    if (command.name !=
        "LOGIN") {
        return;
    }

    const std::vector<std::string> words =
        split_words(
            command.raw_arguments
        );

    if (words.size() == 2U) {
        state_.pending_login_username =
            words[0];
    }
}
