#pragma once

#include <cstdint>
#include <string>
//protobuf编解码
//好友事件序列化
/*
1.客户端数据结构体，对于数字来说翻译成英文
2.序列化和反序列化函数
*/
namespace chat{
enum class FriendEventType : std::uint32_t {
    Unspecified = 0,//默认值，未知值
    RequestSent = 1,//发送好友请求
    RequestAccepted = 2,//接受好友请求
    RequestRejected = 3,//拒绝好友请求
    FriendRemoved = 4//删除好友
};
//存储反序列化后的结果
struct FriendEvent {
    FriendEventType type =FriendEventType::Unspecified;

    std::string actor_username;
    std::string target_username;
    std::int64_t occurred_at_unix_ms = 0;//事件发生的准确时间，毫秒
};
//序列化和反序列化的函数
class FriendEventCodec {
public:
//序列化
    static bool serialize(
        const FriendEvent& event,
        std::string& output,
        std::string& error
    );
//反序列化
    static bool deserialize(
        const std::string& input,
        FriendEvent& event,
        std::string& error
    );

private:
//处理整数的变长压缩编码
    static void append_varint(
        std::uint64_t value,
        std::string& output
    );

    static bool read_varint(
        const std::string& input,
        std::size_t& offset,
        std::uint64_t& value
    );
//负责按照“字段编号 + 类型 + 长度 + 内容” 的格式，把字符串追加到二进制流中
    static void append_string_field(
        std::uint32_t field_number,
        const std::string& value,
        std::string& output
    );
//跳过不认识的字段
    static bool skip_field(
        std::uint32_t wire_type,
        const std::string& input,
        std::size_t& offset
    );
};

}