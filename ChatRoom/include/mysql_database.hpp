#pragma once

#include "config.hpp"
#include "proto_types.hpp"

#include <mysql/mysql.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct StoredMessage {
    std::uint64_t id = 0;
    ChatMessagePayload payload;
};

enum class GroupRole : std::uint32_t {
    Owner = 1,
    Admin = 2,
    Member = 3
};

struct GroupInfo {
    std::uint64_t id = 0;
    std::string name;
    std::string owner_username;
};

struct GroupMembership {
    GroupInfo group;
    GroupRole role = GroupRole::Member;
};

struct GroupMemberInfo {
    std::string username;
    GroupRole role = GroupRole::Member;
};

struct ManagedGroupRequestCount {
    std::string group_name;
    std::size_t pending_count = 0;
};

struct StoredGroupMessage {
    std::uint64_t id = 0;
    GroupMessagePayload payload;
};

struct StoredFileTransfer {
    std::uint64_t id = 0;
    FileTransferMetadata metadata;
};

class MySqlDatabase {
public:
    MySqlDatabase() = default;
    ~MySqlDatabase();

    MySqlDatabase(const MySqlDatabase&) = delete;
    MySqlDatabase& operator=(const MySqlDatabase&) = delete;

    bool connect(const MySqlConfig& config, std::string& error);
    bool ping(std::string& error);
    //注册时检查用户名是否已经被占用
    bool user_exists(
        const std::string& username,
        bool& exists,
        std::string& error
    );
    //注册新用户
    bool create_user(
        const std::string& username,
        const std::string& password_hash,
        std::string& error
    );
    //登录时根据用户名取密码哈希验证身份
    bool get_password_hash(
        const std::string& username,
        std::optional<std::string>& password_hash,
        std::string& error
    );
    //检查双方是不是好友
    bool are_friends(
        const std::string& left,
        const std::string& right,
        bool& result,
        std::string& error
    );
    //检查是否屏蔽
    bool is_friend_blocked(
        const std::string& blocker,
        const std::string& blocked,
        bool& result,
        std::string& error
    );
    
    bool add_friend_block(
        const std::string& blocker,
        const std::string& blocked,
        bool& created,
        std::string& error
    );

    bool remove_friend_block(
        const std::string& blocker,
        const std::string& blocked,
        bool& removed,
        std::string& error
    );

    bool list_blocked_friends(
        const std::string& blocker,
        std::vector<std::string>& users,
        std::string& error
    );
    //检查是否已经发送过请求
    bool has_friend_request(
        const std::string& requester,
        const std::string& target,
        bool& result,
        std::string& error
    );
    //发送好友申请
    bool add_friend_request(
        const std::string& requester,
        const std::string& target,
        std::string& error
    );
    
    bool accept_friend_request(
        const std::string& requester,
        const std::string& target,
        std::string& error
    );

    bool reject_friend_request(
        const std::string& requester,
        const std::string& target,
        bool& removed,
        std::string& error
    );
    //删除好友
    bool remove_friendship(
        const std::string& left,
        const std::string& right,
        bool& removed,
        std::string& error
    );
    //获取好友列表
    bool list_friends(
        const std::string& username,
        std::vector<std::string>& friends,
        std::string& error
    );

    bool list_incoming_requests(
        const std::string& username,
        std::vector<std::string>& users,
        std::string& error
    );

    bool list_outgoing_requests(
        const std::string& username,
        std::vector<std::string>& users,
        std::string& error
    );
    //增加好友事件
    bool add_friend_event(
        const FriendEventPayload& event,
        std::string& error
    );

    bool add_message(
        const ChatMessagePayload& payload,
        std::uint64_t& message_id,
        std::string& error
    );
    //发送私聊消息，并且插入一条delivery记录
    bool add_private_message_with_delivery(
        const ChatMessagePayload& payload,
        std::uint64_t& message_id,
        std::string& error
    );
    //获取公聊记录
    bool recent_public_messages(
        std::size_t count,
        std::vector<StoredMessage>& messages,
        std::string& error
    );
    //获取私聊记录
    bool recent_private_messages(
        const std::string& user_a,
        const std::string& user_b,
        std::size_t count,
        std::vector<StoredMessage>& messages,
        std::string& error
    );
    //登陆后拉取未送达的离线私聊消息
    bool pending_private_messages(
        const std::string& recipient,
        std::size_t count,
        std::vector<StoredMessage>& messages,
        std::string& error
    );
    //确认客户端已经受到，标记已经送达
    bool mark_private_message_delivered(
        std::uint64_t message_id,
        const std::string& recipient,
        std::int64_t delivered_at_unix_ms,
        std::string& error
    );

