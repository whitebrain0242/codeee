#include "friend_event_codec.hpp"
#include "friend_event.pb.h"

namespace chat{
namespace{
//friendevent转二进制protobuf字节流
chatroom::proto::FriendEventType to_proto_type(
    FriendEventType type
) {
    switch (type) {
        case FriendEventType::RequestSent:
        //转换成数字编号1
            return chatroom::proto::
                FRIEND_REQUEST_SENT;

        case FriendEventType::RequestAccepted:
            return chatroom::proto::
                FRIEND_REQUEST_ACCEPTED;

        case FriendEventType::RequestRejected:
            return chatroom::proto::
                FRIEND_REQUEST_REJECTED;

        case FriendEventType::FriendRemoved:
            return chatroom::proto::
                FRIEND_REMOVED;

        case FriendEventType::Unspecified:
        default:
            return chatroom::proto::
                FRIEND_EVENT_TYPE_UNSPECIFIED;
    }
}
//二进制protobuf转friendevent结构体
FriendEventType from_proto_type(
    chatroom::proto::FriendEventType type
) {
    switch (type) {
        case chatroom::proto::
            FRIEND_REQUEST_SENT:
            return FriendEventType::RequestSent;

        case chatroom::proto::
            FRIEND_REQUEST_ACCEPTED:
            return FriendEventType::RequestAccepted;

        case chatroom::proto::
            FRIEND_REQUEST_REJECTED:
            return FriendEventType::RequestRejected;

        case chatroom::proto::FRIEND_REMOVED:
            return FriendEventType::FriendRemoved;

        case chatroom::proto::
            FRIEND_EVENT_TYPE_UNSPECIFIED:
        default:
            return FriendEventType::Unspecified;
    }
}
}
//序列化
bool FriendEventCodec::serialize(
    const FriendEvent& event,
    std::string& output,
    std::string& error
) {
    //先清除
    output.clear();
    error.clear();

    //检查类型
    if (
        event.type ==
            FriendEventType::Unspecified ||
        event.actor_username.empty() ||
        event.target_username.empty()
    ) {
        error =
            "friend event contains invalid "
            "required fields";
        return false;
    }

    //创建对象
    chatroom::proto::FriendEvent message;

    //填充，类型需要转换
    message.set_type(
        to_proto_type(event.type)
    );

    message.set_actor_username(
        event.actor_username
    );

    message.set_target_username(
        event.target_username
    );

    message.set_occurred_at_unix_ms(
        event.occurred_at_unix_ms
    );

    //调用序列化函数进行序列化，将之前保存的数据拍扁称二进制字节流
    if (!message.SerializeToString(&output)) {
        error =
            "official Protobuf serialization "
            "failed";
        return false;
    }

    return true;
}

bool FriendEventCodec::deserialize(
    const std::string& input,
    FriendEvent& event,
    std::string& error
) {
    //清除，初始化
    event = {};
    error.clear();

    //创建保存二进制字节流的对象
    chatroom::proto::FriendEvent message;

    //反序列化，将二进制字节流还原
    if (!message.ParseFromString(input)) {
        error =
            "official Protobuf parsing failed";
        return false;
    }
    //填充信息

    event.type =
        from_proto_type(message.type());

    event.actor_username =
        message.actor_username();

    event.target_username =
        message.target_username();

    event.occurred_at_unix_ms =
        message.occurred_at_unix_ms();

        //再次检查类型是否合理
    if (
        event.type ==
            FriendEventType::Unspecified ||
        event.actor_username.empty() ||
        event.target_username.empty()
    ) {
        error =
            "parsed Protobuf friend event "
            "is incomplete";
        return false;
    }

    return true;
}


}