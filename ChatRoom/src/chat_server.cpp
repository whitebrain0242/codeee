#include "chat_server.hpp"

#include "password.hpp"
#include "protocol.hpp"

#include "minimuduo/net/Buffer.hpp"
#include "minimuduo/net/EventLoop.hpp"
#include "minimuduo/net/TcpConnection.hpp"
#include "minimuduo/net/TcpServer.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <any>
#include <cctype>
#include <charconv>
#include <chrono>
#include <ctime>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

bool parse_uint64_value(const std::string &text, std::uint64_t &value) {
  if (text.empty()) {
    return false;
  }

  std::uint64_t parsed = 0;
  const char *begin = text.data();
  const char *end = begin + text.size();

  const auto result = std::from_chars(begin, end, parsed);

  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }

  value = parsed;
  return true;
}

std::string encode_text_token(const std::string &text) {
  return fileutil::percent_encode(text);
}

} // namespace

ChatServer::ChatServer(minimuduo::net::TcpServer &tcp_server,
                       MySqlDatabase &database, RedisClient &redis,
                       std::string server_instance_id,
                       unsigned int presence_ttl_seconds,
                       std::filesystem::path file_storage_root)
    : tcp_server_(tcp_server), database_(database), redis_(redis),
      server_instance_id_(std::move(server_instance_id)),
      presence_ttl_seconds_(presence_ttl_seconds),
      file_transfer_service_(std::move(file_storage_root), 2U),
      direct_message_policy_(database) {
  std::string file_error;
  if (!file_transfer_service_.initialize(file_error)) {
    throw std::runtime_error("file transfer storage initialization failed: " +
                             file_error);
  }
  tcp_server_.setConnectionCallback([this](const TcpConnectionPtr &connection) {
    on_connection(connection);
  });

  tcp_server_.setMessageCallback([this](const TcpConnectionPtr &connection,
                                        minimuduo::net::Buffer *buffer) {
    on_message(connection, buffer);
  });

  configure_command_routes();
  presence_refresh_thread_ = std::thread([this] { presence_refresh_loop(); });
}

ChatServer::~ChatServer() {
  file_transfer_service_.stop();

  stopping_.store(true);
  presence_wait_cv_.notify_all();

  if (presence_refresh_thread_.joinable()) {
    presence_refresh_thread_.join();
  }

  const std::vector<std::string> usernames = online_users_.usernames();

  for (const std::string &username : usernames) {
    remove_redis_presence_best_effort(username);
  }
}

void ChatServer::on_connection(const TcpConnectionPtr &connection) {
  if (connection->connected()) {
    connection->setContext(std::make_shared<ClientSession>());

    std::cout << "client connected: " << connection->name() << " from "
              << connection->peerAddressText() << '\n';

    spdlog::info("client connected: {} from {}", connection->name(),
                 connection->peerAddressText());

    connection->send("[system] connected to chatroom v8.4 "
                     "(Reactor + TLS + client-initiated heartbeat + blocking + "
                     "raw resumable files).\n"
                     "[system] Type HELP for commands.\n");
    return;
  }

  const std::shared_ptr<ClientSession> session = session_of(connection);

  if (session != nullptr) {
    detach_active_upload(*session);
  }

  if (session != nullptr && session->logged_in) {
    const std::string username = session->username;
    remove_online_user(username, connection);
    remove_redis_presence_best_effort(username);

    broadcast_to_logged_in("[system] " + username + " is offline.\n",
                           connection);
  }

  std::cout << "client disconnected: " << connection->name() << '\n';

  spdlog::info("client disconnected: {}", connection->name());
}

void ChatServer::on_message(const TcpConnectionPtr &connection,
                            minimuduo::net::Buffer *buffer) {
  const std::shared_ptr<ClientSession> session = session_of(connection);

  if (session == nullptr) {
    connection->send("[error] session state is unavailable.\n");
    connection->forceClose();
    return;
  }

  while (connection->connected()) {
    if (session->binary_upload &&
        session->binary_upload->remaining_bytes > 0U) {
      if (!session->upload ||
          session->upload->token != session->binary_upload->token) {
        session->binary_upload.reset();
        connection->send("[error] binary upload state is invalid.\n");
        connection->forceClose();
        return;
      }

      if (buffer->readableBytes() == 0U) {
        return;
      }

      PendingBinaryUploadFrame &frame = *session->binary_upload;

      const std::size_t consume =
          static_cast<std::size_t>(std::min<std::uint64_t>(
              frame.remaining_bytes,
              static_cast<std::uint64_t>(buffer->readableBytes())));

      std::uint64_t accepted_offset = 0U;
      std::string error;

      if (!file_transfer_service_.append_upload_bytes(
              session->upload->temp_path, frame.next_offset, buffer->peek(),
              consume, accepted_offset, error)) {
        const std::string token = session->upload->token;

        pause_file_upload(connection, *session, token, error);
        return;
      }

      buffer->retrieve(consume);

      frame.next_offset = accepted_offset;

      frame.remaining_bytes -= static_cast<std::uint64_t>(consume);

      session->upload->received_size = accepted_offset;

      if (frame.remaining_bytes == 0U) {
        session->binary_upload.reset();
      }

      continue;
    }

    const char *eol = buffer->findEOL();

    if (eol == nullptr) {
      break;
    }

    const std::size_t line_size =
        static_cast<std::size_t>(eol - buffer->peek());

    std::string line = buffer->retrieveAsString(line_size);

    buffer->retrieve(1U);

    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    const Command command = parse_command(line);

    if (!command.name.empty()) {
      handle_command(connection, *session, command);
    }
  }

  if (!session->binary_upload && buffer->readableBytes() > kMaxInputBuffer) {
    connection->send("[error] input line is too long; "
                     "connection will close.\n");
    connection->shutdown();
  }
}

void ChatServer::handle_command(const TcpConnectionPtr &connection,
                                ClientSession &session,
                                const Command &command) {
  if (!command_router_.dispatch(connection, session, command)) {
    connection->send("[error] unknown command. "
                     "Type HELP to see available commands.\n");
  }
}

void ChatServer::send_help(const TcpConnectionPtr &connection) {
  connection->send("[system] commands:\n"
                   "  REGISTER <username> <password>\n"
                   "  LOGIN <username> <password>\n"
                   "  LOGOUT\n"
                   "  SAY <message>\n"
                   "  MSG <username> <message>\n"
                   "  WHO\n"
                   "  ADD_FRIEND <username>\n"
                   "  ACCEPT_FRIEND <username>\n"
                   "  REJECT_FRIEND <username>\n"
                   "  REMOVE_FRIEND <username>\n"
                   "  BLOCK_FRIEND <username>\n"
                   "  UNBLOCK_FRIEND <username>\n"
                   "  BLOCKED_FRIENDS\n"
                   "  FRIENDS\n"
                   "  FRIEND_REQUESTS\n"
                   "  HISTORY_PUBLIC [count]\n"
                   "  HISTORY_PRIVATE <username> [count]\n"
                   "  CREATE_GROUP <group_name>\n"
                   "  DISSOLVE_GROUP <group_name>\n"
                   "  APPLY_GROUP <group_name>\n"
                   "  MY_GROUPS\n"
                   "  LEAVE_GROUP <group_name>\n"
                   "  GROUP_MEMBERS <group_name>\n"
                   "  ADD_GROUP_ADMIN <group_name> <username>\n"
                   "  REMOVE_GROUP_ADMIN <group_name> <username>\n"
                   "  GROUP_REQUESTS <group_name>\n"
                   "  APPROVE_GROUP <group_name> <username>\n"
                   "  REJECT_GROUP <group_name> <username>\n"
                   "  REMOVE_GROUP_MEMBER <group_name> <username>\n"
                   "  GROUP_MSG <group_name> <message>\n"
                   "  HISTORY_GROUP <group_name> [count]\n"
                   "  PENDING\n"
                   "  QUIT\n"
                   "[system] PING is a reserved client heartbeat command; "
                   "server replies PONG.\n"
                   "[system] chat_client local file commands:\n"
                   "  SEND_FILE <username> <path>\n"
                   "  SEND_GROUP_FILE <group_name> <path>\n"
                   "  LOCAL_FILES [count]\n");
}

void ChatServer::handle_register(const TcpConnectionPtr &connection,
                                 ClientSession &session,
                                 const std::string &arguments) {
  if (session.logged_in) {
    connection->send("[error] LOGOUT before registering "
                     "another account.\n");
    return;
  }

  const std::vector<std::string> words = split_words(arguments);

  if (words.size() != 2U || !is_valid_username(words[0]) ||
      !is_valid_password(words[1])) {
    connection->send("[error] usage: REGISTER <username> <password>; "
                     "username 3-20 letters/digits/underscore, "
                     "password 4-64 non-space characters.\n");
    return;
  }

  bool exists = false;
  std::string error;

  if (!database_.user_exists(words[0], exists, error)) {
    database_error(connection, "checking username", error);
    return;
  }

  if (exists) {
    connection->send("[error] username already exists.\n");
    return;
  }

  std::string encoded;
  try {
    encoded = hash_password_pbkdf2(words[1]);
  } catch (const std::exception &exception) {
    connection->send("[error] password hashing failed.\n");
    std::cerr << exception.what() << '\n';
    return;
  }

  if (!database_.create_user(words[0], encoded, error)) {
    database_error(connection, "creating account", error);
    return;
  }

  connection->send("[system] registration successful. "
                   "Use LOGIN <username> <password>.\n");
}

void ChatServer::handle_login(const TcpConnectionPtr &connection,
                              ClientSession &session,
                              const std::string &arguments) {
  if (session.logged_in) {
    connection->send("[error] this connection is already logged in.\n");
    return;
  }

  const std::vector<std::string> words = split_words(arguments);

  if (words.size() != 2U) {
    connection->send("[error] usage: LOGIN <username> <password>\n");
    return;
  }

  std::optional<std::string> password_hash;
  std::string error;

  if (!database_.get_password_hash(words[0], password_hash, error)) {
    database_error(connection, "loading account", error);
    return;
  }

  if (!password_hash || !verify_password_pbkdf2(words[1], *password_hash)) {
    connection->send("[error] invalid username or password.\n");
    return;
  }

  if (!register_online_user(words[0], connection)) {
    connection->send("[error] this account is already logged in.\n");
    return;
  }

  if (!claim_redis_presence(words[0], connection)) {
    remove_online_user(words[0], connection);
    return;
  }

  session.logged_in = true;
  session.username = words[0];

  connection->send("[system] login successful. Welcome, " + session.username +
                   ".\n");

  broadcast_to_logged_in("[system] " + session.username + " is online.\n",
                         connection);

  notify_pending_requests(connection, session.username);

  send_redis_unread_summary_best_effort(connection, session.username);

  deliver_pending_messages(connection, session.username);

  deliver_pending_files(connection, session.username);
}

