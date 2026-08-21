#pragma once

#include <filesystem>
#include <string>

struct ServerConsoleInfo {
    int port = 0;
    int worker_threads = 0;
    std::string tls_certificate;
    std::string server_instance_id;
    std::filesystem::path file_storage_root;
    std::filesystem::path log_directory;
    unsigned int mysql_pool_size = 0U;
};

void print_server_console(
    const ServerConsoleInfo& info
);
