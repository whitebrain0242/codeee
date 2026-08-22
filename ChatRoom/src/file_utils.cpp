#include "file_utils.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>
// 加密、编解码、文件安全操作和随机令牌生成
namespace fileutil {

std::string percent_encode(const std::string &text) {
  static constexpr char hex[] = "0123456789ABCDEF";

  std::string encoded;
  encoded.reserve(text.size() * 3U);

  for (unsigned char ch : text) {
    const bool unreserved = std::isalnum(ch) != 0 || ch == '-' || ch == '_' ||
                            ch == '.' || ch == '~';

    if (unreserved) {
      encoded.push_back(static_cast<char>(ch));
      continue;
    }

    encoded.push_back('%');
    encoded.push_back(hex[(ch >> 4U) & 0x0FU]);
    encoded.push_back(hex[ch & 0x0FU]);
  }

  return encoded;
}

bool percent_decode(const std::string &encoded, std::string &text,
                    std::string &error) {
  auto hex_value = [](unsigned char ch) -> int {
    if (ch >= '0' && ch <= '9') {
      return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
      return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
      return 10 + ch - 'A';
    }
    return -1;
  };

  std::string decoded;
  decoded.reserve(encoded.size());

  for (std::size_t i = 0U; i < encoded.size();) {
    if (encoded[i] != '%') {
      decoded.push_back(encoded[i]);
      ++i;
      continue;
    }

    if (i + 2U >= encoded.size()) {
      error = "truncated percent escape";
      return false;
    }

    const int high = hex_value(static_cast<unsigned char>(encoded[i + 1U]));

    const int low = hex_value(static_cast<unsigned char>(encoded[i + 2U]));

    if (high < 0 || low < 0) {
      error = "invalid percent escape";
      return false;
    }

    decoded.push_back(static_cast<char>((high << 4) | low));

    i += 3U;
  }

  text = std::move(decoded);
  return true;
}

bool sha256_file_hex(const std::filesystem::path &path, std::string &hex_digest,
                     std::string &error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "cannot open file for SHA-256: " + path.string();
    return false;
  }

  using ContextPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  ContextPtr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context) {
    error = "EVP_MD_CTX_new failed";
    return false;
  }

  if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
    error = "EVP_DigestInit_ex failed";
    return false;
  }

  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0 && EVP_DigestUpdate(context.get(), buffer.data(),
                                      static_cast<std::size_t>(count)) != 1) {
      error = "EVP_DigestUpdate failed";
      return false;
    }
  }
  if (!input.eof()) {
    error = "failed while reading file for SHA-256";
    return false;
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1) {
    error = "EVP_DigestFinal_ex failed";
    return false;
  }

  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned int i = 0; i < digest_size; ++i)
    output << std::setw(2) << static_cast<unsigned int>(digest[i]);

  hex_digest = output.str();
  return true;
}

std::string make_transfer_token() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    return {};

  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned char byte : bytes)
    output << std::setw(2) << static_cast<unsigned int>(byte);
  return output.str();
}

std::string sanitize_filename(const std::string &filename) {
  const std::filesystem::path path(filename);
  std::string name = path.filename().string(); // 只取文件名，去掉路径

  // 处理危险名称
  if (name.empty() || name == "." || name == "..")
    return "file.bin";

  // 仅允许字母、数字、点、下划线、短横线，其余替换为下划线
  for (char &c : name) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) == 0 && c != '.' && c != '_' && c != '-')
      c = '_';
  }

  // 截断过长的文件名（防止文件名过长导致存储问题）
  if (name.size() > 180U)
    name.resize(180U);

  return name.empty() ? "file.bin" : name;
}

bool is_valid_transfer_token(const std::string &token) {
  if (token.size() != 32U)
    return false;
  return std::all_of(token.begin(), token.end(),
                     [](unsigned char c) { return std::isxdigit(c) != 0; });
}

bool is_valid_sha256_hex(const std::string &value) {
  if (value.size() != 64U)
    return false;
  return std::all_of(value.begin(), value.end(),
                     [](unsigned char c) { return std::isxdigit(c) != 0; });
}

} // namespace fileutil
