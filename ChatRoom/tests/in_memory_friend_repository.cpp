#include "in_memory_friend_repository.hpp"

#include <algorithm>

namespace chat::test {
namespace {

constexpr char kSeparator = '\x1f';

}  // namespace

bool InMemoryFriendRepository::initialize(
    std::string& error
) {
    error.clear();
    return true;
}

bool InMemoryFriendRepository::load_state(
    FriendState& state,
    std::string& error
) {
    error.clear();
    state = {};

    for (const std::string& key : friendships_) {
        state.friendships.push_back(
            split_key(key)
        );
    }

    for (const std::string& key : requests_) {
        state.pending_requests.push_back(
            split_key(key)
        );
    }

    std::sort(
        state.friendships.begin(),
        state.friendships.end()
    );

    std::sort(
        state.pending_requests.begin(),
        state.pending_requests.end()
    );

    return true;
}

FriendMutationResult
InMemoryFriendRepository::create_request(
    const std::string& sender_username,
    const std::string& receiver_username,
    const std::string& protobuf_event,
    std::string& error
) {
    error.clear();

    const std::string key =
        request_key(
            sender_username,
            receiver_username
        );

    if (!requests_.insert(key).second) {
        return FriendMutationResult::AlreadyExists;
    }

    add_event(
        sender_username,
        receiver_username,
        protobuf_event
    );

    return FriendMutationResult::Success;
}

FriendMutationResult
InMemoryFriendRepository::accept_request(
    const std::string& requester_username,
    const std::string& accepter_username,
    const std::string& protobuf_event,
    std::string& error
) {
    error.clear();

    if (
        requests_.erase(
            request_key(
                requester_username,
                accepter_username
            )
        ) != 1
    ) {
        return FriendMutationResult::NotFound;
    }

    const std::string friend_key =
        friendship_key(
            requester_username,
            accepter_username
        );

    if (!friendships_.insert(friend_key).second) {
        requests_.insert(
            request_key(
                requester_username,
                accepter_username
            )
        );

        return FriendMutationResult::AlreadyExists;
    }

    add_event(
        accepter_username,
        requester_username,
        protobuf_event
    );

    return FriendMutationResult::Success;
}

FriendMutationResult
InMemoryFriendRepository::reject_request(
    const std::string& requester_username,
    const std::string& rejecter_username,
    const std::string& protobuf_event,
    std::string& error
) {
    error.clear();

    if (
        requests_.erase(
            request_key(
                requester_username,
                rejecter_username
            )
        ) != 1
    ) {
        return FriendMutationResult::NotFound;
    }

    add_event(
        rejecter_username,
        requester_username,
        protobuf_event
    );

    return FriendMutationResult::Success;
}

FriendMutationResult
InMemoryFriendRepository::remove_friend(
    const std::string& actor_username,
    const std::string& target_username,
    const std::string& protobuf_event,
    std::string& error
) {
    error.clear();

    if (
        friendships_.erase(
            friendship_key(
                actor_username,
                target_username
            )
        ) != 1
    ) {
        return FriendMutationResult::NotFound;
    }

    add_event(
        actor_username,
        target_username,
        protobuf_event
    );

    return FriendMutationResult::Success;
}

bool InMemoryFriendRepository::load_recent_events(
    const std::string& username,
    std::size_t count,
    std::vector<StoredFriendEvent>& events,
    std::string& error
) {
    error.clear();
    events.clear();

    std::vector<StoredFriendEvent> matching;

    for (const EventRow& row : events_) {
        if (
            row.actor == username ||
            row.target == username
        ) {
            matching.push_back(
                StoredFriendEvent{
                    row.id,
                    row.payload
                }
            );
        }
    }

    const std::size_t start =
        matching.size() > count
            ? matching.size() - count
            : 0;

    events.insert(
        events.end(),
        matching.begin() +
            static_cast<std::ptrdiff_t>(start),
        matching.end()
    );

    return true;
}

std::string
InMemoryFriendRepository::request_key(
    const std::string& sender,
    const std::string& receiver
) {
    return sender +
        std::string(1, kSeparator) +
        receiver;
}

std::string
InMemoryFriendRepository::friendship_key(
    const std::string& left,
    const std::string& right
) {
    if (left <= right) {
        return request_key(left, right);
    }

    return request_key(right, left);
}

std::pair<std::string, std::string>
InMemoryFriendRepository::split_key(
    const std::string& key
) {
    const std::size_t separator =
        key.find(kSeparator);

    if (separator == std::string::npos) {
        return {key, ""};
    }

    return {
        key.substr(0, separator),
        key.substr(separator + 1)
    };
}

void InMemoryFriendRepository::add_event(
    const std::string& actor,
    const std::string& target,
    const std::string& payload
) {
    events_.push_back(
        EventRow{
            next_event_id_++,
            actor,
            target,
            payload
        }
    );
}

}  // namespace chat::test
