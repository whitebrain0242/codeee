#include "friend_event_codec.hpp"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string to_hex(const std::string& value) {
    std::ostringstream output;

    output << std::hex << std::setfill('0');

    for (const unsigned char byte : value) {
        output
            << std::setw(2)
            << static_cast<unsigned int>(byte);
    }

    return output.str();
}

bool expect(
    bool condition,
    const std::string& name
) {
    std::cout
        << name
        << ": "
        << (condition ? "PASS" : "FAIL")
        << '\n';

    return condition;
}

}  // namespace

int main() {
    const chat::FriendEvent event{
        chat::FriendEventType::RequestSent,
        "alice",
        "bob",
        123
    };

    std::string encoded;
    std::string error;

    if (
        !expect(
            chat::FriendEventCodec::serialize(
                event,
                encoded,
                error
            ),
            "serialize friend event"
        )
    ) {
        std::cerr << error << '\n';
        return 1;
    }

    if (
        !expect(
            to_hex(encoded) ==
                "08011205616c696365"
                "1a03626f62207b",
            "matches proto3 wire bytes"
        )
    ) {
        std::cerr
            << "actual: "
            << to_hex(encoded)
            << '\n';
        return 1;
    }

    chat::FriendEvent decoded;

    if (
        !expect(
            chat::FriendEventCodec::deserialize(
                encoded,
                decoded,
                error
            ),
            "deserialize friend event"
        )
    ) {
        std::cerr << error << '\n';
        return 1;
    }

    if (
        !expect(
            decoded.type == event.type &&
            decoded.actor_username ==
                event.actor_username &&
            decoded.target_username ==
                event.target_username &&
            decoded.occurred_at_unix_ms ==
                event.occurred_at_unix_ms,
            "round trip preserves fields"
        )
    ) {
        return 1;
    }

    std::string with_unknown = encoded;
    with_unknown.push_back(
        static_cast<char>((9U << 3U) | 0U)
    );
    with_unknown.push_back(
        static_cast<char>(1)
    );

    if (
        !expect(
            chat::FriendEventCodec::deserialize(
                with_unknown,
                decoded,
                error
            ),
            "unknown field is skipped"
        )
    ) {
        std::cerr << error << '\n';
        return 1;
    }

    return 0;
}
