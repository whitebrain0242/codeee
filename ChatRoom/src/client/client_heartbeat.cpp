#include "client/client_heartbeat.hpp"

#include "client/client_common.hpp"
#include "client/tls_client_transport.hpp"
#include "protocol.hpp"

#include <cstdint>

ClientHeartbeat::ClientHeartbeat(std::chrono::milliseconds ping_interval,
                                 std::chrono::milliseconds pong_timeout)
    : ping_interval_(ping_interval), pong_timeout_(pong_timeout),
      last_ping_(Clock::now()), pending_since_(Clock::now()) {}


bool ClientHeartbeat::tick(TlsClientTransport &transport, std::string &error) {
  const Clock::time_point now = Clock::now();
  //检测是不是在等PONG
  if (waiting_for_pong_) {
    //检测时间是否超过60秒
    if (now - pending_since_ >= pong_timeout_) {
      error = "服务端未响应客户端 TCP 心跳 PING";
      return false;
    }

    return true;
  }
  //距离上次发送PING的间隔时间有没有20秒
  if (now - last_ping_ < ping_interval_) {
    return true;
  }
  //生成唯一序列号
  const std::uint64_t nonce = next_nonce_++;

  if (!transport.queue_send("PING " + std::to_string(nonce) + "\n", error)) {
    return false;
  }

  last_ping_ = now;//记录发送时间
  pending_since_ = now;//开始等待PONG的时间
  pending_nonce_ = nonce;//记录本次的NOnce
  waiting_for_pong_ = true;//标记正在等待
  return true;
}

bool ClientHeartbeat::consume_protocol_line(const std::string &line,
                                            TlsClientTransport &transport) {
  (void)transport;

  const Command command = parse_command(line);

  if (command.name == "PONG") {
    std::uint64_t nonce = 0U;

    if (parse_uint64(command.raw_arguments, nonce) && waiting_for_pong_ &&
        nonce == pending_nonce_) {
      waiting_for_pong_ = false;
      pending_nonce_ = 0U;
    }

    return true;
  }

  if (command.name == "PING") {
    // The revised protocol is client-initiated only. Consume any
    // unexpected legacy server PING without treating it as user data.
    return true;
  }

  return false;
}

bool ClientHeartbeat::timed_out() const {
  return waiting_for_pong_ && Clock::now() - pending_since_ >= pong_timeout_;
}