void ChatServer::handle_logout(const TcpConnectionPtr &connection,
                               ClientSession &session) {
  if (!require_login(connection, session, "logging out")) {
    return;
  }

  const std::string username = session.username;
  detach_active_upload(session);
  remove_online_user(username, connection);
  remove_redis_presence_best_effort(username);

  session.logged_in = false;
  session.username.clear();

  connection->send("[system] logout successful.\n");

  broadcast_to_logged_in("[system] " + username + " is offline.\n", connection);
}

void ChatServer::handle_public_message(const TcpConnectionPtr &connection,
                                       const ClientSession &session,
                                       const std::string &message) {
  if (!require_login(connection, session, "chatting")) {
    return;
  }

  const std::string cleaned = trim(message);
  if (cleaned.empty() || cleaned.size() > kMaxChatMessage) {
    connection->send("[error] SAY requires 1-1000 bytes "
                     "of message text.\n");
    return;
  }

  ChatMessagePayload payload;
  payload.set_type(chatroom::v7::PUBLIC);
  payload.set_sender_username(session.username);
  payload.set_content(cleaned);
  payload.set_created_at_unix_ms(now_unix_ms());

  std::uint64_t message_id = 0;
  std::string error;

  if (!database_.add_message(payload, message_id, error)) {
    database_error(connection, "saving public message", error);
    return;
  }

  broadcast_to_logged_in("[#" + std::to_string(message_id) + "] [" +
                         payload.sender_username() + "] " + payload.content() +
                         "\n");
}

void ChatServer::handle_private_message(const TcpConnectionPtr &connection,
                                        const ClientSession &session,
                                        const std::string &arguments) {
  if (!require_login(connection, session, "sending private messages")) {
    return;
  }

  std::string target;
  std::string message;

  if (!split_first_token(arguments, target, message) ||
      !is_valid_username(target) || message.empty() ||
      message.size() > kMaxChatMessage) {
    connection->send("[error] usage: MSG <username> <message>\n");
    return;
  }

  const std::string sender = session.username;
  if (target == sender) {
    connection->send("[error] you cannot message yourself.\n");
    return;
  }

  std::string error;
  std::uint64_t message_id = 0;

  ChatMessagePayload payload;
  payload.set_type(chatroom::v7::PRIVATE);
  payload.set_sender_username(sender);
  payload.set_recipient_username(target);
  payload.set_content(message);
  payload.set_created_at_unix_ms(now_unix_ms());

  {
    // BLOCK_FRIEND and direct send must have a deterministic order
    // across different SubReactors.
    std::lock_guard<std::mutex> operation_lock(friend_operation_mutex_);

    const DirectMessageDecision decision =
        direct_message_policy_.evaluate(sender, target, error);

    if (decision == DirectMessageDecision::DatabaseError) {
      database_error(connection, "checking private-message policy", error);
      return;
    }

    if (decision == DirectMessageDecision::TargetMissing) {
      connection->send("[error] target account does not exist.\n");
      return;
    }

    if (decision == DirectMessageDecision::NotFriends) {
      connection->send("[error] private messaging is allowed "
                       "only between friends.\n");
      return;
    }

    if (decision == DirectMessageDecision::BlockedByRecipient) {
      connection->send("[error] recipient privacy settings "
                       "do not accept direct messages from you.\n");
      return;
    }

    if (!database_.add_private_message_with_delivery(payload, message_id,
                                                     error)) {
      database_error(connection, "saving private message", error);
      return;
    }
  }

  adjust_redis_unread_best_effort(target, "private", 1);

  TcpConnectionPtr target_connection;

  bool blocked_before_live_delivery = false;

  if (!database_.is_friend_blocked(target, sender, blocked_before_live_delivery,
                                   error)) {
    // The message is already durable at this point. A transient
    // re-check failure must not make the sender think persistence
    // failed or cause an accidental duplicate on retry.
    std::cerr << "failed to recheck block before live delivery of #"
              << message_id << ": " << error << '\n';

    blocked_before_live_delivery = true;
  }

  if (!blocked_before_live_delivery &&
      find_online_user(target, target_connection)) {
    target_connection->send(
        "[#" + std::to_string(message_id) + "] [private from " + sender + "] " +
            message + "\n",
        [this, message_id, target] {
          std::string delivery_error;
          if (!database_.mark_private_message_delivered(
                  message_id, target, now_unix_ms(), delivery_error)) {
            std::cerr << "failed to mark private message #" << message_id
                      << " delivered: " << delivery_error << '\n';
          } else {
            adjust_redis_unread_best_effort(target, "private", -1);
          }
        });

    connection->send("[#" + std::to_string(message_id) + "] [private to " +
                     target + "] " + message + "\n");
    return;
  }

  connection->send("[#" + std::to_string(message_id) + "] [private to " +
                   target + "] " + message +
                   "\n"
                   "[system] the message is stored and remains pending "
                   "until the recipient is eligible for direct delivery.\n");
}

void ChatServer::handle_who(const TcpConnectionPtr &connection,
                            const ClientSession &session) {
  if (!require_login(connection, session, "using WHO")) {
    return;
  }

  const std::vector<std::string> names = online_users_.usernames();

  connection->send("[system] online users (" + std::to_string(names.size()) +
                   "): " + join_names(names) + "\n");
}

void ChatServer::handle_add_friend(const TcpConnectionPtr &connection,
                                   const ClientSession &session,
                                   const std::string &arguments) {
  if (!require_login(connection, session, "adding friends")) {
    return;
  }

  std::string target;
  if (!extract_single_username(connection, arguments, "ADD_FRIEND <username>",
                               target)) {
    return;
  }

  const std::string sender = session.username;
  if (sender == target) {
    connection->send("[error] you cannot add yourself.\n");
    return;
  }

  std::string error;

  {
    std::lock_guard<std::mutex> operation_lock(friend_operation_mutex_);

    bool exists = false;
    bool friends = false;
    bool already_sent = false;
    bool reverse_request = false;

    if (!database_.user_exists(target, exists, error)) {
      database_error(connection, "checking target user", error);
      return;
    }

    if (!exists) {
      connection->send("[error] user " + target + " does not exist.\n");
      return;
    }

    if (!database_.are_friends(sender, target, friends, error)) {
      database_error(connection, "checking friendship", error);
      return;
    }

    if (friends) {
      connection->send("[error] " + target + " is already your friend.\n");
      return;
    }

    if (!database_.has_friend_request(sender, target, already_sent, error) ||
        !database_.has_friend_request(target, sender, reverse_request, error)) {
      database_error(connection, "checking friend request", error);
      return;
    }

    if (already_sent) {
      connection->send("[error] friend request already sent.\n");
      return;
    }

    if (reverse_request) {
      connection->send("[error] " + target +
                       " already sent you a request. "
                       "Use ACCEPT_FRIEND " +
                       target + ".\n");
      return;
    }

    if (!database_.add_friend_request(sender, target, error)) {
      database_error(connection, "saving friend request", error);
      return;
    }

    FriendEventPayload event;
    event.set_type(chatroom::v7::FRIEND_REQUEST_SENT);
    event.set_actor_username(sender);
    event.set_target_username(target);
    event.set_occurred_at_unix_ms(now_unix_ms());

    if (!database_.add_friend_event(event, error)) {
      std::cerr << "friend event insert failed: " << error << '\n';
    }
  }

  connection->send("[system] friend request sent to " + target + ".\n");

  notify_user_if_online(target, "[system] friend request from " + sender +
                                    ". Use ACCEPT_FRIEND " + sender +
                                    " or REJECT_FRIEND " + sender + ".\n");
}

void ChatServer::handle_accept_friend(const TcpConnectionPtr &connection,
                                      const ClientSession &session,
                                      const std::string &arguments) {
  if (!require_login(connection, session, "accepting friend requests")) {
    return;
  }

  std::string requester;
  if (!extract_single_username(connection, arguments,
                               "ACCEPT_FRIEND <username>", requester)) {
    return;
  }

  const std::string current = session.username;
  std::string error;

  {
    std::lock_guard<std::mutex> operation_lock(friend_operation_mutex_);

    if (!database_.accept_friend_request(requester, current, error)) {
      if (error == "friend request does not exist") {
        connection->send("[error] no pending friend request from " + requester +
                         ".\n");
      } else {
        database_error(connection, "accepting friend request", error);
      }
      return;
    }

    FriendEventPayload event;
    event.set_type(chatroom::v7::FRIEND_REQUEST_ACCEPTED);
    event.set_actor_username(current);
    event.set_target_username(requester);
    event.set_occurred_at_unix_ms(now_unix_ms());

    if (!database_.add_friend_event(event, error)) {
      std::cerr << "friend event insert failed: " << error << '\n';
    }
  }

  connection->send("[system] you and " + requester + " are now friends.\n");

  notify_user_if_online(requester, "[system] " + current +
                                       " accepted your friend request.\n");
}

void ChatServer::handle_reject_friend(const TcpConnectionPtr &connection,
                                      const ClientSession &session,
                                      const std::string &arguments) {
  if (!require_login(connection, session, "rejecting friend requests")) {
    return;
  }

  std::string requester;
  if (!extract_single_username(connection, arguments,
                               "REJECT_FRIEND <username>", requester)) {
    return;
  }

  const std::string current = session.username;
  std::string error;

  {
    std::lock_guard<std::mutex> operation_lock(friend_operation_mutex_);

    bool removed = false;

    if (!database_.reject_friend_request(requester, current, removed, error)) {
      database_error(connection, "rejecting friend request", error);
      return;
    }

    if (!removed) {
      connection->send("[error] no pending friend request from " + requester +
                       ".\n");
      return;
    }

    FriendEventPayload event;
    event.set_type(chatroom::v7::FRIEND_REQUEST_REJECTED);
    event.set_actor_username(current);
    event.set_target_username(requester);
    event.set_occurred_at_unix_ms(now_unix_ms());

    if (!database_.add_friend_event(event, error)) {
      std::cerr << "friend event insert failed: " << error << '\n';
    }
  }

  connection->send("[system] friend request from " + requester +
                   " rejected.\n");

  notify_user_if_online(requester, "[system] " + current +
                                       " rejected your friend request.\n");
}

