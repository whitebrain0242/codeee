#pragma once

#include "config.hpp"
#include "minimuduo/net/TlsContext.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>

enum class TransportReadStatus { Data, Closed, Retry, Error };

struct TransportReadResult {
  TransportReadStatus status = TransportReadStatus::Error;
  std::size_t bytes = 0;
  std::string error;
};

class TlsClientTransport {
public:
  TlsClientTransport() = default;
  ~TlsClientTransport();

  TlsClientTransport(const TlsClientTransport &) = delete;
  TlsClientTransport &operator=(const TlsClientTransport &) = delete;

  bool connect(const std::string &ip, int port,
               const TlsClientConfig &config, std::string &error);

  // 同步发送：菜单控制、文件协议头等需要严格立即完成的操作使用。
  bool send(const std::string &data, std::string &error);

  // 非阻塞文本队列：聊天消息和心跳使用。
  bool queue_send(const std::string &data, std::string &error);
  bool flush_queued(std::string &error);
  bool has_pending_output() const noexcept;
  bool wants_write_event() const noexcept;
  std::size_t queued_output_bytes() const noexcept;

  bool send_file(const std::filesystem::path &path, std::uint64_t offset,
                 std::uint64_t byte_count, bool &used_zero_copy,
                 std::string &error);

  bool zero_copy_send_available() const noexcept;

  TransportReadResult receive(char *buffer, std::size_t capacity);

  int fd() const noexcept;
  int pending() const noexcept;
  std::string tls_version() const;
  std::string cipher_name() const;
  const std::string &peer_identity() const noexcept;
  bool connected() const noexcept;
  void shutdown();

private:
  enum class QueuedWriteWait {
    None,
    Read,
    Write
  };

  bool enqueue_output(const std::string &data, std::string &error);
  bool flush_queued_blocking(std::string &error);
  bool wait_for_socket(short events, int timeout_ms, std::string &error);

  minimuduo::net::TlsClientContext tls_context_;
  minimuduo::net::SslPtr ssl_;

  int socket_fd_ = -1;

  std::deque<std::string> output_queue_;
  std::size_t output_offset_ = 0U;
  std::size_t queued_output_bytes_ = 0U;
  QueuedWriteWait queued_write_wait_ = QueuedWriteWait::None;

  std::string peer_identity_;
  bool connected_ = false;
};
