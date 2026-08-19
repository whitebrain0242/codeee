#include "file_transfer_service.hpp"

#include "minimuduo/net/TcpConnection.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <system_error>
#include <utility>

namespace {

constexpr std::size_t kFileChunkBytes =
    3072U;

std::string scope_text(
    chatroom::v9::FileTransferScope scope
) {
    switch (scope) {
        case chatroom::v9::FILE_TRANSFER_PRIVATE:
            return "PRIVATE";

        case chatroom::v9::FILE_TRANSFER_GROUP:
            return "GROUP";

        default:
            return "UNKNOWN";
    }
}
//验证元数据
bool same_metadata_identity(
    const FileTransferMetadata& left,
    const FileTransferMetadata& right
) {
    return
        left.transfer_token() ==
            right.transfer_token() &&
        left.scope() ==
            right.scope() &&
        left.sender_username() ==
            right.sender_username() &&
        left.recipient_username() ==
            right.recipient_username() &&
        left.group_id() ==
            right.group_id() &&
        left.group_name() ==
            right.group_name() &&
        left.file_name() ==
            right.file_name() &&
        left.file_size() ==
            right.file_size() &&
        left.sha256_hex() ==
            right.sha256_hex();
}

}  // namespace

FileTransferService::FileTransferService(
    std::filesystem::path storage_root,
    std::size_t worker_count
)
    : storage_root_(
          std::move(storage_root)
      ),
      temp_root_(
          storage_root_ / "tmp"
      ),
      files_root_(
          storage_root_ / "files"
      ) {
    worker_count =
        std::max<std::size_t>(
            1U,
            worker_count
        );

    workers_.reserve(
        worker_count
    );

    for (std::size_t index = 0U;
         index < worker_count;
         ++index) {
        workers_.emplace_back(
            [this] {
                worker_loop();
            }
        );
    }
}

FileTransferService::~FileTransferService() {
    stop();
}

bool FileTransferService::initialize(
    std::string& error
) {
    std::error_code filesystem_error;

    std::filesystem::create_directories(
        temp_root_,
        filesystem_error
    );

    if (filesystem_error) {
        error =
            "cannot create file-transfer temp directory: " +
            filesystem_error.message();
        return false;
    }

    std::filesystem::create_directories(
        files_root_,
        filesystem_error
    );

    if (filesystem_error) {
        error =
            "cannot create file-transfer storage directory: " +
            filesystem_error.message();
        return false;
    }

    return true;
}

bool FileTransferService::begin_or_resume_upload(
    const FileUploadResumeState& requested,
    FileUploadResumeState& persisted,
    std::filesystem::path& temp_path,
    std::uint64_t& accepted_offset,
    std::string& error
) {
    std::lock_guard<std::mutex> lock(
        upload_mutex_
    );

    if (!requested.has_metadata()) {
        error =
            "upload resume state has no metadata";
        return false;
    }

    const FileTransferMetadata& metadata =
        requested.metadata();

    if (!fileutil::is_valid_transfer_token(
            metadata.transfer_token()
        )) {
        error =
            "invalid transfer token";
        return false;
    }

    temp_path =
        upload_part_path(
            metadata.transfer_token()
        );

    const std::filesystem::path meta_path =
        upload_meta_path(
            metadata.transfer_token()
        );

    std::error_code filesystem_error;

    const bool part_exists =
        std::filesystem::exists(
            temp_path,
            filesystem_error
        );

    if (filesystem_error) {
        error =
            "cannot inspect upload part file";
        return false;
    }

    filesystem_error.clear();

    const bool meta_exists =
        std::filesystem::exists(
            meta_path,
            filesystem_error
        );

    if (filesystem_error) {
        error =
            "cannot inspect upload resume metadata";
        return false;
    }
    //这两二个文件必须同时存在,缺一不可,一个存数据,一个元数据
    if (part_exists && meta_exists) {
        FileUploadResumeState existing;

        if (!read_resume_state(
                meta_path,
                existing,
                error
            )) {
            return false;
        }
        
        if (!same_resume_identity(
                requested,
                existing
            )) {
            error =
                "transfer token already belongs "
                "to different upload metadata";
            return false;
        }

        const std::uint64_t current_size =
            static_cast<std::uint64_t>(
                std::filesystem::file_size(
                    temp_path,
                    filesystem_error
                )
            );

        if (filesystem_error) {
            error =
                "cannot inspect resumed upload size";
            return false;
        }

        if (current_size >
            metadata.file_size()) {
            error =
                "server partial file is larger "
                "than declared file size";
            return false;
        }

        persisted =
            std::move(existing);

        accepted_offset =
            current_size;

        return true;
    }

    // Orphaned half of a resume pair is not trusted.
    if (part_exists || meta_exists) {
        filesystem_error.clear();
        std::filesystem::remove(
            temp_path,
            filesystem_error
        );

        filesystem_error.clear();
        std::filesystem::remove(
            meta_path,
            filesystem_error
        );
    }

    {
        std::ofstream output(
            temp_path,
            std::ios::binary |
                std::ios::trunc
        );

        if (!output) {
            error =
                "cannot create temporary upload file";
            return false;
        }
    }

    if (!write_resume_state(
            meta_path,
            requested,
            error
        )) {
        filesystem_error.clear();
        std::filesystem::remove(
            temp_path,
            filesystem_error
        );
        return false;
    }

    persisted = requested;
    accepted_offset = 0U;
    return true;
}