void ChatServer::handle_remove_friend(const TcpConnectionPtr &connection,
                                      const ClientSession &session,
                                      const std::string &arguments) {
  if (!require_login(connection, session, "removing friends")) {
    return;
  }

  std::string target;
  if (!extract_single_username(connection, arguments,
                               "REMOVE_FRIEND <username>", target)) {
    return;
  }

  const std::string current = session.username;
  std::string error;

  {
    std::lock_guard<std::mutex> operation_lock(friend_operation_mutex_);

    bool removed = false;

    if (!database_.remove_friendship(current, target, removed, error)) {
      database_error(connection, "removing friend", error);
      return;
    }

    if (!removed) {
      connection->send("[error] " + target + " is not your friend.\n");
      return;
    }

    FriendEventPayload event;
    event.set_type(chatroom::v7::FRIEND_REMOVED);
    event.set_actor_username(current);
    event.set_target_username(target);
    event.set_occurred_at_unix_ms(now_unix_ms());

    if (!database_.add_friend_event(event, error)) {
      std::cerr << "friend event insert failed: " << error << '\n';
    }
  }

  connection->send("[system] removed " + target + " from friends.\n");

  notify_user_if_online(target, "[system] " + current +
                                    " removed you from their friend list.\n");
}

void ChatServer::handle_friends(const TcpConnectionPtr &connection,
                                const ClientSession &session) {
  if (!require_login(connection, session, "viewing friends")) {
    return;
  }

  std::vector<std::string> friends;
  std::string error;

  if (!database_.list_friends(session.username, friends, error)) {
    database_error(connection, "loading friends", error);
    return;
  }

  std::ostringstream output;
  output << "[system] friends (" << friends.size() << "):\n";

  for (const std::string &name : friends) {
    output << "  " << name << " ["
           << (is_user_online(name) ? "online" : "offline") << "]\n";
  }

  if (friends.empty()) {
    output << "  (none)\n";
  }

  connection->send(output.str());
}

void ChatServer::handle_friend_requests(const TcpConnectionPtr &connection,
                                        const ClientSession &session) {
  if (!require_login(connection, session, "viewing friend requests")) {
    return;
  }

  std::vector<std::string> incoming;
  std::vector<std::string> outgoing;
  std::string error;

  if (!database_.list_incoming_requests(session.username, incoming, error) ||
      !database_.list_outgoing_requests(session.username, outgoing, error)) {
    database_error(connection, "loading friend requests", error);
    return;
  }

  connection->send(
      "[system] incoming requests (" + std::to_string(incoming.size()) +
      "): " + join_names(incoming) +
      "\n"
      "[system] outgoing requests (" +
      std::to_string(outgoing.size()) + "): " + join_names(outgoing) + "\n");
}

void ChatServer::handle_history_public(const TcpConnectionPtr &connection,
                                       const ClientSession &session,
                                       const std::string &arguments) {
  if (!require_login(connection, session, "viewing public history")) {
    return;
  }

  std::size_t count = kDefaultHistoryCount;

  if (!trim(arguments).empty() &&
      !parse_count(trim(arguments), 1U, kMaxHistoryCount, count)) {
    connection->send("[error] usage: HISTORY_PUBLIC [count], "
                     "count must be 1-100.\n");
    return;
  }

  std::vector<StoredMessage> messages;
  std::string error;

  if (!database_.recent_public_messages(count, messages, error)) {
    database_error(connection, "loading public history", error);
    return;
  }

  std::ostringstream output;
  output << "[history public] showing " << messages.size() << " message(s):\n";

  for (const StoredMessage &message : messages) {
    output << "  #" << message.id << " "
           << format_unix_ms(message.payload.created_at_unix_ms()) << " ["
           << message.payload.sender_username() << "] "
           << message.payload.content() << "\n";
  }

  connection->send(output.str());
}

void ChatServer::handle_history_private(const TcpConnectionPtr &connection,
                                        const ClientSession &session,
                                        const std::string &arguments) {
  if (!require_login(connection, session, "viewing private history")) {
    return;
  }

  const std::vector<std::string> words = split_words(arguments);

  if (words.empty() || words.size() > 2U || !is_valid_username(words[0])) {
    connection->send("[error] usage: HISTORY_PRIVATE "
                     "<username> [count]\n");
    return;
  }

  std::size_t count = kDefaultHistoryCount;

  if (words.size() == 2U &&
      !parse_count(words[1], 1U, kMaxHistoryCount, count)) {
    connection->send("[error] HISTORY_PRIVATE count "
                     "must be 1-100.\n");
    return;
  }

  bool exists = false;
  std::string error;

  if (!database_.user_exists(words[0], exists, error)) {
    database_error(connection, "checking history user", error);
    return;
  }

  if (!exists) {
    connection->send("[error] user does not exist.\n");
    return;
  }

  std::vector<StoredMessage> messages;

  if (!database_.recent_private_messages(session.username, words[0], count,
                                         messages, error)) {
    database_error(connection, "loading private history", error);
    return;
  }

  std::ostringstream output;
  output << "[history private with " << words[0] << "] showing "
         << messages.size() << " message(s):\n";

  for (const StoredMessage &message : messages) {
    output << "  #" << message.id << " "
           << format_unix_ms(message.payload.created_at_unix_ms()) << " "
           << message.payload.sender_username() << " -> "
           << message.payload.recipient_username() << ": "
           << message.payload.content() << "\n";
  }

  connection->send(output.str());
}

void ChatServer::handle_create_group(const TcpConnectionPtr &connection,
                                     const ClientSession &session,
                                     const std::string &arguments) {
  if (!require_login(connection, session, "creating groups")) {
    return;
  }

  std::string group_name;
  if (!extract_single_group_name(connection, arguments,
                                 "CREATE_GROUP <group_name>", group_name)) {
    return;
  }

  std::uint64_t group_id = 0;
  std::string error;

  {
    std::lock_guard<std::mutex> operation_lock(group_operation_mutex_);

    std::optional<GroupInfo> existing;
    if (!database_.get_group(group_name, existing, error)) {
      database_error(connection, "checking group name", error);
      return;
    }

    if (existing) {
      connection->send("[error] group name is already in use.\n");
      return;
    }

    if (!database_.create_group(group_name, session.username, group_id,
                                error)) {
      database_error(connection, "creating group", error);
      return;
    }
  }

  connection->send("[system] group " + group_name +
                   " created. You are the owner. group_id=" +
                   std::to_string(group_id) + ".\n");
}

void ChatServer::handle_dissolve_group(const TcpConnectionPtr &connection,
                                       const ClientSession &session,
                                       const std::string &arguments) {
  if (!require_login(connection, session, "dissolving groups")) {
    return;
  }

  std::string group_name;
  if (!extract_single_group_name(connection, arguments,
                                 "DISSOLVE_GROUP <group_name>", group_name)) {
    return;
  }

  std::vector<std::string> members;
  std::string error;

  {
    std::lock_guard<std::mutex> operation_lock(group_operation_mutex_);

    std::optional<GroupRole> role;
    if (!database_.get_group_role(group_name, session.username, role, error)) {
      database_error(connection, "checking group ownership", error);
      return;
    }

    if (!role || *role != GroupRole::Owner) {
      connection->send("[error] only the group owner can dissolve "
                       "this group.\n");
      return;
    }

    if (!database_.list_group_member_usernames(group_name, members, error)) {
      database_error(connection, "loading group members", error);
      return;
    }

    bool removed = false;
    if (!database_.dissolve_group(group_name, session.username, removed,
                                  error)) {
      database_error(connection, "dissolving group", error);
      return;
    }

    if (!removed) {
      connection->send("[error] group no longer exists.\n");
      return;
    }
  }

  connection->send("[system] group " + group_name + " dissolved.\n");

  for (const std::string &member : members) {
    if (member != session.username) {
      notify_user_if_online(member, "[system] group " + group_name +
                                        " was dissolved by owner " +
                                        session.username + ".\n");
    }
  }
}

void ChatServer::handle_apply_group(const TcpConnectionPtr &connection,
                                    const ClientSession &session,
                                    const std::string &arguments) {
  if (!require_login(connection, session, "applying to groups")) {
    return;
  }

  std::string group_name;
  if (!extract_single_group_name(connection, arguments,
                                 "APPLY_GROUP <group_name>", group_name)) {
    return;
  }

  std::string error;

  {
    std::lock_guard<std::mutex> operation_lock(group_operation_mutex_);

    std::optional<GroupInfo> group;
    if (!database_.get_group(group_name, group, error)) {
      database_error(connection, "checking group", error);
      return;
    }

    if (!group) {
      connection->send("[error] group does not exist.\n");
      return;
    }

    std::optional<GroupRole> role;
    if (!database_.get_group_role(group_name, session.username, role, error)) {
      database_error(connection, "checking group membership", error);
      return;
    }

    if (role) {
      connection->send("[error] you are already a member of this group.\n");
      return;
    }

    bool pending = false;
    if (!database_.has_group_join_request(group_name, session.username, pending,
                                          error)) {
      database_error(connection, "checking group join request", error);
      return;
    }

    if (pending) {
      connection->send("[error] group join request is already pending.\n");
      return;
    }

    if (!database_.add_group_join_request(group_name, session.username,
                                          error)) {
      database_error(connection, "saving group join request", error);
      return;
    }
  }

  connection->send("[system] join request sent to group " + group_name + ".\n");

  notify_group_managers(
      group_name,
      "[system] " + session.username + " applied to join group " + group_name +
          ". Use APPROVE_GROUP " + group_name + " " + session.username +
          " or REJECT_GROUP " + group_name + " " + session.username + ".\n",
      session.username);
}

void ChatServer::handle_my_groups(const TcpConnectionPtr &connection,
                                  const ClientSession &session) {
  if (!require_login(connection, session, "viewing groups")) {
    return;
  }

  std::vector<GroupMembership> groups;
  std::string error;

  if (!database_.list_user_groups(session.username, groups, error)) {
    database_error(connection, "loading joined groups", error);
    return;
  }

  std::ostringstream output;
  output << "[system] joined groups (" << groups.size() << "):\n";

  for (const GroupMembership &membership : groups) {
    output << "  " << membership.group.name << " ["
           << group_role_name(membership.role)
           << "] owner=" << membership.group.owner_username << "\n";
  }

  if (groups.empty()) {
    output << "  (none)\n";
  }

  connection->send(output.str());
}

