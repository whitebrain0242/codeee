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
    //回调函数1.表示成功或者失败2. 失败时具体的错误描述
    using CompletionCallback =
        std::function<
            void(bool, const std::string&)
        >;
    //初始化存储根目录,启动指定数量的后台工作线程
    //1. 服务端磁盘位置也就是存文件的位置2. 线程池大小,默认为2
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
    //初始化
    bool initialize(std::string& error);

    bool begin_or_resume_upload(
        const FileUploadResumeState& requested,
        FileUploadResumeState& persisted,
        std::filesystem::path& temp_path,
        std::uint64_t& accepted_offset,
        std::string& error
    );
    
    bool append_upload_chunk(
        const std::filesystem::path& temp_path,//.part的路径
        std::uint64_t expected_offset,//客户端当前偏移量
        const std::vector<unsigned char>& bytes,//追加的二进制数据块
        std::uint64_t& accepted_offset,//写入后,服务器磁盘中新的文件大小,告诉客户端当前文件大小
        std::string& error
    );
    //检验文件大小+比对哈希值+rename临时文件移动到内存目录中,删除元数据文件read+rename+unlink
    bool finalize_upload(
        const std::filesystem::path& temp_path,//,part文件路径
        const std::string& token,
        const std::string& original_file_name,//客户端传来的原始文件名
        std::uint64_t expected_size,//客户端在begin时声明的文件大小
        const std::string& expected_sha256_hex,//客户端计算的文件的十六进制字符传
        std::string& stored_relative_path,//最终磁盘上的相对路径
        std::string& error
    );
    //取消上传,删除token对应的.part.resume.pb unlink
    void cancel_upload(
        const std::string& token
    );
//===文件下载/发送--线程池
    
    void deliver_async(
        std::uint64_t transfer_id,//文件下载时的ID,一般是由数据库自增
        const FileTransferMetadata& metadata,//元数据
        std::uint64_t start_offset,//服务器偏移量
        const minimuduo::net::TcpConnectionPtr& connection,//目标客户端
        CompletionCallback completion//一个回调对象---当文件传输完成,利用这个回调会通知上层业务
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
    //服务根目录,文件操作
    std::filesystem::path storage_root_;
    //临时文件目录,根目录下的tmp文件夹---存放.part.resume.pb,上传完成后会删除
    std::filesystem::path temp_root_;
    //最终存储目录
    std::filesystem::path files_root_;

    std::mutex upload_mutex_;
    //任务队列互斥锁
    std::mutex queue_mutex_;
    //条件变量
    std::condition_variable queue_cv_;
    bool stopping_ = false;
    std::queue<std::function<void()>>
        tasks_;
    //线程池
    std::vector<std::thread> workers_;

    void worker_loop();

    void submit(
        std::function<void()> task
    );

    void deliver_file(
        std::uint64_t transfer_id,//传输的ID
        FileTransferMetadata metadata,//元数据
        std::uint64_t start_offset,//服务器数据偏移量
        minimuduo::net::TcpConnectionPtr connection,//目标客户端
        CompletionCallback completion//完成回调的时候,通知上层
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
