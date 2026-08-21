#include "file_transfer_service.hpp"
#include "file_utils.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "chatroom_v8_4_resume_storage_test";

    std::error_code ignored;
    std::filesystem::remove_all(
        root,
        ignored
    );

    FileTransferService service(
        root,
        1U
    );

    std::string error;

    if (!service.initialize(error)) {
        std::cerr
            << error
            << '\n';
        return EXIT_FAILURE;
    }

    const std::string token =
        "0123456789abcdef0123456789abcdef";

    const std::vector<unsigned char> first{
        'h', 'e', 'l', 'l', 'o', ' '
    };

    const std::vector<unsigned char> second{
        'r', 'e', 's', 'u', 'm', 'e'
    };

    const std::vector<unsigned char> complete{
        'h', 'e', 'l', 'l', 'o', ' ',
        'r', 'e', 's', 'u', 'm', 'e'
    };

    const std::filesystem::path expected_source =
        root / "expected.bin";

    {
        std::ofstream output(
            expected_source,
            std::ios::binary |
                std::ios::trunc
        );

        output.write(
            reinterpret_cast<const char*>(
                complete.data()
            ),
            static_cast<std::streamsize>(
                complete.size()
            )
        );
    }

    std::string expected_sha;

    if (!fileutil::sha256_file_hex(
            expected_source,
            expected_sha,
            error
        )) {
        std::cerr
            << error
            << '\n';
        return EXIT_FAILURE;
    }

    FileUploadResumeState requested;

    FileTransferMetadata* metadata =
        requested.mutable_metadata();

    metadata->set_transfer_token(
        token
    );
    metadata->set_scope(
        chatroom::v9::FILE_TRANSFER_GROUP
    );
    metadata->set_sender_username(
        "alice"
    );
    metadata->set_group_id(42U);
    metadata->set_group_name(
        "cpp"
    );
    metadata->set_file_name(
        "hello resume.txt"
    );
    metadata->set_file_size(
        static_cast<std::uint64_t>(
            complete.size()
        )
    );
    metadata->set_sha256_hex(
        expected_sha
    );

    requested.add_recipient_usernames(
        "bob"
    );
    requested.add_recipient_usernames(
        "charlie"
    );

    FileUploadResumeState persisted;
    std::filesystem::path temp_path;
    std::uint64_t accepted_offset = 0U;

    if (!service.begin_or_resume_upload(
            requested,
            persisted,
            temp_path,
            accepted_offset,
            error
        ) ||
        accepted_offset != 0U ||
        persisted.recipient_usernames_size() !=
            2) {
        std::cerr
            << "initial resume state failed: "
            << error
            << '\n';
        return EXIT_FAILURE;
    }

    if (!service.append_upload_chunk(
            temp_path,
            0U,
            first,
            accepted_offset,
            error
        ) ||
        accepted_offset !=
            first.size()) {
        std::cerr
            << "first partial upload failed: "
            << error
            << '\n';
        return EXIT_FAILURE;
    }

    // Simulate reconnect/restart of ChatServer business state:
    // same service storage is queried again with the same token+identity.
    FileUploadResumeState second_request =
        requested;

    // Membership changed while disconnected. The persisted recipient
    // snapshot must win instead of recalculating delivery recipients.
    second_request.clear_recipient_usernames();
    second_request.add_recipient_usernames(
        "david"
    );

    FileUploadResumeState resumed_state;
    std::filesystem::path resumed_path;
    std::uint64_t resumed_offset = 0U;

    if (!service.begin_or_resume_upload(
            second_request,
            resumed_state,
            resumed_path,
            resumed_offset,
            error
        ) ||
        resumed_path != temp_path ||
        resumed_offset !=
            first.size() ||
        resumed_state.recipient_usernames_size() !=
            2 ||
        resumed_state.recipient_usernames(0) !=
            "bob" ||
        resumed_state.recipient_usernames(1) !=
            "charlie") {
        std::cerr
            << "resume checkpoint restore failed: "
            << error
            << '\n';
        return EXIT_FAILURE;
    }

    if (!service.append_upload_chunk(
            resumed_path,
            resumed_offset,
            second,
            accepted_offset,
            error
        ) ||
        accepted_offset !=
            complete.size()) {
        std::cerr
            << "resumed chunk append failed: "
            << error
            << '\n';
        return EXIT_FAILURE;
    }

    std::string relative_path;

    if (!service.finalize_upload(
            resumed_path,
            token,
            "../../hello resume.txt",
            static_cast<std::uint64_t>(
                complete.size()
            ),
            expected_sha,
            relative_path,
            error
        )) {
        std::cerr
            << "resume finalize failed: "
            << error
            << '\n';
        return EXIT_FAILURE;
    }

    const std::filesystem::path final_path =
        root / relative_path;

    std::string actual_sha;

    if (!std::filesystem::is_regular_file(
            final_path
        ) ||
        !fileutil::sha256_file_hex(
            final_path,
            actual_sha,
            error
        ) ||
        actual_sha != expected_sha ||
        final_path.filename().string().find(
            "hello_resume.txt"
        ) == std::string::npos ||
        std::filesystem::exists(
            root /
            "tmp" /
            (token + ".resume.pb")
        )) {
        std::cerr
            << "final resume verification failed: "
            << error
            << '\n';
        return EXIT_FAILURE;
    }

    service.stop();

    std::filesystem::remove_all(
        root,
        ignored
    );

    std::cout
        << "resumable file storage tests passed\n";

    return EXIT_SUCCESS;
}