void ChatServer::handle_leave_group(const TcpConnectionPtr &connection,
                                    const ClientSession &session,
                                    const std::string &arguments) {
  if (!require_login(connection, session, "leaving groups")) {
    return;
  }

  std::string group_name;
  if (!extract_single_group_name(connection, arguments,
                                 "LEAVE_GROUP <group_name>", group_name)) {
    return;
  }

  std::string error;

  {
    std::lock_guard<std::mutex> operation_lock(group_operation_mutex_);

    std::optional<GroupRole> role;
    if (!database_.get_group_role(group_name, session.username, role, error)) {
      database_error(connection, "checking group membership", error);
      return;
    }

    if (!role) {
      connection->send("[error] you are not a member of this group.\n");
      return;
    }

    if (*role == GroupRole::Owner) {
      connection->send("[error] the owner cannot leave directly; "
                       "use DISSOLVE_GROUP.\n");
      return;
    }

    bool removed = false;
    if (!database_.remove_group_member(group_name, session.username, removed,
                                       error)) {
      database_error(connection, "leaving group", error);
      return;
    }

    if (!removed) {
      connection->send("[error] membership no longer exists.\n");
      return;
    }
  }

  connection->send("[system] left group " + group_name + ".\n");

  notify_group_managers(group_name,
                        "[system] " + session.username + " left group " +
                            group_name + ".\n",
                        session.username);
}

void ChatServer::handle_group_members(const TcpConnectionPtr &connection,
                                      const ClientSession &session,
                                      const std::string &arguments) {
  if (!require_login(connection, session, "viewing group members")) {
    return;
  }

  std::string group_name;
  if (!extract_single_group_name(connection, arguments,
                                 "GROUP_MEMBERS <group_name>", group_name)) {
    return;
  }

  std::optional<GroupRole> current_role;
  std::string error;

  if (!database_.get_group_role(group_name, session.username, current_role,
                                error)) {
    database_error(connection, "checking group membership", error);
    return;
  }

  if (!current_role) {
    connection->send("[error] only group members can view "
                     "the member list.\n");
    return;
  }

  std::vector<GroupMemberInfo> members;

  if (!database_.list_group_members(group_name, members, error)) {
    database_error(connection, "loading group members", error);
    return;
  }

  std::ostringstream output;
  output << "[group " << group_name << "] members (" << members.size()
         << "):\n";

  for (const GroupMemberInfo &member : members) {
    output << "  " << member.username << " [" << group_role_name(member.role)
           << "] [" << (is_user_online(member.username) ? "online" : "offline")
           << "]\n";
  }

  connection->send(output.str());
}

void ChatServer::handle_add_group_admin(const TcpConnectionPtr &connection,
                                        const ClientSession &session,
                                        const std::string &arguments) {
  if (!require_login(connection, session, "adding group administrators")) {
    return;
  }

  std::string group_name;
  std::string target;

  if (!extract_group_and_username(connection, arguments,
                                  "ADD_GROUP_ADMIN <group_name> <username>",
                                  group_name, target)) {
    return;
  }

  std::string error;

  {
    std::lock_guard<std::mutex> operation_lock(group_operation_mutex_);

    std::optional<GroupRole> current_role;
    std::optional<GroupRole> target_role;

    if (!database_.get_group_role(group_name, session.username, current_role,
                                  error) ||
        !database_.get_group_role(group_name, target, target_role, error)) {
      database_error(connection, "checking group roles", error);
      return;
    }

    if (!current_role || *current_role != GroupRole::Owner) {
      connection->send("[error] only the group owner can add "
                       "administrators.\n");
      return;
    }

    if (!target_role) {
      connection->send("[error] target user is not a group member.\n");
      return;
    }

    if (*target_role == GroupRole::Owner) {
      connection->send("[error] the owner is already above admin role.\n");
      return;
    }

    if (*target_role == GroupRole::Admin) {
      connection->send("[error] target user is already an administrator.\n");
      return;
    }

    bool changed = false;
    if (!database_.set_group_member_role(group_name, target, GroupRole::Admin,
                                         changed, error)) {
      database_error(connection, "adding group administrator", error);
      return;
    }

    if (!changed) {
      connection->send("[error] group role was not changed.\n");
      return;
    }
  }

  connection->send("[system] " + target + " is now an administrator of " +
                   group_name + ".\n");

  notify_user_if_online(target, "[system] you were promoted to administrator "
                                "of group " +
                                    group_name + " by " + session.username +
                                    ".\n");
}

void ChatServer::handle_remove_group_admin(const TcpConnectionPtr &connection,
                                           const ClientSession &session,
                                           const std::string &arguments) {
  if (!require_login(connection, session, "removing group administrators")) {
    return;
  }

  std::string group_name;
  std::string target;

  if (!extract_group_and_username(connection, arguments,
                                  "REMOVE_GROUP_ADMIN <group_name> <username>",
                                  group_name, target)) {
    return;
  }

  std::string error;

  {
    std::lock_guard<std::mutex> operation_lock(group_operation_mutex_);

    std::optional<GroupRole> current_role;
    std::optional<GroupRole> target_role;

    if (!database_.get_group_role(group_name, session.username, current_role,
                                  error) ||
        !database_.get_group_role(group_name, target, target_role, error)) {
      database_error(connection, "checking group roles", error);
      return;
    }

    if (!current_role || *current_role != GroupRole::Owner) {
      connection->send("[error] only the group owner can remove "
                       "administrators.\n");
      return;
    }

    if (!target_role || *target_role != GroupRole::Admin) {
      connection->send("[error] target user is not an administrator.\n");
      return;
    }

    bool changed = false;
    if (!database_.set_group_member_role(group_name, target, GroupRole::Member,
                                         changed, error)) {
      database_error(connection, "removing group administrator", error);
      return;
    }

    if (!changed) {
      connection->send("[error] group role was not changed.\n");
      return;
    }
  }

  connection->send("[system] " + target + " is now a normal member of " +
                   group_name + ".\n");

  notify_user_if_online(target, "[system] your administrator role in group " +
                                    group_name + " was removed by " +
                                    session.username + ".\n");
}

void ChatServer::handle_group_requests(const TcpConnectionPtr &connection,
                                       const ClientSession &session,
                                       const std::string &arguments) {
  if (!require_login(connection, session, "viewing group join requests")) {
    return;
  }

  std::string group_name;
  if (!extract_single_group_name(connection, arguments,
                                 "GROUP_REQUESTS <group_name>", group_name)) {
    return;
  }

  std::optional<GroupRole> role;
  std::string error;

  if (!database_.get_group_role(group_name, session.username, role, error)) {
    database_error(connection, "checking group role", error);
    return;
  }

  if (!role || !is_group_manager(*role)) {
    connection->send("[error] only the group owner/admin can view "
                     "join requests.\n");
    return;
  }

  std::vector<std::string> users;

  if (!database_.list_group_join_requests(group_name, users, error)) {
    database_error(connection, "loading group join requests", error);
    return;
  }

  connection->send("[group " + group_name + "] pending join requests (" +
                   std::to_string(users.size()) + "): " + join_names(users) +
                   "\n");
}

void ChatServer::handle_approve_group(const TcpConnectionPtr &connection,
                                      const ClientSession &session,
                                      const std::string &arguments) {
  if (!require_login(connection, session, "approving group members")) {
    return;
  }

  std::string group_name;
  std::string target;

  if (!extract_group_and_username(connection, arguments,
                                  "APPROVE_GROUP <group_name> <username>",
                                  group_name, target)) {
    return;
  }

  std::string error;

  {
    std::lock_guard<std::mutex> operation_lock(group_operation_mutex_);

    std::optional<GroupRole> current_role;
    if (!database_.get_group_role(group_name, session.username, current_role,
                                  error)) {
      database_error(connection, "checking group role", error);
      return;
    }

    if (!current_role || !is_group_manager(*current_role)) {
      connection->send("[error] only the group owner/admin can approve "
                       "join requests.\n");
      return;
    }

    bool pending = false;
    if (!database_.has_group_join_request(group_name, target, pending, error)) {
      database_error(connection, "checking group join request", error);
      return;
    }

    if (!pending) {
      connection->send("[error] no pending join request from " + target +
                       ".\n");
      return;
    }

    if (!database_.approve_group_join_request(group_name, target, error)) {
      database_error(connection, "approving group join request", error);
      return;
    }
  }

  connection->send("[system] " + target + " joined group " + group_name +
                   ".\n");

  notify_user_if_online(target, "[system] your request to join group " +
                                    group_name + " was approved by " +
                                    session.username + ".\n");
}

void ChatServer::handle_reject_group(const TcpConnectionPtr &connection,
                                     const ClientSession &session,
                                     const std::string &arguments) {
  if (!require_login(connection, session, "rejecting group join requests")) {
    return;
  }

  std::string group_name;
  std::string target;

  if (!extract_group_and_username(connection, arguments,
                                  "REJECT_GROUP <group_name> <username>",
                                  group_name, target)) {
    return;
  }

  std::string error;

  {
    std::lock_guard<std::mutex> operation_lock(group_operation_mutex_);

    std::optional<GroupRole> current_role;
    if (!database_.get_group_role(group_name, session.username, current_role,
                                  error)) {
      database_error(connection, "checking group role", error);
      return;
    }

    if (!current_role || !is_group_manager(*current_role)) {
      connection->send("[error] only the group owner/admin can reject "
                       "join requests.\n");
      return;
    }

    bool removed = false;
    if (!database_.reject_group_join_request(group_name, target, removed,
                                             error)) {
      database_error(connection, "rejecting group join request", error);
      return;
    }

    if (!removed) {
      connection->send("[error] no pending join request from " + target +
                       ".\n");
      return;
    }
  }

  connection->send("[system] rejected " + target + "'s request to join " +
                   group_name + ".\n");

  notify_user_if_online(target, "[system] your request to join group " +
                                    group_name + " was rejected by " +
                                    session.username + ".\n");
}

