#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
// 客户端发起但是换没有完成的上传请求
struct PendingUpload {
  std::string token;
  std::string scope;  // 上传范围
  std::string target; // 对象ID或者是群组ID

  std::filesystem::path source_path; // 客户端磁盘文件路径

  std::string file_name;       // 原始文件名
  std::uint64_t file_size = 0; // 文件总字节数
  std::string sha256_hex;      // 哈希值
  // 新建任务在 prepare_upload 已经算过 SHA-256，不需要马上再扫一遍整文件。
  // 从 SQLite 恢复的任务默认为 false，恢复时仍会重新校验。
  bool source_sha_verified = false;

  std::int64_t created_at_unix_ms = 0;
};
// 服务端正在向客户端推送文件的状态
struct IncomingDownload {
  std::uint64_t transfer_id = 0; // 服务端ID

  std::string scope; // 饲料or群聊
  std::string sender_username;
  std::string group_name;
  std::string file_name;

  std::uint64_t expected_size = 0; // 预期大小
  std::uint64_t received_size = 0; // 已经接受的字节数

  std::string sha256_hex;

  std::filesystem::path temp_path;  // 临时文件路径
  std::filesystem::path final_path; // 最终文件路径
};
//二进制文件下载状态
struct PendingBinaryDownloadFrame {
  std::uint64_t transfer_id = 0;//文件全局ID
  std::uint64_t remaining_bytes = 0;//还剩多少字节没有接收
};

// 当前聊天会话类型。只有服务端校验通过后才会进入 Private/Group。
enum class ClientChatScope {
  None,
  Private,
  Group
};


enum class ChatInputMode {
  None,
  Instant,
  MultiLine
};

enum class GroupRequestAction {
  None,
  Approve,
  Reject
};

struct PendingGroupJoinRequestItem {
  std::string group_name;
  std::string username;
};

// 客户端状态
struct ClientState {
  std::string active_username;        // 以登陆的用户名
  std::string pending_login_username; // 可能正正在登陆但是没有认证完成

  // 数字命令 8/9 的会话状态。pending_* 只表示等待服务端校验，
  // chat_scope/chat_target 只在收到 [chat-enter-ok] 后写入。
  ClientChatScope chat_scope = ClientChatScope::None;
  std::string chat_target;
  ClientChatScope pending_chat_scope = ClientChatScope::None;
  std::string pending_chat_target;
  bool chat_entry_pending = false;

  // 进入好友/群会话后不会自动进入消息输入。
  // 必须在会话菜单输入 6，随后再选择 1=回车立即发送、2=长文本模式。
  bool chat_mode_selection_active = false;
  ChatInputMode chat_input_mode = ChatInputMode::None;
  bool chat_multiline_finished = false;
  std::vector<std::string> chat_message_lines;

  // 33/34 的公共“可处理入群申请”选择流程。
  GroupRequestAction group_request_action = GroupRequestAction::None;
  bool group_request_loading = false;
  bool group_request_selection_active = false;
  std::vector<PendingGroupJoinRequestItem> group_request_items;

  // 不在对应会话时，消息正文只缓存不显示；进入对应会话后再统一展示。
  // unordered_set 用来保证“某好友/某群有未读”在当前未查看期间只提示一次。
  std::unordered_map<std::string, std::vector<std::string>>
      pending_private_message_lines;
  std::unordered_map<std::string, std::vector<std::string>>
      pending_group_message_lines;
  std::unordered_set<std::string> private_unread_notified;
  std::unordered_set<std::string> group_unread_notified;

  // FILE_OFFER 只是一份元数据。若当前没有进入对应会话，先把 offer 缓存，
  // 不向服务端发送 FILE_RESUME_REQUEST，因此文件正文不会提前下载。
  std::unordered_map<std::string, std::vector<std::string>>
      pending_private_file_offers;
  std::unordered_map<std::string, std::vector<std::string>>
      pending_group_file_offers;
  std::unordered_set<std::string> private_file_unread_notified;
  std::unordered_set<std::string> group_file_unread_notified;

  std::filesystem::path download_root; // 客户端接受文件的根目录

  std::unordered_map<std::string,
                     PendingUpload> pending_uploads; // 上传文件集合

  std::deque<std::string> upload_queue; // 上传任务队列

  std::string active_upload_token; // 正在激活的上传会话的token

  std::unordered_map<std::uint64_t,
                     IncomingDownload>
      downloads; // 以当前transferID为键的下载任务集合

  PendingBinaryDownloadFrame binary_download;//二进制下载
};