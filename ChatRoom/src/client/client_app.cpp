#include "client/client_app.hpp"
#include "client/client_common.hpp"
#include "client/client_chat_session.hpp"
#include "client/client_numeric_commands.hpp"
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

int ClientApp::run(const ClientAppConfig &config) {
  if (!initialize(config)) return 1;

  std::cout << "TLS connected to " << config.host << ":" << config.port << '\n'
            << "[tls] version: " << transport_.tls_version()
            << ", cipher: " << transport_.cipher_name() << '\n'
            << "[tls] verified peer identity: " << transport_.peer_identity() << '\n'
            << "[heartbeat] client sends PING every 20s; "
               "server only replies PONG; timeout 60s.\n"
            << "[local] SQLite cache: " << cache_.database_path() << '\n'
            << "[local] download root: " << state_.download_root.string() << '\n'
            << "[local] User commands are numeric only. Type 39 for help.\n";

  pollfd descriptors[2]{};
  descriptors[0].fd = STDIN_FILENO;
  descriptors[0].events = POLLIN;
  descriptors[1].fd = transport_.fd();
  descriptors[1].events = POLLIN;

  bool running = true;
  while (running) {
    if (transport_.pending() > 0 && !read_tls_available()) break;

    const int result = ::poll(descriptors, 2, 1000);
    if (result < 0) {
      if (errno == EINTR) continue;
      std::cerr << "poll failed: " << std::strerror(errno) << '\n';
      break;
    }

    if (descriptors[0].revents & POLLIN) {
      std::string line;
      if (!std::getline(std::cin, line)) break;
      if (!handle_user_input(line)) break;
    }

    // 6/7 持续编辑时仍在同一个 poll 循环中处理服务端推送和心跳。
    if (descriptors[1].revents & POLLIN) {
      if (!read_tls_available()) running = false;
    }

    if (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      if (transport_.pending() > 0) (void)read_tls_available();
      break;
    }

    std::string heartbeat_error;
    if (!heartbeat_.tick(transport_, heartbeat_error)) {
      std::cerr << "[heartbeat] " << heartbeat_error << "; closing connection.\n";
      break;
    }
  }

  if (!server_buffer_.empty()) process_server_line(server_buffer_);
  preserve_partial_downloads(state_);
  transport_.shutdown();
  return 0;
}

bool ClientApp::handle_user_input(const std::string &line) {
  if (state_.chat_editor_active) {
    return handle_chat_editor_input(transport_, state_, line);
  }

  ParsedNumericCommand command;
  if (!parse_numeric_command_line(line, command)) {
    std::cout << "[local error] only numeric command IDs are accepted. "
                 "Type 39 for help.\n";
    return true;
  }

  if (command.number == 6 || command.number == 7) {
    if (!command.arguments.empty()) {
      std::cout << "[local error] command " << command.number
                << " takes no arguments.\n";
      return true;
    }
    const ClientChatScope expected =
        command.number == 6 ? ClientChatScope::Private : ClientChatScope::Group;
    (void)begin_continuous_chat(state_, expected);
    return true;
  }

  if (command.number == 8 || command.number == 9) {
    const std::vector<std::string> words = split_words(command.arguments);
    if (words.size() != 1U) {
      std::cout << "[local error] command " << command.number
                << " requires exactly one "
                << (command.number == 8 ? "username" : "group name") << ".\n";
      return true;
    }
    const ClientChatScope scope =
        command.number == 8 ? ClientChatScope::Private : ClientChatScope::Group;
    return request_chat_entry(transport_, state_, scope, words[0]);
  }

  if (command.number == 10) {
    if (!command.arguments.empty()) {
      std::cout << "[local error] command 10 takes no arguments.\n";
      return true;
    }
    leave_chat_session(state_, true);
    return true;
  }

  if (handle_local_numeric_command(transport_, command.number, command.arguments,
                                   state_, cache_)) {
    return true;
  }

  const char *name = numeric_command_name(command.number);
  if (name == nullptr) {
    std::cout << "[local error] unknown numeric command. Type 39 for help.\n";
    return true;
  }

  std::string wire = name;
  if (!command.arguments.empty()) {
    wire += " ";
    wire += command.arguments;
  }
  wire += "\n";

  remember_login_attempt(wire);

  std::string send_error;
  if (!transport_.send(wire, send_error)) {
    std::cerr << "TLS send failed: " << send_error << '\n';
    return false;
  }
  return true;
}

