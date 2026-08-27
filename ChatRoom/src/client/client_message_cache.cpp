#include "client/client_message_cache.hpp"

#include "client/client_common.hpp"

#include <iostream>

bool parse_private_message_line(const std::string &line,
                                const std::string &active_username,
                                LocalPrivateMessage &message) {
  if (active_username.empty()) {
    return false;
  }

  bool offline = false;
  std::size_t id_begin = 0U;

  if (starts_with(line, "[offline #")) {
    offline = true;

    id_begin = std::string("[offline #").size();
  } else if (starts_with(line, "[#") && !starts_with(line, "[#G")) {
    id_begin = 2U;
  } else {
    return false;
  }

  const std::size_t id_end = line.find(']', id_begin);

  if (id_end == std::string::npos ||
      !parse_uint64(line.substr(id_begin, id_end - id_begin),
                    message.server_message_id)) {
    return false;
  }

  const std::string from_marker = " [private from ";
  const std::string legacy_from_marker = " [private，来源：";
  const std::string to_marker = " [private to ";

  const std::size_t marker_begin = id_end + 1U;

  bool outgoing = false;
  std::size_t name_begin = 0U;

  if (line.compare(marker_begin, from_marker.size(), from_marker) == 0) {
    name_begin = marker_begin + from_marker.size();
  } else if (line.compare(marker_begin, legacy_from_marker.size(),
                          legacy_from_marker) == 0) {
    // 兼容 V4 中误把协议标记中文化的服务端，正文仍然会正常 percent-decode。
    name_begin = marker_begin + legacy_from_marker.size();
  } else if (line.compare(marker_begin, to_marker.size(), to_marker) == 0) {
    outgoing = true;

    name_begin = marker_begin + to_marker.size();
  } else {
    return false;
  }

  const std::size_t name_end = line.find("] ", name_begin);

  if (name_end == std::string::npos) {
    return false;
  }

  const std::string peer = line.substr(name_begin, name_end - name_begin);

  if (peer.empty()) {
    return false;
  }

  message.account_username = active_username;

  message.peer_username = peer;

  message.outgoing = outgoing;

  message.offline_delivery = offline;

  message.received_at_unix_ms = client_now_unix_ms();

  {
    const std::string encoded_content = line.substr(name_end + 2U);
    std::string decode_error;
    if (!decode_text_token(encoded_content, message.content, decode_error)) {
      // 兼容旧服务端的未编码单行消息；新多行消息统一 percent-encoded。
      message.content = encoded_content;
    }
  }

  if (outgoing) {
    message.sender_username = active_username;

    message.recipient_username = peer;
  } else {
    message.sender_username = peer;

    message.recipient_username = active_username;
  }

  return true;
}

bool parse_group_message_line(const std::string &line,
                              const std::string &active_username,
                              LocalGroupMessage &message) {
  if (active_username.empty()) {
    return false;
  }

  bool offline = false;
  std::size_t id_begin = 0U;

  if (starts_with(line, "[offline #G")) {
    offline = true;

    id_begin = std::string("[offline #G").size();
  } else if (starts_with(line, "[#G")) {
    id_begin = 3U;
  } else {
    return false;
  }

  const std::size_t id_end = line.find(']', id_begin);

  if (id_end == std::string::npos ||
      !parse_uint64(line.substr(id_begin, id_end - id_begin),
                    message.server_message_id)) {
    return false;
  }

  const std::string group_marker = " [group ";

  const std::size_t group_begin = id_end + 1U;

  if (line.compare(group_begin, group_marker.size(), group_marker) != 0) {
    return false;
  }

  const std::size_t group_name_begin = group_begin + group_marker.size();

  const std::size_t group_name_end = line.find("] [", group_name_begin);

  if (group_name_end == std::string::npos) {
    return false;
  }

  const std::size_t sender_begin = group_name_end + 3U;

  const std::size_t sender_end = line.find("] ", sender_begin);

  if (sender_end == std::string::npos) {
    return false;
  }

  message.account_username = active_username;

  message.group_name =
      line.substr(group_name_begin, group_name_end - group_name_begin);

  message.sender_username =
      line.substr(sender_begin, sender_end - sender_begin);

  {
    const std::string encoded_content = line.substr(sender_end + 2U);
    std::string decode_error;
    if (!decode_text_token(encoded_content, message.content, decode_error)) {
      message.content = encoded_content;
    }
  }

  message.received_at_unix_ms = client_now_unix_ms();

  message.outgoing = message.sender_username == active_username;

  message.offline_delivery = offline;

  return !message.group_name.empty() && !message.sender_username.empty();
}

