#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace chat{
//消息类型保存
enum class MessageKind{
    Public,
    Private
};

//历史记录格式
struct ChatMessage{
    std::uint64_t id = 0;//历史消息编号
    MessageKind kind = MessageKind::Public;//类型：默认公开
    std::chrono::system_clock::time_point created_at;//时间戳
    std::string sender;//发送者名字
    std::string recipient;
    std::string content;//消息内容
};


class InMemoryMessageStore{
public:
    //构造函数
    explicit InMemoryMessageStore(
        std::size_t max_public_messages = 1000,
        std::size_t max_private_messages_per_conversation = 500
    );
    ///创造一条公共消息，塞进public message尾部，调用trim public 淘汰旧消息
    ChatMessage add_public(
        const std::string& sender,
        const std::string& content
    );
    //计算conservation——key,找到对应的deque,追加新消息，用trim private淘汰对话的旧消息
    ChatMessage add_private(
        const std::string& sender,
        const std::string& recipient,
        const std::string& content
    );
    //从pubic message尾部去最近n条消息
    std::vector<ChatMessage>recent_public(std::size_t count)const;
    //计算conservation——key,从对应的deque尾部去最近n条消息
    std::vector<ChatMessage>recent_private(
        const std::string& user_a,
        const std::string& user_b,
        std::size_t count
    )const;
    
    //静态工具函数，把 chrono::time_point 转成 "2025-07-25 14:30:00" 这样的可读字符串
    static std::string format_timestamp(
        const std::chrono::system_clock::time_point& time
    );


private:

    //静态工具函数，保证两个人的名字按照字典需排列后，中间家上ASCCll31作为分隔符，这样无论是好友双方都可以查到的是一组历史消息
    static std::string conversation_key(
        const std::string& user_a,
        const std::string& user_b
    );
    //静态工具函数：从任意deque尾部取n条，返回vector<chatmessage>
    static std::vector<ChatMessage> tail_copy(
        const std::deque<ChatMessage>& messages,
        std::size_t count
    );

    //如果public message超过上线，从头部（最旧的 循环pop front
    void trim_public();
    //如果对话的deque超过上限，从头循环pop front
    void trim_private(
        std::deque<ChatMessage>& conversation
    );


    std::uint64_t next_message_id_ = 1;//历史消息的id
    std::size_t max_public_messages_;//公共消息最大上线1000
    std::size_t max_private_messages_per_conversation_;//私聊消息最大上限500

    std::deque<ChatMessage> public_messages_;//公共消息队列，先进先出
    //私聊消息队列：键是 conversation_key（如 "Alice\x1fBob"），值是这个对话的 deque<ChatMessage>。
    std::unordered_map<std::string,std::deque<ChatMessage>> private_messages_;

};

}