void ChatServer::handle_remove_group_member(const TcpConnectionPtr &connection,
                                            const ClientSession &session,
                                            const std::string &arguments) {
  if (!require_login(connection, session, "removing group members")) {
    return;
  }

  std::string group_name;
  std::string target;

  if (!extract_group_and_username(connection, arguments,
                                  "REMOVE_GROUP_MEMBER <group_name> <username>",
                                  group_name, target)) {
    return;
  }

  if (target == session.username) {
    connection->send("[error] use LEAVE_GROUP to leave yourself.\n");
    return;
  }

  std::string error;

  {
    std::lock_guard<std::mutex> operation_lock(group_operation_mutex_);

    std::optional<GroupRole> current_role;
    std::optional<GroupRole> target_role;

    if (!database_.get_group_role(group_name, session.username, current_role,
                                  error) ||
        !database_.get_group_role(group_name, target, target_role, error)) {
      database_error(connection, "checking group roles", error);
      return;
    }

    if (!current_role || !is_group_manager(*current_role)) {
      connection->send("[error] only group owner/admin can remove members.\n");
      return;
    }

    if (!target_role) {
      connection->send("[error] target user is not a group member.\n");
      return;
    }

    if (*target_role == GroupRole::Owner) {
      connection->send("[error] the group owner cannot be removed.\n");
      return;
    }

    if (*current_role == GroupRole::Admin && *target_role == GroupRole::Admin) {
      connection->send("[error] an administrator cannot remove "
                       "another administrator.\n");
      return;
    }

    bool removed = false;
    if (!database_.remove_group_member(group_name, target, removed, error)) {
      database_error(connection, "removing group member", error);
      return;
    }

    if (!removed) {
      connection->send("[error] group membership no longer exists.\n");
      return;
    }
  }

  connection->send("[system] removed " + target + " from group " + group_name +
                   ".\n");

  notify_user_if_online(target, "[system] you were removed from group " +
                                    group_name + " by " + session.username +
                                    ".\n");
}

void ChatServer::handle_group_message(const TcpConnectionPtr &connection,
                                      const ClientSession &session,
                                      const std::string &arguments) {
  if (!require_login(connection, session, "sending group messages")) {
    return;
  }

  std::string group_name;
  std::string message;

  if (!split_first_token(arguments, group_name, message) ||
      !is_valid_group_name(group_name) || message.empty() ||
      message.size() > kMaxChatMessage) {
    connection->send("[error] usage: GROUP_MSG <group_name> <message>; "
                     "message must be 1-1000 bytes.\n");
    return;
  }

  std::optional<GroupInfo> group;
  std::optional<GroupRole> role;
  std::vector<std::string> members;
  std::string error;

  if (!database_.get_group(group_name, group, error) ||
      !database_.get_group_role(group_name, session.username, role, error) ||
      !database_.list_group_member_usernames(group_name, members, error)) {
    database_error(connection, "loading group chat state", error);
    return;
  }

  if (!group) {
    connection->send("[error] group does not exist.\n");
    return;
  }

  if (!role) {
    connection->send("[error] only group members can send group messages.\n");
    return;
  }

  std::vector<std::string> recipients;
  recipients.reserve(members.size());

  for (const std::string &member : members) {
    if (member != session.username) {
      recipients.push_back(member);
    }
  }

  GroupMessagePayload payload;
  payload.set_group_id(group->id);
  payload.set_group_name(group_name);
  payload.set_sender_username(session.username);
  payload.set_content(message);
  payload.set_created_at_unix_ms(now_unix_ms());

  std::uint64_t message_id = 0;

  if (!database_.add_group_message(payload, recipients, message_id, error)) {
    database_error(connection, "saving group message", error);
    return;
  }

  for (const std::string &recipient : recipients) {
    adjust_redis_unread_best_effort(recipient, "group", 1);
  }

  const std::string wire_text = "[#G" + std::to_string(message_id) +
                                "] [group " + group_name + "] [" +
                                session.username + "] " + message + "\n";

  connection->send(wire_text);

  for (const std::string &recipient : recipients) {
    TcpConnectionPtr target_connection;

    if (!find_online_user(recipient, target_connection)) {
      continue;
    }

    target_connection->send(wire_text, [this, message_id, recipient] {
      std::string delivery_error;
      if (!database_.mark_group_message_delivered(
              message_id, recipient, now_unix_ms(), delivery_error)) {
        std::cerr << "failed to mark group message #" << message_id
                  << " delivered to " << recipient << ": " << delivery_error
                  << '\n';
      } else {
        adjust_redis_unread_best_effort(recipient, "group", -1);
      }
    });
  }
}

void ChatServer::handle_history_group(const TcpConnectionPtr &connection,
                                      const ClientSession &session,
                                      const std::string &arguments) {
  if (!require_login(connection, session, "viewing group history")) {
    return;
  }

  const std::vector<std::string> words = split_words(arguments);

  if (words.empty() || words.size() > 2U || !is_valid_group_name(words[0])) {
    connection->send("[error] usage: HISTORY_GROUP "
                     "<group_name> [count]\n");
    return;
  }

  std::size_t count = kDefaultHistoryCount;

  if (words.size() == 2U &&
      !parse_count(words[1], 1U, kMaxHistoryCount, count)) {
    connection->send("[error] HISTORY_GROUP count must be 1-100.\n");
    return;
  }

  std::optional<GroupRole> role;
  std::string error;

  if (!database_.get_group_role(words[0], session.username, role, error)) {
    database_error(connection, "checking group membership", error);
    return;
  }

  if (!role) {
    connection->send("[error] only group members can view "
                     "group history.\n");
    return;
  }

  std::vector<StoredGroupMessage> messages;

  if (!database_.recent_group_messages(words[0], count, messages, error)) {
    database_error(connection, "loading group history", error);
    return;
  }

  std::ostringstream output;
  output << "[history group " << words[0] << "] showing " << messages.size()
         << " message(s):\n";

  for (const StoredGroupMessage &message : messages) {
    output << "  #G" << message.id << " "
           << format_unix_ms(message.payload.created_at_unix_ms()) << " ["
           << message.payload.sender_username() << "] "
           << message.payload.content() << "\n";
  }

  connection->send(output.str());
}

void ChatServer::handle_pending(const TcpConnectionPtr &connection,
                                const ClientSession &session) {
  if (!require_login(connection, session, "checking pending notifications")) {
    return;
  }

  notify_pending_requests(connection, session.username);

  send_redis_unread_summary_best_effort(connection, session.username);

  deliver_pending_messages(connection, session.username);

  deliver_pending_files(connection, session.username);
}

void ChatServer::handle_file_begin_private(const TcpConnectionPtr &connection,
                                           ClientSession &session,
                                           const std::string &arguments) {
  if (!require_login(connection, session, "sending files")) {
    return;
  }

  const std::vector<std::string> words = split_words(arguments);

  const std::string token = words.empty() ? std::string("unknown") : words[0];

  if (words.size() != 5U) {
    reject_file_upload(connection, session, token,
                       "invalid FILE_BEGIN_PRIVATE arguments");
    return;
  }

  const std::string &target = words[1];

  std::uint64_t file_size = 0U;
  std::string filename;
  std::string error;

  if (!fileutil::is_valid_transfer_token(token) || !is_valid_username(target) ||
      !parse_uint64_value(words[3], file_size) || file_size > kMaxFileSize ||
      !fileutil::is_valid_sha256_hex(words[4]) ||
      !fileutil::percent_decode(words[2], filename, error) ||
      filename.empty() || filename.size() > 255U) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "invalid private file metadata" : error);
    return;
  }

  if (session.upload) {
    reject_file_upload(connection, session, token,
                       "another upload is already active");
    return;
  }

  if (target == session.username) {
    reject_file_upload(connection, session, token,
                       "cannot send a private file to yourself");
    return;
  }

  {
    std::lock_guard<std::mutex> operation_lock(friend_operation_mutex_);

    const DirectMessageDecision decision =
        direct_message_policy_.evaluate(session.username, target, error);

    if (decision == DirectMessageDecision::DatabaseError) {
      reject_file_upload(connection, session, token,
                         "database error while checking "
                         "direct-file policy");
      return;
    }

    if (decision == DirectMessageDecision::TargetMissing) {
      reject_file_upload(connection, session, token,
                         "target account does not exist");
      return;
    }

    if (decision == DirectMessageDecision::NotFriends) {
      reject_file_upload(connection, session, token,
                         "private files are allowed only between friends");
      return;
    }

    if (decision == DirectMessageDecision::BlockedByRecipient) {
      reject_file_upload(connection, session, token,
                         "recipient privacy settings do not accept "
                         "direct files from you");
      return;
    }
  }

  FileUploadResumeState requested;

  FileTransferMetadata *metadata = requested.mutable_metadata();

  metadata->set_transfer_token(token);
  metadata->set_scope(chatroom::v9::FILE_TRANSFER_PRIVATE);
  metadata->set_sender_username(session.username);
  metadata->set_recipient_username(target);
  metadata->set_file_name(fileutil::sanitize_filename(filename));
  metadata->set_file_size(file_size);
  metadata->set_sha256_hex(words[4]);
  metadata->set_created_at_unix_ms(now_unix_ms());

  requested.add_recipient_usernames(target);

  FileUploadResumeState persisted;
  std::filesystem::path temp_path;
  std::uint64_t accepted_offset = 0U;

  if (!file_transfer_service_.begin_or_resume_upload(
          requested, persisted, temp_path, accepted_offset, error)) {
    reject_file_upload(connection, session, token, error);
    return;
  }

  IncomingFileUpload upload;
  upload.token = token;
  upload.scope = persisted.metadata().scope();
  upload.target = persisted.metadata().recipient_username();
  upload.file_name = persisted.metadata().file_name();
  upload.expected_size = persisted.metadata().file_size();
  upload.received_size = accepted_offset;
  upload.sha256_hex = persisted.metadata().sha256_hex();
  upload.temp_path = std::move(temp_path);
  upload.resume_state = persisted;

  upload.recipients.reserve(
      static_cast<std::size_t>(persisted.recipient_usernames_size()));

  for (const std::string &recipient : persisted.recipient_usernames()) {
    upload.recipients.push_back(recipient);
  }

  session.upload = std::move(upload);

  connection->send("FILE_READY " + token + " " +
                   std::to_string(accepted_offset) + "\n");
}