bool ClientApp::initialize(const ClientAppConfig &config) {
  std::string error;

  if (!cache_.open(config.sqlite_path, error)) {
    std::cerr << "SQLite open failed: " << error << '\n';

    return false;
  }

  TlsClientConfig tls_config;

  if (!load_tls_client_config(config.tls_config_path, tls_config, error)) {
    std::cerr << "TLS client config failed: " << error << '\n';

    return false;
  }

  if (!transport_.connect(config.host, config.port, tls_config, error)) {
    std::cerr << "TLS connection failed: " << error << '\n';

    return false;
  }

  state_.download_root = config.download_root;

  return true;
}

bool ClientApp::read_tls_available() {
  char buffer[kClientBufferSize]{};

  while (true) {
    const TransportReadResult result =
        transport_.receive(buffer, sizeof(buffer));

    if (result.status == TransportReadStatus::Data) {
      server_buffer_.append(buffer, result.bytes);

      consume_complete_lines();

      if (transport_.pending() <= 0) {
        return true;
      }

      continue;
    }

    if (result.status == TransportReadStatus::Retry) {
      return true;
    }

    if (result.status == TransportReadStatus::Closed) {
      return false;
    }

    std::cerr << "TLS receive failed: " << result.error << '\n';

    return false;
  }
}

void ClientApp::consume_complete_lines() {
  while (true) {
    // 正在接收二进制文件数据
    if (state_.binary_download.remaining_bytes > 0U) {
      if (server_buffer_.empty()) {
        break;
      }

      if (!consume_file_binary_payload(transport_, server_buffer_, state_)) {
        std::cerr << "[file error] failed to persist "
                     "raw binary download payload.\n";
        break;
      }

      continue;
    }
    // 查找文本行 用\n分隔
    const std::size_t newline = server_buffer_.find('\n');

    if (newline == std::string::npos) {
      break; // 没有完整行就等待数据
    }

    std::string line = server_buffer_.substr(0, newline);

    server_buffer_.erase(0, newline + 1U);

    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    process_server_line(line);
  }

  std::cout.flush();
}

void ClientApp::process_server_line(const std::string &line) {
  if (heartbeat_.consume_protocol_line(line, transport_)) return;
  if (handle_file_protocol_line(transport_, line, state_, cache_)) return;
  if (consume_chat_entry_response(line, state_)) return;

  // 聊天正文在线路上保持 percent-encoded；先完整落库，再解码显示。
  if (display_chat_message_line(line, state_)) {
    cache_server_message(line, state_, cache_);
    return;
  }

  std::cout << line << '\n';

  if (starts_with(line, "[system] login successful.") &&
      !state_.pending_login_username.empty()) {
    state_.active_username = state_.pending_login_username;
    state_.pending_login_username.clear();
    std::cout << "[local] SQLite cache account: " << state_.active_username << '\n';
    (void)load_and_resume_pending_uploads(transport_, state_, cache_);
  }

  const bool logged_out = starts_with(line, "[system] logout successful.");
  const bool account_deleted =
      starts_with(line, "[system] account deleted successfully.");

  // 如果是永久注销账号，删除这个账号对应的 SQLite 缓存
  if (account_deleted && !state_.active_username.empty()) {
    std::string cache_error;
    if (!cache_.clear_account_data(state_.active_username, cache_error)) {
      std::cout << "[local sqlite warning] "
                   "account was deleted on the server, "
                   "but local cache cleanup failed: "
                << cache_error << '\n';
    }
  }

  // LOGOUT 和 DELETE_ACCOUNT 都要清理客户端登录状态以及当前聊天会话
  if (logged_out || account_deleted) {
    leave_chat_session(state_, false);
    state_.active_username.clear();
    state_.pending_login_username.clear();
    state_.pending_uploads.clear();
    state_.upload_queue.clear();
    state_.active_upload_token.clear();
    state_.downloads.clear();
    state_.binary_download = {};
  }
}

void ClientApp::remember_login_attempt(const std::string &line) {
  const Command command = parse_command(line);

  if (command.name != "LOGIN") {
    return;
  }

  const std::vector<std::string> words = split_words(command.raw_arguments);

  if (words.size() == 2U) {
    state_.pending_login_username = words[0];
  }
}
