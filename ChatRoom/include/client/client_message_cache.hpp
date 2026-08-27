#pragma once

#include "client/client_state.hpp"
#include "integration/sqlite_client.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <variant>

bool parse_private_message_line(
    const std::string& line,
    const std::string& active_username,
    LocalPrivateMessage& message
);

bool parse_group_message_line(
    const std::string& line,
    const std::string& active_username,
    LocalGroupMessage& message
);

void cache_server_message(
    const std::string& line,
    const ClientState& state,
    SqliteClient& cache
);

bool display_chat_message_line(
    const std::string& line,
    const ClientState& state
);



// 接收热路径专用：消息正文先显示，SQLite 在后台线程落库。
// 这样一秒内突发大量消息时，本地磁盘写入不会阻塞 TLS 接收和心跳。
class AsyncMessageCacheWriter {
public:
  AsyncMessageCacheWriter() = default;
  ~AsyncMessageCacheWriter();

  AsyncMessageCacheWriter(const AsyncMessageCacheWriter &) = delete;
  AsyncMessageCacheWriter &operator=(const AsyncMessageCacheWriter &) = delete;

  void start(SqliteClient &cache);
  void enqueue(LocalPrivateMessage message);
  void enqueue(LocalGroupMessage message);

  // 等待当前队列全部写完，但不停止后台线程。
  void flush();

  // 排空队列并停止线程。
  void stop();

  std::size_t pending() const;

private:
  using Job = std::variant<LocalPrivateMessage, LocalGroupMessage>;

  void worker_loop();

  SqliteClient *cache_ = nullptr;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable drained_cv_;
  std::deque<Job> jobs_;
  std::thread worker_;
  bool stopping_ = false;
  bool active_ = false;
};
