#pragma once

#include <filesystem>
#include <string>

bool initialize_server_logging(
    const std::filesystem::path& log_directory,
    std::string& error
);
