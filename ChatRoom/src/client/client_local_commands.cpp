#include "client/client_local_commands.hpp"

#include "client/client_chat_session.hpp"
#include "client/client_common.hpp"
#include "client/client_file_transfer.hpp"
#include "client/client_numeric_commands.hpp"
#include "client/tls_client_transport.hpp"
#include "integration/sqlite_client.hpp"
#include "protocol.hpp"

#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::string format_local_time(std::int64_t unix_ms) {
  const std::time_t seconds = static_cast<std::time_t>(unix_ms / 1000);
  std::tm value{};
#if defined(_WIN32)
  localtime_s(&value, &seconds);
#else
  localtime_r(&seconds, &value);
#endif
  std::ostringstream output;
  output << std::put_time(&value, "%Y-%m-%d %H:%M:%S");
  return output.str();
}

void print_indented_content(const std::string &content) {
  if (content.empty()) {
    std::cout << "      <空内容>\n";
    return;
  }

  std::size_t begin = 0U;
  while (begin <= content.size()) {
    const std::size_t newline = content.find('\n', begin);
    const std::string text_line =
        newline == std::string::npos ? content.substr(begin)
                                     : content.substr(begin, newline - begin);
    std::cout << "      " << text_line << '\n';
    if (newline == std::string::npos) break;
    begin = newline + 1U;
  }
}

bool require_no_arguments(int number, const std::string &arguments) {
  if (trim(arguments).empty()) return true;
  std::cout << "[本地错误] 命令 " << number << " 不接受额外参数。\n";
  return false;
}
} // namespace

void print_local_help() { print_numeric_help(); }

bool handle_local_numeric_command(TlsClientTransport &transport,
                                  int command_number,
                                  const std::string &arguments,
                                  ClientState &state,
                                  SqliteClient &cache) {
  if (command_number == 39) {
    if (require_no_arguments(command_number, arguments)) print_local_help();
    return true;
  }

  if (command_number == 40) {
    if (!require_no_arguments(command_number, arguments)) return true;
    std::cout << "[本地] SQLite 数据库：" << cache.database_path() << '\n'
              << "[本地] 下载目录：" << state.download_root.string() << '\n';
    return true;
  }

  if (command_number != 11 && command_number != 12 &&
      command_number != 13 && command_number != 14) {
    return false;
  }

  if (!require_local_account(state)) return true;

  if (command_number == 11) {
    const std::vector<std::string> words = split_words(arguments);
    if (words.empty() || words.size() > 2U) {
      std::cout << "[本地错误] 命令 11 需要输入 <好友用户名> [条数]。\n";
      return true;
    }

    std::size_t count = kDefaultLocalHistory;
    if (words.size() == 2U &&
        !parse_count(words[1], 1U, kMaxLocalHistory, count)) {
      std::cout << "[本地错误] 历史条数必须在 1-200 之间。\n";
      return true;
    }

    std::vector<LocalPrivateMessage> messages;
    std::string error;
    if (!cache.recent_private_messages(state.active_username, words[0], count,
                                       messages, error)) {
      std::cout << "[本地 SQLite 错误] " << error << '\n';
      return true;
    }

    std::cout << "[本地私聊历史] 好友：" << words[0] << "，共 "
              << messages.size() << " 条：\n";
    for (const LocalPrivateMessage &message : messages) {
      std::cout << "  #" << message.server_message_id << "  "
                << format_local_time(message.received_at_unix_ms) << "  "
                << message.sender_username << " -> " << message.recipient_username;
      if (message.offline_delivery) std::cout << "  [离线投递]";
      std::cout << '\n';
      print_indented_content(message.content);
    }
    return true;
  }

  if (command_number == 12) {
    const std::vector<std::string> words = split_words(arguments);
    if (words.empty() || words.size() > 2U) {
      std::cout << "[本地错误] 命令 12 需要输入 <群名称> [条数]。\n";
      return true;
    }

    std::size_t count = kDefaultLocalHistory;
    if (words.size() == 2U &&
        !parse_count(words[1], 1U, kMaxLocalHistory, count)) {
      std::cout << "[本地错误] 历史条数必须在 1-200 之间。\n";
      return true;
    }

    std::vector<LocalGroupMessage> messages;
    std::string error;
    if (!cache.recent_group_messages(state.active_username, words[0], count,
                                     messages, error)) {
      std::cout << "[本地 SQLite 错误] " << error << '\n';
      return true;
    }

    std::cout << "[本地群聊历史] 群：" << words[0] << "，共 "
              << messages.size() << " 条：\n";
    for (const LocalGroupMessage &message : messages) {
      std::cout << "  #G" << message.server_message_id << "  "
                << format_local_time(message.received_at_unix_ms) << "  ["
                << message.sender_username << "]";
      if (message.offline_delivery) std::cout << "  [离线投递]";
      std::cout << '\n';
      print_indented_content(message.content);
    }
    return true;
  }

  if (command_number == 13) {
    if (!require_no_arguments(command_number, arguments)) return true;
    if (!has_active_chat(state)) {
      std::cout << "[本地错误] 命令 13 需要当前已有活动会话，请先使用 8 或 9 进入会话。\n";
      return true;
    }

    const std::string scope =
        state.chat_scope == ClientChatScope::Private ? "PRIVATE" : "GROUP";
    const char *scope_name =
        state.chat_scope == ClientChatScope::Private ? "好友私聊" : "群聊";
    std::vector<LocalFileTransfer> files;
    std::string error;
    if (!cache.recent_file_transfers_for_chat(
            state.active_username, scope, state.chat_target, kMaxLocalHistory,
            files, error)) {
      std::cout << "[本地 SQLite 错误] " << error << '\n';
      return true;
    }

    std::cout << "[本地文件记录] " << scope_name << "：" << state.chat_target
              << "，共 " << files.size() << " 条：\n";
    for (const LocalFileTransfer &file : files) {
      std::cout << "  #F" << file.server_transfer_id << " "
                << (file.outgoing ? "已发送" : "已接收") << " "
                << file.file_name << "（" << file.file_size
                << " 字节）路径=" << file.local_path << '\n';
    }
    return true;
  }

  if (!has_active_chat(state)) {
    std::cout << "[本地错误] 命令 14 需要当前已有活动会话，请先使用 8 或 9 进入会话。\n";
    return true;
  }

  const std::string path = trim(arguments);
  if (path.empty()) {
    std::cout << "[本地错误] 命令 14 需要输入文件路径。\n";
    return true;
  }

  const std::string scope =
      state.chat_scope == ClientChatScope::Private ? "PRIVATE" : "GROUP";
  (void)prepare_upload(transport, state, cache, scope, state.chat_target, path);
  return true;
}
