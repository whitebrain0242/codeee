#pragma once

#include "client/client_state.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
//网络IO缓冲区大小
inline constexpr std::size_t
    kClientBufferSize = 4096U;
//客户端本地默认消息历史数
inline constexpr std::size_t
    kDefaultLocalHistory = 20U;
//本地历史消息的最大上限
inline constexpr std::size_t
    kMaxLocalHistory = 200U;
//文件传输时单次发送/接受的数据快大小
inline constexpr std::size_t
    kFileChunkBytes = 3072U;
//客户端可接受/发送的最大文件大小设置
inline constexpr std::uint64_t
    kMaxFileSize =
        20ULL * 1024ULL * 1024ULL;//20MB
//获取当前系统的时间戳
std::int64_t client_now_unix_ms();
//判断字符串text是否以prefix 开头,用于快速路由
bool starts_with(
    const std::string& text,//需要被检查的完整字符串
    const std::string& prefix//要匹配的前缀
);
//将text解析成无符号64位整数.存入value
bool parse_uint64(
    const std::string& text,//带解析的字符串
    std::uint64_t& value//通过引用传递
);
//对输入字符串进行Base64编码,返回编码后的字符串
std::string encode_text_base64(
    const std::string& text
);
//将编码后的字符串解码为原始文本,存入text
bool decode_text_base64(
    const std::string& encoded,//已经编码的字符串
    std::string& text,//解码后还原出的原始字符串
    std::string& error
);
//检查当前客户端是否已经有激活的登陆账户
bool require_local_account(
    const ClientState& state
);
