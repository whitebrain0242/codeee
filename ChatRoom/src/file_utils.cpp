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
//加密、编解码、文件安全操作和随机令牌生成
namespace fileutil {

std::string base64_encode(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) return {};
    // 输出长度=4 * ceil(输入长度 / 3)
    const int output_size = 4 * static_cast<int>((bytes.size() + 2U) / 3U);
    std::string encoded(static_cast<std::size_t>(output_size), '\0');
    //使用 OpenSSL 的 EVP_EncodeBlock（底层 C 函数），它不会自动补 =，但会返回实际写入的字符数（不含 \0）。
    const int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        bytes.data(),
        static_cast<int>(bytes.size())
    );

    if (written < 0) return {};
    encoded.resize(static_cast<std::size_t>(written));
    return encoded;
}

bool base64_decode(const std::string& encoded, std::vector<unsigned char>& bytes, std::string& error) {
    if (encoded.empty()) { bytes.clear(); return true; }
    if (encoded.size() % 4U != 0U) { error = "invalid base64 length"; return false; }
    //先检查长度是否为 4 的倍数（Base64 规范）
    std::vector<unsigned char> decoded(encoded.size() / 4U * 3U);
    //EVP_DecodeBlock 会解码包括 = 在内的所有字符，但返回的是填充前的实际字节数（即包括 = 的占位）。所以需要手动根据末尾 = 数量调整最终长度（一个 = 减 1 字节，两个 = 减 2 字节）
    const int written = EVP_DecodeBlock(
        decoded.data(),
        reinterpret_cast<const unsigned char*>(encoded.data()),
        static_cast<int>(encoded.size())
    );

    if (written < 0) { error = "invalid base64 data"; return false; }

    // 处理末尾的 '=' 填充符（Base64 标准填充）
    std::size_t actual = static_cast<std::size_t>(written);
    if (!encoded.empty() && encoded.back() == '=') --actual;
    if (encoded.size() >= 2U && encoded[encoded.size() - 2U] == '=') --actual;

    decoded.resize(actual);
    bytes = std::move(decoded);
    return true;
}

bool sha256_file_hex(const std::filesystem::path& path, std::string& hex_digest, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = "cannot open file for SHA-256: " + path.string(); return false; }

    using ContextPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    ContextPtr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context) { error = "EVP_MD_CTX_new failed"; return false; }

    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        error = "EVP_DigestInit_ex failed"; return false;
    }

    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0 && EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(count)) != 1) {
            error = "EVP_DigestUpdate failed"; return false;
        }
    }
    if (!input.eof()) { error = "failed while reading file for SHA-256"; return false; }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1) {
        error = "EVP_DigestFinal_ex failed"; return false;
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
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) return {};

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char byte : bytes)
        output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
}

std::string sanitize_filename(const std::string& filename) {
    const std::filesystem::path path(filename);
    std::string name = path.filename().string();  // 只取文件名，去掉路径

    // 处理危险名称
    if (name.empty() || name == "." || name == "..") return "file.bin";

    // 仅允许字母、数字、点、下划线、短横线，其余替换为下划线
    for (char& c : name) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) == 0 && c != '.' && c != '_' && c != '-')
            c = '_';
    }

    // 截断过长的文件名（防止文件名过长导致存储问题）
    if (name.size() > 180U) name.resize(180U);

    return name.empty() ? "file.bin" : name;
}

bool is_valid_transfer_token(const std::string& token) {
    if (token.size() != 32U) return false;
    return std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

bool is_valid_sha256_hex(const std::string& value) {
    if (value.size() != 64U) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

}  // namespace fileutil
