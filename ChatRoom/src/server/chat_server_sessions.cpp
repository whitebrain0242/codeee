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
                                      ClientSession &session,
                                      const std::string &arguments) {
  if (!session.logged_in || session.username.empty()) {
    send_enter_error(connection, "PRIVATE", "请先登录");
    return;
  }

  const std::vector<std::string> words = split_words(arguments);
  if (words.size() != 1U || !is_valid_username(words[0])) {
    send_enter_error(connection, "PRIVATE", "用户名无效");
    return;
  }

  const std::string &target = words[0];
  if (target == session.username) {
    send_enter_error(connection, "PRIVATE",
                     "不能与自己建立好友私聊会话");
    return;
  }

  std::string error;
  const DirectMessageDecision decision =
      direct_message_policy_.evaluate(session.username, target, error);

  switch (decision) {
    case DirectMessageDecision::Allowed:
        session.realtime_private_target = target;
        session.realtime_private_generation =
            direct_policy_generation_.load(std::memory_order_relaxed);
        session.realtime_group_name.clear();
        session.realtime_group_id = 0U;
        session.realtime_group_recipients.clear();
        session.realtime_group_generation = 0U;
        connection->send("[chat-enter-ok] PRIVATE " + target + "\n");
        return;
    case DirectMessageDecision::TargetMissing:
        send_enter_error(connection, "PRIVATE", "目标账户不存在");
        return;
    case DirectMessageDecision::NotFriends:
        send_enter_error(connection, "PRIVATE",
                         "只有好友之间才能进入私聊");
        return;
    case DirectMessageDecision::BlockedByRecipient:
        send_enter_error(connection, "PRIVATE",
                         "对方已屏蔽你的消息");
        return;
    case DirectMessageDecision::BlockedBySender:    // 新增
        send_enter_error(connection, "PRIVATE",
                         "你已屏蔽该用户，请先解除屏蔽");
        return;
    case DirectMessageDecision::DatabaseError:
        send_enter_error(connection, "PRIVATE",
                         "校验私聊权限时数据库出错");
        return;
  }
}

void ChatServer::handle_enter_group(const TcpConnectionPtr &connection,
                                    ClientSession &session,
                                    const std::string &arguments) {
  if (!session.logged_in || session.username.empty()) {
    send_enter_error(connection, "GROUP", "请先登录");
    return;
  }

  const std::vector<std::string> words = split_words(arguments);
  if (words.size() != 1U || !is_valid_group_name(words[0])) {
    send_enter_error(connection, "GROUP", "群名称无效");
    return;
  }

  const std::string &group_name = words[0];
  std::string error;
  std::optional<GroupInfo> group;
  if (!database_.get_group(group_name, group, error)) {
    send_enter_error(connection, "GROUP",
                     "检查群信息时数据库出错");
    return;
  }
  if (!group) {
    send_enter_error(connection, "GROUP", "群不存在");
    return;
  }

  std::optional<GroupRole> role;
  if (!database_.get_group_role(group_name, session.username, role, error)) {
    send_enter_error(connection, "GROUP",
                     "检查群成员身份时数据库出错");
    return;
  }
  if (!role) {
    send_enter_error(connection, "GROUP", "你不是该群成员");
    return;
  }

  std::vector<std::string> members;
  if (!database_.list_group_member_usernames(group_name, members, error)) {
    send_enter_error(connection, "GROUP", "加载群成员列表时数据库出错");
    return;
  }

  session.realtime_group_name = group_name;
  session.realtime_group_id = group->id;
  session.realtime_group_recipients.clear();
  session.realtime_group_recipients.reserve(members.size());
  for (const std::string &member : members) {
    if (member != session.username) {
      session.realtime_group_recipients.push_back(member);
    }
  }
  session.realtime_group_generation =
      group_policy_generation_.load(std::memory_order_relaxed);

  session.realtime_private_target.clear();
  session.realtime_private_generation = 0U;

  connection->send("[chat-enter-ok] GROUP " + group_name + "\n");
}
