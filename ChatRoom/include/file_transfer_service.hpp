#pragma once

#include "file_utils.hpp"
#include "proto_types.hpp"

#include "minimuduo/net/Callbacks.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

class FileTransferService {
public:
    using CompletionCallback =
        std::function<
            void(bool, const std::string&)
        >;

    explicit FileTransferService(
        std::filesystem::path storage_root,
        std::size_t worker_count = 2
    );

    ~FileTransferService();

    FileTransferService(
        const FileTransferService&
    ) = delete;

    FileTransferService& operator=(
        const FileTransferService&
    ) = delete;

    bool initialize(std::string& error);

    bool begin_or_resume_upload(
        const FileUploadResumeState& requested,
        FileUploadResumeState& persisted,
        std::filesystem::path& temp_path,
        std::uint64_t& accepted_offset,
        std::string& error
    );

    bool append_upload_chunk(
        const std::filesystem::path& temp_path,
        std::uint64_t expected_offset,
        const std::vector<unsigned char>& bytes,
        std::uint64_t& accepted_offset,
        std::string& error
    );

    bool finalize_upload(
        const std::filesystem::path& temp_path,
        const std::string& token,
        const std::string& original_file_name,
        std::uint64_t expected_size,
        const std::string& expected_sha256_hex,
        std::string& stored_relative_path,
        std::string& error
    );

    void cancel_upload(
        const std::string& token
    );

    void deliver_async(
        std::uint64_t transfer_id,
        const FileTransferMetadata& metadata,
        std::uint64_t start_offset,
        const minimuduo::net::TcpConnectionPtr& connection,
        CompletionCallback completion
    );

    std::string make_offer_line(
        std::uint64_t transfer_id,
        const FileTransferMetadata& metadata
    ) const;

    void stop();

    const std::filesystem::path&
    storage_root() const {
        return storage_root_;
    }

private:
    std::filesystem::path storage_root_;
    std::filesystem::path temp_root_;
    std::filesystem::path files_root_;

    std::mutex upload_mutex_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    bool stopping_ = false;
    std::queue<std::function<void()>>
        tasks_;
    std::vector<std::thread> workers_;

    void worker_loop();

    void submit(
        std::function<void()> task
    );

    void deliver_file(
        std::uint64_t transfer_id,
        FileTransferMetadata metadata,
        std::uint64_t start_offset,
        minimuduo::net::TcpConnectionPtr connection,
        CompletionCallback completion
    );

    std::filesystem::path upload_part_path(
        const std::string& token
    ) const;

    std::filesystem::path upload_meta_path(
        const std::string& token
    ) const;

    bool write_resume_state(
        const std::filesystem::path& path,
        const FileUploadResumeState& state,
        std::string& error
    ) const;

    bool read_resume_state(
        const std::filesystem::path& path,
        FileUploadResumeState& state,
        std::string& error
    ) const;

    static bool same_resume_identity(
        const FileUploadResumeState& left,
        const FileUploadResumeState& right
    );
};
