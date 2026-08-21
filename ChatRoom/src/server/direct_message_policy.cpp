#include "server/direct_message_policy.hpp"

DirectMessageDecision
DirectMessagePolicy::evaluate(
    const std::string& sender,
    const std::string& recipient,
    std::string& error
) {
    bool target_exists = false;

    if (!database_.user_exists(
            recipient,
            target_exists,
            error
        )) {
        return
            DirectMessageDecision::
                DatabaseError;
    }

    if (!target_exists) {
        return
            DirectMessageDecision::
                TargetMissing;
    }

    bool friends = false;

    if (!database_.are_friends(
            sender,
            recipient,
            friends,
            error
        )) {
        return
            DirectMessageDecision::
                DatabaseError;
    }

    if (!friends) {
        return
            DirectMessageDecision::
                NotFriends;
    }

    bool blocked = false;

    if (!database_.is_friend_blocked(
            recipient,
            sender,
            blocked,
            error
        )) {
        return
            DirectMessageDecision::
                DatabaseError;
    }

    if (blocked) {
        return
            DirectMessageDecision::
                BlockedByRecipient;
    }

    return
        DirectMessageDecision::
            Allowed;
}
