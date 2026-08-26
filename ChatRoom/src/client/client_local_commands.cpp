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
    std::cout << "      <empty content>\n";
    return;
  }

  std::size_t begin = 0U;
  while (begin <= content.size()) {
    const std::size_t newline = content.find('\n', begin);
    const std::string line =
        newline == std::string::npos ? content.substr(begin)
                                     : content.substr(begin, newline - begin);
    std::cout << "      " << line << '\n';
    if (newline == std::string::npos) break;
    begin = newline + 1U;
  }
}

bool require_no_arguments(int number, const std::string &arguments) {
  if (trim(arguments).empty()) return true;
  std::cout << "[local error] command " << number << " takes no arguments.\n";
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
    std::cout << "[local] SQLite database: " << cache.database_path() << '\n'
              << "[local] download root: " << state.download_root.string() << '\n';
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
      std::cout << "[local error] command 11 requires <username> [count]; "
                   "the target must be explicit.\n";
      return true;
    }

    std::size_t count = kDefaultLocalHistory;
    if (words.size() == 2U &&
        !parse_count(words[1], 1U, kMaxLocalHistory, count)) {
      std::cout << "[local error] count must be 1-200.\n";
      return true;
    }

    std::vector<LocalPrivateMessage> messages;
    std::string error;
    if (!cache.recent_private_messages(state.active_username, words[0], count,
                                       messages, error)) {
      std::cout << "[local sqlite error] " << error << '\n';
      return true;
    }

    std::cout << "[local private history with " << words[0] << "] "
              << messages.size() << " message(s):\n";
    for (const LocalPrivateMessage &message : messages) {
      std::cout << "  #" << message.server_message_id << "  "
                << format_local_time(message.received_at_unix_ms) << "  "
                << message.sender_username << " -> " << message.recipient_username;
      if (message.offline_delivery) std::cout << "  [offline-delivery]";
      std::cout << '\n';
      print_indented_content(message.content);
    }
    return true;
  }

  if (command_number == 12) {
    const std::vector<std::string> words = split_words(arguments);
    if (words.empty() || words.size() > 2U) {
      std::cout << "[local error] command 12 requires <group_name> [count]; "
                   "the target must be explicit.\n";
      return true;
    }

    std::size_t count = kDefaultLocalHistory;
    if (words.size() == 2U &&
        !parse_count(words[1], 1U, kMaxLocalHistory, count)) {
      std::cout << "[local error] count must be 1-200.\n";
      return true;
    }

    std::vector<LocalGroupMessage> messages;
    std::string error;
    if (!cache.recent_group_messages(state.active_username, words[0], count,
                                     messages, error)) {
      std::cout << "[local sqlite error] " << error << '\n';
      return true;
    }

    std::cout << "[local group history " << words[0] << "] "
              << messages.size() << " message(s):\n";
    for (const LocalGroupMessage &message : messages) {
      std::cout << "  #G" << message.server_message_id << "  "
                << format_local_time(message.received_at_unix_ms) << "  ["
                << message.sender_username << "]";
      if (message.offline_delivery) std::cout << "  [offline-delivery]";
      std::cout << '\n';
      print_indented_content(message.content);
    }
    return true;
  }

  if (command_number == 13) {
    if (!require_no_arguments(command_number, arguments)) return true;
    if (!has_active_chat(state)) {
      std::cout << "[local error] command 13 requires an active chat session; "
                   "use 8 or 9 first.\n";
      return true;
    }

    const std::string scope =
        state.chat_scope == ClientChatScope::Private ? "PRIVATE" : "GROUP";
    std::vector<LocalFileTransfer> files;
    std::string error;
    if (!cache.recent_file_transfers_for_chat(
            state.active_username, scope, state.chat_target, kMaxLocalHistory,
            files, error)) {
      std::cout << "[local sqlite error] " << error << '\n';
      return true;
    }

    std::cout << "[local files] " << scope << " " << state.chat_target << ": "
              << files.size() << " record(s):\n";
    for (const LocalFileTransfer &file : files) {
      std::cout << "  #F" << file.server_transfer_id << " "
                << (file.outgoing ? "sent" : "received") << " "
                << file.file_name << " (" << file.file_size
                << " bytes) path=" << file.local_path << '\n';
    }
    return true;
  }

  // command 14
  if (!has_active_chat(state)) {
    std::cout << "[local error] command 14 requires an active chat session; "
                 "use 8 or 9 first.\n";
    return true;
  }
  const std::string path = trim(arguments);
  if (path.empty()) {
    std::cout << "[local error] command 14 requires <file_path>.\n";
    return true;
  }

  const std::string scope =
      state.chat_scope == ClientChatScope::Private ? "PRIVATE" : "GROUP";
  (void)prepare_upload(transport, state, cache, scope, state.chat_target, path);
  return true;
}
