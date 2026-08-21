#pragma once

#include "mysql_database.hpp"
#include "proto_types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
//当客户端上传文件的时候，服务端记录文件的进度和细节，避免丢包
struct IncomingFileUpload {
    std::string token;
    //文件是饲料还是群聊
    chatroom::v9::FileTransferScope scope =
        chatroom::v9::FILE_TRANSFER_SCOPE_UNSPECIFIED;
    //饲料：存对方的用户名，群聊：存群组标识
    std::string target;
    std::uint64_t group_id = 0;

    std::string file_name;
    std::uint64_t expected_size = 0;//总大小
    //已经接受的字节
    std::uint64_t received_size = 0;
    //文件的哈希值，用于后来比对
    std::string sha256_hex;
    //磁盘上的文件临时存放路径，接受完毕移动到正式目录
    std::filesystem::path temp_path;
    //接受者列表
    std::vector<std::string> recipients;
    //断电续传状态
    FileUploadResumeState resume_state;
};

struct PendingBinaryUploadFrame {
    std::string token;
    std::uint64_t next_offset = 0;
    std::uint64_t remaining_bytes = 0;
};


//客户端的一个完整会话
struct ClientSession {
    bool logged_in = false;
    std::string username;//登陆的账号
    //当前正在进行的上传任务
    std::optional<IncomingFileUpload>upload;

    std::optional<PendingBinaryUploadFrame>binary_upload;
    //正在接收/下载中的文件传输ID集合。
    std::unordered_set<std::uint64_t>file_deliveries_in_progress;
    //已接收并存储、准备供此用户下载的离线文件列表。
    std::unordered_map<std::uint64_t,StoredFileTransfer> offered_files;
};
