#pragma once

#include "client/client_state.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
//网络IO缓冲区大小
inline constexpr std::size_t
    kClientBufferSize = 64U * 1024U;
//客户端本地默认消息历史数
inline constexpr std::size_t
    kDefaultLocalHistory = 20U;
//本地历史消息的最大上限
inline constexpr std::size_t
    kMaxLocalHistory = 200U;
//客户端可接受/发送的最大文件大小设置
inline constexpr std::uint64_t
    kMaxFileSize =
        100ULL * 1024ULL * 1024ULL * 1024ULL;
// 文件上传帧：16 MiB。配合服务端“整帧聚合后一次落盘”，显著减少系统调用。
inline constexpr std::uint64_t
    kFileFrameBytes =
        16ULL * 1024ULL * 1024ULL;
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
//将文本进行 Percent-Encoding,返回编码后的字符串
std::string encode_text_token(
    const std::string& text
);
//：将编码后的令牌 解码回原始文本,存入text
bool decode_text_token(
    const std::string& encoded,
    std::string& text,
    std::string& error
);
//检查当前客户端是否已经有激活的登陆账户
bool require_local_account(
    const ClientState& state
);
