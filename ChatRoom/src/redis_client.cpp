#include "integration/redis_client.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>

namespace {

using ReplyPtr = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;
// 错误信息提取
std::string reply_error_text(redisContext *context, const redisReply *reply) {
  if (reply != nullptr && reply->type == REDIS_REPLY_ERROR &&
      reply->str != nullptr) {
    return std::string(reply->str, reply->len);
  }

  if (context != nullptr && context->err != 0) {
    return context->errstr;
  }

  return "unknown Redis error";
}
// 检查回复是否是OK状态
bool is_ok_status(const redisReply *reply) {
  return reply != nullptr && reply->type == REDIS_REPLY_STATUS &&
         reply->str != nullptr && std::string(reply->str, reply->len) == "OK";
}

} // namespace

RedisClient::~RedisClient() {
  std::lock_guard<std::mutex> lock(mutex_);
  close_locked();
}

bool RedisClient::connect(const RedisConfig &config, std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = config;
  close_locked();
  return connect_locked(error);
}
// 心跳检测器：验证Redis是否还活着，当前TCP连接是否存活
bool RedisClient::ping(std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!ensure_connected_locked(error)) {
    return false;
  }

  ReplyPtr reply(static_cast<redisReply *>(redisCommand(context_, "PING")),
                 freeReplyObject);

  if (!reply || reply->type != REDIS_REPLY_STATUS || reply->str == nullptr ||
      std::string(reply->str, reply->len) != "PONG") {
    error = reply_error_text(context_, reply.get());
    close_locked();
    return false;
  }

  return true;
}
// 看看这个用户名有没有注册
bool RedisClient::claim_presence(const std::string &username,
                                 const std::string &server_instance_id,
                                 unsigned int ttl_seconds,
                                 bool &claimed, // 是不是属于当前服务器
                                 std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!ensure_connected_locked(error)) {
    return false;
  }

  const std::string key = presence_key(username);

  ReplyPtr reply(static_cast<redisReply *>(
                     redisCommand(context_, "SET %b %b NX EX %u", key.data(),
                                  key.size(), server_instance_id.data(),
                                  server_instance_id.size(), ttl_seconds)),
                 freeReplyObject);

  if (!reply) {
    error = reply_error_text(context_, nullptr);
    close_locked();
    return false;
  }

  if (reply->type == REDIS_REPLY_NIL) {
    claimed = false;
    return true;
  }

  if (!is_ok_status(reply.get())) {
    error = reply_error_text(context_, reply.get());
    return false;
  }

  claimed = true;
  return true;
}
// 刷新过期时间
bool RedisClient::refresh_presence_if_owned(
    const std::string &username, const std::string &server_instance_id,
    unsigned int ttl_seconds, bool &refreshed, std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!ensure_connected_locked(error)) {
    return false;
  }

  static constexpr const char *script =
      "if redis.call('get', KEYS[1]) == ARGV[1] "
      "then return redis.call('expire', KEYS[1], ARGV[2]) "
      "else return 0 end";

  const std::string key = presence_key(username);
  const std::string ttl = std::to_string(ttl_seconds);

  ReplyPtr reply(static_cast<redisReply *>(redisCommand(
                     context_, "EVAL %s 1 %b %b %b", script, key.data(),
                     key.size(), server_instance_id.data(),
                     server_instance_id.size(), ttl.data(), ttl.size())),
                 freeReplyObject);

  if (!reply || reply->type != REDIS_REPLY_INTEGER) {
    error = reply_error_text(context_, reply.get());
    if (!reply) {
      close_locked();
    }
    return false;
  }

  refreshed = reply->integer == 1;
  return true;
}
// 清除属于自己的键
bool RedisClient::remove_presence_if_owned(
    const std::string &username, const std::string &server_instance_id,
    bool &removed, std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!ensure_connected_locked(error)) {
    return false;
  }

  static constexpr const char *script =
      "if redis.call('get', KEYS[1]) == ARGV[1] "
      "then return redis.call('del', KEYS[1]) "
      "else return 0 end";

  const std::string key = presence_key(username);

  ReplyPtr reply(
      static_cast<redisReply *>(redisCommand(
          context_, "EVAL %s 1 %b %b", script, key.data(), key.size(),
          server_instance_id.data(), server_instance_id.size())),
      freeReplyObject);

  if (!reply || reply->type != REDIS_REPLY_INTEGER) {
    error = reply_error_text(context_, reply.get());
    if (!reply) {
      close_locked();
    }
    return false;
  }

  removed = reply->integer == 1;
  return true;
}
// 根据键查询值
bool RedisClient::presence_owner(const std::string &username,
                                 std::optional<std::string> &owner,
                                 std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!ensure_connected_locked(error)) {
    return false;
  }

  const std::string key = presence_key(username);

  ReplyPtr reply(static_cast<redisReply *>(
                     redisCommand(context_, "GET %b", key.data(), key.size())),
                 freeReplyObject);

  if (!reply) {
    error = reply_error_text(context_, nullptr);
    close_locked();
    return false;
  }

  if (reply->type == REDIS_REPLY_NIL) {
    owner.reset();
    return true;
  }

  if (reply->type != REDIS_REPLY_STRING || reply->str == nullptr) {
    error = reply_error_text(context_, reply.get());
    return false;
  }

  owner = std::string(reply->str, reply->len);
  return true;
}
// 调整未读计数
bool RedisClient::adjust_unread(const std::string &username,
                                const std::string &kind, std::int64_t delta,
                                std::int64_t &result, std::string &error) {
  if (!is_supported_unread_kind(kind)) {
    error = "unsupported Redis unread kind: " + kind;
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (!ensure_connected_locked(error)) {
    return false;
  }

  const std::string key = unread_key(username);

  ReplyPtr reply(static_cast<redisReply *>(redisCommand(
                     context_, "HINCRBY %b %b %lld", key.data(), key.size(),
                     kind.data(), kind.size(), static_cast<long long>(delta))),
                 freeReplyObject);

  if (!reply || reply->type != REDIS_REPLY_INTEGER) {
    error = reply_error_text(context_, reply.get());
    if (!reply) {
      close_locked();
    }
    return false;
  }

  result = std::max<std::int64_t>(0, static_cast<std::int64_t>(reply->integer));

  if (result == 0 && reply->integer < 0) {
    ReplyPtr clamp_reply(static_cast<redisReply *>(redisCommand(
                             context_, "HSET %b %b 0", key.data(), key.size(),
                             kind.data(), kind.size())),
                         freeReplyObject);

    if (!clamp_reply) {
      error = reply_error_text(context_, nullptr);
      close_locked();
      return false;
    }
  }

  ReplyPtr expire_reply(
      static_cast<redisReply *>(redisCommand(
          context_, "EXPIRE %b %d", key.data(), key.size(), 30 * 24 * 60 * 60)),
      freeReplyObject);

  if (!expire_reply) {
    error = reply_error_text(context_, nullptr);
    close_locked();
    return false;
  }

  return true;
}
// 拉取全部未读计数
bool RedisClient::unread_counts(const std::string &username,
                                RedisUnreadCounts &counts, std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!ensure_connected_locked(error)) {
    return false;
  }

  const std::string key = unread_key(username);

  ReplyPtr reply(static_cast<redisReply *>(redisCommand(
                     context_, "HMGET %b private group private_file group_file",
                     key.data(), key.size())),
                 freeReplyObject);

  if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements != 4U) {
    error = reply_error_text(context_, reply.get());
    if (!reply) {
      close_locked();
    }
    return false;
  }

  auto parse_count = [](const redisReply *item) -> std::int64_t {
    if (item == nullptr || item->type == REDIS_REPLY_NIL) {
      return 0;
    }

    if (item->type != REDIS_REPLY_STRING || item->str == nullptr) {
      return 0;
    }

    try {
      return std::max<std::int64_t>(
          0, std::stoll(std::string(item->str, item->len)));
    } catch (...) {
      return 0;
    }
  };

  counts.private_messages = parse_count(reply->element[0]);
  counts.group_messages = parse_count(reply->element[1]);
  counts.private_files = parse_count(reply->element[2]);
  counts.group_files = parse_count(reply->element[3]);

  return true;
}

