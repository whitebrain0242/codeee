#pragma once

#include <string>

struct NumericCommand {
  int number = 0;
  std::string arguments;
};

bool parse_numeric_command(const std::string &line, NumericCommand &command);
bool map_numeric_network_command(int number, std::string &english_name);
void print_numeric_help();
