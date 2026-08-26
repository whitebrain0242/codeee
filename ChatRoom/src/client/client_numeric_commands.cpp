#include "client/client_numeric_commands.hpp"
#include "protocol.hpp"
#include <charconv>
#include <iostream>

bool parse_numeric_command_line(const std::string &line,
                                ParsedNumericCommand &command) {
  const std::string cleaned = trim(line);
  if (cleaned.empty()) return false;

  const std::size_t separator = cleaned.find_first_of(" \t");
  const std::string number_text =
      separator == std::string::npos ? cleaned : cleaned.substr(0, separator);

  int number = 0;
  const char *begin = number_text.data();
  const char *end = begin + number_text.size();
  const auto result = std::from_chars(begin, end, number);

  if (result.ec != std::errc() || result.ptr != end ||
      !is_known_numeric_command(number)) {
    return false;
  }

  command.number = number;
  command.arguments =
      separator == std::string::npos ? std::string()
                                     : trim(cleaned.substr(separator + 1U));
  return true;
}

const char *numeric_command_name(int number) {
  switch (number) {
  case 1: return "REGISTER";
  case 2: return "LOGIN";
  case 3: return "LOGOUT";
  case 4: return "DELETE_ACCOUNT";
  case 5: return "SAY";
  case 6: return "MSG";
  case 7: return "GROUP_MSG";
  case 8: return "ENTER_PRIVATE";
  case 9: return "ENTER_GROUP";
  case 10: return "LEAVE_CHAT";
  case 11: return "HISTORY_PRIVATE";
  case 12: return "HISTORY_GROUP";
  case 13: return "FILES";
  case 14: return "SEND_FILE";
  case 15: return "FRIENDS";
  case 16: return "FRIEND_REQUESTS";
  case 17: return "ADD_FRIEND";
  case 18: return "ACCEPT_FRIEND";
  case 19: return "REJECT_FRIEND";
  case 20: return "REMOVE_FRIEND";
  case 21: return "BLOCK_FRIEND";
  case 22: return "UNBLOCK_FRIEND";
  case 23: return "BLOCKED_FRIENDS";
  case 24: return "CREATE_GROUP";
  case 25: return "DISSOLVE_GROUP";
  case 26: return "APPLY_GROUP";
  case 27: return "MY_GROUPS";
  case 28: return "LEAVE_GROUP";
  case 29: return "GROUP_MEMBERS";
  case 30: return "ADD_GROUP_ADMIN";
  case 31: return "REMOVE_GROUP_ADMIN";
  case 32: return "GROUP_REQUESTS";
  case 33: return "APPROVE_GROUP";
  case 34: return "REJECT_GROUP";
  case 35: return "REMOVE_GROUP_MEMBER";
  case 36: return "WHO";
  case 37: return "PENDING";
  case 38: return "HELP";
  case 39: return "LOCAL_HELP";
  case 40: return "LOCAL_DB";
  case 41: return "HISTORY_PUBLIC";
  default: return nullptr;
  }
}

bool is_known_numeric_command(int number) {
  return numeric_command_name(number) != nullptr;
}

void print_numeric_help() {
  std::cout
      << "[local] Numeric commands only:\n"
      << "  1  <username> <password>             REGISTER\n"
      << "  2  <username> <password>             LOGIN\n"
      << "  3                                    LOGOUT\n"
      << "  4  <password> CONFIRM                DELETE_ACCOUNT\n"
      << "  5  <message>                         public message\n"
      << "  6                                    continuous private editor\n"
      << "  7                                    continuous group editor\n"
      << "  8  <username>                        enter validated private chat\n"
      << "  9  <group_name>                      enter validated group chat\n"
      << " 10                                    leave current chat\n"
      << " 11  <username> [count]                local private history (1-200)\n"
      << " 12  <group_name> [count]              local group history (1-200)\n"
      << " 13                                    files for current chat\n"
      << " 14  <file_path>                       send file to current chat\n"
      << " 15                                    friends\n"
      << " 16                                    friend requests\n"
      << " 17  <username>                        add friend\n"
      << " 18  <username>                        accept friend\n"
      << " 19  <username>                        reject friend\n"
      << " 20  <username>                        remove friend\n"
      << " 21  <username>                        block friend\n"
      << " 22  <username>                        unblock friend\n"
      << " 23                                    blocked friends\n"
      << " 24  <group_name>                      create group\n"
      << " 25  <group_name>                      dissolve group\n"
      << " 26  <group_name>                      apply to group\n"
      << " 27                                    my groups\n"
      << " 28  <group_name>                      leave group\n"
      << " 29  <group_name>                      group members\n"
      << " 30  <group_name> <username>           add group admin\n"
      << " 31  <group_name> <username>           remove group admin\n"
      << " 32  <group_name>                      group requests\n"
      << " 33  <group_name> <username>           approve group request\n"
      << " 34  <group_name> <username>           reject group request\n"
      << " 35  <group_name> <username>           remove group member\n"
      << " 36                                    online users\n"
      << " 37                                    pending offline messages/files\n"
      << " 38                                    server help\n"
      << " 39                                    this numeric help\n"
      << " 40                                    local SQLite/download paths\n"
      << " 41  [count]                           public history (optional)\n"
      << "[local] In 6/7 editor: /send sends one message and stays in editor; "
         "/quit cancels the buffer and leaves the chat.\n";
}