void ChatServer::handle_file_begin_group(const TcpConnectionPtr &connection,
                                         ClientSession &session,
                                         const std::string &arguments) {
  if (!require_login(connection, session, "sending group files")) {
    return;
  }

  const std::vector<std::string> words = split_words(arguments);

  const std::string token = words.empty() ? std::string("unknown") : words[0];

  if (words.size() != 5U) {
    reject_file_upload(connection, session, token,
                       "invalid FILE_BEGIN_GROUP arguments");
    return;
  }

  const std::string &group_name = words[1];

  std::uint64_t file_size = 0U;
  std::string filename;
  std::string error;

  if (!fileutil::is_valid_transfer_token(token) ||
      !is_valid_group_name(group_name) ||
      !parse_uint64_value(words[3], file_size) || file_size > kMaxFileSize ||
      !fileutil::is_valid_sha256_hex(words[4]) ||
      !fileutil::percent_decode(words[2], filename, error) ||
      filename.empty() || filename.size() > 255U) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "invalid group file metadata" : error);
    return;
  }

  if (session.upload) {
    reject_file_upload(connection, session, token,
                       "another upload is already active");
    return;
  }

  std::optional<GroupInfo> group;
  std::optional<GroupRole> role;

  if (!database_.get_group(group_name, group, error) ||
      !database_.get_group_role(group_name, session.username, role, error)) {
    reject_file_upload(connection, session, token,
                       "database error while checking group membership");
    return;
  }

  if (!group) {
    reject_file_upload(connection, session, token, "group does not exist");
    return;
  }

  if (!role) {
    reject_file_upload(connection, session, token,
                       "only group members can send group files");
    return;
  }

  std::vector<std::string> members;

  if (!database_.list_group_member_usernames(group_name, members, error)) {
    reject_file_upload(connection, session, token,
                       "database error while loading group members");
    return;
  }

  std::vector<std::string> current_recipients;
  current_recipients.reserve(members.size());

  for (const std::string &member : members) {
    if (member != session.username) {
      current_recipients.push_back(member);
    }
  }

  if (current_recipients.empty()) {
    reject_file_upload(connection, session, token,
                       "group has no other members to receive the file");
    return;
  }

  FileUploadResumeState requested;

  FileTransferMetadata *metadata = requested.mutable_metadata();

  metadata->set_transfer_token(token);
  metadata->set_scope(chatroom::v9::FILE_TRANSFER_GROUP);
  metadata->set_sender_username(session.username);
  metadata->set_group_id(group->id);
  metadata->set_group_name(group_name);
  metadata->set_file_name(fileutil::sanitize_filename(filename));
  metadata->set_file_size(file_size);
  metadata->set_sha256_hex(words[4]);
  metadata->set_created_at_unix_ms(now_unix_ms());

  for (const std::string &recipient : current_recipients) {
    requested.add_recipient_usernames(recipient);
  }

  FileUploadResumeState persisted;
  std::filesystem::path temp_path;
  std::uint64_t accepted_offset = 0U;

  if (!file_transfer_service_.begin_or_resume_upload(
          requested, persisted, temp_path, accepted_offset, error)) {
    reject_file_upload(connection, session, token, error);
    return;
  }

  IncomingFileUpload upload;
  upload.token = token;
  upload.scope = persisted.metadata().scope();
  upload.target = persisted.metadata().group_name();
  upload.group_id = persisted.metadata().group_id();
  upload.file_name = persisted.metadata().file_name();
  upload.expected_size = persisted.metadata().file_size();
  upload.received_size = accepted_offset;
  upload.sha256_hex = persisted.metadata().sha256_hex();
  upload.temp_path = std::move(temp_path);
  upload.resume_state = persisted;

  upload.recipients.reserve(
      static_cast<std::size_t>(persisted.recipient_usernames_size()));

  for (const std::string &recipient : persisted.recipient_usernames()) {
    upload.recipients.push_back(recipient);
  }

  session.upload = std::move(upload);

  connection->send("FILE_READY " + token + " " +
                   std::to_string(accepted_offset) + "\n");
}

void ChatServer::handle_file_chunk(const TcpConnectionPtr &connection,
                                   ClientSession &session,
                                   const std::string &arguments) {
  if (!require_login(connection, session, "uploading files")) {
    return;
  }

  const std::vector<std::string> words = split_words(arguments);

  const std::string token = words.empty() ? std::string("unknown") : words[0];

  if (words.size() != 3U || !session.upload || session.upload->token != token ||
      session.binary_upload) {
    pause_file_upload(connection, session, token,
                      "invalid or overlapping binary FILE_CHUNK frame");
    return;
  }

  std::uint64_t offset = 0U;
  std::uint64_t byte_count = 0U;

  if (!parse_uint64_value(words[1], offset) ||
      !parse_uint64_value(words[2], byte_count) || byte_count == 0U ||
      byte_count > kMaxFileFrameBytes ||
      offset != session.upload->received_size ||
      session.upload->received_size + byte_count >
          session.upload->expected_size) {
    pause_file_upload(connection, session, token,
                      "invalid binary file frame offset/length");
    return;
  }

  PendingBinaryUploadFrame frame;
  frame.token = token;
  frame.next_offset = offset;
  frame.remaining_bytes = byte_count;

  session.binary_upload = std::move(frame);
}

void ChatServer::handle_file_end(const TcpConnectionPtr &connection,
                                 ClientSession &session,
                                 const std::string &arguments) {
  if (!require_login(connection, session, "finishing file uploads")) {
    return;
  }

  const std::vector<std::string> words = split_words(arguments);

  const std::string token = words.empty() ? std::string("unknown") : words[0];

  if (words.size() != 1U || !session.upload || session.upload->token != token) {
    pause_file_upload(connection, session, token,
                      "no matching active upload; resend FILE_BEGIN to resume");
    return;
  }

  if (session.upload->received_size != session.upload->expected_size) {
    pause_file_upload(
        connection, session, token,
        "uploaded byte count is incomplete; resume from server offset");
    return;
  }

  IncomingFileUpload upload = std::move(*session.upload);

  session.upload.reset();

  std::string stored_relative_path;
  std::string error;

  if (upload.scope == chatroom::v9::FILE_TRANSFER_PRIVATE) {
    std::lock_guard<std::mutex> operation_lock(friend_operation_mutex_);

    const DirectMessageDecision decision =
        direct_message_policy_.evaluate(session.username, upload.target, error);

    if (decision != DirectMessageDecision::Allowed) {
      file_transfer_service_.cancel_upload(upload.token);

      std::string reason = "private-file permission changed "
                           "during upload";

      if (decision == DirectMessageDecision::BlockedByRecipient) {
        reason = "recipient privacy settings now block "
                 "direct files from you";
      } else if (decision == DirectMessageDecision::NotFriends) {
        reason = "recipient is no longer your friend";
      } else if (decision == DirectMessageDecision::TargetMissing) {
        reason = "recipient account no longer exists";
      } else if (decision == DirectMessageDecision::DatabaseError) {
        reason = "database error while rechecking "
                 "private-file permission";
      }

      connection->send("FILE_REJECT " + token + " " +
                       encode_text_token(reason) + "\n");

      return;
    }
  }

  if (!file_transfer_service_.finalize_upload(
          upload.temp_path, upload.token, upload.file_name,
          upload.expected_size, upload.sha256_hex, stored_relative_path,
          error)) {
    // SHA/size validation at this point means the retained bytes are
    // not a valid prefix of the declared file. Cancel instead of
    // repeatedly resuming corrupted data.
    file_transfer_service_.cancel_upload(upload.token);

    connection->send("FILE_REJECT " + token + " " + encode_text_token(error) +
                     "\n");
    return;
  }

  FileTransferMetadata metadata = upload.resume_state.metadata();

  metadata.set_stored_relative_path(stored_relative_path);

  std::uint64_t transfer_id = 0U;

  if (!database_.add_file_transfer(metadata, upload.recipients, transfer_id,
                                   error)) {
    std::error_code ignored;

    std::filesystem::remove(
        file_transfer_service_.storage_root() / stored_relative_path, ignored);

    database_error(connection, "saving file transfer metadata", error);

    connection->send(
        "FILE_REJECT " + token + " " +
        encode_text_token("database failed to persist file transfer") + "\n");
    return;
  }

  const std::string unread_kind =
      upload.scope == chatroom::v9::FILE_TRANSFER_PRIVATE ? "private_file"
                                                          : "group_file";

  for (const std::string &recipient : upload.recipients) {
    adjust_redis_unread_best_effort(recipient, unread_kind, 1);
  }

  connection->send("FILE_UPLOAD_OK " + token + " " +
                   std::to_string(transfer_id) + "\n");

  connection->send("[file #F" + std::to_string(transfer_id) + "] stored " +
                   upload.file_name + " (" +
                   std::to_string(upload.expected_size) +
                   " bytes). Online recipients receive it now; "
                   "offline recipients receive it after login.\n");

  StoredFileTransfer stored;
  stored.id = transfer_id;
  stored.metadata = std::move(metadata);

  for (const std::string &recipient : upload.recipients) {
    TcpConnectionPtr target_connection;

    if (find_online_user(recipient, target_connection)) {
      deliver_file_to_user(stored, recipient, target_connection);
    }
  }
}

void ChatServer::handle_file_abort(const TcpConnectionPtr &connection,
                                   ClientSession &session,
                                   const std::string &arguments) {
  const std::vector<std::string> words = split_words(arguments);

  if (words.size() != 1U || !fileutil::is_valid_transfer_token(words[0])) {
    return;
  }

  const std::string token = words[0];

  // A token by itself is not authorization to delete a server-side
  // checkpoint. Only the connection that currently owns that validated
  // upload session may explicitly cancel it.
  if (!session.upload || session.upload->token != token) {
    connection->send("[error] no matching active upload to cancel.\n");
    return;
  }

  session.upload.reset();

  file_transfer_service_.cancel_upload(token);

  connection->send("FILE_REJECT " + token + " " +
                   encode_text_token("client cancelled upload") + "\n");
}