void cache_server_message(const std::string &line, const ClientState &state,
                          SqliteClient &cache) {
  if (state.active_username.empty()) {
    return;
  }

  std::string error;

  LocalPrivateMessage private_message;

  if (parse_private_message_line(line, state.active_username,
                                 private_message)) {
    if (!cache.cache_private_message(private_message, error)) {
      std::cerr << "[本地 SQLite 错误] " << error << '\n';
    }

    return;
  }

  LocalGroupMessage group_message;

  if (parse_group_message_line(line, state.active_username, group_message)) {
    if (!cache.cache_group_message(group_message, error)) {
      std::cerr << "[本地 SQLite 错误] " << error << '\n';
    }
  }
}


bool display_chat_message_line(const std::string &line,
                               const ClientState &state) {
  if (state.active_username.empty()) return false;

  LocalPrivateMessage private_message;
  if (parse_private_message_line(line, state.active_username, private_message)) {
    std::cout << (private_message.offline_delivery ? "[离线消息 #" : "[消息 #")
              << private_message.server_message_id << "] [私聊"
              << (private_message.outgoing ? "发给 " : "来自 ")
              << private_message.peer_username << "] "
              << private_message.content << '\n';
    return true;
  }

  LocalGroupMessage group_message;
  if (parse_group_message_line(line, state.active_username, group_message)) {
    std::cout << (group_message.offline_delivery ? "[离线群消息 #G" : "[群消息 #G")
              << group_message.server_message_id << "] [群 "
              << group_message.group_name << "] [发送者 "
              << group_message.sender_username << "] "
              << group_message.content << '\n';
    return true;
  }
  return false;
}


AsyncMessageCacheWriter::~AsyncMessageCacheWriter() { stop(); }

void AsyncMessageCacheWriter::start(SqliteClient &cache) {
  stop();

  {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_ = &cache;
    stopping_ = false;
    active_ = false;
  }

  worker_ = std::thread([this] { worker_loop(); });
}

void AsyncMessageCacheWriter::enqueue(LocalPrivateMessage message) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || cache_ == nullptr) return;
    jobs_.emplace_back(std::move(message));
  }
  cv_.notify_one();
}

void AsyncMessageCacheWriter::enqueue(LocalGroupMessage message) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || cache_ == nullptr) return;
    jobs_.emplace_back(std::move(message));
  }
  cv_.notify_one();
}

void AsyncMessageCacheWriter::flush() {
  std::unique_lock<std::mutex> lock(mutex_);
  drained_cv_.wait(lock, [this] { return jobs_.empty() && !active_; });
}

void AsyncMessageCacheWriter::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!worker_.joinable()) {
      jobs_.clear();
      cache_ = nullptr;
      stopping_ = false;
      active_ = false;
      return;
    }
    stopping_ = true;
  }

  cv_.notify_all();
  worker_.join();

  std::lock_guard<std::mutex> lock(mutex_);
  jobs_.clear();
  cache_ = nullptr;
  stopping_ = false;
  active_ = false;
}

std::size_t AsyncMessageCacheWriter::pending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return jobs_.size() + (active_ ? 1U : 0U);
}

void AsyncMessageCacheWriter::worker_loop() {
  while (true) {
    Job job;

    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });

      if (jobs_.empty()) {
        if (stopping_) break;
        continue;
      }

      job = std::move(jobs_.front());
      jobs_.pop_front();
      active_ = true;
    }

    std::string error;
    bool ok = true;

    if (std::holds_alternative<LocalPrivateMessage>(job)) {
      ok = cache_->cache_private_message(
          std::get<LocalPrivateMessage>(job), error);
    } else {
      ok = cache_->cache_group_message(
          std::get<LocalGroupMessage>(job), error);
    }

    if (!ok) {
      std::cerr << "[本地 SQLite 后台写入错误] " << error << '\n';
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_ = false;
      if (jobs_.empty()) drained_cv_.notify_all();
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
    drained_cv_.notify_all();
  }
}
