#include "client/client_local_commands.hpp"

#include "client/client_common.hpp"
#include "client/client_file_transfer.hpp"
#include "client/client_numeric_commands.hpp"
#include "client/tls_client_transport.hpp"
#include "integration/sqlite_client.hpp"
#include "protocol.hpp"

#include <iostream>
#include <string>
#include <vector>

void print_local_help() { print_numeric_help(); }

bool handle_local_command(TlsClientTransport &transport,
                          const std::string &line, ClientState &state,
                          SqliteClient &cache) {
  NumericCommand command;
  if (!parse_numeric_command(line, command)) return false;

  if (command.number == 39) {
    print_numeric_help();
    return true;
  }

  if (command.number == 40) {
    std::cout << "[local] SQLite database: " << cache.database_path() << '\n'
              << "[local] download root: " << state.download_root.string() << '\n';
    return true;
  }

  if (command.number < 11 || command.number > 14) return false;

  if (!require_local_account(state)) return true;

  if (command.number == 11) {
    const std::vector<std::string> words = split_words(command.arguments);
    if (words.empty() || words.size() > 2U) {
      std::cout << "[local error] 请指定用户名: 11 <username> [count]\n";
      return true;
    }
    std::size_t count = kDefaultLocalHistory;
    if (words.size() == 2U && !parse_count(words[1], 1U, kMaxLocalHistory, count)) {
      std::cout << "[local error] count must be 1-200.\n";
      return true;
    }
    std::vector<LocalPrivateMessage> messages;
    std::string error;
    if (!cache.recent_private_messages(state.active_username, words[0], count, messages, error)) {
      std::cout << "[local sqlite error] " << error << '\n';
      return true;
    }
    std::cout << "[local private history with " << words[0] << "] " << messages.size() << " message(s):\n";
    for (const auto &message : messages) {
      std::cout << "  #" << message.server_message_id << " " << message.sender_username << " -> "
                << message.recipient_username << ": " << message.content;
      if (message.offline_delivery) std::cout << " [offline-delivery]";
      std::cout << '\n';
    }
    return true;
  }

  if (command.number == 12) {
    const std::vector<std::string> words = split_words(command.arguments);
    if (words.empty() || words.size() > 2U) {
      std::cout << "[local error] 请指定群名: 12 <group_name> [count]\n";
      return true;
    }
    std::size_t count = kDefaultLocalHistory;
    if (words.size() == 2U && !parse_count(words[1], 1U, kMaxLocalHistory, count)) {
      std::cout << "[local error] count must be 1-200.\n";
      return true;
    }
    std::vector<LocalGroupMessage> messages;
    std::string error;
    if (!cache.recent_group_messages(state.active_username, words[0], count, messages, error)) {
      std::cout << "[local sqlite error] " << error << '\n';
      return true;
    }
    std::cout << "[local group history " << words[0] << "] " << messages.size() << " message(s):\n";
    for (const auto &message : messages) {
      std::cout << "  #G" << message.server_message_id << " [" << message.sender_username << "] " << message.content;
      if (message.offline_delivery) std::cout << " [offline-delivery]";
      std::cout << '\n';
    }
    return true;
  }

  if (state.chat_scope == ClientState::ChatScope::None || state.chat_target.empty()) {
    std::cout << "[local error] 请先通过 8 <username> 或 9 <group_name> 进入会话。\n";
    return true;
  }

  if (command.number == 13) {
    if (!command.arguments.empty()) {
      std::cout << "[local error] usage: 13\n";
      return true;
    }
    std::vector<LocalFileTransfer> files;
    std::string error;
    if (!cache.recent_file_transfers(state.active_username, kMaxLocalHistory, files, error)) {
      std::cout << "[local sqlite error] " << error << '\n';
      return true;
    }
    std::size_t shown = 0;
    std::cout << "[local files with " << state.chat_target << "]:\n";
    for (const auto &file : files) {
      const bool match = state.chat_scope == ClientState::ChatScope::Private
          ? (file.scope == "PRIVATE" && file.peer_username == state.chat_target)
          : (file.scope == "GROUP" && file.group_name == state.chat_target);
      if (!match) continue;
      ++shown;
      std::cout << "  #F" << file.server_transfer_id << " " << (file.outgoing ? "sent" : "received")
                << " " << file.file_name << " (" << file.file_size << " bytes) path=" << file.local_path << '\n';
    }
    if (shown == 0) std::cout << "  (none)\n";
    return true;
  }

  if (command.arguments.empty()) {
    std::cout << "[local error] usage: 14 <file_path>\n";
    return true;
  }
  const std::string scope = state.chat_scope == ClientState::ChatScope::Private ? "PRIVATE" : "GROUP";
  (void)prepare_upload(transport, state, cache, scope, state.chat_target, command.arguments);
  return true;
}
