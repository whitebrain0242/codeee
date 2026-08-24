#pragma once

#include "config.hpp"
#include "proto_types.hpp"
#include "integration/mysql_connection_pool.hpp"

#include <cstddef>
#include <cstdint>
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
    std::size_t pool_size() const noexcept;

    bool user_exists(
        const std::string& username,
        bool& exists,
        std::string& error
    );

    bool create_user(
        const std::string& username,
        const std::string& password_hash,
        std::string& error
    );

    bool delete_user(
        const std::string& username,
        bool& removed,
        std::string& error
    );

    bool get_password_hash(
        const std::string& username,
        std::optional<std::string>& password_hash,
        std::string& error
    );

    bool are_friends(
        const std::string& left,
        const std::string& right,
        bool& result,
        std::string& error
    );


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

    bool has_friend_request(
        const std::string& requester,
        const std::string& target,
        bool& result,
        std::string& error
    );

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

    bool remove_friendship(
        const std::string& left,
        const std::string& right,
        bool& removed,
        std::string& error
    );

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

    bool add_friend_event(
        const FriendEventPayload& event,
        std::string& error
    );

    bool add_message(
        const ChatMessagePayload& payload,
        std::uint64_t& message_id,
        std::string& error
    );

    bool add_private_message_with_delivery(
        const ChatMessagePayload& payload,
        std::uint64_t& message_id,
        std::string& error
    );

    bool recent_public_messages(
        std::size_t count,
        std::vector<StoredMessage>& messages,
        std::string& error
    );

    bool recent_private_messages(
        const std::string& user_a,
        const std::string& user_b,
        std::size_t count,
        std::vector<StoredMessage>& messages,
        std::string& error
    );

    bool pending_private_messages(
        const std::string& recipient,
        std::size_t count,
        std::vector<StoredMessage>& messages,
        std::string& error
    );

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

    bool get_group(
        const std::string& group_name,
        std::optional<GroupInfo>& group,
        std::string& error
    );

    bool dissolve_group(
        const std::string& group_name,
        const std::string& owner_username,
        bool& removed,
        std::string& error
    );

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

    bool recent_group_messages(
        const std::string& group_name,
        std::size_t count,
        std::vector<StoredGroupMessage>& messages,
        std::string& error
    );

    bool pending_group_messages(
        const std::string& recipient,
        std::size_t count,
        std::vector<StoredGroupMessage>& messages,
        std::string& error
    );

    bool mark_group_message_delivered(
        std::uint64_t message_id,
        const std::string& recipient,
        std::int64_t delivered_at_unix_ms,
        std::string& error
    );

    bool add_file_transfer(
        const FileTransferMetadata& metadata,
        const std::vector<std::string>& recipients,
        std::uint64_t& transfer_id,
        std::string& error
    );

    bool pending_file_transfers(
        const std::string& recipient,
        std::size_t count,
        std::vector<StoredFileTransfer>& transfers,
        std::string& error
    );

    bool file_transfer_for_recipient(
        std::uint64_t transfer_id,
        const std::string& recipient,
        std::optional<StoredFileTransfer>& transfer,
        std::string& error
    );

    bool mark_file_transfer_delivered(
        std::uint64_t transfer_id,
        const std::string& recipient,
        std::int64_t delivered_at_unix_ms,
        std::string& error
    );

private:
    MySqlConnectionPool pool_;

    bool begin(std::string& error);
    bool commit(std::string& error);
    void rollback();

    bool group_id_by_name(
        const std::string& group_name,
        std::optional<std::uint64_t>& group_id,
        std::string& error
    );

    bool insert_message_locked(
        const ChatMessagePayload& payload,
        std::uint64_t& message_id,
        std::string& error
    );

    static std::pair<std::string, std::string> normalize_pair(
        const std::string& left,
        const std::string& right
    );
};
