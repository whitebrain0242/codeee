#include "chat_server.hpp"
#include "config.hpp"
#include "protocol.hpp"
#include "server/server_console.hpp"
#include "server/server_log.hpp"

#include "integration/redis_client.hpp"

#include "minimuduo/net/EventLoop.hpp"
#include "minimuduo/net/TcpServer.hpp"
#include "minimuduo/net/TlsContext.hpp"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <string>

int main(int argc, char *argv[]) {
  int port = 9000;

  std::string mysql_config_path = "config/mysql.conf";

  std::string redis_config_path = "config/redis.conf";

  std::string tls_config_path = "config/tls_server.conf";

  int worker_threads = 4;

  std::filesystem::path file_storage_root = "data/server_files";

  std::filesystem::path log_directory = "logs";

  if (argc >= 2 && !parse_port(argv[1], port)) {
    std::cerr << "服务端端口号无效\n";
    return 1;
  }

  if (argc >= 3) {
    mysql_config_path = argv[2];
  }

  if (argc >= 4) {
    redis_config_path = argv[3];
  }

  if (argc >= 5) {
    tls_config_path = argv[4];
  }

  if (argc >= 6) {
    std::size_t parsed = 0U;

    if (!parse_count(argv[5], 1U, 64U, parsed)) {
      std::cerr << "工作线程数必须在 1-64 之间\n";
      return 1;
    }

    worker_threads = static_cast<int>(parsed);
  }

  if (argc >= 7) {
    file_storage_root = argv[6];
  }

  if (argc >= 8) {
    log_directory = argv[7];
  }

  (void)std::signal(SIGPIPE, SIG_IGN);

  std::string error;

  if (!initialize_server_logging(log_directory, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  spdlog::info("正在启动聊天室服务");

  MySqlConfig mysql_config;

  if (!load_mysql_config(mysql_config_path, mysql_config, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  RedisConfig redis_config;

  if (!load_redis_config(redis_config_path, redis_config, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  TlsServerConfig tls_config;

  if (!load_tls_server_config(tls_config_path, tls_config, error)) {
    std::cerr << "TLS 服务端配置加载失败：" << error << '\n';
    return 1;
  }

  auto tls_context = std::make_shared<minimuduo::net::TlsServerContext>();

  if (!tls_context->initialize(tls_config, error)) {
    std::cerr << "TLS 服务端初始化失败：" << error << '\n';
    return 1;
  }

  MySqlDatabase database;

  if (!database.connect(mysql_config, error)) {
    std::cerr << "MySQL 连接失败：" << error << '\n';
    return 1;
  }

  if (!database.ping(error)) {
    spdlog::error("MySQL 连接池健康检查失败：{}", error);
    return 1;
  }

  spdlog::info("MySQL 连接池已就绪，共 {} 个连接", database.pool_size());

  RedisClient redis;

  if (!redis.connect(redis_config, error) || !redis.ping(error)) {
    std::cerr << "Redis 连接失败：" << error << '\n';
    return 1;
  }

  sockaddr_in listen_address{};
  listen_address.sin_family = AF_INET;
  listen_address.sin_addr.s_addr = htonl(INADDR_ANY);
  listen_address.sin_port = htons(static_cast<std::uint16_t>(port));

  minimuduo::net::EventLoop main_loop;

  std::optional<ChatServer> chat_server;

  minimuduo::net::TcpServer tcp_server(&main_loop, listen_address, "chat");

  tcp_server.setThreadNum(worker_threads);

  tcp_server.setTlsContext(tls_context);

  const std::string server_instance_id =
      redis_config.server_name + ":" + std::to_string(port);

  try {
    chat_server.emplace(tcp_server, database, redis, server_instance_id,
                        redis_config.presence_ttl_seconds, file_storage_root);
  } catch (const std::exception &exception) {
    std::cerr << "聊天室服务初始化失败：" << exception.what()
              << '\n';
    return 1;
  }

  tcp_server.start();

  ServerConsoleInfo console_info;
  console_info.port = port;
  console_info.worker_threads = worker_threads;
  console_info.tls_certificate = tls_config.certificate_file;
  console_info.server_instance_id = server_instance_id;
  console_info.file_storage_root = file_storage_root;
  console_info.log_directory = log_directory;
  console_info.mysql_pool_size =
      static_cast<unsigned int>(database.pool_size());

  print_server_console(console_info);

  spdlog::info("服务端已就绪，端口 {}，工作 Reactor 数量 {}；"
               "heartbeat=client PING/server PONG; file_payload=raw binary",
               port, worker_threads);

  main_loop.loop();

  return 0;
}
