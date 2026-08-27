#include "server/server_log.hpp"

#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <exception>
#include <filesystem>
#include <memory>
#include <vector>

bool initialize_server_logging(const std::filesystem::path &log_directory,
                               std::string &error) {
  try {
    std::error_code filesystem_error;
    std::filesystem::create_directories(log_directory, filesystem_error);

    if (filesystem_error) {
      error = "无法创建日志目录：" + filesystem_error.message();
      return false;
    }

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

    console_sink->set_pattern("%^[%H:%M:%S] [%l] %v%$");

    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        (log_directory / "chat_server.log").string(), 10U * 1024U * 1024U, 5U,
        true);

    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] "
                           "[%l] [thread %t] %v");

    std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};

    auto logger = std::make_shared<spdlog::logger>("chat_server", sinks.begin(),
                                                   sinks.end());

    logger->set_level(spdlog::level::info);

    logger->flush_on(spdlog::level::warn);

    spdlog::set_default_logger(std::move(logger));

    return true;
  } catch (const std::exception &exception) {
    error = "spdlog 初始化失败：" + std::string(exception.what());
    return false;
  }
}
