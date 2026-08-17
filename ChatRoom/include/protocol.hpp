#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct Command {
    std::string name;
    std::string raw_arguments;
};

std::string trim(const std::string& text);
std::string to_upper_ascii(std::string text);
Command parse_command(const std::string& line);
std::vector<std::string> split_words(const std::string& text);
bool split_first_token(const std::string& text, std::string& first, std::string& rest);
bool parse_port(const std::string& text, int& port);
bool parse_count(const std::string& text, std::size_t minimum, std::size_t maximum, std::size_t& value);
