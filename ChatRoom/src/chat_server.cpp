#include "chat_server.hpp"

#include "password.hpp"
#include "protocol.hpp"
#include "file_utils.hpp"

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
//将字符串解析为 uint64_t 无符号整数，供文件大小、偏移量等解析使用
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
//对字符串进行百分号编码（URL Encoding），用于在协议报文中安全传输错误原因或文件名
std::string encode_text_token(const std::string &text) {
  return fileutil::percent_encode(text);
}

} // namespace
//初始化成员变量（数据库、Redis、文件存储根目录）
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
  delivery_persist_thread_ = std::thread([this] { delivery_persist_loop(); });
}
//停止文件传输服务，设置停止标志，唤醒后台线程并等待其结束
ChatServer::~ChatServer() {
  file_transfer_service_.stop();

  stopping_.store(true);
  presence_wait_cv_.notify_all();
  delivery_persist_cv_.notify_all();

  if (presence_refresh_thread_.joinable()) {
    presence_refresh_thread_.join();
  }
  if (delivery_persist_thread_.joinable()) {
    delivery_persist_thread_.join();
  }

  const std::vector<std::string> usernames = online_users_.usernames();

  for (const std::string &username : usernames) {
    remove_redis_presence_best_effort(username);
  }
}
//连接事件处理
void ChatServer::on_connection(const TcpConnectionPtr &connection) {
  if (connection->connected()) {
    connection->setContext(std::make_shared<ClientSession>());

    std::cout << "客户端已连接：" << connection->name() << "，来源："
              << connection->peerAddressText() << '\n';

    spdlog::info("客户端已连接：{}，来源：{}", connection->name(),
                 connection->peerAddressText());

    connection->send("[system] 已连接到聊天室服务（TLS、客户端心跳、好友屏蔽、文件断点续传已启用）。\n"
                     "[system] 可输入数字命令 38 查看帮助。\n");
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

    broadcast_to_logged_in("[system] " + username + " 已离线。\n",
                           connection);
  }

  std::cout << "客户端已断开：" << connection->name() << '\n';

  spdlog::info("客户端已断开：{}", connection->name());
}
//消息事件处理
void ChatServer::on_message(const TcpConnectionPtr &connection,
                            minimuduo::net::Buffer *buffer) {
  const std::shared_ptr<ClientSession> session = session_of(connection);

  if (session == nullptr) {
    connection->send("[error] 会话状态不可用。\n");
    connection->forceClose();
    return;
  }

  std::size_t processed_text_commands = 0U;
  static constexpr std::size_t kRealtimeCommandBatch = 32U;

  while (connection->connected()) {
    if (session->binary_upload &&
        session->binary_upload->remaining_bytes > 0U) {
      if (!session->upload ||
          session->upload->token != session->binary_upload->token) {
        session->binary_upload.reset();
        connection->send("[error] 二进制上传状态无效：上传任务已失效或传输令牌不匹配。\n");
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

      // 先把网络层的小块聚合进当前文件帧。旧实现每收到一次 Buffer
      // 都会进入 FileTransferService 做 stat/open/append，大文件时系统调用极多。
      const char *begin = buffer->peek();
      frame.bytes.insert(frame.bytes.end(), begin, begin + consume);
      buffer->retrieve(consume);

      frame.next_offset += static_cast<std::uint64_t>(consume);
      frame.remaining_bytes -= static_cast<std::uint64_t>(consume);

      if (frame.remaining_bytes == 0U) {
        std::uint64_t accepted_offset = 0U;
        std::string error;
        const char *bytes = frame.bytes.empty() ? nullptr : frame.bytes.data();

        if (!file_transfer_service_.append_upload_bytes(
                session->upload->temp_path, frame.start_offset, bytes,
                frame.bytes.size(), accepted_offset, error)) {
          const std::string token = session->upload->token;
          pause_file_upload(connection, *session, token, error);
          return;
        }

        session->upload->received_size = accepted_offset;
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
      ++processed_text_commands;
    }

    // 一次 TLS read 可能带来 100 条甚至更多命令。旧实现会在同一个
    // Reactor 回调里把所有数据库操作全部做完，期间同线程的心跳/其他连接得不到调度。
    // 每处理 32 条主动让出事件循环；剩余已在 inputBuffer 的命令下一轮继续。
    if (processed_text_commands >= kRealtimeCommandBatch &&
        !session->binary_upload && buffer->findEOL() != nullptr) {
      connection->getLoop()->queueInLoop(
          [this, connection, buffer] { on_message(connection, buffer); });
      return;
    }
  }

  if (!session->binary_upload && buffer->readableBytes() > kMaxInputBuffer) {
    connection->send("[error] 输入行过长；"
                     "connection will close.\n");
    connection->shutdown();
  }
}
//命令分发器
void ChatServer::handle_command(const TcpConnectionPtr &connection,
                                ClientSession &session,
                                const Command &command) {
  if (!command_router_.dispatch(connection, session, command)) {
    connection->send("[error] 未知命令，请输入当前状态支持的数字命令。 \n"
                     "Type HELP to see available commands.\n");
  }
}

void ChatServer::send_help(const TcpConnectionPtr &connection) {
  connection->send(
      "[system] 数字命令帮助：\n"
      "  1  注册账户\n"
      "  2  登录账户\n"
      "  3  退出登录\n"
      "  4  注销账户\n"
      "  5  发送公共消息\n"
      "  6  当前好友/群会话发送消息（客户端会继续让你选择输入模式）\n"
      "  8  进入好友私聊\n"
      "  9  进入群聊\n"
      " 10  退出当前会话\n"
      " 11  查看私聊历史\n"
      " 12  查看群聊历史\n"
      " 13  查看当前会话文件记录\n"
      " 14  向当前会话发送文件\n"
      " 15  好友列表\n"
      " 16  好友申请\n"
      " 17  添加好友\n"
      " 18  通过好友申请\n"
      " 19  拒绝好友申请\n"
      " 20  删除好友\n"
      " 21  屏蔽好友\n"
      " 22  解除屏蔽\n"
      " 23  屏蔽列表\n"
      " 24  创建群\n"
      " 25  解散群\n"
      " 26  申请加入群\n"
      " 27  我的群\n"
      " 28  退出群\n"
      " 29  查看群成员\n"
      " 30  设置群管理员\n"
      " 31  取消群管理员\n"
      " 33  自动列出全部可处理申请并通过\n"
      " 34  自动列出全部可处理申请并拒绝\n"
      " 35  移出群成员\n"
      " 36  在线用户\n"
      " 37  拉取待处理离线消息/文件\n"
      " 38  查看本帮助\n"
      " 39  查看客户端完整数字命令帮助\n"
      " 40  查看本地数据库/下载路径\n"
      " 41  查看公共消息历史\n"
      "[system] 注意：进入好友或群会话后不会自动弹出消息模式；"
      "必须先输入 6，再选择 1=回车立即发送、2=长文本编辑模式。\n"
      "[system] 数字 32 已取消用户入口，33/34 会自动查询你作为群主或管理员可处理的全部入群申请。\n");
}

void ChatServer::handle_register(const TcpConnectionPtr &connection,
                                 ClientSession &session,
                                 const std::string &arguments) {
  if (session.logged_in) {
    connection->send("[error] 当前连接已经登录，请先退出登录后再注册其他账户。\n");
    return;
  }

  const std::vector<std::string> words = split_words(arguments);

  if (words.size() != 2U || !is_valid_username(words[0]) ||
      !is_valid_password(words[1])) {
    connection->send("[error] 用法：REGISTER <用户名> <密码>；"
                     "用户名长度 3-20，只能包含字母、数字、下划线；"
                     "密码长度 4-64，不能包含空格。\n");
    return;
  }

  bool exists = false;
  std::string error;

  if (!database_.user_exists(words[0], exists, error)) {
    database_error(connection, "checking username", error);
    return;
  }

  if (exists) {
    connection->send("[error] 用户名已存在。\n");
    return;
  }

  std::string encoded;
  try {
    encoded = hash_password_pbkdf2(words[1]);
  } catch (const std::exception &exception) {
    connection->send("[error] 密码哈希处理失败。\n");
    std::cerr << exception.what() << '\n';
    return;
  }

  if (!database_.create_user(words[0], encoded, error)) {
    database_error(connection, "creating account", error);
    return;
  }

  connection->send("[system] 注册成功。请使用数字命令 2 登录。\n");
}

void ChatServer::handle_login(const TcpConnectionPtr &connection,
                              ClientSession &session,
                              const std::string &arguments) {
  if (session.logged_in) {
    connection->send("[error] 当前连接已经登录。\n");
    return;
  }

  const std::vector<std::string> words = split_words(arguments);

  if (words.size() != 2U) {
    connection->send("[error] 用法：LOGIN <用户名> <密码>\n");
    return;
  }

  std::optional<std::string> password_hash;
  std::string error;

  if (!database_.get_password_hash(words[0], password_hash, error)) {
    database_error(connection, "loading account", error);
    return;
  }

  if (!password_hash || !verify_password_pbkdf2(words[1], *password_hash)) {
    connection->send("[error] 用户名或密码错误。\n");
    return;
  }

  if (!register_online_user(words[0], connection)) {
    connection->send("[error] 该账号已经登录。\n");
    return;
  }

  if (!claim_redis_presence(words[0], connection)) {
    remove_online_user(words[0], connection);
    return;
  }

  session.logged_in = true;
  session.username = words[0];

  connection->send("[system] 登录成功，欢迎 " + session.username +
                   ".\n");

  broadcast_to_logged_in("[system] " + session.username + " 已上线。\n",
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
  session.realtime_private_target.clear();
  session.realtime_private_generation = 0U;
  session.realtime_group_name.clear();
  session.realtime_group_id = 0U;
  session.realtime_group_recipients.clear();
  session.realtime_group_generation = 0U;

  connection->send("[system] 退出登录成功。\n");

  broadcast_to_logged_in("[system] " + username + " 已离线。\n", connection);
}

void ChatServer::handle_delete_account(const TcpConnectionPtr &connection,
                                       ClientSession &session,
                                       const std::string &arguments) {

  if (!require_login(connection, session, "deleting the account")) {
    return;
  }

  const std::vector<std::string> words = split_words(arguments);

  if (words.size() != 2U || words[1] != "CONFIRM") {

    connection->send("[error] 用法："
                     "DELETE_ACCOUNT <current_password> CONFIRM\n");

    return;
  }

  if (session.upload || session.binary_upload ||
      !session.file_deliveries_in_progress.empty()) {

    connection->send("[error] 请先完成或取消当前文件传输，再执行账户操作。"
                     "before deleting the account.\n");

    return;
  }

  std::optional<std::string> password_hash;
  std::string error;

  if (!database_.get_password_hash(session.username, password_hash, error)) {

    database_error(connection, "verifying account deletion", error);

    return;
  }


  if (!password_hash || !verify_password_pbkdf2(words[0], *password_hash)) {

    connection->send("[error] 当前密码错误。\n");

    return;
  }

  const std::string username = session.username;

  bool removed = false;

  {
    std::scoped_lock operation_lock(friend_operation_mutex_,
                                    group_operation_mutex_);

    if (!database_.delete_user(username, removed, error)) {

      database_error(connection, "deleting account", error);

      return;
    }
  }

  if (!removed) {
    connection->send("[error] 账户已不存在。\n");

    return;
  }

  direct_policy_generation_.fetch_add(1U, std::memory_order_relaxed);
  group_policy_generation_.fetch_add(1U, std::memory_order_relaxed);

  //MYSQL永久删除
  // 7. 从本服务器在线用户表移除
  remove_online_user(username, connection);

  // 8. 删除 Redis presence
  remove_redis_presence_best_effort(username);

  // 9. 删除 Redis 未读缓存
  std::string redis_error;

  if (!redis_.clear_unread(username, redis_error)) {

    // Redis 是缓存。
    // 即使 Redis 清理失败，也不能把已经从 MySQL
    // 删除的账号“恢复”。
    spdlog::warn("清理 Redis 未读缓存失败："
                 "for deleted account {}: {}",
                 username, redis_error);
  }

  // 10. 清理服务器会话
  session.logged_in = false;
  session.username.clear();

  session.offered_files.clear();
  session.file_deliveries_in_progress.clear();

  // 11. 通知当前客户端
  connection->send("[system] 账户注销成功。"
                   "You are now logged out.\n");

  // 12. 通知其他在线用户
  broadcast_to_logged_in("[system] " + username + " 已离线。\n", connection);

  // 13. spdlog 记录
  spdlog::info("账户已注销：{}", username);
}

void ChatServer::handle_public_message(const TcpConnectionPtr &connection,
                                       const ClientSession &session,
                                       const std::string &message) {
  if (!require_login(connection, session, "chatting")) {
    return;
  }

  const std::string cleaned = trim(message);
  if (cleaned.empty() || cleaned.size() > kMaxChatMessage) {
    connection->send("[error] 公共消息长度必须为 1-1000 字节。"
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
                                        ClientSession &session,
                                        const std::string &arguments) {
  if (!require_login(connection, session, "发送好友私聊消息")) {
    return;
  }

  std::string target;
  std::string encoded_message;
  if (!split_first_token(arguments, target, encoded_message) ||
      !is_valid_username(target) || encoded_message.empty()) {
    connection->send("[error] 用法：MSG <用户名> <百分号编码消息>\n");
    return;
  }

  std::string message;
  std::string decode_error;
  if (!fileutil::percent_decode(encoded_message, message, decode_error)) {
    connection->send("[error] 私聊消息编码无效。\n");
    return;
  }

  if (message.empty() || message.size() > kMaxChatMessage) {
    connection->send("[error] 私聊消息解码后长度必须为 1-1000 字节。\n");
    return;
  }

  const std::string sender = session.username;
  if (target == sender) {
    connection->send("[error] 不能给自己发送私聊消息。\n");
    return;
  }

  ChatMessagePayload payload;
  payload.set_type(chatroom::v7::PRIVATE);
  payload.set_sender_username(sender);
  payload.set_recipient_username(target);
  payload.set_content(message);
  payload.set_created_at_unix_ms(now_unix_ms());

  std::string error;
  std::uint64_t message_id = 0U;
  std::uint64_t policy_generation_used = 0U;

  {
    // 好友/屏蔽变更也使用同一把锁。只要 generation 没变，
    // 进入会话时已经验证过的目标就不需要每条消息再做 4 次权限 SQL。
    std::lock_guard<std::mutex> operation_lock(friend_operation_mutex_);

    const std::uint64_t current_generation =
        direct_policy_generation_.load(std::memory_order_relaxed);

    const bool cached_allowed =
        session.realtime_private_target == target &&
        session.realtime_private_generation == current_generation;

    if (!cached_allowed) {
      const DirectMessageDecision decision =
          direct_message_policy_.evaluate(sender, target, error);

      if (decision == DirectMessageDecision::DatabaseError) {
        database_error(connection, "检查私聊权限", error);
        return;
      }
      if (decision == DirectMessageDecision::TargetMissing) {
        connection->send("[error] 目标账户不存在。\n");
        return;
      }
      if (decision == DirectMessageDecision::NotFriends) {
        connection->send("[error] 当前不允许向该用户发送私聊消息，只有好友之间可以私聊。\n");
        return;
      }
      if (decision == DirectMessageDecision::BlockedByRecipient) {
        connection->send("[error] 对方已屏蔽你。\n");
        return;
      }
      if (decision == DirectMessageDecision::BlockedBySender) {
        connection->send("[error] 你已屏蔽该用户，请先解除屏蔽。\n");
        return;
      }

      session.realtime_private_target = target;
      session.realtime_private_generation = current_generation;
    }

    policy_generation_used = session.realtime_private_generation;

    if (!database_.add_private_message_with_delivery(payload, message_id,
                                                     error)) {
      database_error(connection, "保存私聊消息", error);
      return;
    }
  }

  bool live_delivery_allowed = true;

  // 如果消息持久化期间恰好发生了好友/屏蔽关系变化，只在这种少见情况重新校验。
  if (policy_generation_used !=
      direct_policy_generation_.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> operation_lock(friend_operation_mutex_);
    const DirectMessageDecision decision =
        direct_message_policy_.evaluate(sender, target, error);
    live_delivery_allowed = decision == DirectMessageDecision::Allowed;

    if (live_delivery_allowed) {
      session.realtime_private_target = target;
      session.realtime_private_generation =
          direct_policy_generation_.load(std::memory_order_relaxed);
    }
  }

  TcpConnectionPtr target_connection;
  if (live_delivery_allowed &&
      find_online_user(target, target_connection)) {
    const std::string target_wire =
        "[#" + std::to_string(message_id) + "] [private from " + sender + "] " +
        encode_text_token(message) + "\n";

    target_connection->send(
        target_wire,
        [this, message_id, target] {
          // 只排队，真正 UPDATE 由后台线程做；绝不在接收方 Reactor 里查数据库。
          enqueue_delivery_persist(DeliveryPersistKind::Private,
                                   message_id, target);
        });

    connection->send("[#" + std::to_string(message_id) + "] [private to " +
                     target + "] " + encode_text_token(message) + "\n");
    return;
  }

  // Redis 只是未读缓存。在线实时投递不再做 +1 再 -1 的两次网络往返；
  // 只有当前没有实时投递时才记录未读。
  adjust_redis_unread_best_effort(target, "private", 1);

  connection->send("[#" + std::to_string(message_id) + "] [private to " +
                   target + "] " + encode_text_token(message) +
                   "\n"
                   "[system] 消息已保存；对方满足投递条件后会自动收到。\n");
}

void ChatServer::handle_who(const TcpConnectionPtr &connection,
                            const ClientSession &session) {
  if (!require_login(connection, session, "using WHO")) {
    return;
  }

  const std::vector<std::string> names = online_users_.usernames();

  connection->send("[system] 在线用户（" + std::to_string(names.size()) +
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
    connection->send("[error] 不能添加自己为好友。\n");
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
      connection->send("[error] 用户 " + target + " 不存在。\n");
      return;
    }

    if (!database_.are_friends(sender, target, friends, error)) {
      database_error(connection, "checking friendship", error);
      return;
    }

    if (friends) {
      connection->send("[error] " + target + " 已经是你的好友。\n");
      return;
    }

    if (!database_.has_friend_request(sender, target, already_sent, error) ||
        !database_.has_friend_request(target, sender, reverse_request, error)) {
      database_error(connection, "checking friend request", error);
      return;
    }

    if (already_sent) {
      connection->send("[error] 好友申请已经发送过。\n");
      return;
    }

    if (reverse_request) {
      connection->send("[error] " + target +
                       " 已经向你发送好友申请，请使用数字命令 18 通过该申请。\n");
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
      std::cerr << "写入好友事件失败：" << error << '\n';
    }
  }

  connection->send("[system] 已向 " + target + " 发送好友申请。\n");

  notify_user_if_online(target, "[system] 收到来自 " + sender +
                                    " 的好友申请。使用 18 通过，或使用 19 拒绝。\n");
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
        connection->send("[error] 没有来自 " + requester + " 的待处理好友申请。\n");
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
      std::cerr << "写入好友事件失败：" << error << '\n';
    }
  }

  direct_policy_generation_.fetch_add(1U, std::memory_order_relaxed);

  connection->send("[system] 你和 " + requester + " 现在已经是好友。\n");

  notify_user_if_online(requester, "[system] " + current +
                                       " 已通过你的好友申请。\n");
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
      connection->send("[error] 没有来自 " + requester + " 的待处理好友申请。\n");
      return;
    }

    FriendEventPayload event;
    event.set_type(chatroom::v7::FRIEND_REQUEST_REJECTED);
    event.set_actor_username(current);
    event.set_target_username(requester);
    event.set_occurred_at_unix_ms(now_unix_ms());

    if (!database_.add_friend_event(event, error)) {
      std::cerr << "写入好友事件失败：" << error << '\n';
    }
  }

  connection->send("[system] 已拒绝来自 " + requester + " 的好友申请。\n");

  notify_user_if_online(requester, "[system] " + current +
                                       " 已拒绝你的好友申请。\n");
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
      connection->send("[error] " + target + " 不是你的好友。\n");
      return;
    }

    FriendEventPayload event;
    event.set_type(chatroom::v7::FRIEND_REMOVED);
    event.set_actor_username(current);
    event.set_target_username(target);
    event.set_occurred_at_unix_ms(now_unix_ms());

    if (!database_.add_friend_event(event, error)) {
      std::cerr << "写入好友事件失败：" << error << '\n';
    }
  }

  direct_policy_generation_.fetch_add(1U, std::memory_order_relaxed);

  connection->send("[system] 已从好友列表删除 " + target + "。\n");

  notify_user_if_online(target, "[system] " + current +
                                    " 已将你从好友列表删除。\n");
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
  output << "[system] 好友列表（" << friends.size() << "）：\n";

  for (const std::string &name : friends) {
    output << "  " << name << " ["
           << (is_user_online(name) ? "在线" : "离线") << "]\n";
  }

  if (friends.empty()) {
    output << "  （无）\n";
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
      "[system] 收到的好友申请（" + std::to_string(incoming.size()) +
      "）：" + join_names(incoming) +
      "\n"
      "[system] 已发出的好友申请（" +
      std::to_string(outgoing.size()) + "）：" + join_names(outgoing) + "\n");
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
    connection->send("[error] 用法：HISTORY_PUBLIC [条数]，"
                     "条数必须在 1-100 之间。\n");
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
    connection->send("[error] 用法：HISTORY_PRIVATE "
                     "<用户名> [条数]\n");
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
    connection->send("[error] 用户不存在。\n");
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
      connection->send("[error] 群名称已被使用。\n");
      return;
    }

    if (!database_.create_group(group_name, session.username, group_id,
                                error)) {
      database_error(connection, "creating group", error);
      return;
    }
  }

  group_policy_generation_.fetch_add(1U, std::memory_order_relaxed);

  connection->send("[system] 群 " + group_name +
                   " 创建成功，你是群主。群ID=" +
                   std::to_string(group_id) + "。\n");
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
      connection->send("[error] 只有群主可以解散该群。\n");
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
      connection->send("[error] 群已不存在。\n");
      return;
    }
  }

  group_policy_generation_.fetch_add(1U, std::memory_order_relaxed);
  connection->send("[system] 群 " + group_name + " 已解散。\n");

  for (const std::string &member : members) {
    if (member != session.username) {
      notify_user_if_online(member, "[system] 群 " + group_name +
                                        " 已被群主解散，群主：" +
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
      connection->send("[error] 群不存在。\n");
      return;
    }

    std::optional<GroupRole> role;
    if (!database_.get_group_role(group_name, session.username, role, error)) {
      database_error(connection, "checking group membership", error);
      return;
    }

    if (role) {
      connection->send("[error] 你已经是该群成员。\n");
      return;
    }

    bool pending = false;
    if (!database_.has_group_join_request(group_name, session.username, pending,
                                          error)) {
      database_error(connection, "checking group join request", error);
      return;
    }

    if (pending) {
      connection->send("[error] 入群申请已经在等待处理。\n");
      return;
    }

    if (!database_.add_group_join_request(group_name, session.username,
                                          error)) {
      database_error(connection, "saving group join request", error);
      return;
    }
  }

  connection->send("[system] 已向群 " + group_name + " 提交入群申请。\n");

  notify_group_managers(
      group_name,
      "[system] " + session.username + " 申请加入群 " + group_name +
          "。请使用数字命令 33 通过或 34 拒绝，客户端会自动列出并编号。\n",
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
  output << "[system] 已加入的群（" << groups.size() << "）：\n";

  for (const GroupMembership &membership : groups) {
    output << "  " << membership.group.name << " ["
           << group_role_name(membership.role)
           << "] 群主=" << membership.group.owner_username << "\n";
  }

  if (groups.empty()) {
    output << "  （无）\n";
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
      connection->send("[error] 你不是该群成员。\n");
      return;
    }

    if (*role == GroupRole::Owner) {
      connection->send("[error] 群主不能直接退出群，请先解散群或转移职责。"
                       "use command 25.\n");
      return;
    }

    bool removed = false;
    if (!database_.remove_group_member(group_name, session.username, removed,
                                       error)) {
      database_error(connection, "leaving group", error);
      return;
    }

    if (!removed) {
      connection->send("[error] 群成员关系已不存在。\n");
      return;
    }
  }

  group_policy_generation_.fetch_add(1U, std::memory_order_relaxed);
  connection->send("[system] 已退出群 " + group_name + "。\n");

  notify_group_managers(group_name,
                        "[system] " + session.username + " 已退出群 " +
                            group_name + "。\n",
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
    connection->send("[error] 只有群成员可以查看该内容。"
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
      connection->send("[error] 只有群主可以设置管理员。\n");
      return;
    }

    if (!target_role) {
      connection->send("[error] 目标用户不是群成员。\n");
      return;
    }

    if (*target_role == GroupRole::Owner) {
      connection->send("[error] 群主权限高于管理员，无需设置。\n");
      return;
    }

    if (*target_role == GroupRole::Admin) {
      connection->send("[error] 目标用户已经是管理员。\n");
      return;
    }

    bool changed = false;
    if (!database_.set_group_member_role(group_name, target, GroupRole::Admin,
                                         changed, error)) {
      database_error(connection, "adding group administrator", error);
      return;
    }

    if (!changed) {
      connection->send("[error] 群角色没有发生变化。\n");
      return;
    }
  }

  connection->send("[system] " + target + " 现在是管理员，群：" +
                   group_name + ".\n");

  notify_user_if_online(target, "[system] 你已被提升为管理员，"
                                "of group " +
                                    group_name + "，操作者：" + session.username +
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
      connection->send("[error] 只有群主可以取消管理员。\n");
      return;
    }

    if (!target_role || *target_role != GroupRole::Admin) {
      connection->send("[error] 目标用户不是管理员。\n");
      return;
    }

    bool changed = false;
    if (!database_.set_group_member_role(group_name, target, GroupRole::Member,
                                         changed, error)) {
      database_error(connection, "removing group administrator", error);
      return;
    }

    if (!changed) {
      connection->send("[error] 群角色没有发生变化。\n");
      return;
    }
  }

  connection->send("[system] " + target + " 现在是普通成员，群：" +
                   group_name + ".\n");

  notify_user_if_online(target, "[system] 你在群 " +
                                    group_name + " 的管理员身份已被取消，操作者：" +
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
    connection->send("[error] 只有群主或管理员可以查看入群申请。\n");
    return;
  }

  std::vector<std::string> users;

  if (!database_.list_group_join_requests(group_name, users, error)) {
    database_error(connection, "loading group join requests", error);
    return;
  }

  connection->send("[group " + group_name + "] 待处理入群申请（" +
                   std::to_string(users.size()) + "): " + join_names(users) +
                   "\n");
}

