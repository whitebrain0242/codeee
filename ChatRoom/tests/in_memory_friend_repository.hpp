#pragma once

#include "friend_repository.hpp"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace chat::test {

class InMemoryFriendRepository final
    : public IFriendRepository {
public:
    bool initialize(
        std::string& error
    ) override;

    bool load_state(
        FriendState& state,
        std::string& error
    ) override;

    FriendMutationResult create_request(
        const std::string& sender_username,
        const std::string& receiver_username,
        const std::string& protobuf_event,
        std::string& error
    ) override;

    FriendMutationResult accept_request(
        const std::string& requester_username,
        const std::string& accepter_username,
        const std::string& protobuf_event,
        std::string& error
    ) override;

    FriendMutationResult reject_request(
        const std::string& requester_username,
        const std::string& rejecter_username,
        const std::string& protobuf_event,
        std::string& error
    ) override;

    FriendMutationResult remove_friend(
        const std::string& actor_username,
        const std::string& target_username,
        const std::string& protobuf_event,
        std::string& error
    ) override;

    bool load_recent_events(
        const std::string& username,
        std::size_t count,
        std::vector<StoredFriendEvent>& events,
        std::string& error
    ) override;

private:
    struct EventRow {
        std::uint64_t id = 0;
        std::string actor;
        std::string target;
        std::string payload;
    };

    static std::string request_key(
        const std::string& sender,
        const std::string& receiver
    );

    static std::string friendship_key(
        const std::string& left,
        const std::string& right
    );

    static std::pair<std::string, std::string>
    split_key(const std::string& key);

    void add_event(
        const std::string& actor,
        const std::string& target,
        const std::string& payload
    );

    std::unordered_set<std::string> requests_;
    std::unordered_set<std::string> friendships_;
    std::vector<EventRow> events_;
    std::uint64_t next_event_id_ = 1;
};

}  // namespace chat::test
