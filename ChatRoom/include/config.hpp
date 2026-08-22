#pragma once

#include "tls_config.hpp"

#include <string>
// 配置读取
struct MySqlConfig {
  std::string host = "127.0.0.1";
  unsigned int port = 3306;
  std::string user;
  std::string password;
  std::string database = "chatroom";
  unsigned int connect_timeout_seconds = 5;
  unsigned int pool_size = 8;
};

struct RedisConfig {
  std::string host = "127.0.0.1";
  unsigned int port = 6379;
  std::string password;
  unsigned int database = 0;
  unsigned int connect_timeout_ms = 1000;
  unsigned int presence_ttl_seconds = 120;
  std::string key_prefix = "chatroom";
  std::string server_name = "chat-server";
};

bool load_mysql_config(const std::string &path, MySqlConfig &config,
                       std::string &error);

bool load_redis_config(const std::string &path, RedisConfig &config,
                       std::string &error);

bool load_tls_server_config(const std::string &path, TlsServerConfig &config,
                            std::string &error);

bool load_tls_client_config(const std::string &path, TlsClientConfig &config,
                            std::string &error);