void ChatServer::handle_group_requests_all(const TcpConnectionPtr &connection,
                                           const ClientSession &session) {
  if (!require_login(connection, session, "查看全部可处理入群申请")) {
    return;
  }

  std::vector<ManagedGroupRequestCount> groups;
  std::string error;
  if (!database_.list_managed_group_request_counts(session.username, groups,
                                                   error)) {
    connection->send("[group-requests-error] " +
                     encode_text_token("加载可管理群的入群申请失败：" + error) +
                     "\n");
    return;
  }

  connection->send("[group-requests-begin]\n");

  // 这里只展示当前用户是群主或管理员的群。数据库查询已经限制 member_role IN (1,2)。
  // 对每个有待处理申请的群读取申请人，客户端统一编号后供 33/34 选择。
  for (const ManagedGroupRequestCount &group : groups) {
    std::vector<std::string> users;
    if (!database_.list_group_join_requests(group.group_name, users, error)) {
      connection->send("[group-requests-error] " +
                       encode_text_token("加载群 " + group.group_name +
                                         " 的入群申请失败：" + error) +
                       "\n");
      return;
    }

    for (const std::string &username : users) {
      connection->send("[group-request-item] " +
                       encode_text_token(group.group_name) + " " +
                       encode_text_token(username) + "\n");
    }
  }

  connection->send("[group-requests-end]\n");
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
      connection->send("[error] 只有群主或管理员可以通过入群申请。\n");
      return;
    }

    bool pending = false;
    if (!database_.has_group_join_request(group_name, target, pending, error)) {
      database_error(connection, "checking group join request", error);
      return;
    }

    if (!pending) {
      connection->send("[error] 没有来自 " + target + " 的待处理入群申请。\n");
      return;
    }

    if (!database_.approve_group_join_request(group_name, target, error)) {
      database_error(connection, "approving group join request", error);
      return;
    }
  }

  group_policy_generation_.fetch_add(1U, std::memory_order_relaxed);

  connection->send("[system] " + target + " 已加入群 " + group_name +
                   "。\n");

  notify_user_if_online(target, "[system] 你申请加入群 " +
                                    group_name + " 已通过，处理人：" +
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
      connection->send("[error] 只有群主或管理员可以拒绝入群申请。\n");
      return;
    }

    bool removed = false;
    if (!database_.reject_group_join_request(group_name, target, removed,
                                             error)) {
      database_error(connection, "rejecting group join request", error);
      return;
    }

    if (!removed) {
      connection->send("[error] 没有来自 " + target + " 的待处理入群申请。\n");
      return;
    }
  }

  connection->send("[system] 已拒绝 " + target + " 加入群 " +
                   group_name + ".\n");

  notify_user_if_online(target, "[system] 你申请加入群 " +
                                    group_name + " 已被拒绝，处理人：" +
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
    connection->send("[error] 不能通过移出成员操作移出自己，请使用退出群命令。\n");
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
      connection->send("[error] 只有群主或管理员可以移出成员。\n");
      return;
    }

    if (!target_role) {
      connection->send("[error] 目标用户不是群成员。\n");
      return;
    }

    if (*target_role == GroupRole::Owner) {
      connection->send("[error] 不能移出群主。\n");
      return;
    }

    if (*current_role == GroupRole::Admin && *target_role == GroupRole::Admin) {
      connection->send("[error] 管理员不能移出同级或更高权限成员。"
                       "another administrator.\n");
      return;
    }

    bool removed = false;
    if (!database_.remove_group_member(group_name, target, removed, error)) {
      database_error(connection, "removing group member", error);
      return;
    }

    if (!removed) {
      connection->send("[error] 群成员关系已不存在。\n");
      return;
    }
  }

  group_policy_generation_.fetch_add(1U, std::memory_order_relaxed);
  connection->send("[system] 已将 " + target + " 移出群 " + group_name +
                   "。\n");

  // 如果被移出成员当前正拿着这个群的 FILE_OFFER，或者文件已经开始下行，
  // 立即撤销。发送泵会在当前帧结束后检查取消标记，不再发送后续帧。
  TcpConnectionPtr removed_connection;
  if (find_online_user(target, removed_connection)) {
    const std::shared_ptr<ClientSession> removed_session =
        session_of(removed_connection);
    if (removed_session != nullptr) {
      for (auto it = removed_session->offered_files.begin();
           it != removed_session->offered_files.end();) {
        const StoredFileTransfer &offered = it->second;
        if (offered.metadata.scope() != chatroom::v9::FILE_TRANSFER_GROUP ||
            offered.metadata.group_name() != group_name) {
          ++it;
          continue;
        }

        const std::uint64_t transfer_id = it->first;
        const auto cancel =
            removed_session->file_delivery_cancel_flags.find(transfer_id);
        if (cancel != removed_session->file_delivery_cancel_flags.end() &&
            cancel->second) {
          cancel->second->store(true);
        }

        removed_connection->send(
            "FILE_ACCESS_REVOKED " + std::to_string(transfer_id) + " " +
            encode_text_token("群成员权限已被撤销") + "\n");

        if (removed_session->file_deliveries_in_progress.count(transfer_id) ==
            0U) {
          it = removed_session->offered_files.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  notify_user_if_online(target, "[system] 你已被移出群 " +
                                    group_name + "，操作者：" + session.username +
                                    "。\n");
}

void ChatServer::handle_group_message(const TcpConnectionPtr &connection,
                                      ClientSession &session,
                                      const std::string &arguments) {
  if (!require_login(connection, session, "发送群消息")) {
    return;
  }

  std::string group_name;
  std::string encoded_message;
  if (!split_first_token(arguments, group_name, encoded_message) ||
      !is_valid_group_name(group_name) || encoded_message.empty()) {
    connection->send("[error] 用法：GROUP_MSG <群名称> <百分号编码消息>。\n");
    return;
  }

  std::string message;
  std::string decode_error;
  if (!fileutil::percent_decode(encoded_message, message, decode_error)) {
    connection->send("[error] 群消息编码无效。\n");
    return;
  }

  if (message.empty() || message.size() > kMaxChatMessage) {
    connection->send("[error] 群消息解码后长度必须为 1-1000 字节。\n");
    return;
  }

  std::uint64_t group_id = 0U;
  std::vector<std::string> recipients;
  std::string error;

  const std::uint64_t current_generation =
      group_policy_generation_.load(std::memory_order_relaxed);

  const bool cached_group =
      session.realtime_group_name == group_name &&
      session.realtime_group_id != 0U &&
      session.realtime_group_generation == current_generation;

  if (cached_group) {
    group_id = session.realtime_group_id;
    recipients = session.realtime_group_recipients;
  } else {
    // 群成员没有变化时，这一组 SQL 只在进入群聊/版本失效后的第一条消息执行。
    std::optional<GroupInfo> group;
    std::optional<GroupRole> role;
    std::vector<std::string> members;

    if (!database_.get_group(group_name, group, error) ||
        !database_.get_group_role(group_name, session.username, role, error) ||
        !database_.list_group_member_usernames(group_name, members, error)) {
      database_error(connection, "加载群聊状态", error);
      return;
    }

    if (!group) {
      connection->send("[error] 群不存在。\n");
      return;
    }
    if (!role) {
      connection->send("[error] 只有群成员可以发送群消息。\n");
      return;
    }

    group_id = group->id;
    recipients.reserve(members.size());
    for (const std::string &member : members) {
      if (member != session.username) {
        recipients.push_back(member);
      }
    }

    session.realtime_group_name = group_name;
    session.realtime_group_id = group_id;
    session.realtime_group_recipients = recipients;
    session.realtime_group_generation = current_generation;
  }

  GroupMessagePayload payload;
  payload.set_group_id(group_id);
  payload.set_group_name(group_name);
  payload.set_sender_username(session.username);
  payload.set_content(message);
  payload.set_created_at_unix_ms(now_unix_ms());

  std::uint64_t message_id = 0U;
  if (!database_.add_group_message(payload, recipients, message_id, error)) {
    database_error(connection, "保存群消息", error);
    return;
  }

  const std::string wire_text =
      "[#G" + std::to_string(message_id) + "] [group " + group_name + "] [" +
      session.username + "] " + encode_text_token(message) + "\n";

  // 发送者也立即收到带服务端消息 ID 的回显。
  connection->send(wire_text);

  for (const std::string &recipient : recipients) {
    TcpConnectionPtr target_connection;

    if (find_online_user(recipient, target_connection)) {
      target_connection->send(
          wire_text,
          [this, message_id, recipient] {
            enqueue_delivery_persist(DeliveryPersistKind::Group,
                                     message_id, recipient);
          });
      continue;
    }

    // 在线实时发送时不再 Redis +1/-1；只有离线才累加未读缓存。
    adjust_redis_unread_best_effort(recipient, "group", 1);
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
    connection->send("[error] 用法：HISTORY_GROUP "
                     "<群名称> [条数]\n");
    return;
  }

  std::size_t count = kDefaultHistoryCount;

  if (words.size() == 2U &&
      !parse_count(words[1], 1U, kMaxHistoryCount, count)) {
    connection->send("[error] 群历史条数必须在 1-100 之间。\n");
    return;
  }

  std::optional<GroupRole> role;
  std::string error;

  if (!database_.get_group_role(words[0], session.username, role, error)) {
    database_error(connection, "checking group membership", error);
    return;
  }

  if (!role) {
    connection->send("[error] 只有群成员可以查看该内容。"
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
                       "your parameter is not five");
    return;
  }

  const std::string &target = words[1];

  std::uint64_t file_size = 0U;
  std::string filename;
  std::string error;

  if (!fileutil::is_valid_transfer_token(token)) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "the token is fault" : error);
    return;
  }
  if (!is_valid_username(target)) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "the username is fault" : error);
    return;
  }
  if (!parse_uint64_value(words[3], file_size)) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "your file size is not available"
                                     : error);
    return;
  }
  if (file_size > kMaxFileSize) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "your file size is too large" : error);
    return;
  }
  if (!fileutil::is_valid_sha256_hex(words[4])) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "your haxi format is fault" : error);
    return;
  }
  if (!fileutil::percent_decode(words[2], filename, error)) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "restore the fileanme is false" : error);
    return;
  }
  if (filename.empty()) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "your file name is empty" : error);
    return;
  }

  if (filename.size() > 255U) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "the size of filename is too large"
                                     : error);
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
                               "数据库操作出错：checking direct-file policy");
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
                               "recipient 有 blocked you");
            return;
        }

        // 新增
        if (decision == DirectMessageDecision::BlockedBySender) {
            reject_file_upload(connection, session, token,
                               "you have blocked this user, please unblock first");
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
                       "the count of parameter is not five");
    return;
  }

  const std::string &group_name = words[1];

  std::uint64_t file_size = 0U;
  std::string filename;
  std::string error;

  if (!fileutil::is_valid_transfer_token(token)) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "the token is fault" : error);
    return;
  }
  if (!is_valid_group_name(group_name)) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "the groupname is fault" : error);
    return;
  }
  if (!parse_uint64_value(words[3], file_size)) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "your file size is not +zhengshu"
                                     : error);
    return;
  }
  if (file_size > kMaxFileSize) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "your file size is too large" : error);
    return;
  }
  if (!fileutil::is_valid_sha256_hex(words[4])) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "your haxi format is fault" : error);
    return;
  }
  if (!fileutil::percent_decode(words[2], filename, error)) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "restore the fileanme is false" : error);
    return;
  }
  if (filename.empty()) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "your file name is empty" : error);
    return;
  }

  if (filename.size() > 255U) {
    reject_file_upload(connection, session, token,
                       error.empty() ? "the size of filename is too large"
                                     : error);
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
                       "数据库操作出错：checking group membership");
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
                       "数据库操作出错：loading group members");
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
                       "group 有 no other members to receive the file");
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

  if (words.size() != 3U) {
    pause_file_upload(connection, session, token,
                      "your count of parameter is false");
    return;
  }
  if (!session.upload) {
    pause_file_upload(connection, session, token,
                      "the  server donot have the client session");
    return;
  }
  if (session.upload->token != token) {
    pause_file_upload(connection, session, token,
                      "the token of client is false");
    return;
  }
  if (session.binary_upload) {
    pause_file_upload(connection, session, token,
                      "the previous chunk is not receive completely");
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
  frame.start_offset = offset;
  frame.next_offset = offset;
  frame.remaining_bytes = byte_count;
  frame.bytes.reserve(static_cast<std::size_t>(byte_count));

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
        "uploaded byte count is incomplete; resume，来源：server offset");
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

        std::string reason;
        if (decision == DirectMessageDecision::BlockedByRecipient) {
            reason = "接收方已屏蔽你";
        } else if (decision == DirectMessageDecision::BlockedBySender) {    // 新增
            reason = "你已屏蔽接收方";
        } else if (decision == DirectMessageDecision::NotFriends) {
            reason = "接收方已不再是你的好友";
        } else if (decision == DirectMessageDecision::TargetMissing) {
            reason = "接收方账户已不存在";
        } else {
            reason = "上传过程中权限发生变化";
        }

        connection->send("FILE_REJECT " + token + " " +
                         encode_text_token(reason) + "\n");
        return;
    }
  } else if (upload.scope == chatroom::v9::FILE_TRANSFER_GROUP) {
    // 群文件从 FILE_BEGIN 到 FILE_END 期间成员关系可能已经变化。
    // 不能继续使用开始上传时的成员快照，否则被踢成员仍可能成为收件人。
    std::lock_guard<std::mutex> operation_lock(group_operation_mutex_);

    std::optional<GroupRole> sender_role;
    std::vector<std::string> current_members;
    if (!database_.get_group_role(upload.target, session.username, sender_role,
                                  error) ||
        !database_.list_group_member_usernames(upload.target, current_members,
                                               error)) {
      file_transfer_service_.cancel_upload(upload.token);
      connection->send("FILE_REJECT " + token + " " +
                       encode_text_token(
                           "重新检查群成员权限时数据库出错") +
                       "\n");
      return;
    }

    if (!sender_role) {
      file_transfer_service_.cancel_upload(upload.token);
      connection->send("FILE_REJECT " + token + " " +
                       encode_text_token(
                           "你已不再是该群成员") +
                       "\n");
      return;
    }

    upload.recipients.clear();
    for (const std::string &member : current_members) {
      if (member != session.username) upload.recipients.push_back(member);
    }

    if (upload.recipients.empty()) {
      file_transfer_service_.cancel_upload(upload.token);
      connection->send("FILE_REJECT " + token + " " +
                       encode_text_token(
                           "当前群内没有其他可接收该文件的成员") +
                       "\n");
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
        encode_text_token("数据库未能保存文件传输记录") + "\n");
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

  connection->send("[file #F" + std::to_string(transfer_id) + "] 已保存 " +
                   upload.file_name + "（" +
                   std::to_string(upload.expected_size) +
                   " 字节）。在线接收方会立即收到，离线接收方登录后接收。\n");

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
    connection->send("[error] 没有匹配的活动上传任务可取消。\n");
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
    connection->send("[error] 文件断点续传请求无效。\n");
    return;
  }

  const auto offered = session.offered_files.find(transfer_id);

  if (offered == session.offered_files.end()) {
    connection->send("[error] 文件 #F" + std::to_string(transfer_id) +
                     " 当前不在可接收列表中，请使用数字命令 37 重新拉取。\n");
    return;
  }

  // 群文件真正开始下行前再检查一次成员身份。这样即使 FILE_OFFER 已经
  // 发到客户端，只要随后被移出群，就不会因为旧 offer 继续拿到文件。
  if (offered->second.metadata.scope() == chatroom::v9::FILE_TRANSFER_GROUP) {
    std::optional<GroupRole> role;
    std::string membership_error;
    if (!database_.get_group_role(offered->second.metadata.group_name(),
                                  session.username, role, membership_error)) {
      connection->send("[error] 重新检查群文件成员权限失败，文件 #F" +
                       std::to_string(transfer_id) + ".\n");
      return;
    }
    if (!role) {
      session.offered_files.erase(offered);
      connection->send("FILE_ACCESS_REVOKED " +
                       std::to_string(transfer_id) + " " +
                       encode_text_token(
                           "你已不再是该群成员") +
                       "\n");
      return;
    }
  }

  if (start_offset > offered->second.metadata.file_size()) {
    connection->send(
        "[error] 断点位置超过文件大小；客户端应删除本地临时文件并从 0 重新接收。\n");
    return;
  }

  if (!session.file_deliveries_in_progress.insert(transfer_id).second) {
    return;
  }

  const StoredFileTransfer transfer = offered->second;

  auto cancel_flag = std::make_shared<std::atomic_bool>(false);
  session.file_delivery_cancel_flags[transfer_id] = cancel_flag;

  file_transfer_service_.deliver_async(
      transfer.id, transfer.metadata, start_offset, connection,
      [cancel_flag] { return !cancel_flag->load(); },
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
          current->getLoop()->queueInLoop(
              [this, current, transfer_id, delivery_error] {
            const std::shared_ptr<ClientSession> current_session =
                session_of(current);

            if (current_session != nullptr) {
              current_session->file_deliveries_in_progress.erase(transfer_id);
              current_session->file_delivery_cancel_flags.erase(transfer_id);
              if (delivery_error == "delivery permission revoked") {
                current_session->offered_files.erase(transfer_id);
              }
            }
          });
        }

        std::cerr << "恢复文件传输 #" << transfer_id << " 给 "
                  << recipient << " 失败：" << delivery_error << '\n';
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
    connection->send("[error] 文件接收确认格式无效。\n");
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
    session.file_delivery_cancel_flags.erase(transfer_id);

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
    session.file_delivery_cancel_flags.erase(transfer_id);

    connection->send("[error] 文件 SHA-256 接收确认不匹配；"
                     "the file remains pending for retry.\n");
    return;
  }

  if (!database_.mark_file_transfer_delivered(transfer_id, session.username,
                                              now_unix_ms(), error)) {
    database_error(connection, "marking file delivered", error);
    return;
  }

  session.file_deliveries_in_progress.erase(transfer_id);
  session.file_delivery_cancel_flags.erase(transfer_id);

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
  session.file_delivery_cancel_flags.erase(transfer_id);

  connection->send("[system] 文件 #F" + std::to_string(transfer_id) +
                   " remains pending; use command 37 to retry/resume.\n");
}

