#include "protocol.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>
//一些工具函数
// 字符串解析
//去除字符串首尾的空白字符（空格、制表符、换行等）
std::string trim(const std::string &text) {
  //从两端分别用 isspace 检查并移动索引
  std::size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }

  std::size_t end = text.size();
  while (end > begin &&std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  //最后用 substr 截取中间部分
  return text.substr(begin, end - begin);
}
//将字符串中的字母全部转为大写
std::string to_upper_ascii(std::string text) {
  //用 std::transform 配合 toupper 遍历每个字符并原地修改
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return text;
}
//解析一行命令文本，分离出命令名和参数部分
Command parse_command(const std::string &line) {
  //先 trim 去掉首尾空白，然后查找第一个空白符作为分隔；若没有分隔符，则参数为空字符串，命令名转为大写。
  const std::string cleaned = trim(line);
  const std::size_t separator = cleaned.find_first_of(" \t");

  if (separator == std::string::npos) {
    return Command{to_upper_ascii(cleaned), ""};
  }

  return Command{to_upper_ascii(cleaned.substr(0, separator)),
                 trim(cleaned.substr(separator + 1))};
}
//按空白符将字符串拆分成多个单词
std::vector<std::string> split_words(const std::string &text) {
  std::istringstream input(text);
  std::vector<std::string> words;
  std::string word;

  while (input >> word) {
    words.push_back(word);
  }

  return words;
}
//提取第一个单词（token）和剩余部分，分别存入 first 和 rest
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
//将字符串解析为有效的端口号
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
//将字符串解析为数值，并校验其是否在给定的 [minimum, maximum] 范围内
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
