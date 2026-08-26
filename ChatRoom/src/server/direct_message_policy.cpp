#include "server/direct_message_policy.hpp"

DirectMessageDecision
DirectMessagePolicy::evaluate(const std::string &sender,
                              const std::string &recipient,
                              std::string &error) {
  bool target_exists = false;

  if (!database_.user_exists(recipient, target_exists, error)) {
    return DirectMessageDecision::DatabaseError;
  }

  if (!target_exists) {
    return DirectMessageDecision::TargetMissing;
  }

  bool friends = false;

  if (!database_.are_friends(sender, recipient, friends, error)) {
    return DirectMessageDecision::DatabaseError;
  }

  if (!friends) {
    return DirectMessageDecision::NotFriends;
  }

  // 检查对方是否屏蔽了你
    bool blocked_by_recipient = false;
    if (!database_.is_friend_blocked(recipient, sender, blocked_by_recipient, error)) {
        return DirectMessageDecision::DatabaseError;
    }
    if (blocked_by_recipient) {
        return DirectMessageDecision::BlockedByRecipient;
    }

    // 检查你是否屏蔽了对方（新增）
    bool blocked_sender = false;
    if (!database_.is_friend_blocked(sender, recipient, blocked_sender, error)) {
        return DirectMessageDecision::DatabaseError;
    }
    if (blocked_sender) {
        return DirectMessageDecision::BlockedBySender;  // 或者新增一个 BlockedBySender
    }


  return DirectMessageDecision::Allowed;
}