void ChatServer::deliver_pending_files(const TcpConnectionPtr &connection,
                                       const std::string &username) {
  std::vector<StoredFileTransfer> transfers;

  std::string error;

  if (!database_.pending_file_transfers(username, kOfflineFileDeliveryBatch,
                                        transfers, error)) {
    std::cerr << "加载待处理文件失败，用户：" << username << ": "
              << error << '\n';
    return;
  }

  if (!transfers.empty()) {
    connection->send("[system] 正在提供 " + std::to_string(transfers.size()) +
                     " pending file(s); local partial files "
                     "will resume，来源：their saved offsets.\n");
  }

  for (const StoredFileTransfer &transfer : transfers) {
    deliver_file_to_user(transfer, username, connection);
  }

  if (transfers.size() == kOfflineFileDeliveryBatch) {
    connection->send("[system] 可能还有待接收文件；"
                     "use command 37 again after current downloads finish.\n");
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
      std::cerr << "检查文件屏蔽策略失败，文件 #F" << transfer.id
                << ": " << block_error << '\n';

      return;
    }

    if (blocked) {
      return;
    }
  } else if (transfer.metadata.scope() == chatroom::v9::FILE_TRANSFER_GROUP) {
    std::optional<GroupRole> role;
    std::string membership_error;
    if (!database_.get_group_role(transfer.metadata.group_name(), recipient,
                                  role, membership_error)) {
      std::cerr << "检查群文件成员权限失败，文件 #F"
                << transfer.id << ": " << membership_error << '\n';
      return;
    }
    if (!role) {
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

  (void)action;
  connection->send("[error] 请先登录后再执行该操作。\n");

  return false;
}

bool ChatServer::extract_single_username(const TcpConnectionPtr &connection,
                                         const std::string &arguments,
                                         const std::string &usage,
                                         std::string &username) const {
  const std::vector<std::string> words = split_words(arguments);

  if (words.size() != 1U || !is_valid_username(words[0])) {
    connection->send("[error] 用法：" + usage + "\n");
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
    connection->send("[error] 用法：" + usage +
                     "；群名称长度 2-32，只能包含字母、数字、下划线或短横线。\n");
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
    connection->send("[error] 用法：" + usage + "\n");
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
      connection->send("[system] 你有 " +
                       std::to_string(pending_friends.size()) +
                       " pending friend request(s). "
                       "Use command 16.\n");
    }
  } else {
    std::cerr << "加载待处理好友申请失败，用户：" << username
              << ": " << error << '\n';
  }

  std::vector<ManagedGroupRequestCount> group_requests;

  if (database_.list_managed_group_request_counts(username, group_requests,
                                                  error)) {
    for (const ManagedGroupRequestCount &request : group_requests) {
      connection->send("[system] 群 " + request.group_name + " 有 " +
                       std::to_string(request.pending_count) +
                       " pending join request(s). "
                       "Use command 32 for group requests: 32 " +
                       request.group_name + ".\n");
    }
  } else {
    std::cerr << "加载可管理群的入群申请失败，用户：" << username
              << ": " << error << '\n';
  }
}