bool RedisClient::clear_unread(const std::string &username,
                               std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!ensure_connected_locked(error)) {
    return false;
  }

  const std::string key = unread_key(username);

  ReplyPtr reply(
      static_cast<redisReply *>(
          redisCommand(
              context_,
              "DEL %b",
              key.data(),
              key.size())),
      freeReplyObject);

  if (!reply || reply->type != REDIS_REPLY_INTEGER) {
    error = reply_error_text(context_, reply.get());

    if (!reply) {
      close_locked();
    }

    return false;
  }

  return true;
}

// 建立底层连接
bool RedisClient::connect_locked(std::string &error) {
  const timeval timeout{
      static_cast<time_t>(config_.connect_timeout_ms / 1000U),
      static_cast<suseconds_t>((config_.connect_timeout_ms % 1000U) * 1000U)};

  context_ = redisConnectWithTimeout(config_.host.c_str(),
                                     static_cast<int>(config_.port), timeout);

  if (context_ == nullptr) {
    error = "redisConnectWithTimeout returned null";
    return false;
  }

  if (context_->err != 0) {
    error = context_->errstr;
    close_locked();
    return false;
  }

  return authenticate_and_select_locked(error);
}
// 检查context
bool RedisClient::ensure_connected_locked(std::string &error) {
  if (context_ != nullptr && context_->err == 0) {
    return true;
  }

  close_locked();
  return connect_locked(error);
}
// 认证密码和切换数据库
bool RedisClient::authenticate_and_select_locked(std::string &error) {
  if (!config_.password.empty()) {
    ReplyPtr auth_reply(static_cast<redisReply *>(redisCommand(
                            context_, "AUTH %b", config_.password.data(),
                            config_.password.size())),
                        freeReplyObject);

    if (!is_ok_status(auth_reply.get())) {
      error = reply_error_text(context_, auth_reply.get());
      close_locked();
      return false;
    }
  }

  ReplyPtr select_reply(static_cast<redisReply *>(redisCommand(
                            context_, "SELECT %u", config_.database)),
                        freeReplyObject);

  if (!is_ok_status(select_reply.get())) {
    error = reply_error_text(context_, select_reply.get());
    close_locked();
    return false;
  }

  return true;
}
// 直接释放内存
void RedisClient::close_locked() {
  if (context_ != nullptr) {
    redisFree(context_);
    context_ = nullptr;
  }
}
// 字符串拼接
std::string RedisClient::presence_key(const std::string &username) const {
  return config_.key_prefix + ":presence:" + username;
}
// 生成键名
std::string RedisClient::unread_key(const std::string &username) const {
  return config_.key_prefix + ":unread:" + username;
}
// 拒绝执行错误字符串
bool RedisClient::is_supported_unread_kind(const std::string &kind) {
  return kind == "private" || kind == "group" || kind == "private_file" ||
         kind == "group_file";
}
