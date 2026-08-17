#include "password.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>
//使用 PBKDF2-HMAC-SHA256 算法
namespace {
constexpr int kIterations = 210000;
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kHashBytes = 32;
//字节数组转十六进制字符串，用于在数据库存储
std::string to_hex(const unsigned char* data, std::size_t size) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');

    for (std::size_t i = 0; i < size; ++i) {
        output << std::setw(2) << static_cast<unsigned int>(data[i]);
    }

    return output.str();
}
//十六进制转字节数组
bool from_hex(const std::string& text, std::vector<unsigned char>& output) {
    if (text.size() % 2 != 0) {
        return false;
    }

    output.clear();
    output.reserve(text.size() / 2);

    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned int value = 0;
        std::istringstream input(text.substr(i, 2));
        input >> std::hex >> value;
        if (!input || value > 255) {
            return false;
        }
        output.push_back(static_cast<unsigned char>(value));
    }

    return true;
}
//执行 PBKDF2 密钥派生
//调用 OpenSSL 的 PKCS5_PBKDF2_HMAC，使用 SHA-256 作为伪随机函数，对密码进行反复哈希（迭代）生成固定长度派生密钥
bool derive(
    const std::string& password,
    const unsigned char* salt,
    std::size_t salt_size,
    int iterations,
    unsigned char* output,
    std::size_t output_size
) {
    return PKCS5_PBKDF2_HMAC(
        password.data(),
        static_cast<int>(password.size()),
        salt,
        static_cast<int>(salt_size),
        iterations,
        EVP_sha256(),
        static_cast<int>(output_size),
        output
    ) == 1;
}
}
//对明文密码进行哈希处理，返回一个自包含的字符串，包含了算法、迭代次数、盐和哈希值
std::string hash_password_pbkdf2(const std::string& password) {
    std::array<unsigned char, kSaltBytes> salt{};
    std::array<unsigned char, kHashBytes> hash{};

    // 生成随机盐
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1)
        throw std::runtime_error("RAND_bytes failed");

    // 执行 PBKDF2
    if (!derive(password, salt.data(), salt.size(), kIterations, hash.data(), hash.size()))
        throw std::runtime_error("PBKDF2 failed");

    // 格式化输出：算法$迭代次数$盐_hex$哈希_hex
    return "pbkdf2_sha256$" + std::to_string(kIterations) + "$" +
           to_hex(salt.data(), salt.size()) + "$" +
           to_hex(hash.data(), hash.size());
}
//验证用户输入的密码是否与存储的哈希匹配1
bool verify_password_pbkdf2(const std::string& password, const std::string& encoded) {
    // 1. 解析存储字符串，提取字段
    const std::size_t first = encoded.find('$');
    const std::size_t second = encoded.find('$', first + 1);
    const std::size_t third = encoded.find('$', second + 1);

    if (first == std::string::npos || second == std::string::npos || third == std::string::npos ||
        encoded.substr(0, first) != "pbkdf2_sha256") {
        return false;
    }

    // 2. 解析迭代次数
    int iterations = 0;
    try {
        iterations = std::stoi(encoded.substr(first + 1, second - first - 1));
    } catch (...) {
        return false;
    }
    if (iterations < 10000 || iterations > 2000000) return false;  // 防御性范围检查

    // 3. 从十六进制还原盐和期望哈希
    std::vector<unsigned char> salt;
    std::vector<unsigned char> expected;
    if (!from_hex(encoded.substr(second + 1, third - second - 1), salt) ||
        !from_hex(encoded.substr(third + 1), expected) ||
        salt.empty() || expected.empty()) {
        return false;
    }

    // 4. 使用相同的盐和迭代次数，对输入的明文密码重新计算哈希
    std::vector<unsigned char> actual(expected.size());
    if (!derive(password, salt.data(), salt.size(), iterations, actual.data(), actual.size())) {
        return false;
    }

    // 5. 使用恒定时间比较防止时序攻击
    return CRYPTO_memcmp(actual.data(), expected.data(), expected.size()) == 0;
}
