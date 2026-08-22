#include "client/client_common.hpp"

#include "file_utils.hpp"

#include <charconv>
#include <chrono>
#include <iostream>
#include <vector>

std::int64_t client_now_unix_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool starts_with(const std::string &text, const std::string &prefix) {
  return text.size() >= prefix.size() &&
         text.compare(0, prefix.size(), prefix) == 0;
}

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

std::string encode_text_token(const std::string &text) {
  return fileutil::percent_encode(text);
}

bool decode_text_token(const std::string &encoded, std::string &text,
                       std::string &error) {
  if (encoded == "-") {
    text.clear();
    return true;
  }

  return fileutil::percent_decode(encoded, text, error);
}

bool require_local_account(const ClientState &state) {
  if (!state.active_username.empty()) {
    return true;
  }

  std::cout << "[local error] login first so the "
               "SQLite account context is known.\n";

  return false;
}