void ChatServer::deliver_pending_messages(const TcpConnectionPtr &connection,
                                          const std::string &username) {
  std::string error;
  std::vector<StoredMessage> private_messages;

  if (!database_.pending_private_messages(username, kOfflineDeliveryBatch,
                                          private_messages, error)) {
    std::cerr << "加载离线私聊消息失败，用户：" << username
              << ": " << error << '\n';
  } else {
    if (!private_messages.empty()) {
      connection->send("[system] 正在投递 " +
                       std::to_string(private_messages.size()) +
                       " 条离线私聊消息。\n");
    }

    for (const StoredMessage &message : private_messages) {
      bool blocked = false;
      std::string block_error;

      if (!database_.is_friend_blocked(username,
                                       message.payload.sender_username(),
                                       blocked, block_error)) {
        std::cerr << "离线私聊投递前检查屏蔽关系失败，消息 #"
                  << message.id << ": " << block_error << '\n';

        continue;
      }

      if (blocked) {
        continue;
      }

      connection->send(
          "[offline #" + std::to_string(message.id) + "] [private from " +
              message.payload.sender_username() + "] " +
              encode_text_token(message.payload.content()) + "\n",
          [this, message_id = message.id, username] {
            enqueue_delivery_persist(DeliveryPersistKind::Private,
                                     message_id, username, true);
          });
    }

    if (private_messages.size() == kOfflineDeliveryBatch) {
      connection->send("[system] 可能还有离线私聊消息；"
                       "请再次使用数字命令 37。\n");
    }
  }

  std::vector<StoredGroupMessage> group_messages;

  if (!database_.pending_group_messages(username, kOfflineDeliveryBatch,
                                        group_messages, error)) {
    std::cerr << "加载离线群消息失败，用户：" << username
              << ": " << error << '\n';
    return;
  }

  if (!group_messages.empty()) {
    connection->send("[system] 正在投递 " +
                     std::to_string(group_messages.size()) +
                     " 条离线群消息。\n");
  }

  for (const StoredGroupMessage &message : group_messages) {
    connection->send(
        "[offline #G" + std::to_string(message.id) + "] [group " +
            message.payload.group_name() + "] [" +
            message.payload.sender_username() + "] " +
            encode_text_token(message.payload.content()) + "\n",
        [this, message_id = message.id, username] {
          enqueue_delivery_persist(DeliveryPersistKind::Group,
                                   message_id, username, true);
        });
  }

  if (group_messages.size() == kOfflineDeliveryBatch) {
    connection->send("[system] 可能还有离线群消息；"
                     "请再次使用数字命令 37。\n");
  }
}

