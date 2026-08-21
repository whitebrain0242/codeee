#include "server/server_console.hpp"

#include <unistd.h>

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string row(
    const std::string& label,
    const std::string& value
) {
    std::ostringstream output;
    output
        << "  "
        << std::left
        << std::setw(18)
        << label
        << value;
    return output.str();
}

}  // namespace

void print_server_console(
    const ServerConsoleInfo& info
) {
    const bool color =
        ::isatty(STDOUT_FILENO) != 0;

    const char* cyan =
        color ? "\033[36m" : "";
    const char* green =
        color ? "\033[32m" : "";
    const char* bold =
        color ? "\033[1m" : "";
    const char* reset =
        color ? "\033[0m" : "";

    std::cout
        << '\n'
        << cyan
        << "+----------------------------------------------------------+"
        << reset
        << '\n'
        << cyan
        << "| "
        << bold
        << "Chatroom Server - Binary Zero-Copy Edition"
        << reset
        << cyan
        << "                 |"
        << reset
        << '\n'
        << cyan
        << "+----------------------------------------------------------+"
        << reset
        << '\n'
        << row(
               "Listen",
               "0.0.0.0:" +
                   std::to_string(info.port)
           )
        << '\n'
        << row(
               "Reactors",
               "1 main + " +
                   std::to_string(
                       info.worker_threads
                   ) +
                   " worker(s)"
           )
        << '\n'
        << row(
               "Heartbeat",
               "client PING -> server PONG + TCP keepalive"
           )
        << '\n'
        << row(
               "File payload",
               "raw binary; kTLS SSL_sendfile when available"
           )
        << '\n'
        << row(
               "MySQL pool",
               std::to_string(
                   info.mysql_pool_size
               ) +
                   " connection(s)"
           )
        << '\n'
        << row(
               "Server ID",
               info.server_instance_id
           )
        << '\n'
        << row(
               "File storage",
               info.file_storage_root.string()
           )
        << '\n'
        << row(
               "Log directory",
               info.log_directory.string()
           )
        << '\n'
        << row(
               "TLS certificate",
               info.tls_certificate
           )
        << '\n'
        << cyan
        << "+----------------------------------------------------------+"
        << reset
        << '\n'
        << green
        << "  [READY] server event loop started"
        << reset
        << "\n\n";
}
