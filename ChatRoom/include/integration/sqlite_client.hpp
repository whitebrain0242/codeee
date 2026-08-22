#pragma once

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
//私聊消息
struct LocalPrivateMessage {
    std::uint64_t server_message_id = 0;//全局消息ID
    std::string account_username;//登陆的用户
    std::string peer_username;//饲料对方
    std::string sender_username;//发送人
    std::string recipient_username;//接收人
    std::string content;//正文
    std::int64_t received_at_unix_ms = 0;//时间戳
    bool outgoing = false;//是否是发送方
    bool offline_delivery = false;//是否是离线消息
};
//群聊消息
struct LocalGroupMessage {
    std::uint64_t server_message_id = 0;
    std::string account_username;
    std::string group_name;
    std::string sender_username;
    std::string content;
    std::int64_t received_at_unix_ms = 0;
    bool outgoing = false;
    bool offline_delivery = false;
};
//已完成文件记录
struct LocalFileTransfer {
    std::uint64_t server_transfer_id = 0;//全局文件传输ID
    std::string account_username;
    std::string scope;//区分饲料和群发
    std::string peer_username;//对方
    std::string group_name;//对方
    std::string sender_username;
    std::string file_name;
    std::string local_path;//保存在本地的完整路径
    std::uint64_t file_size = 0;//文件大小
    std::string sha256_hex;//哈希值
    std::int64_t received_at_unix_ms = 0;
    bool outgoing = false;//是否是发送方 true自己发送的 false是接受的
};
//待上传任务
struct LocalPendingUpload {
    std::string transfer_token;
    std::string account_username;
    std::string scope;
    std::string target;
    std::string source_path;//要上传的文件在本地的位置
    std::string file_name;
    std::uint64_t file_size = 0;
    std::string sha256_hex;
    std::int64_t created_at_unix_ms = 0;
};
//没有下载完成的文件
struct LocalPartialDownload {
    std::uint64_t server_transfer_id = 0;
    std::string account_username;
    std::string scope;
    std::string sender_username;
    std::string group_name;
    std::string file_name;
    std::string temp_path;//下载文件临时存储位置
    std::uint64_t file_size = 0;
    std::string sha256_hex;
};
//缓存统计
struct LocalCacheStats {
    std::size_t private_messages = 0;
    std::size_t group_messages = 0;
    std::size_t files = 0;
};

class SqliteClient {
public:
    SqliteClient() = default;
    ~SqliteClient();

    SqliteClient(const SqliteClient&) = delete;
    SqliteClient& operator=(const SqliteClient&) = delete;
    //打开或者创建数据库
    bool open(
        const std::string& database_path,
        std::string& error
    );
    //保存聊天记录
    bool cache_private_message(
        const LocalPrivateMessage& message,
        std::string& error
    );

    bool cache_group_message(
        const LocalGroupMessage& message,
        std::string& error
    );
    //查看聊天记录
    bool recent_private_messages(
        const std::string& account_username,
        const std::string& peer_username,
        std::size_t count,
        std::vector<LocalPrivateMessage>& messages,
        std::string& error
    );

    bool recent_group_messages(
        const std::string& account_username,
        const std::string& group_name,
        std::size_t count,
        std::vector<LocalGroupMessage>& messages,
        std::string& error
    );

    
    //记录已经完成的文件
    //存记录
    bool cache_file_transfer(
        const LocalFileTransfer& file,
        std::string& error
    );
    //查记录
    bool recent_file_transfers(
        const std::string& account_username,
        std::size_t count,
        std::vector<LocalFileTransfer>& files,
        std::string& error
    );
    //断点续传-上传
    //存任务
    bool save_pending_upload(
        const LocalPendingUpload& upload,
        std::string& error
    );
    //查任务
    bool list_pending_uploads(
        const std::string& account_username,
        std::vector<LocalPendingUpload>& uploads,
        std::string& error
    );
    //删任务
    bool remove_pending_upload(
        const std::string& account_username,
        const std::string& transfer_token,
        std::string& error
    );
    //断点续传-下载
    //存进度
    bool save_partial_download(
        const LocalPartialDownload& download,
        std::string& error
    );
    //查进度
    bool get_partial_download(
        const std::string& account_username,
        std::uint64_t transfer_id,
        std::optional<LocalPartialDownload>& download,
        std::string& error
    );
    //删进度
    bool remove_partial_download(
        const std::string& account_username,
        std::uint64_t transfer_id,
        std::string& error
    );
    //查看本地存储了多少消息和文件
    bool stats(
        const std::string& account_username,
        LocalCacheStats& stats,
        std::string& error
    );

    const std::string& database_path() const {
        return database_path_;
    }

private:
    sqlite3* database_ = nullptr;
    std::string database_path_;
    mutable std::mutex mutex_;

    bool execute(
        const std::string& sql,
        std::string& error
    );

    bool initialize_schema(std::string& error);
    void close_locked();
};