bool FileTransferService::append_upload_chunk(
    const std::filesystem::path& temp_path,
    std::uint64_t expected_offset,
    const std::vector<unsigned char>& bytes,
    std::uint64_t& accepted_offset,
    std::string& error
) {
    std::lock_guard<std::mutex> lock(
        upload_mutex_
    );

    std::error_code filesystem_error;

    const std::uint64_t current_size =
        static_cast<std::uint64_t>(
            std::filesystem::file_size(
                temp_path,
                filesystem_error
            )
        );

    if (filesystem_error) {
        error =
            "cannot inspect temporary upload file";
        return false;
    }
    //要求服务器记录的文件量和客户端的偏移量相同才可以进行下去
    if (current_size !=
        expected_offset) {
        error =
            "file chunk offset mismatch";
        return false;
    }

    std::ofstream output(
        temp_path,
        std::ios::binary |
            std::ios::app
    );

    if (!output) {
        error =
            "cannot append temporary upload file";
        return false;
    }

    if (!bytes.empty()) {
        output.write(
            reinterpret_cast<const char*>(
                bytes.data()
            ),
            static_cast<std::streamsize>(
                bytes.size()
            )
        );
    }

    if (!output) {
        error =
            "failed while writing upload chunk";
        return false;
    }

    accepted_offset =
        current_size +
        static_cast<std::uint64_t>(
            bytes.size()
        );

    return true;
}

bool FileTransferService::finalize_upload(
    const std::filesystem::path& temp_path,
    const std::string& token,
    const std::string& original_file_name,
    std::uint64_t expected_size,
    const std::string& expected_sha256_hex,
    std::string& stored_relative_path,
    std::string& error
) {
    std::lock_guard<std::mutex> lock(
        upload_mutex_
    );

    std::error_code filesystem_error;

    const std::uint64_t actual_size =
        static_cast<std::uint64_t>(
            std::filesystem::file_size(
                temp_path,
                filesystem_error
            )
        );

    if (filesystem_error) {
        error =
            "cannot inspect completed upload";
        return false;
    }

    if (actual_size !=
        expected_size) {
        error =
            "uploaded file size does not match "
            "FILE_BEGIN";
        return false;
    }

    std::string actual_sha256;

    if (!fileutil::sha256_file_hex(
            temp_path,
            actual_sha256,
            error
        )) {
        return false;
    }

    std::string expected =
        expected_sha256_hex;

    std::transform(
        expected.begin(),
        expected.end(),
        expected.begin(),
        [](unsigned char character) {
            return static_cast<char>(
                std::tolower(character)
            );
        }
    );

    if (actual_sha256 != expected) {
        error =
            "uploaded file SHA-256 mismatch";
        return false;
    }

    const std::string safe_name =
        fileutil::sanitize_filename(
            original_file_name
        );

    const std::filesystem::path final_name =
        token +
        "_" +
        safe_name;

    const std::filesystem::path final_path =
        files_root_ /
        final_name;

    std::filesystem::rename(
        temp_path,
        final_path,
        filesystem_error
    );

    if (filesystem_error) {
        filesystem_error.clear();

        std::filesystem::copy_file(
            temp_path,
            final_path,
            std::filesystem::copy_options::
                overwrite_existing,
            filesystem_error
        );

        if (filesystem_error) {
            error =
                "cannot move completed file "
                "into storage: " +
                filesystem_error.message();
            return false;
        }

        filesystem_error.clear();
        std::filesystem::remove(
            temp_path,
            filesystem_error
        );
    }

    filesystem_error.clear();
    std::filesystem::remove(
        upload_meta_path(token),
        filesystem_error
    );

    stored_relative_path =
        (
            std::filesystem::path("files") /
            final_name
        ).generic_string();

    return true;
}

