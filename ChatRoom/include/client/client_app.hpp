#pragma once

#include "client/client_heartbeat.hpp"
#include "client/client_message_cache.hpp"
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

  // 正常菜单、消息模式选择、消息输入、参数输入分别由不同状态处理。
  bool handle_user_input(const std::string &line);

  // 数字菜单和“输入编号后再输入参数”是两个不同的输入状态。
  // pending_input_command_ != 0 时，下一行是参数，不再按数字命令解析。
  bool execute_numeric_command(int number, const std::string &arguments);
  bool command_requires_followup_input(int number) const;
  void begin_followup_input(int number);
  void print_current_menu() const;
  void request_menu_refresh();
  void flush_menu_refresh();

  // 进入会话后，把之前只缓存未显示的消息和文件 offer 交给当前会话。
  void flush_deferred_chat_items();
  bool handle_chat_message_visibility(const std::string &line);

  // 33/34 共用的入群申请加载与编号选择流程；32 不再暴露给用户。
  bool begin_group_request_action(GroupRequestAction action);
  bool handle_group_request_protocol_line(const std::string &line);
  bool handle_group_request_selection_input(const std::string &line);
  void clear_group_request_action();

  void remember_login_attempt(const std::string &line);

  SqliteClient cache_;//SQLite
  AsyncMessageCacheWriter cache_writer_;//消息后台落库，不阻塞网络接收
  TlsClientTransport transport_;//TLS网络传输
  ClientHeartbeat heartbeat_;//心跳管理
  ClientState state_;//客户端状态

  int pending_input_command_ = 0;
  bool menu_refresh_pending_ = false;

  std::string server_buffer_;//接收缓冲区
};
