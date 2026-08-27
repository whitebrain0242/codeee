#include "chat_server.hpp"
#include "minimuduo/net/TcpConnection.hpp"

void ChatServer::handle_block_friend(const TcpConnectionPtr &connection,
                                     const ClientSession &session,
                                     const std::string &arguments) {
  if (!require_login(connection, session, "blocking friend messages")) {
    return;
  }

  std::string target;

  if (!extract_single_username(connection, arguments, "BLOCK_FRIEND <username>",
                               target)) {
    return;
  }

  if (target == session.username) {
    connection->send("[error] 不能屏蔽自己。\n");
    return;
  }

  std::string error;
  bool created = false;

  {
    std::lock_guard<std::mutex> operation_lock(friend_operation_mutex_);

    bool friends = false;

    if (!database_.are_friends(session.username, target, friends, error)) {
      database_error(connection, "checking friendship before blocking", error);
      return;
    }

    if (!friends) {
      connection->send("[error] 只能屏蔽当前好友的私聊消息和文件。\n");
      return;
    }

    if (!database_.add_friend_block(session.username, target, created, error)) {
      database_error(connection, "blocking friend messages", error);
      return;
    }
  }

  if (!created) {
    connection->send("[system] " + target +
                     " 已经在屏蔽列表中。\n");
    return;
  }

  direct_policy_generation_.fetch_add(1U, std::memory_order_relaxed);

  connection->send("[system] 已屏蔽来自 " + target +
                   " 的私聊消息和文件。好友关系、历史记录、公共聊天和群聊不受影响。\n"
                   "[system] 屏蔽期间，该好友发送的待处理私聊消息/文件仍会保存，但不会投递。\n");
}

void ChatServer::handle_unblock_friend(const TcpConnectionPtr &connection,
                                       const ClientSession &session,
                                       const std::string &arguments) {
  if (!require_login(connection, session, "unblocking friend messages")) {
    return;
  }

  std::string target;

  if (!extract_single_username(connection, arguments,
                               "UNBLOCK_FRIEND <username>", target)) {
    return;
  }

  bool removed = false;
  std::string error;

  {
    std::lock_guard<std::mutex> operation_lock(friend_operation_mutex_);

    if (!database_.remove_friend_block(session.username, target, removed,
                                       error)) {
      database_error(connection, "unblocking friend messages", error);
      return;
    }
  }

  if (!removed) {
    connection->send("[error] " + target +
                     " 不在你的好友屏蔽列表中。\n");
    return;
  }

  direct_policy_generation_.fetch_add(1U, std::memory_order_relaxed);

  connection->send("[system] 已解除对 " + target +
                   " 的私聊消息/文件屏蔽。可使用数字命令 37 立即重试已保存的离线投递。\n");
}

void ChatServer::handle_blocked_friends(const TcpConnectionPtr &connection,
                                        const ClientSession &session) {
  if (!require_login(connection, session, "viewing blocked friends")) {
    return;
  }

  std::vector<std::string> blocked;
  std::string error;

  if (!database_.list_blocked_friends(session.username, blocked, error)) {
    database_error(connection, "listing blocked friends", error);
    return;
  }

  connection->send("[system] 已屏蔽好友（" +
                   std::to_string(blocked.size()) +
                   "）：" + join_names(blocked) + "\n");
}