void FileTransferService::cancel_upload(
    const std::string& token
) {
    if (!fileutil::is_valid_transfer_token(
            token
        )) {
        return;
    }

    std::lock_guard<std::mutex> lock(
        upload_mutex_
    );

    std::error_code ignored;

    std::filesystem::remove(
        upload_part_path(token),
        ignored
    );

    ignored.clear();

    std::filesystem::remove(
        upload_meta_path(token),
        ignored
    );
}

void FileTransferService::deliver_async(
    std::uint64_t transfer_id,
    const FileTransferMetadata& metadata,
    std::uint64_t start_offset,
    const minimuduo::net::TcpConnectionPtr& connection,
    CompletionCallback completion
) {
    submit(
        [
            this,
            transfer_id,
            metadata,
            start_offset,
            connection,
            completion =
                std::move(completion)
        ]() mutable {
            deliver_file(
                transfer_id,
                std::move(metadata),
                start_offset,
                connection,
                std::move(completion)
            );
        }
    );
}

std::string FileTransferService::make_offer_line(
    std::uint64_t transfer_id,
    const FileTransferMetadata& metadata
) const {
    const std::vector<unsigned char>
        filename_bytes(
            metadata.file_name().begin(),
            metadata.file_name().end()
        );

    const std::vector<unsigned char>
        group_bytes(
            metadata.group_name().begin(),
            metadata.group_name().end()
        );

    const std::string encoded_group =
        group_bytes.empty()
            ? std::string("-")
            : fileutil::base64_encode(
                  group_bytes
              );

    return
        "FILE_OFFER " +
        std::to_string(
            transfer_id
        ) +
        " " +
        scope_text(
            metadata.scope()
        ) +
        " " +
        metadata.sender_username() +
        " " +
        encoded_group +
        " " +
        fileutil::base64_encode(
            filename_bytes
        ) +
        " " +
        std::to_string(
            metadata.file_size()
        ) +
        " " +
        metadata.sha256_hex() +
        "\n";
}

void FileTransferService::stop() {
    {
        std::lock_guard<std::mutex> lock(
            queue_mutex_
        );

        if (stopping_) {
            return;
        }

        stopping_ = true;
    }

    queue_cv_.notify_all();

    for (std::thread& worker :
         workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    workers_.clear();

    std::queue<std::function<void()>>
        empty;

    {
        std::lock_guard<std::mutex> lock(
            queue_mutex_
        );
        tasks_.swap(empty);
    }
}

void FileTransferService::worker_loop() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(
                queue_mutex_
            );

            queue_cv_.wait(
                lock,
                [this] {
                    return
                        stopping_ ||
                        !tasks_.empty();
                }
            );

            if (stopping_ &&
                tasks_.empty()) {
                return;
            }

            task =
                std::move(
                    tasks_.front()
                );

            tasks_.pop();
        }

        task();
    }
}

void FileTransferService::submit(
    std::function<void()> task
) {
    {
        std::lock_guard<std::mutex> lock(
            queue_mutex_
        );

        if (stopping_) {
            return;
        }

        tasks_.push(
            std::move(task)
        );
    }

    queue_cv_.notify_one();
}