void ChatServer::handle_file_resume_request(const TcpConnectionPtr &connection,
                                            ClientSession &session,
                                            const std::string &arguments) {
  if (!require_login(connection, session, "resuming file downloads")) {
    return;
  }

  const std::vector<std::string> words = split_words(arguments);

  std::uint64_t transfer_id = 0U;
  std::uint64_t start_offset = 0U;

  if (words.size() != 2U || !parse_uint64_value(words[0], transfer_id) ||
      !parse_uint64_value(words[1], start_offset)) {
    connection->send("[error] invalid FILE_RESUME_REQUEST.\n");
    return;
  }

  const auto offered = session.offered_files.find(transfer_id);

  if (offered == session.offered_files.end()) {
    connection->send("[error] file #F" + std::to_string(transfer_id) +
                     " is not currently offered; use PENDING.\n");
    return;
  }

  if (start_offset > offered->second.metadata.file_size()) {
    connection->send(
        "[error] resume offset exceeds file size; "
        "client should discard the partial file and retry from 0.\n");
    return;
  }

  if (!session.file_deliveries_in_progress.insert(transfer_id).second) {
    return;
  }

  const StoredFileTransfer transfer = offered->second;

  file_transfer_service_.deliver_async(
      transfer.id, transfer.metadata, start_offset, connection,
      [this, transfer_id, recipient = session.username,
       weak_connection = std::weak_ptr<minimuduo::net::TcpConnection>(
           connection)](bool success, const std::string &delivery_error) {
        if (success) {
          // The transfer remains "in progress" until the receiver
          // validates the complete local file and sends FILE_RECEIVED.
          return;
        }

        const TcpConnectionPtr current = weak_connection.lock();

        if (current != nullptr) {
          current->getLoop()->queueInLoop([this, current, transfer_id] {
            const std::shared_ptr<ClientSession> current_session =
                session_of(current);

            if (current_session != nullptr) {
              current_session->file_deliveries_in_progress.erase(transfer_id);
            }
          });
        }

        std::cerr << "resumed file transfer #" << transfer_id << " to "
                  << recipient << " failed: " << delivery_error << '\n';
      });
}

void ChatServer::handle_file_received(const TcpConnectionPtr &connection,
                                      ClientSession &session,
                                      const std::string &arguments) {
  if (!require_login(connection, session, "acknowledging files")) {
    return;
  }

  const std::vector<std::string> words = split_words(arguments);

  std::uint64_t transfer_id = 0U;

  if (words.size() != 2U || !parse_uint64_value(words[0], transfer_id) ||
      !fileutil::is_valid_sha256_hex(words[1])) {
    connection->send("[error] invalid FILE_RECEIVED acknowledgement.\n");
    return;
  }

  std::optional<StoredFileTransfer> transfer;

  std::string error;

  if (!database_.file_transfer_for_recipient(transfer_id, session.username,
                                             transfer, error)) {
    database_error(connection, "loading file acknowledgement", error);
    return;
  }

  if (!transfer) {
    session.file_deliveries_in_progress.erase(transfer_id);

    session.offered_files.erase(transfer_id);

    return;
  }

  std::string received_sha = words[1];

  std::transform(received_sha.begin(), received_sha.end(), received_sha.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });

  if (received_sha != transfer->metadata.sha256_hex()) {
    session.file_deliveries_in_progress.erase(transfer_id);

    connection->send("[error] file SHA-256 acknowledgement mismatch; "
                     "the file remains pending for retry.\n");
    return;
  }

  if (!database_.mark_file_transfer_delivered(transfer_id, session.username,
                                              now_unix_ms(), error)) {
    database_error(connection, "marking file delivered", error);
    return;
  }

  session.file_deliveries_in_progress.erase(transfer_id);

  session.offered_files.erase(transfer_id);

  const std::string unread_kind =
      transfer->metadata.scope() == chatroom::v9::FILE_TRANSFER_PRIVATE
          ? "private_file"
          : "group_file";

  adjust_redis_unread_best_effort(session.username, unread_kind, -1);

  connection->send("FILE_ACK_OK " + std::to_string(transfer_id) + "\n");
}

void ChatServer::handle_file_receive_failed(const TcpConnectionPtr &connection,
                                            ClientSession &session,
                                            const std::string &arguments) {
  const std::vector<std::string> words = split_words(arguments);

  std::uint64_t transfer_id = 0U;

  if (words.size() != 1U || !parse_uint64_value(words[0], transfer_id)) {
    return;
  }

  session.file_deliveries_in_progress.erase(transfer_id);

  connection->send("[system] file #F" + std::to_string(transfer_id) +
                   " remains pending; use PENDING to retry/resume.\n");
}

void ChatServer::deliver_pending_files(const TcpConnectionPtr &connection,
                                       const std::string &username) {
  std::vector<StoredFileTransfer> transfers;

  std::string error;

  if (!database_.pending_file_transfers(username, kOfflineFileDeliveryBatch,
                                        transfers, error)) {
    std::cerr << "failed to load pending files for " << username << ": "
              << error << '\n';
    return;
  }

  if (!transfers.empty()) {
    connection->send("[system] offering " + std::to_string(transfers.size()) +
                     " pending file(s); local partial files "
                     "will resume from their saved offsets.\n");
  }

  for (const StoredFileTransfer &transfer : transfers) {
    deliver_file_to_user(transfer, username, connection);
  }

  if (transfers.size() == kOfflineFileDeliveryBatch) {
    connection->send("[system] more pending files may remain; "
                     "use PENDING again after current downloads finish.\n");
  }
}

void ChatServer::deliver_file_to_user(const StoredFileTransfer &transfer,
                                      const std::string &recipient,
                                      const TcpConnectionPtr &connection) {
  if (!connection->getLoop()->isInLoopThread()) {
    connection->getLoop()->queueInLoop([this, transfer, recipient, connection] {
      deliver_file_to_user(transfer, recipient, connection);
    });
    return;
  }

  const std::shared_ptr<ClientSession> session = session_of(connection);

  if (session == nullptr || !session->logged_in ||
      session->username != recipient) {
    return;
  }

  if (transfer.metadata.scope() == chatroom::v9::FILE_TRANSFER_PRIVATE) {
    bool blocked = false;
    std::string block_error;

    if (!database_.is_friend_blocked(recipient,
                                     transfer.metadata.sender_username(),
                                     blocked, block_error)) {
      std::cerr << "failed to check file block policy for #F" << transfer.id
                << ": " << block_error << '\n';

      return;
    }

    if (blocked) {
      return;
    }
  }

  if (session->file_deliveries_in_progress.count(transfer.id) != 0U) {
    return;
  }

  session->offered_files[transfer.id] = transfer;

  // The server sends only metadata first. The receiver inspects its
  // local .part file and answers FILE_RESUME_REQUEST <id> <offset>.
  connection->send(
      file_transfer_service_.make_offer_line(transfer.id, transfer.metadata));
}

void ChatServer::detach_active_upload(ClientSession &session) {
  // Intentionally keep server tmp/<token>.part and .resume.pb.
  // They are the durable upload checkpoint used after reconnect/restart.
  session.upload.reset();
  session.binary_upload.reset();
}

void ChatServer::pause_file_upload(const TcpConnectionPtr &connection,
                                   ClientSession &session,
                                   const std::string &token,
                                   const std::string &reason) {
  detach_active_upload(session);

  connection->send("FILE_PAUSED " + token + " " + encode_text_token(reason) +
                   "\n");
}

void ChatServer::reject_file_upload(const TcpConnectionPtr &connection,
                                    ClientSession &session,
                                    const std::string &token,
                                    const std::string &reason) {
  detach_active_upload(session);

  connection->send("FILE_REJECT " + token + " " + encode_text_token(reason) +
                   "\n");
}

std::shared_ptr<ClientSession>
ChatServer::session_of(const TcpConnectionPtr &connection) const {
  const auto *stored =
      std::any_cast<std::shared_ptr<ClientSession>>(&connection->getContext());

  if (stored == nullptr) {
    return nullptr;
  }

  return *stored;
}

bool ChatServer::require_login(const TcpConnectionPtr &connection,
                               const ClientSession &session,
                               const std::string &action) const {
  if (session.logged_in) {
    return true;
  }

  connection->send("[error] you must LOGIN before " + action + ".\n");

  return false;
}

bool ChatServer::extract_single_username(const TcpConnectionPtr &connection,
                                         const std::string &arguments,
                                         const std::string &usage,
                                         std::string &username) const {
  const std::vector<std::string> words = split_words(arguments);

  if (words.size() != 1U || !is_valid_username(words[0])) {
    connection->send("[error] usage: " + usage + "\n");
    return false;
  }

  username = words[0];
  return true;
}

bool ChatServer::extract_single_group_name(const TcpConnectionPtr &connection,
                                           const std::string &arguments,
                                           const std::string &usage,
                                           std::string &group_name) const {
  const std::vector<std::string> words = split_words(arguments);

  if (words.size() != 1U || !is_valid_group_name(words[0])) {
    connection->send("[error] usage: " + usage +
                     "; group_name=2-32 letters/digits/_/-.\n");
    return false;
  }

  group_name = words[0];
  return true;
}

bool ChatServer::extract_group_and_username(const TcpConnectionPtr &connection,
                                            const std::string &arguments,
                                            const std::string &usage,
                                            std::string &group_name,
                                            std::string &username) const {
  const std::vector<std::string> words = split_words(arguments);

  if (words.size() != 2U || !is_valid_group_name(words[0]) ||
      !is_valid_username(words[1])) {
    connection->send("[error] usage: " + usage + "\n");
    return false;
  }

  group_name = words[0];
  username = words[1];
  return true;
}

bool ChatServer::find_online_user(const std::string &username,
                                  TcpConnectionPtr &connection) {
  return online_users_.find(username, connection);
}

bool ChatServer::is_user_online(const std::string &username) {
  return online_users_.is_online(username);
}

bool ChatServer::register_online_user(const std::string &username,
                                      const TcpConnectionPtr &connection) {
  return online_users_.add(username, connection);
}

void ChatServer::remove_online_user(const std::string &username,
                                    const TcpConnectionPtr &connection) {
  online_users_.remove(username, connection);
}

void ChatServer::notify_user_if_online(const std::string &username,
                                       const std::string &message) {
  TcpConnectionPtr connection;

  if (online_users_.find(username, connection)) {
    connection->send(message);
  }
}

void ChatServer::broadcast_to_logged_in(const std::string &message,
                                        const TcpConnectionPtr &except) {
  const std::vector<TcpConnectionPtr> recipients =
      online_users_.connections(except);

  for (const TcpConnectionPtr &recipient : recipients) {
    recipient->send(message);
  }
}

void ChatServer::notify_pending_requests(const TcpConnectionPtr &connection,
                                         const std::string &username) {
  std::vector<std::string> pending_friends;
  std::string error;

  if (database_.list_incoming_requests(username, pending_friends, error)) {
    if (!pending_friends.empty()) {
      connection->send("[system] you have " +
                       std::to_string(pending_friends.size()) +
                       " pending friend request(s). "
                       "Use FRIEND_REQUESTS.\n");
    }
  } else {
    std::cerr << "failed to load pending friend requests for " << username
              << ": " << error << '\n';
  }

  std::vector<ManagedGroupRequestCount> group_requests;

  if (database_.list_managed_group_request_counts(username, group_requests,
                                                  error)) {
    for (const ManagedGroupRequestCount &request : group_requests) {
      connection->send("[system] group " + request.group_name + " has " +
                       std::to_string(request.pending_count) +
                       " pending join request(s). "
                       "Use GROUP_REQUESTS " +
                       request.group_name + ".\n");
    }
  } else {
    std::cerr << "failed to load managed group requests for " << username
              << ": " << error << '\n';
  }
}