void ChatServer::notify_group_managers(const std::string &group_name,
                                       const std::string &message,
                                       const std::string &except_username) {
  std::vector<std::string> managers;
  std::string error;

  if (!database_.list_group_managers(group_name, managers, error)) {
    std::cerr << "加载群管理员失败，群：" << group_name << ": "
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
    return "群主";
  case GroupRole::Admin:
    return "管理员";
  case GroupRole::Member:
    return "成员";
  }

  return "unknown";
}

bool ChatServer::is_group_manager(GroupRole role) {
  return role == GroupRole::Owner || role == GroupRole::Admin;
}

void ChatServer::enqueue_delivery_persist(DeliveryPersistKind kind,
                                          std::uint64_t message_id,
                                          std::string recipient,
                                          bool decrement_unread) {
  {
    std::lock_guard<std::mutex> lock(delivery_persist_mutex_);
    delivery_persist_queue_.push_back(
        DeliveryPersistTask{kind, message_id, std::move(recipient),
                            decrement_unread});
  }
  delivery_persist_cv_.notify_one();
}

void ChatServer::delivery_persist_loop() {
  while (true) {
    DeliveryPersistTask task;

    {
      std::unique_lock<std::mutex> lock(delivery_persist_mutex_);
      delivery_persist_cv_.wait(lock, [this] {
        return stopping_.load() || !delivery_persist_queue_.empty();
      });

      if (delivery_persist_queue_.empty()) {
        if (stopping_.load()) break;
        continue;
      }

      task = std::move(delivery_persist_queue_.front());
      delivery_persist_queue_.pop_front();
    }

    std::string error;
    bool ok = false;

    if (task.kind == DeliveryPersistKind::Private) {
      ok = database_.mark_private_message_delivered(
          task.message_id, task.recipient, now_unix_ms(), error);
    } else {
      ok = database_.mark_group_message_delivered(
          task.message_id, task.recipient, now_unix_ms(), error);
    }

    if (!ok) {
      std::cerr << "后台更新消息投递状态失败，消息 #" << task.message_id
                << "，接收者 " << task.recipient << "：" << error << '\n';
      continue;
    }

    if (task.decrement_unread) {
      adjust_redis_unread_best_effort(
          task.recipient,
          task.kind == DeliveryPersistKind::Private ? "private" : "group",
          -1);
    }
  }
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
      std::cerr << "Redis 在线状态刷新失败，用户：" << username << ": "
                << error << '\n';
      continue;
    }

    if (refreshed) {
      continue;
    }

    bool claimed = false;
    if (!redis_.claim_presence(username, server_instance_id_,
                               presence_ttl_seconds_, claimed, error)) {
      std::cerr << "Redis 在线状态回收失败，用户：" << username << ": "
                << error << '\n';
    } else if (!claimed) {
      std::cerr << "Redis 在线状态所有权异常，用户：" << username
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
    std::cerr << "Redis 在线状态占用失败，用户：" << username << ": " << error
              << '\n';

    connection->send("[error] Redis 在线状态服务不可用；"
                     "login cannot complete.\n");
    return false;
  }

  if (claimed) {
    return true;
  }

  std::optional<std::string> owner;
  if (!redis_.presence_owner(username, owner, error)) {
    std::cerr << "Redis 在线状态查询失败，用户：" << username << ": "
              << error << '\n';

    connection->send("[error] Redis 在线状态服务不可用；"
                     "login cannot complete.\n");
    return false;
  }

  if (owner && *owner == server_instance_id_) {
    bool refreshed = false;

    if (!redis_.refresh_presence_if_owned(username, server_instance_id_,
                                          presence_ttl_seconds_, refreshed,
                                          error) ||
        !refreshed) {
      std::cerr << "Redis 在线状态刷新失败，用户：" << username << ": "
                << error << '\n';

      connection->send("[error] Redis 在线状态刷新失败；"
                       "login cannot complete.\n");
      return false;
    }

    return true;
  }

  connection->send("[error] 该账号已经在线。"
                   "on another server instance.\n");
  return false;
}

