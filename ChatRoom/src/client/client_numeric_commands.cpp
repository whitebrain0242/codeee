#include "client/client_numeric_commands.hpp"
#include "protocol.hpp"

#include <charconv>
#include <iostream>
#include <unordered_map>

bool parse_numeric_command(const std::string &line, NumericCommand &command) {
  const std::string cleaned = trim(line);
  if (cleaned.empty()) return false;

  const std::size_t separator = cleaned.find_first_of(" \t");
  const std::string number_text = cleaned.substr(0, separator);
  int number = 0;
  const auto result = std::from_chars(number_text.data(), number_text.data() + number_text.size(), number);
  if (result.ec != std::errc() || result.ptr != number_text.data() + number_text.size()) return false;

  command.number = number;
  command.arguments = separator == std::string::npos ? "" : trim(cleaned.substr(separator + 1));
  return true;
}

bool map_numeric_network_command(int number, std::string &english_name) {
  static const std::unordered_map<int, std::string> mapping = {
      {1,"REGISTER"},{2,"LOGIN"},{3,"LOGOUT"},{4,"DELETE_ACCOUNT"},{5,"SAY"},
      {15,"FRIENDS"},{16,"FRIEND_REQUESTS"},{17,"ADD_FRIEND"},{18,"ACCEPT_FRIEND"},
      {19,"REJECT_FRIEND"},{20,"REMOVE_FRIEND"},{21,"BLOCK_FRIEND"},{22,"UNBLOCK_FRIEND"},
      {23,"BLOCKED_FRIENDS"},{24,"CREATE_GROUP"},{25,"DISSOLVE_GROUP"},{26,"APPLY_GROUP"},
      {27,"MY_GROUPS"},{28,"LEAVE_GROUP"},{29,"GROUP_MEMBERS"},{30,"ADD_GROUP_ADMIN"},
      {31,"REMOVE_GROUP_ADMIN"},{32,"GROUP_REQUESTS"},{33,"APPROVE_GROUP"},{34,"REJECT_GROUP"},
      {35,"REMOVE_GROUP_MEMBER"},{36,"WHO"},{37,"PENDING"},{38,"HELP"}
  };
  const auto it = mapping.find(number);
  if (it == mapping.end()) return false;
  english_name = it->second;
  return true;
}

void print_numeric_help() {
  std::cout << "[local help] numeric commands only:\n"
            << "  1 REGISTER <username> <password>\n  2 LOGIN <username> <password>\n  3 LOGOUT\n"
            << "  4 DELETE_ACCOUNT <password> CONFIRM\n  5 SAY <message>\n"
            << "  6 private continuous message mode (after 8)\n  7 group continuous message mode (after 9)\n"
            << "  8 <username> enter private chat\n  9 <group_name> enter group chat\n  10 leave current chat\n"
            << "  11 <username> [count] local private history\n  12 <group_name> [count] local group history\n"
            << "  13 local files for current chat\n  14 <file_path> send file to current chat\n"
            << "  15 FRIENDS  16 FRIEND_REQUESTS  17 ADD_FRIEND <username>\n"
            << "  18 ACCEPT_FRIEND <username>  19 REJECT_FRIEND <username>  20 REMOVE_FRIEND <username>\n"
            << "  21 BLOCK_FRIEND <username>  22 UNBLOCK_FRIEND <username>  23 BLOCKED_FRIENDS\n"
            << "  24 CREATE_GROUP <group>  25 DISSOLVE_GROUP <group>  26 APPLY_GROUP <group>\n"
            << "  27 MY_GROUPS  28 LEAVE_GROUP <group>  29 GROUP_MEMBERS <group>\n"
            << "  30 ADD_GROUP_ADMIN <group> <user>  31 REMOVE_GROUP_ADMIN <group> <user>\n"
            << "  32 GROUP_REQUESTS <group>  33 APPROVE_GROUP <group> <user>\n"
            << "  34 REJECT_GROUP <group> <user>  35 REMOVE_GROUP_MEMBER <group> <user>\n"
            << "  36 WHO  37 PENDING  38 HELP  39 LOCAL_HELP  40 LOCAL_DB\n";
}
