#include "chat_server.hpp"
#include "file_utils.hpp"
#include "minimuduo/net/TcpConnection.hpp"

#include <optional>
#include <string>
#include <vector>

namespace {
void send_enter_error(const ChatServer::TcpConnectionPtr &connection,
                      const std::string &scope, const std::string &reason) {
  connection->send("[chat-enter-error] " + scope + " " +
                   fileutil::percent_encode(reason) + "\n");
}
} // namespace

void ChatServer::handle_enter_private(const TcpConnectionPtr &connection,
                                      const ClientSession &session,
                                      const std::string &arguments) {
  if (!session.logged_in || session.username.empty()) {
    send_enter_error(connection, "PRIVATE", "please login first");
    return;
  }

  const std::vector<std::string> words = split_words(arguments);
  if (words.size() != 1U || !is_valid_username(words[0])) {
    send_enter_error(connection, "PRIVATE", "invalid username");
    return;
  }

  const std::string &target = words[0];
  if (target == session.username) {
    send_enter_error(connection, "PRIVATE",
                     "you cannot enter a private chat with yourself");
    return;
  }

  std::string error;
  const DirectMessageDecision decision =
      direct_message_policy_.evaluate(session.username, target, error);

  switch (decision) {
    case DirectMessageDecision::Allowed:
        connection->send("[chat-enter-ok] PRIVATE " + target + "\n");
        return;
    case DirectMessageDecision::TargetMissing:
        send_enter_error(connection, "PRIVATE", "target account does not exist");
        return;
    case DirectMessageDecision::NotFriends:
        send_enter_error(connection, "PRIVATE",
                         "private chat is allowed only between friends");
        return;
    case DirectMessageDecision::BlockedByRecipient:
        send_enter_error(connection, "PRIVATE",
                         "target has blocked messages from you");
        return;
    case DirectMessageDecision::BlockedBySender:    // 新增
        send_enter_error(connection, "PRIVATE",
                         "you have blocked this user, please unblock first");
        return;
    case DirectMessageDecision::DatabaseError:
        send_enter_error(connection, "PRIVATE",
                         "database error while validating private chat");
        return;
  }
}

void ChatServer::handle_enter_group(const TcpConnectionPtr &connection,
                                    const ClientSession &session,
                                    const std::string &arguments) {
  if (!session.logged_in || session.username.empty()) {
    send_enter_error(connection, "GROUP", "please login first");
    return;
  }

  const std::vector<std::string> words = split_words(arguments);
  if (words.size() != 1U || !is_valid_group_name(words[0])) {
    send_enter_error(connection, "GROUP", "invalid group name");
    return;
  }

  const std::string &group_name = words[0];
  std::string error;
  std::optional<GroupInfo> group;
  if (!database_.get_group(group_name, group, error)) {
    send_enter_error(connection, "GROUP",
                     "database error while checking group");
    return;
  }
  if (!group) {
    send_enter_error(connection, "GROUP", "group does not exist");
    return;
  }

  std::optional<GroupRole> role;
  if (!database_.get_group_role(group_name, session.username, role, error)) {
    send_enter_error(connection, "GROUP",
                     "database error while checking group membership");
    return;
  }
  if (!role) {
    send_enter_error(connection, "GROUP",
                     "you are not a member of this group");
    return;
  }

  connection->send("[chat-enter-ok] GROUP " + group_name + "\n");
}