void ChatServer::deliver_pending_messages(const TcpConnectionPtr &connection,
                                          const std::string &username) {
  std::string error;
  std::vector<StoredMessage> private_messages;

  if (!database_.pending_private_messages(username, kOfflineDeliveryBatch,
                                          private_messages, error)) {
    std::cerr << "failed to load offline private messages for " << username
              << ": " << error << '\n';
  } else {
    if (!private_messages.empty()) {
      connection->send("[system] delivering " +
                       std::to_string(private_messages.size()) +
                       " offline private message(s).\n");
    }

    for (const StoredMessage &message : private_messages) {
      bool blocked = false;
      std::string block_error;

      if (!database_.is_friend_blocked(username,
                                       message.payload.sender_username(),
                                       blocked, block_error)) {
        std::cerr << "failed to recheck offline private block for #"
                  << message.id << ": " << block_error << '\n';

        continue;
      }

      if (blocked) {
        continue;
      }

      connection->send(
          "[offline #" + std::to_string(message.id) + "] [private from " +
              message.payload.sender_username() + "] " +
              message.payload.content() + "\n",
          [this, message_id = message.id, username] {
            std::string mark_error;
            if (!database_.mark_private_message_delivered(
                    message_id, username, now_unix_ms(), mark_error)) {
              std::cerr << "failed to mark offline private #" << message_id
                        << " delivered: " << mark_error << '\n';
            } else {
              adjust_redis_unread_best_effort(username, "private", -1);
            }
          });
    }

    if (private_messages.size() == kOfflineDeliveryBatch) {
      connection->send("[system] more offline private messages may remain; "
                       "use PENDING again.\n");
    }
  }

  std::vector<StoredGroupMessage> group_messages;

  if (!database_.pending_group_messages(username, kOfflineDeliveryBatch,
                                        group_messages, error)) {
    std::cerr << "failed to load offline group messages for " << username
              << ": " << error << '\n';
    return;
  }

  if (!group_messages.empty()) {
    connection->send("[system] delivering " +
                     std::to_string(group_messages.size()) +
                     " offline group message(s).\n");
  }

  for (const StoredGroupMessage &message : group_messages) {
    connection->send(
        "[offline #G" + std::to_string(message.id) + "] [group " +
            message.payload.group_name() + "] [" +
            message.payload.sender_username() + "] " +
            message.payload.content() + "\n",
        [this, message_id = message.id, username] {
          std::string mark_error;
          if (!database_.mark_group_message_delivered(
                  message_id, username, now_unix_ms(), mark_error)) {
            std::cerr << "failed to mark offline group #" << message_id
                      << " delivered: " << mark_error << '\n';
          } else {
            adjust_redis_unread_best_effort(username, "group", -1);
          }
        });
  }

  if (group_messages.size() == kOfflineDeliveryBatch) {
    connection->send("[system] more offline group messages may remain; "
                     "use PENDING again.\n");
  }
}

void ChatServer::notify_group_managers(const std::string &group_name,
                                       const std::string &message,
                                       const std::string &except_username) {
  std::vector<std::string> managers;
  std::string error;

  if (!database_.list_group_managers(group_name, managers, error)) {
    std::cerr << "failed to load group managers for " << group_name << ": "
              << error << '\n';
    return;
  }

  for (const std::string &manager : managers) {
    if (manager != except_username) {
      notify_user_if_online(manager, message);
    }
  }
}

bool ChatServer::is_valid_username(const std::string &username) {
  if (username.size() < 3U || username.size() > 20U) {
    return false;
  }

  for (unsigned char character : username) {
    if (std::isalnum(character) == 0 && character != '_') {
      return false;
    }
  }

  return true;
}

bool ChatServer::is_valid_password(const std::string &password) {
  if (password.size() < 4U || password.size() > 64U) {
    return false;
  }

  for (unsigned char character : password) {
    if (std::isspace(character) != 0 || std::iscntrl(character) != 0) {
      return false;
    }
  }

  return true;
}

bool ChatServer::is_valid_group_name(const std::string &group_name) {
  if (group_name.size() < 2U || group_name.size() > 32U) {
    return false;
  }

  for (unsigned char character : group_name) {
    if (std::isalnum(character) == 0 && character != '_' && character != '-') {
      return false;
    }
  }

  return true;
}

std::int64_t ChatServer::now_unix_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string ChatServer::format_unix_ms(std::int64_t value) {
  const std::time_t seconds = static_cast<std::time_t>(value / 1000);

  std::tm time_parts{};
  (void)localtime_r(&seconds, &time_parts);

  char buffer[32]{};
  (void)std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &time_parts);

  return buffer;
}

std::string ChatServer::join_names(const std::vector<std::string> &names) {
  if (names.empty()) {
    return "(none)";
  }

  std::ostringstream output;

  for (std::size_t index = 0U; index < names.size(); ++index) {
    if (index > 0U) {
      output << ", ";
    }

    output << names[index];
  }

  return output.str();
}

std::string ChatServer::group_role_name(GroupRole role) {
  switch (role) {
  case GroupRole::Owner:
    return "owner";
  case GroupRole::Admin:
    return "admin";
  case GroupRole::Member:
    return "member";
  }

  return "unknown";
}

bool ChatServer::is_group_manager(GroupRole role) {
  return role == GroupRole::Owner || role == GroupRole::Admin;
}

void ChatServer::presence_refresh_loop() {
  const unsigned int refresh_seconds = std::max(5U, presence_ttl_seconds_ / 3U);

  std::unique_lock<std::mutex> wait_lock(presence_wait_mutex_);

  while (!stopping_.load()) {
    const bool stopping = presence_wait_cv_.wait_for(
        wait_lock, std::chrono::seconds(refresh_seconds),
        [this] { return stopping_.load(); });

    if (stopping) {
      break;
    }

    wait_lock.unlock();
    refresh_all_presence_best_effort();
    wait_lock.lock();
  }
}

void ChatServer::refresh_all_presence_best_effort() {
  const std::vector<std::string> usernames = online_users_.usernames();

  for (const std::string &username : usernames) {
    bool refreshed = false;
    std::string error;

    if (!redis_.refresh_presence_if_owned(username, server_instance_id_,
                                          presence_ttl_seconds_, refreshed,
                                          error)) {
      std::cerr << "Redis presence refresh failed for " << username << ": "
                << error << '\n';
      continue;
    }

    if (refreshed) {
      continue;
    }

    bool claimed = false;
    if (!redis_.claim_presence(username, server_instance_id_,
                               presence_ttl_seconds_, claimed, error)) {
      std::cerr << "Redis presence reclaim failed for " << username << ": "
                << error << '\n';
    } else if (!claimed) {
      std::cerr << "Redis presence ownership for " << username
                << " belongs to another server_name. "
                << "Check config/redis.conf server_name values.\n";
    }
  }
}

bool ChatServer::claim_redis_presence(const std::string &username,
                                      const TcpConnectionPtr &connection) {
  bool claimed = false;
  std::string error;

  if (!redis_.claim_presence(username, server_instance_id_,
                             presence_ttl_seconds_, claimed, error)) {
    std::cerr << "Redis presence claim failed for " << username << ": " << error
              << '\n';

    connection->send("[error] Redis presence service is unavailable; "
                     "login cannot complete.\n");
    return false;
  }

  if (claimed) {
    return true;
  }

  std::optional<std::string> owner;
  if (!redis_.presence_owner(username, owner, error)) {
    std::cerr << "Redis presence lookup failed for " << username << ": "
              << error << '\n';

    connection->send("[error] Redis presence service is unavailable; "
                     "login cannot complete.\n");
    return false;
  }

  if (owner && *owner == server_instance_id_) {
    bool refreshed = false;

    if (!redis_.refresh_presence_if_owned(username, server_instance_id_,
                                          presence_ttl_seconds_, refreshed,
                                          error) ||
        !refreshed) {
      std::cerr << "Redis presence refresh failed for " << username << ": "
                << error << '\n';

      connection->send("[error] Redis presence refresh failed; "
                       "login cannot complete.\n");
      return false;
    }

    return true;
  }

  connection->send("[error] this account is already online "
                   "on another server instance.\n");
  return false;
}

void ChatServer::remove_redis_presence_best_effort(
    const std::string &username) {
  bool removed = false;
  std::string error;

  if (!redis_.remove_presence_if_owned(username, server_instance_id_, removed,
                                       error)) {
    std::cerr << "Redis presence cleanup failed for " << username << ": "
              << error << '\n';
  }
}

void ChatServer::adjust_redis_unread_best_effort(const std::string &username,
                                                 const std::string &kind,
                                                 std::int64_t delta) {
  std::int64_t result = 0;
  std::string error;

  if (!redis_.adjust_unread(username, kind, delta, result, error)) {
    std::cerr << "Redis unread update failed for " << username
              << " kind=" << kind << ": " << error << '\n';
  }
}

void ChatServer::send_redis_unread_summary_best_effort(
    const TcpConnectionPtr &connection, const std::string &username) {
  RedisUnreadCounts counts;
  std::string error;

  if (!redis_.unread_counts(username, counts, error)) {
    std::cerr << "Redis unread lookup failed for " << username << ": " << error
              << '\n';
    return;
  }

  if (counts.private_messages == 0 && counts.group_messages == 0 &&
      counts.private_files == 0 && counts.group_files == 0) {
    return;
  }

  connection->send(
      "[system] Redis unread cache: private_messages=" +
      std::to_string(counts.private_messages) +
      ", group_messages=" + std::to_string(counts.group_messages) +
      ", private_files=" + std::to_string(counts.private_files) +
      ", group_files=" + std::to_string(counts.group_files) +
      ". Durable delivery/filtering still comes from MySQL; "
      "this Redis cache can temporarily include direct items held by "
      "BLOCK_FRIEND.\n");
}

void ChatServer::database_error(const TcpConnectionPtr &connection,
                                const std::string &operation,
                                const std::string &error) const {
  spdlog::error("database operation failed: {}: {}", operation, error);

  std::cerr << "database error while " << operation << ": " << error << '\n';

  connection->send("[error] database operation failed while " + operation +
                   ".\n");
}
