#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace chat{
    //将用户在数据库中的好友列表和待处理请求列表加载到内存中，方便业务代码快速读取
    //或者直接通过网络发送给客户端
struct FriendState {
    //好友关系列表          两个用户名
    std::vector<std::pair<std::string, std::string>> friendships;
    //待处理请求列表         发送者         接受者
    std::vector<std::pair<std::string, std::string>> pending_requests;
};
//存储friendevent中的一行记录，id+payload,映射到内存中
//暂时不解析内部的二进制数据，方便在不同层级之间传递
struct StoredFriendEvent {
    std::uint64_t database_id = 0;
    std::string protobuf_payload;
};
//好友操作的统一状态码：判断事件状态
enum class FriendMutationResult {
    Success,
    AlreadyExists,
    NotFound,
    Error
};
//好友仓库类：只依赖接口，对于具体使用什么数据库都无所谓，具体的实现不管
//只负责存储字节流，不在意protobuf是怎么编辑码
class IFriendRepository {
public:
    virtual ~IFriendRepository() = default;
//初始化仓库，检查表是否存在
    virtual bool initialize(
        std::string& error
    ) = 0;
//加载某个用户的好友列表和待处理请求
    virtual bool load_state(
        FriendState& state,
        std::string& error
    ) = 0;
//发送好友请求
    virtual FriendMutationResult create_request(
        const std::string& sender_username,
        const std::string& receiver_username,
        const std::string& protobuf_event,
        std::string& error
    ) = 0;
//同意好友请求
    virtual FriendMutationResult accept_request(
        const std::string& requester_username,
        const std::string& accepter_username,
        const std::string& protobuf_event,
        std::string& error
    ) = 0;
//拒绝好友请求
    virtual FriendMutationResult reject_request(
        const std::string& requester_username,
        const std::string& rejecter_username,
        const std::string& protobuf_event,
        std::string& error
    ) = 0;
//删除好友
    virtual FriendMutationResult remove_friend(
        const std::string& actor_username,
        const std::string& target_username,
        const std::string& protobuf_event,
        std::string& error
    ) = 0;
//加载最近的n条好友动态
    virtual bool load_recent_events(
        const std::string& username,
        std::size_t count,
        std::vector<StoredFriendEvent>& events,
        std::string& error
    ) = 0;
};
}