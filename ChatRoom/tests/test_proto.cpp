#include "proto_types.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

int main() {
    FriendEventPayload event;
    event.set_type(
        chatroom::v7::FRIEND_REQUEST_SENT
    );
    event.set_actor_username("alice");
    event.set_target_username("bob");
    event.set_occurred_at_unix_ms(123);

    std::string bytes;

    if (!event.SerializeToString(&bytes)) {
        std::cerr
            << "official protobuf friend serialization failed\n";
        return EXIT_FAILURE;
    }

    FriendEventPayload decoded;

    if (!decoded.ParseFromString(bytes) ||
        decoded.type() !=
            chatroom::v7::FRIEND_REQUEST_SENT ||
        decoded.actor_username() != "alice" ||
        decoded.target_username() != "bob" ||
        decoded.occurred_at_unix_ms() != 123) {
        std::cerr
            << "official protobuf friend round-trip failed\n";
        return EXIT_FAILURE;
    }

    // This is the historical v7.x wire payload produced by the previous
    // lightweight codec. Official protobuf must still parse it so existing
    // MySQL BLOB history remains readable.
    const std::string historical_friend_bytes(
        "\x08\x01\x12\x05"
        "alice"
        "\x1a\x03"
        "bob"
        "\x20\x7b",
        16
    );

    FriendEventPayload historical;

    if (!historical.ParseFromString(
            historical_friend_bytes
        ) ||
        historical.actor_username() != "alice" ||
        historical.target_username() != "bob" ||
        historical.occurred_at_unix_ms() != 123) {
        std::cerr
            << "historical protobuf compatibility failed\n";
        return EXIT_FAILURE;
    }

    ChatMessagePayload chat;
    chat.set_type(chatroom::v7::PRIVATE);
    chat.set_sender_username("alice");
    chat.set_recipient_username("bob");
    chat.set_content("hello");
    chat.set_created_at_unix_ms(456);

    bytes.clear();

    ChatMessagePayload decoded_chat;

    if (!chat.SerializeToString(&bytes) ||
        !decoded_chat.ParseFromString(bytes) ||
        decoded_chat.content() != "hello" ||
        decoded_chat.type() !=
            chatroom::v7::PRIVATE) {
        std::cerr
            << "official chat protobuf round-trip failed\n";
        return EXIT_FAILURE;
    }

    GroupMessagePayload group;
    group.set_group_id(42);
    group.set_group_name("cpp");
    group.set_sender_username("alice");
    group.set_content("group hello");
    group.set_created_at_unix_ms(789);

    bytes.clear();

    GroupMessagePayload decoded_group;

    if (!group.SerializeToString(&bytes) ||
        !decoded_group.ParseFromString(bytes) ||
        decoded_group.group_id() != 42U ||
        decoded_group.group_name() != "cpp") {
        std::cerr
            << "official group protobuf round-trip failed\n";
        return EXIT_FAILURE;
    }

    FileTransferMetadata file;
    file.set_transfer_token(
        "0123456789abcdef0123456789abcdef"
    );
    file.set_scope(
        chatroom::v9::FILE_TRANSFER_PRIVATE
    );
    file.set_sender_username("alice");
    file.set_recipient_username("bob");
    file.set_file_name("note.txt");
    file.set_file_size(1234);
    file.set_sha256_hex(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    );
    file.set_created_at_unix_ms(999);
    file.set_stored_relative_path(
        "files/token_note.txt"
    );

    bytes.clear();

    FileTransferMetadata decoded_file;

    if (!file.SerializeToString(&bytes) ||
        !decoded_file.ParseFromString(bytes) ||
        decoded_file.scope() !=
            chatroom::v9::FILE_TRANSFER_PRIVATE ||
        decoded_file.file_name() != "note.txt" ||
        decoded_file.file_size() != 1234U) {
        std::cerr
            << "official file protobuf round-trip failed\n";
        return EXIT_FAILURE;
    }


FileUploadResumeState resume;
*resume.mutable_metadata() =
    file;
resume.add_recipient_usernames(
    "bob"
);
resume.add_recipient_usernames(
    "charlie"
);

bytes.clear();

FileUploadResumeState
    decoded_resume;

if (!resume.SerializeToString(
        &bytes
    ) ||
    !decoded_resume.ParseFromString(
        bytes
    ) ||
    !decoded_resume.has_metadata() ||
    decoded_resume.metadata()
            .transfer_token() !=
        file.transfer_token() ||
    decoded_resume
            .recipient_usernames_size() !=
        2 ||
    decoded_resume
            .recipient_usernames(1) !=
        "charlie") {
    std::cerr
        << "official upload-resume protobuf round-trip failed\n";
    return EXIT_FAILURE;
}

    std::cout
        << "official protobuf tests passed\n";
    return EXIT_SUCCESS;
}