void FileTransferService::deliver_file(
    std::uint64_t transfer_id,
    FileTransferMetadata metadata,
    std::uint64_t start_offset,
    minimuduo::net::TcpConnectionPtr connection,
    CompletionCallback completion
) {
    if (connection == nullptr ||
        !connection->connected()) {
        if (completion) {
            completion(
                false,
                "recipient connection is closed"
            );
        }
        return;
    }

    if (start_offset >
        metadata.file_size()) {
        if (completion) {
            completion(
                false,
                "resume offset exceeds file size"
            );
        }
        return;
    }

    const std::filesystem::path file_path =
        storage_root_ /
        metadata.stored_relative_path();

    std::ifstream input(
        file_path,
        std::ios::binary
    );

    if (!input) {
        if (completion) {
            completion(
                false,
                "stored file is missing"
            );
        }
        return;
    }

    input.seekg(
        static_cast<std::streamoff>(
            start_offset
        ),
        std::ios::beg
    );

    if (!input) {
        if (completion) {
            completion(
                false,
                "failed to seek stored file "
                "to resume offset"
            );
        }
        return;
    }

    connection->send(
        "FILE_RESUME_START " +
        std::to_string(transfer_id) +
        " " +
        std::to_string(start_offset) +
        "\n"
    );

    std::array<
        unsigned char,
        kFileChunkBytes
    > buffer{};

    std::uint64_t offset =
        start_offset;

    while (input) {
        input.read(
            reinterpret_cast<char*>(
                buffer.data()
            ),
            static_cast<std::streamsize>(
                buffer.size()
            )
        );

        const std::streamsize count =
            input.gcount();

        if (count <= 0) {
            break;
        }

        std::vector<unsigned char> chunk(
            buffer.begin(),
            buffer.begin() + count
        );

        const std::string encoded =
            fileutil::base64_encode(
                chunk
            );

        connection->send(
            "FILE_DATA " +
            std::to_string(
                transfer_id
            ) +
            " " +
            std::to_string(
                offset
            ) +
            " " +
            encoded +
            "\n"
        );

        offset +=
            static_cast<std::uint64_t>(
                count
            );
    }

    if (!input.eof()) {
        if (completion) {
            completion(
                false,
                "failed while reading stored file"
            );
        }
        return;
    }

    if (offset !=
        metadata.file_size()) {
        if (completion) {
            completion(
                false,
                "stored file size no longer "
                "matches metadata"
            );
        }
        return;
    }

    connection->send(
        "FILE_DONE " +
        std::to_string(
            transfer_id
        ) +
        "\n",
        [
            completion =
                std::move(completion)
        ]() mutable {
            if (completion) {
                completion(
                    true,
                    {}
                );
            }
        }
    );
}

std::filesystem::path
FileTransferService::upload_part_path(
    const std::string& token
) const {
    return
        temp_root_ /
        (token + ".part");
}

std::filesystem::path
FileTransferService::upload_meta_path(
    const std::string& token
) const {
    return
        temp_root_ /
        (token + ".resume.pb");
}

bool FileTransferService::write_resume_state(
    const std::filesystem::path& path,
    const FileUploadResumeState& state,
    std::string& error
) const {
    std::string bytes;

    if (!state.SerializeToString(
            &bytes
        )) {
        error =
            "official protobuf failed to "
            "serialize FileUploadResumeState";
        return false;
    }

    const std::filesystem::path temp =
        path.string() + ".tmp";

    {
        std::ofstream output(
            temp,
            std::ios::binary |
                std::ios::trunc
        );

        if (!output) {
            error =
                "cannot create upload resume sidecar";
            return false;
        }

        output.write(
            bytes.data(),
            static_cast<std::streamsize>(
                bytes.size()
            )
        );

        if (!output) {
            error =
                "failed to write upload "
                "resume sidecar";
            return false;
        }
    }

    std::error_code filesystem_error;

    std::filesystem::rename(
        temp,
        path,
        filesystem_error
    );

    if (filesystem_error) {
        filesystem_error.clear();

        std::filesystem::remove(
            path,
            filesystem_error
        );

        filesystem_error.clear();

        std::filesystem::rename(
            temp,
            path,
            filesystem_error
        );
    }

    if (filesystem_error) {
        error =
            "cannot finalize upload resume "
            "sidecar: " +
            filesystem_error.message();
        return false;
    }

    return true;
}

bool FileTransferService::read_resume_state(
    const std::filesystem::path& path,
    FileUploadResumeState& state,
    std::string& error
) const {
    std::ifstream input(
        path,
        std::ios::binary
    );

    if (!input) {
        error =
            "cannot open upload resume sidecar";
        return false;
    }

    std::string bytes(
        (
            std::istreambuf_iterator<char>(
                input
            )
        ),
        std::istreambuf_iterator<char>()
    );

    if (!input.eof() &&
        input.fail()) {
        error =
            "failed to read upload "
            "resume sidecar";
        return false;
    }

    if (!state.ParseFromString(
            bytes
        )) {
        error =
            "official protobuf failed to parse "
            "FileUploadResumeState";
        return false;
    }

    return true;
}
//不仅仅检查的是token,而且还检查其他的,身份验证不仅仅检查token
bool FileTransferService::same_resume_identity(
    const FileUploadResumeState& left,
    const FileUploadResumeState& right
) {
    if (!left.has_metadata() ||
        !right.has_metadata()) {
        return false;
    }

    return same_metadata_identity(
        left.metadata(),
        right.metadata()
    );
}