void ChatServer::remove_redis_presence_best_effort(
    const std::string &username) {
  bool removed = false;
  std::string error;

  if (!redis_.remove_presence_if_owned(username, server_instance_id_, removed,
                                       error)) {
    std::cerr << "Redis 在线状态清理失败，用户：" << username << ": "
              << error << '\n';
  }
}

void ChatServer::adjust_redis_unread_best_effort(const std::string &username,
                                                 const std::string &kind,
                                                 std::int64_t delta) {
  std::int64_t result = 0;
  std::string error;

  if (!redis_.adjust_unread(username, kind, delta, result, error)) {
    std::cerr << "Redis 未读状态更新失败，用户：" << username
              << " kind=" << kind << ": " << error << '\n';
  }
}

void ChatServer::send_redis_unread_summary_best_effort(
    const TcpConnectionPtr &connection, const std::string &username) {
  RedisUnreadCounts counts;
  std::string error;

  if (!redis_.unread_counts(username, counts, error)) {
    std::cerr << "Redis 未读状态查询失败，用户：" << username << ": " << error
              << '\n';
    return;
  }

  if (counts.private_messages == 0 && counts.group_messages == 0 &&
      counts.private_files == 0 && counts.group_files == 0) {
    return;
  }

  connection->send(
      "[system] Redis 未读缓存：私聊消息=" +
      std::to_string(counts.private_messages) +
      "，群消息=" + std::to_string(counts.group_messages) +
      "，私聊文件=" + std::to_string(counts.private_files) +
      "，群文件=" + std::to_string(counts.group_files) +
      "。最终持久化投递状态仍以 MySQL 为准。\n");
}

void ChatServer::database_error(const TcpConnectionPtr &connection,
                                const std::string &operation,
                                const std::string &error) const {
  (void)operation;
  spdlog::error("数据库操作失败：{}", error);

  std::cerr << "数据库操作失败：" << error << '\n';

  connection->send("[error] 数据库操作失败：" + error + "\n");
}