    bool create_group(
        const std::string& group_name,
        const std::string& owner_username,
        std::uint64_t& group_id,
        std::string& error
    );
    //查询群组
    bool get_group(
        const std::string& group_name,
        std::optional<GroupInfo>& group,
        std::string& error
    );
    //解散
    bool dissolve_group(
        const std::string& group_name,
        const std::string& owner_username,
        bool& removed,
        std::string& error
    );
    //检查用户在群内的权限
    bool get_group_role(
        const std::string& group_name,
        const std::string& username,
        std::optional<GroupRole>& role,
        std::string& error
    );

    bool list_user_groups(
        const std::string& username,
        std::vector<GroupMembership>& groups,
        std::string& error
    );
    
    bool list_group_members(
        const std::string& group_name,
        std::vector<GroupMemberInfo>& members,
        std::string& error
    );

    bool list_group_member_usernames(
        const std::string& group_name,
        std::vector<std::string>& members,
        std::string& error
    );

    bool has_group_join_request(
        const std::string& group_name,
        const std::string& username,
        bool& exists,
        std::string& error
    );

    bool add_group_join_request(
        const std::string& group_name,
        const std::string& username,
        std::string& error
    );

    bool list_group_join_requests(
        const std::string& group_name,
        std::vector<std::string>& users,
        std::string& error
    );

    bool list_group_managers(
        const std::string& group_name,
        std::vector<std::string>& users,
        std::string& error
    );

    bool list_managed_group_request_counts(
        const std::string& username,
        std::vector<ManagedGroupRequestCount>& requests,
        std::string& error
    );

    bool approve_group_join_request(
        const std::string& group_name,
        const std::string& username,
        std::string& error
    );

    bool reject_group_join_request(
        const std::string& group_name,
        const std::string& username,
        bool& removed,
        std::string& error
    );
    //设置管理员
    bool set_group_member_role(
        const std::string& group_name,
        const std::string& username,
        GroupRole role,
        bool& changed,
        std::string& error
    );

    bool remove_group_member(
        const std::string& group_name,
        const std::string& username,
        bool& removed,
        std::string& error
    );

    bool add_group_message(
        const GroupMessagePayload& payload,
        const std::vector<std::string>& recipients,
        std::uint64_t& message_id,
        std::string& error
    );
    //获取群聊历史记录
    bool recent_group_messages(
        const std::string& group_name,
        std::size_t count,
        std::vector<StoredGroupMessage>& messages,
        std::string& error
    );
    //用户登陆之后拉取离线群聊消息
    bool pending_group_messages(
        const std::string& recipient,
        std::size_t count,
        std::vector<StoredGroupMessage>& messages,
        std::string& error
    );
    //标记已经送达
    bool mark_group_message_delivered(
        std::uint64_t message_id,
        const std::string& recipient,
        std::int64_t delivered_at_unix_ms,
        std::string& error
    );
    //记录文件元数据
    bool add_file_transfer(
        const FileTransferMetadata& metadata,
        const std::vector<std::string>& recipients,
        std::uint64_t& transfer_id,
        std::string& error
    );
    //拉取离线文件通知
    bool pending_file_transfers(
        const std::string& recipient,
        std::size_t count,
        std::vector<StoredFileTransfer>& transfers,
        std::string& error
    );
    //获取文件传输的详细信息
    bool file_transfer_for_recipient(
        std::uint64_t transfer_id,
        const std::string& recipient,
        std::optional<StoredFileTransfer>& transfer,
        std::string& error
    );
    //标记已经接受
    bool mark_file_transfer_delivered(
        std::uint64_t transfer_id,
        const std::string& recipient,
        std::int64_t delivered_at_unix_ms,
        std::string& error
    );

private:
    MYSQL* connection_ = nullptr;
    mutable std::mutex mutex_;
    //保证多个SQL操作原子性
    bool begin(std::string& error);
    bool commit(std::string& error);
    void rollback();
    //根据群名查ID
    bool group_id_by_name(
        const std::string& group_name,
        std::optional<std::uint64_t>& group_id,
        std::string& error
    );
    //插入消息
    bool insert_message_locked(
        const ChatMessagePayload& payload,
        std::uint64_t& message_id,
        std::string& error
    );
    // 将两个用户名按字典序排序，用于好友关系表
    static std::pair<std::string, std::string> normalize_pair(
        const std::string& left,
        const std::string& right
    );
};
