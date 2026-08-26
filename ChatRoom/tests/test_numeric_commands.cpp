#include "client/client_numeric_commands.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

int main() {
  ParsedNumericCommand command;

  if (!parse_numeric_command_line("8 bob", command) ||
      command.number != 8 || command.arguments != "bob") {
    std::cerr << "numeric private-entry parse failed\n";
    return EXIT_FAILURE;
  }

  if (!parse_numeric_command_line("14 /tmp/a file.txt", command) ||
      command.number != 14 || command.arguments != "/tmp/a file.txt") {
    std::cerr << "numeric file-path parse failed\n";
    return EXIT_FAILURE;
  }

  if (parse_numeric_command_line("LOGIN alice 1234", command) ||
      parse_numeric_command_line("999 test", command)) {
    std::cerr << "English/unknown command rejection failed\n";
    return EXIT_FAILURE;
  }

  if (std::string(numeric_command_name(8)) != "ENTER_PRIVATE" ||
      std::string(numeric_command_name(9)) != "ENTER_GROUP" ||
      std::string(numeric_command_name(39)) != "LOCAL_HELP") {
    std::cerr << "numeric mapping failed\n";
    return EXIT_FAILURE;
  }

  std::cout << "numeric command tests passed\n";
  return EXIT_SUCCESS;
}
