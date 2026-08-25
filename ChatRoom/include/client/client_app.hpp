#pragma once

#include "client/client_heartbeat.hpp"
#include "client/client_state.hpp"
#include "client/tls_client_transport.hpp"
#include "integration/sqlite_client.hpp"

#include <filesystem>
#include <string>

struct ClientAppConfig {
  std::string host = "127.0.0.1";//IP

  int port = 9000;//端口

  std::string sqlite_path = "chat_client.db";//本地SQlite保存路径

  std::filesystem::path download_root = "downloads";//下载根目录

  std::string tls_config_path = "config/tls_client.conf";//TLS客户端配置文件路径
};

class ClientApp {
public:
  int run(const ClientAppConfig &config);

private:
  //加载配置和TCP TLS连接
  bool initialize(const ClientAppConfig &config);

  bool read_tls_available();

  void consume_complete_lines();

  void process_server_line(const std::string &line);

  void remember_login_attempt(const std::string &line);

  // 6/7 进入后持续发送多条消息；/send 只结束当前消息，/quit 才退出会话。
  bool run_chat_editor();
  bool send_network_line(const std::string &line);

  SqliteClient cache_;//SQlite
  TlsClientTransport transport_;//TLS网络传输
  ClientHeartbeat heartbeat_;//心跳管理
  ClientState state_;//客户端状态

  std::string server_buffer_;//接收缓冲区
};
