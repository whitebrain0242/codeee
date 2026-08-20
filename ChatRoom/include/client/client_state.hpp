#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
//客户端发起但是换没有完成的上传请求
struct PendingUpload {
    std::string token;
    std::string scope;//上传范围
    std::string target;//对象ID或者是群组ID

    std::filesystem::path source_path;//客户端磁盘文件路径

    std::string file_name;//原始文件名
    std::uint64_t file_size = 0;//文件总字节数
    std::string sha256_hex;//哈希值

    std::int64_t created_at_unix_ms = 0;
};
//服务端正在向客户端推送文件的状态
struct IncomingDownload {
    std::uint64_t transfer_id = 0;//服务端ID

    std::string scope;//饲料or群聊
    std::string sender_username;
    std::string group_name;
    std::string file_name;

    std::uint64_t expected_size = 0;//预期大小
    std::uint64_t received_size = 0;//已经接受的字节数

    std::string sha256_hex;

    std::filesystem::path temp_path;//临时文件路径
    std::filesystem::path final_path;//最终文件路径
};
//客户端状态
struct ClientState {
    std::string active_username;//以登陆的用户名
    std::string pending_login_username;//可能正正在登陆但是没有认证完成

    std::filesystem::path download_root;//客户端接受文件的根目录

    std::unordered_map<
        std::string,
        PendingUpload
    > pending_uploads;//上传文件集合

    std::deque<std::string>
        upload_queue;//上传任务队列

    std::string active_upload_token;//正在激活的上传会话的token

    std::unordered_map<
        std::uint64_t,
        IncomingDownload
    > downloads;//以当前transferID为键的下载任务集合
};