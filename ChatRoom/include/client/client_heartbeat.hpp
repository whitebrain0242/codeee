#pragma once

#include <chrono>
#include <cstdint>
#include <string>

class TlsClientTransport;

class ClientHeartbeat {
public:
  using Clock = std::chrono::steady_clock;

  ClientHeartbeat(std::chrono::milliseconds ping_interval =
                      std::chrono::milliseconds(20'000),//发送心跳包的间隔时间
                  std::chrono::milliseconds pong_timeout =
                      std::chrono::milliseconds(60'000));//等待PONG的超时时间

  bool tick(TlsClientTransport &transport, std::string &error);//定期发送PING

  bool consume_protocol_line(const std::string &line,
                             TlsClientTransport &transport);

  bool timed_out() const;

private:
  std::chrono::milliseconds ping_interval_;
  std::chrono::milliseconds pong_timeout_;

  Clock::time_point last_ping_;
  Clock::time_point pending_since_;

  std::uint64_t next_nonce_ = 1U;
  std::uint64_t pending_nonce_ = 0U;
  bool waiting_for_pong_ = false;
};
