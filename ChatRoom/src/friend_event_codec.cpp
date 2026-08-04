#include "friend_event_codec.hpp"

#include <limits>

namespace chat {
namespace {

constexpr std::uint32_t kWireVarint = 0;
constexpr std::uint32_t kWireLengthDelimited = 2;

constexpr std::uint32_t kTypeField = 1;
constexpr std::uint32_t kActorField = 2;
constexpr std::uint32_t kTargetField = 3;
constexpr std::uint32_t kOccurredAtField = 4;

constexpr std::size_t kMaxStringLength = 1024;

bool is_known_event_type(std::uint64_t value) {
    return
        value <= static_cast<std::uint64_t>(
            FriendEventType::FriendRemoved
        );
}

}  // namespace

bool FriendEventCodec::serialize(
    const FriendEvent& event,
    std::string& output,
    std::string& error
) {
    output.clear();
    error.clear();

    if (
        event.type ==
        FriendEventType::Unspecified
    ) {
        error =
            "friend event type cannot be "
            "unspecified";
        return false;
    }

    if (
        event.actor_username.empty() ||
        event.target_username.empty()
    ) {
        error =
            "friend event usernames cannot be "
            "empty";
        return false;
    }

    if (
        event.actor_username.size() >
            kMaxStringLength ||
        event.target_username.size() >
            kMaxStringLength
    ) {
        error =
            "friend event username exceeds "
            "codec limit";
        return false;
    }

    append_varint(
        (kTypeField << 3U) | kWireVarint,
        output
    );

    append_varint(
        static_cast<std::uint32_t>(event.type),
        output
    );

    append_string_field(
        kActorField,
        event.actor_username,
        output
    );

    append_string_field(
        kTargetField,
        event.target_username,
        output
    );

    append_varint(
        (kOccurredAtField << 3U) |
            kWireVarint,
        output
    );

    append_varint(
        static_cast<std::uint64_t>(
            event.occurred_at_unix_ms
        ),
        output
    );

    return true;
}

bool FriendEventCodec::deserialize(
    const std::string& input,
    FriendEvent& event,
    std::string& error
) {
    event = {};
    error.clear();

    std::size_t offset = 0;

    while (offset < input.size()) {
        std::uint64_t tag = 0;

        if (!read_varint(input, offset, tag)) {
            error = "invalid Protobuf field tag";
            return false;
        }

        const std::uint32_t field_number =
            static_cast<std::uint32_t>(
                tag >> 3U
            );

        const std::uint32_t wire_type =
            static_cast<std::uint32_t>(
                tag & 0x07U
            );

        if (field_number == 0) {
            error =
                "Protobuf field number cannot "
                "be zero";
            return false;
        }

        if (
            field_number == kTypeField &&
            wire_type == kWireVarint
        ) {
            std::uint64_t value = 0;

            if (
                !read_varint(
                    input,
                    offset,
                    value
                ) ||
                !is_known_event_type(value)
            ) {
                error =
                    "invalid friend event type";
                return false;
            }

            event.type =
                static_cast<FriendEventType>(
                    value
                );

            continue;
        }

        if (
            (
                field_number == kActorField ||
                field_number == kTargetField
            ) &&
            wire_type == kWireLengthDelimited
        ) {
            std::uint64_t length = 0;

            if (
                !read_varint(
                    input,
                    offset,
                    length
                ) ||
                length > kMaxStringLength ||
                length >
                    input.size() - offset
            ) {
                error =
                    "invalid Protobuf string "
                    "field";
                return false;
            }

            const std::string value =
                input.substr(
                    offset,
                    static_cast<std::size_t>(
                        length
                    )
                );

            offset +=
                static_cast<std::size_t>(
                    length
                );

            if (field_number == kActorField) {
                event.actor_username = value;
            } else {
                event.target_username = value;
            }

            continue;
        }

        if (
            field_number ==
                kOccurredAtField &&
            wire_type == kWireVarint
        ) {
            std::uint64_t value = 0;

            if (
                !read_varint(
                    input,
                    offset,
                    value
                ) ||
                value >
                    static_cast<std::uint64_t>(
                        std::numeric_limits<
                            std::int64_t
                        >::max()
                    )
            ) {
                error =
                    "invalid friend event "
                    "timestamp";
                return false;
            }

            event.occurred_at_unix_ms =
                static_cast<std::int64_t>(
                    value
                );

            continue;
        }

        if (
            !skip_field(
                wire_type,
                input,
                offset
            )
        ) {
            error =
                "unsupported or invalid "
                "Protobuf field";
            return false;
        }
    }

    if (
        event.type ==
            FriendEventType::Unspecified ||
        event.actor_username.empty() ||
        event.target_username.empty()
    ) {
        error =
            "required friend event data is "
            "missing";
        return false;
    }

    return true;
}

void FriendEventCodec::append_varint(
    std::uint64_t value,
    std::string& output
) {
    while (value >= 0x80U) {
        output.push_back(
            static_cast<char>(
                (value & 0x7FU) | 0x80U
            )
        );

        value >>= 7U;
    }

    output.push_back(
        static_cast<char>(value)
    );
}

bool FriendEventCodec::read_varint(
    const std::string& input,
    std::size_t& offset,
    std::uint64_t& value
) {
    value = 0;
    unsigned int shift = 0;

    while (
        offset < input.size() &&
        shift <= 63U
    ) {
        const auto byte =
            static_cast<unsigned char>(
                input[offset++]
            );

        value |=
            static_cast<std::uint64_t>(
                byte & 0x7FU
            ) << shift;

        if ((byte & 0x80U) == 0U) {
            return true;
        }

        shift += 7U;
    }

    return false;
}

void FriendEventCodec::append_string_field(
    std::uint32_t field_number,
    const std::string& value,
    std::string& output
) {
    append_varint(
        (field_number << 3U) |
            kWireLengthDelimited,
        output
    );

    append_varint(value.size(), output);
    output.append(value);
}

bool FriendEventCodec::skip_field(
    std::uint32_t wire_type,
    const std::string& input,
    std::size_t& offset
) {
    if (wire_type == kWireVarint) {
        std::uint64_t ignored = 0;
        return read_varint(
            input,
            offset,
            ignored
        );
    }

    if (wire_type == kWireLengthDelimited) {
        std::uint64_t length = 0;

        if (
            !read_varint(
                input,
                offset,
                length
            ) ||
            length > input.size() - offset
        ) {
            return false;
        }

        offset +=
            static_cast<std::size_t>(length);

        return true;
    }

    return false;
}

}  // namespace chat
