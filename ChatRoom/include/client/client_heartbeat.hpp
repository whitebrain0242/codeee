#pragma once

#include <chrono>
#include <cstdint>
#include <string>

class TlsClientTransport;

class ClientHeartbeat {
public:
    using Clock =
        std::chrono::steady_clock;

    ClientHeartbeat(
        std::chrono::milliseconds ping_interval =
            std::chrono::milliseconds(
                20'000
            ),
        std::chrono::milliseconds pong_timeout =
            std::chrono::milliseconds(
                60'000
            )
    );

    // Compatibility hook: normal server traffic no longer drives liveness.
    // Only the client-originated PING / matching PONG pair does.
    void note_server_activity();

    bool tick(
        TlsClientTransport& transport,
        std::string& error
    );

    bool consume_protocol_line(
        const std::string& line,
        TlsClientTransport& transport
    );

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
