#include "client/client_message_cache.hpp"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int main() {
  LocalPrivateMessage private_message;
  if (!parse_private_message_line(
          "[#12] [private to bob] hello%2C%0Ahow%20are%20you%3F",
          "alice", private_message) ||
      private_message.content != "hello,\nhow are you?" ||
      private_message.sender_username != "alice" ||
      private_message.recipient_username != "bob") {
    std::cerr << "private multiline wire decode failed\n";
    return EXIT_FAILURE;
  }

  LocalGroupMessage group_message;
  if (!parse_group_message_line(
          "[#G7] [group cpp] [bob] first%0Asecond",
          "alice", group_message) ||
      group_message.content != "first\nsecond" ||
      group_message.group_name != "cpp" ||
      group_message.sender_username != "bob") {
    std::cerr << "group multiline wire decode failed\n";
    return EXIT_FAILURE;
  }

  SqliteClient cache;
  std::string error;
  if (!cache.open(":memory:", error)) {
    std::cerr << "SQLite open failed: " << error << '\n';
    return EXIT_FAILURE;
  }

  ClientState state;
  state.active_username = "alice";
  cache_server_message(
      "[#12] [private to bob] hello%2C%0Ahow%20are%20you%3F", state, cache);

  std::vector<LocalPrivateMessage> history;
  if (!cache.recent_private_messages("alice", "bob", 20, history, error) ||
      history.size() != 1U ||
      history[0].content != "hello,\nhow are you?") {
    std::cerr << "multiline private history cache failed: " << error << '\n';
    return EXIT_FAILURE;
  }

  std::cout << "message cache multiline tests passed\n";
  return EXIT_SUCCESS;
}
