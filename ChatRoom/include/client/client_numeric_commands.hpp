#pragma once
#include <string>

struct ParsedNumericCommand {
  int number = 0;
  std::string arguments;
};

bool parse_numeric_command_line(const std::string &line,
                                ParsedNumericCommand &command);
const char *numeric_command_name(int number);
bool is_known_numeric_command(int number);
void print_numeric_help();
