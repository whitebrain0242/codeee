#include "client/client_common.hpp"

#include "file_utils.hpp"

#include <charconv>
#include <chrono>
#include <iostream>
#include <vector>
//客户端工具
//获取当前系统时间的 Unix 毫秒时间戳
std::int64_t client_now_unix_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
//判断字符串 text 是否以 prefix 开头
bool starts_with(const std::string &text, const std::string &prefix) {
  return text.size() >= prefix.size() &&
         text.compare(0, prefix.size(), prefix) == 0;
}
//将字符串解析为 64 位无符号整数（uint64_t）
bool parse_uint64(const std::string &text, std::uint64_t &value) {
  if (text.empty()) {
    return false;
  }

  std::uint64_t parsed = 0;

  const char *begin = text.data();

  const char *end = begin + text.size();

  const auto result = std::from_chars(begin, end, parsed);

  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }

  value = parsed;
  return true;
}
//对文本进行 URL 百分号编码（Percent-encoding），用于安全传输
std::string encode_text_token(const std::string &text) {
  return fileutil::percent_encode(text);
}
//解码百分号编码的字符串，还原原始文本
bool decode_text_token(const std::string &encoded, std::string &text,
                       std::string &error) {
  if (encoded == "-") {
    text.clear();
    return true;
  }

  return fileutil::percent_decode(encoded, text, error);
}
//检查当前客户端状态是否存在有效的本地账户（active_username 非空），若不存在则输出错误提示并返回 false。
bool require_local_account(const ClientState &state) {
  if (!state.active_username.empty()) {
    return true;
  }

  std::cout << "[本地错误] 请先登录，才能"
               "SQLite account context is known.\n";

  return false;
}
