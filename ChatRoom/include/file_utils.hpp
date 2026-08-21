#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
//文件安全传输/交换
namespace fileutil {
std::string percent_encode(
    const std::string& text
);

bool percent_decode(
    const std::string& encoded,
    std::string& text,
    std::string& error
);

//计算文件的 SHA256 哈希值，返回十六进制字符串
//大文件上传/下载完成后，服务端算一遍哈希，和客户端传过来的对比。如果不一致，说明网络传输丢包或磁盘损坏，直接丢弃重传。防止文件损坏被业务层误用
bool sha256_file_hex(
    const std::filesystem::path& path,
    std::string& hex_digest,
    std::string& error
);
//生成一个用于“传输令牌”的随机字符串（可能用于临时授权）
std::string make_transfer_token();
//清理文件名中的危险字符（如 ../、/ 等），防止路径遍历攻击
//恶意客户端上传文件名填 ../../etc/passwd。这个函数会把路径分隔符、特殊字符过滤掉，只保留安全的文件名。防止路径遍历攻击（Directory Traversal）
std::string sanitize_filename(
    const std::string& filename
);
//文件上传不是谁都能传的。服务端先给客户端一个Token（比如 60 秒过期），客户端拿着 Token 才能 POST 文件。防止未授权上传和重放攻击（Replay Attack）
//校验Token是否合法（格式/长度/有效期等）
bool is_valid_transfer_token(
    const std::string& token
);
//检查字符串是否为 64 位十六进制 SHA256 摘要
bool is_valid_sha256_hex(
    const std::string& value
);

}  // namespace fileutil
