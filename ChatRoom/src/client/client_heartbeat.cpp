#include "client/client_heartbeat.hpp"

#include "client/client_common.hpp"
#include "client/tls_client_transport.hpp"
#include "protocol.hpp"

#include <cstdint>

ClientHeartbeat::ClientHeartbeat(
    std::chrono::milliseconds ping_interval,
    std::chrono::milliseconds pong_timeout
)
    : ping_interval_(ping_interval),
      pong_timeout_(pong_timeout),
      last_ping_(Clock::now()),
      pending_since_(Clock::now()) {}

void ClientHeartbeat::note_server_activity() {
    // Intentionally empty. Business/application traffic must not be used
    // as heartbeat evidence. Liveness is based on client PING -> server PONG.
}

bool ClientHeartbeat::tick(
    TlsClientTransport& transport,
    std::string& error
) {
    const Clock::time_point now =
        Clock::now();

    if (waiting_for_pong_) {
        if (now - pending_since_ >=
            pong_timeout_) {
            error =
                "server did not answer client TCP heartbeat PING";
            return false;
        }

        return true;
    }

    if (now - last_ping_ <
        ping_interval_) {
        return true;
    }

    const std::uint64_t nonce =
        next_nonce_++;

    if (!transport.send(
            "PING " +
                std::to_string(nonce) +
                "\n",
            error
        )) {
        return false;
    }

    last_ping_ = now;
    pending_since_ = now;
    pending_nonce_ = nonce;
    waiting_for_pong_ = true;
    return true;
}

bool ClientHeartbeat::consume_protocol_line(
    const std::string& line,
    TlsClientTransport& transport
) {
    (void)transport;

    const Command command =
        parse_command(line);

    if (command.name == "PONG") {
        std::uint64_t nonce = 0U;

        if (parse_uint64(
                command.raw_arguments,
                nonce
            ) &&
            waiting_for_pong_ &&
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
    return
        waiting_for_pong_ &&
        Clock::now() - pending_since_ >=
            pong_timeout_;
}
