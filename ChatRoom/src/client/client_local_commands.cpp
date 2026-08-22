#include "client/client_local_commands.hpp"

#include "client/client_common.hpp"
#include "client/client_file_transfer.hpp"
#include "client/tls_client_transport.hpp"
#include "integration/sqlite_client.hpp"
#include "protocol.hpp"

#include <iostream>
#include <string>
#include <vector>

void print_local_help() {
  std::cout << "[local] SQLite/file commands:\n"
            << "  LOCAL_HELP\n"
            << "  LOCAL_DB\n"
            << "  LOCAL_STATS\n"
            << "  LOCAL_HISTORY_PRIVATE <username> [count]\n"
            << "  LOCAL_HISTORY_GROUP <group_name> [count]\n"
            << "  LOCAL_FILES [count]\n"
            << "  SEND_FILE <username> <path>\n"
            << "  SEND_GROUP_FILE <group_name> <path>\n"
            << "  RESUME_UPLOADS\n"
            << "[local] LOCAL_* and SEND_* are handled by "
               "chat_client; internal FILE_* lines are hidden.\n"
            << "[local] TCP/TLS heartbeat is client-initiated: client PING, "
               "server PONG.\n";
}

bool handle_local_command(TlsClientTransport &transport,
                          const std::string &line, ClientState &state,
                          SqliteClient &cache) {
  const Command command = parse_command(line);

  if (command.name == "SEND_FILE" || command.name == "SEND_GROUP_FILE") {
    std::string target;
    std::string path;

    if (!split_first_token(command.raw_arguments, target, path) ||
        target.empty() || path.empty()) {
      std::cout << "[local error] usage: "
                << (command.name == "SEND_FILE"
                        ? "SEND_FILE <username> <path>"
                        : "SEND_GROUP_FILE <group_name> <path>")
                << '\n';
      return true;
    }

    (void)prepare_upload(transport, state, cache,
                         command.name == "SEND_FILE" ? "PRIVATE" : "GROUP",
                         target, path);
    return true;
  }

  if (command.name == "RESUME_UPLOADS") {
    if (!require_local_account(state)) {
      return true;
    }

    (void)load_and_resume_pending_uploads(transport, state, cache);
    return true;
  }

  if (!starts_with(command.name, "LOCAL_")) {
    return false;
  }

  if (command.name == "LOCAL_HELP") {
    print_local_help();
    return true;
  }

  if (command.name == "LOCAL_DB") {
    std::cout << "[local] SQLite database: " << cache.database_path() << '\n'
              << "[local] download root: " << state.download_root.string()
              << '\n';
    return true;
  }

  if (!require_local_account(state)) {
    return true;
  }

  if (command.name == "LOCAL_STATS") {
    LocalCacheStats stats;
    std::string error;

    if (!cache.stats(state.active_username, stats, error)) {
      std::cout << "[local sqlite error] " << error << '\n';
      return true;
    }

    std::cout << "[local] cached private messages: " << stats.private_messages
              << ", cached group messages: " << stats.group_messages
              << ", cached files: " << stats.files << '\n';
    return true;
  }

  if (command.name == "LOCAL_HISTORY_PRIVATE") {
    const std::vector<std::string> words = split_words(command.raw_arguments);

    if (words.empty() || words.size() > 2U) {
      std::cout << "[local error] usage: "
                   "LOCAL_HISTORY_PRIVATE "
                   "<username> [count]\n";
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
      std::cout << "  #" << message.server_message_id << " "
                << message.sender_username << " -> "
                << message.recipient_username << ": " << message.content;

      if (message.offline_delivery) {
        std::cout << " [offline-delivery]";
      }

      std::cout << '\n';
    }

    return true;
  }

  if (command.name == "LOCAL_HISTORY_GROUP") {
    const std::vector<std::string> words = split_words(command.raw_arguments);

    if (words.empty() || words.size() > 2U) {
      std::cout << "[local error] usage: "
                   "LOCAL_HISTORY_GROUP "
                   "<group_name> [count]\n";
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

    std::cout << "[local group history " << words[0] << "] " << messages.size()
              << " message(s):\n";

    for (const LocalGroupMessage &message : messages) {
      std::cout << "  #G" << message.server_message_id << " ["
                << message.sender_username << "] " << message.content;

      if (message.offline_delivery) {
        std::cout << " [offline-delivery]";
      }

      std::cout << '\n';
    }

    return true;
  }

  if (command.name == "LOCAL_FILES") {
    const std::vector<std::string> words = split_words(command.raw_arguments);

    std::size_t count = kDefaultLocalHistory;

    if (words.size() > 1U ||
        (words.size() == 1U &&
         !parse_count(words[0], 1U, kMaxLocalHistory, count))) {
      std::cout << "[local error] usage: "
                   "LOCAL_FILES [count], count 1-200.\n";
      return true;
    }

    std::vector<LocalFileTransfer> files;
    std::string error;

    if (!cache.recent_file_transfers(state.active_username, count, files,
                                     error)) {
      std::cout << "[local sqlite error] " << error << '\n';
      return true;
    }

    std::cout << "[local files] " << files.size() << " record(s):\n";

    for (const LocalFileTransfer &file : files) {
      std::cout << "  #F" << file.server_transfer_id << " [" << file.scope
                << "] " << (file.outgoing ? "sent" : "received") << " "
                << file.file_name << " (" << file.file_size
                << " bytes) path=" << file.local_path;

      if (!file.group_name.empty()) {
        std::cout << " group=" << file.group_name;
      } else if (!file.peer_username.empty()) {
        std::cout << " peer=" << file.peer_username;
      }

      std::cout << '\n';
    }

    return true;
  }

  std::cout << "[local error] unknown LOCAL_* command. "
               "Use LOCAL_HELP.\n";
  return true;
}
