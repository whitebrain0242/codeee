#include "message_store.hpp"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace chat
{
    //没有看懂这个函数的作用
InMemoryMessageStore::InMemoryMessageStore(
    std::size_t max_public_messages,
    std::size_t max_private_messages_per_conversation
)
    : max_public_messages_(max_public_messages),
      max_private_messages_per_conversation_(
          max_private_messages_per_conversation
      ) {
}

ChatMessage InMemoryMessageStore::add_public(const std::string& sender,const std::string& content){
    ChatMessage message;
    message.id=next_message_id_++;
    message.kind=MessageKind::Public;
    message.created_at=std::chrono::system_clock::now();
    message.sender=sender;
    message.content=content;

    public_messages_.push_back(message);
    trim_public();
    return message;
}

ChatMessage InMemoryMessageStore::add_private(
    const std::string& sender,
    const std::string& recipient,
    const std::string& content
){
    ChatMessage message;
    message.id=next_message_id_++;
    message.kind=MessageKind::Private;
    message.created_at=std::chrono::system_clock::now();
    message.sender=sender;
    message.recipient=recipient;
    message.content=content;

    auto& conversation=private_messages_[conversation_key(sender,recipient)];
    conversation.push_back(message);
    trim_private(conversation);
    return message;
}

//从pubic message尾部去最近n条消息
std::vector<ChatMessage> InMemoryMessageStore::recent_public(std::size_t count)const{
    return tail_copy(public_messages_,count);
}

std::vector<ChatMessage>InMemoryMessageStore::recent_private(
    const std::string& user_a,
    const std::string& user_b,
    std::size_t count
)const{
    const auto it=private_messages_.find(conversation_key(user_a, user_b));
    if(it==private_messages_.end())return {};
    return tail_copy(it->second,count);
}

//重点
 std::string InMemoryMessageStore::format_timestamp(
    const std::chrono::system_clock::time_point& time
){
    const std::time_t value=std::chrono::system_clock::to_time_t(time);
    std::tm local_time{};
    localtime_r(&value, &local_time);

    std::ostringstream output;
    output << std::put_time(
        &local_time,
        "%Y-%m-%d %H:%M:%S"
    );

    return output.str();
}


 std::string InMemoryMessageStore::conversation_key(
    const std::string& user_a,
    const std::string& user_b
){
    if (user_a <= user_b) {
        return user_a + "\x1f" + user_b;
    }

    return user_b + "\x1f" + user_a;
}

 std::vector<ChatMessage> InMemoryMessageStore::tail_copy(
    const std::deque<ChatMessage>& messages,
    std::size_t count
){
    if(messages.empty()||count==0)return {};

    const std::size_t actual_count=std::min(count,messages.size());
    const auto begin =messages.end()-static_cast<std::ptrdiff_t>(actual_count);
    return {begin,messages.end()};
}

void InMemoryMessageStore::trim_public(){
    while(public_messages_.size()>max_public_messages_){
        public_messages_.pop_front();
    }
}
void InMemoryMessageStore::trim_private(std::deque<ChatMessage>& conversation){
    while(conversation.size()>max_private_messages_per_conversation_){
        conversation.pop_front();
    }
}

} // namespace chat
