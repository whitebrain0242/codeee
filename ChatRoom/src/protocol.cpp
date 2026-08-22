#include "protocol.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>
//一些工具函数
// 字符串解析
std::string trim(const std::string &text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }

  std::size_t end = text.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }

  return text.substr(begin, end - begin);
}

std::string to_upper_ascii(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return text;
}

Command parse_command(const std::string &line) {
  const std::string cleaned = trim(line);
  const std::size_t separator = cleaned.find_first_of(" \t");

  if (separator == std::string::npos) {
    return Command{to_upper_ascii(cleaned), ""};
  }

  return Command{to_upper_ascii(cleaned.substr(0, separator)),
                 trim(cleaned.substr(separator + 1))};
}

std::vector<std::string> split_words(const std::string &text) {
  std::istringstream input(text);
  std::vector<std::string> words;
  std::string word;

  while (input >> word) {
    words.push_back(word);
  }

  return words;
}

bool split_first_token(const std::string &text, std::string &first,
                       std::string &rest) {
  const std::string cleaned = trim(text);
  const std::size_t separator = cleaned.find_first_of(" \t");

  if (cleaned.empty() || separator == std::string::npos) {
    return false;
  }

  first = cleaned.substr(0, separator);
  rest = trim(cleaned.substr(separator + 1));
  return !first.empty() && !rest.empty();
}

bool parse_port(const std::string &text, int &port) {
  int parsed = 0;
  const char *begin = text.data();
  const char *end = begin + text.size();
  const auto result = std::from_chars(begin, end, parsed);

  if (result.ec != std::errc() || result.ptr != end || parsed < 1 ||
      parsed > 65535) {
    return false;
  }

  port = parsed;
  return true;
}

bool parse_count(const std::string &text, std::size_t minimum,
                 std::size_t maximum, std::size_t &value) {
  if (text.empty()) {
    return false;
  }

  std::size_t parsed = 0;
  const char *begin = text.data();
  const char *end = begin + text.size();
  const auto result = std::from_chars(begin, end, parsed);

  if (result.ec != std::errc() || result.ptr != end || parsed < minimum ||
      parsed > maximum) {
    return false;
  }

  value = parsed;
  return true;
}
