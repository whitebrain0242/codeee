#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
//文件安全传输/交换
namespace fileutil {
//你通过 JSON 或 HTTP Header 返回文件哈希值或 Token 时，二进制数据没法直接放进去，Base64 把它们转成纯文本。解决二进制数据在文本协议中的传输问题
//将二进制数据编码为 Base64 字符串（常用于文本传输）
std::string base64_encode(
    const std::vector<unsigned char>& bytes
);
//解码 Base64 字符串转二进制
bool base64_decode(
    const std::string& encoded,
    std::vector<unsigned char>& bytes,
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
