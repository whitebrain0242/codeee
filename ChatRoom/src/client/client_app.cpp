#include "client/client_app.hpp"
#include "client/client_common.hpp"
#include "client/client_file_transfer.hpp"
#include "client/client_local_commands.hpp"
#include "client/client_message_cache.hpp"
#include "client/client_numeric_commands.hpp"
#include "config.hpp"
#include "protocol.hpp"
#include <termios.h>
#include <unistd.h>

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
            << "[heartbeat] client sends PING every 20s; server only replies PONG; timeout 60s.\n"
            << "[local] SQLite cache: " << cache_.database_path() << '\n'
            << "[local] download root: " << state_.download_root.string() << '\n'
            << "[local] 输入 39 查看所有数字命令。英文命令已禁用。\n";

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

      NumericCommand numeric;
      if (!parse_numeric_command(line, numeric) || numeric.number < 1 || numeric.number > 40) {
        std::cout << "[local error] 仅支持数字编号命令。输入 39 查看帮助。\n";
        continue;
      }

      if (numeric.number == 8 || numeric.number == 9) {
        if (numeric.arguments.empty()) {
          std::cout << (numeric.number == 8 ? "[local error] usage: 8 <username>\n"
                                           : "[local error] usage: 9 <group_name>\n");
          continue;
        }
        state_.chat_scope = numeric.number == 8 ? ClientState::ChatScope::Private
                                                : ClientState::ChatScope::Group;
        state_.chat_target = numeric.arguments;
        std::cout << "[system] Entered "
                  << (numeric.number == 8 ? "private chat with " : "group chat ")
                  << state_.chat_target << ".\n";
        continue;
      }

      if (numeric.number == 10) {
        if (state_.chat_scope == ClientState::ChatScope::None) {
          std::cout << "[system] No active chat.\n";
        } else {
          state_.chat_scope = ClientState::ChatScope::None;
          state_.chat_target.clear();
          std::cout << "[system] Left chat.\n";
        }
        continue;
      }

      if (numeric.number == 6 || numeric.number == 7) {
        const bool correct_scope =
            (numeric.number == 6 && state_.chat_scope == ClientState::ChatScope::Private) ||
            (numeric.number == 7 && state_.chat_scope == ClientState::ChatScope::Group);
        if (!numeric.arguments.empty()) {
          std::cout << "[local error] " << numeric.number << " 不带参数。\n";
          continue;
        }
        if (!correct_scope || state_.chat_target.empty()) {
          std::cout << "[local error] 请先通过 " << (numeric.number == 6 ? "8 <username>" : "9 <group_name>")
                    << " 进入对应会话。\n";
          continue;
        }
        if (!run_chat_editor()) {
          running = false;
          break;
        }
        continue;
      }

      if (handle_local_command(transport_, line, state_, cache_)) continue;

      std::string english_name;
      if (!map_numeric_network_command(numeric.number, english_name)) {
        std::cout << "[local error] unsupported command number: " << numeric.number << '\n';
        continue;
      }
      const std::string wire = english_name + (numeric.arguments.empty() ? "" : " " + numeric.arguments);
      remember_login_attempt(wire);
      if (!send_network_line(wire)) {
        running = false;
        break;
      }
    }

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

bool ClientApp::send_network_line(const std::string &line) {
  std::string send_error;
  if (!transport_.send(line + "\n", send_error)) {
    std::cerr << "TLS send failed: " << send_error << '\n';
    return false;
  }
  return true;
}

bool ClientApp::run_chat_editor() {
  const bool private_chat = state_.chat_scope == ClientState::ChatScope::Private;
  const std::string command_name = private_chat ? "MSG" : "GROUP_MSG";
  const std::string target = state_.chat_target;

  std::cout << "[multi-line] 已进入与 " << target
            << " 的持续聊天模式。输入内容后用 /send 发送当前消息；发送后可直接继续输入下一条。/quit 退出会话。\n";

  std::vector<std::string> lines;
  pollfd descriptors[2]{};
  descriptors[0].fd = STDIN_FILENO;
  descriptors[0].events = POLLIN;
  descriptors[1].fd = transport_.fd();
  descriptors[1].events = POLLIN;

  while (true) {
    if (transport_.pending() > 0 && !read_tls_available()) return false;

    const int result = ::poll(descriptors, 2, 1000);
    if (result < 0) {
      if (errno == EINTR) continue;
      std::cerr << "poll failed: " << std::strerror(errno) << '\n';
      return false;
    }

    if (descriptors[0].revents & POLLIN) {
      std::string line;
      if (!std::getline(std::cin, line)) return false;

      if (line == "/quit") {
        lines.clear();
        state_.chat_scope = ClientState::ChatScope::None;
        state_.chat_target.clear();
        std::cout << "[system] Left chat.\n";
        return true;
      }

      if (line == "/send") {
        std::string message;
        for (std::size_t i = 0; i < lines.size(); ++i) {
          if (i != 0) message.push_back('\n');
          message += lines[i];
        }
        if (message.empty()) {
          std::cout << "[multi-line] 当前消息为空，请输入内容后再 /send。\n";
          continue;
        }

        const std::string encoded = encode_text_token(message);
        if (!send_network_line(command_name + " " + target + " " + encoded)) return false;
        lines.clear();
        std::cout << "[multi-line] 已发送。可直接输入下一条消息，/send 发送，/quit 退出。\n";
        continue;
      }

      lines.push_back(line); // 空行也保留
    }

    if (descriptors[1].revents & POLLIN) {
      if (!read_tls_available()) return false;
    }
    if (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      if (transport_.pending() > 0) (void)read_tls_available();
      return false;
    }

    std::string heartbeat_error;
    if (!heartbeat_.tick(transport_, heartbeat_error)) {
      std::cerr << "[heartbeat] " << heartbeat_error << "; closing connection.\n";
      return false;
    }
  }
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
  if (heartbeat_.consume_protocol_line(line, transport_)) {
    return;
  }

  if (handle_file_protocol_line(transport_, line, state_, cache_)) {
    return;
  }

  std::cout << line << '\n';

  if (starts_with(line, "[system] login successful.") &&
      !state_.pending_login_username.empty()) {
    state_.active_username = state_.pending_login_username;

    state_.pending_login_username.clear();

    std::cout << "[local] SQLite cache account: " << state_.active_username
              << '\n';

    (void)load_and_resume_pending_uploads(transport_, state_, cache_);
  }

  cache_server_message(line, state_, cache_);

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

  // LOGOUT 和 DELETE_ACCOUNT 都要清理客户端登录状态
  if (logged_out || account_deleted) {

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
