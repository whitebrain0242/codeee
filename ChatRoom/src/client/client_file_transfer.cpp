#include "client/client_file_transfer.hpp"

#include "client/client_common.hpp"
#include "client/tls_client_transport.hpp"
#include "file_utils.hpp"
#include "integration/sqlite_client.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

bool send_all(TlsClientTransport &transport, const std::string &data) {
  std::string error;

  if (transport.send(data, error)) {
    return true;
  }

  std::cerr << "TLS 发送失败：" << error << '\n';

  return false;
}

} // namespace

static PendingUpload
pending_upload_from_local(const LocalPendingUpload &local) {
  PendingUpload upload;
  upload.token = local.transfer_token;
  upload.scope = local.scope;
  upload.target = local.target;
  upload.source_path = local.source_path;
  upload.file_name = local.file_name;
  upload.file_size = local.file_size;
  upload.sha256_hex = local.sha256_hex;
  upload.source_sha_verified = false;
  upload.created_at_unix_ms = local.created_at_unix_ms;
  return upload;
}

static LocalPendingUpload
local_pending_upload(const PendingUpload &upload,//上传前文件数据
                     const std::string &account_username) {
  LocalPendingUpload local;
  local.transfer_token = upload.token;
  local.account_username = account_username;
  local.scope = upload.scope;
  local.target = upload.target;
  local.source_path = upload.source_path.string();
  local.file_name = upload.file_name;
  local.file_size = upload.file_size;
  local.sha256_hex = upload.sha256_hex;
  local.created_at_unix_ms = upload.created_at_unix_ms;
  return local;
}
//对于要上传的任务有没有修改
static bool upload_source_matches(PendingUpload &upload,
                                  std::string &error) {
  std::error_code filesystem_error;
  //检查路径是否指向一个普通文件（不是目录、符号链接、设备文件等）
  if (!std::filesystem::is_regular_file(upload.source_path, filesystem_error)) {
    error = "上传源文件不存在";
    return false;
  }
  //获取文件当前大小
  const std::uint64_t size = static_cast<std::uint64_t>(
      std::filesystem::file_size(upload.source_path, filesystem_error));

  if (filesystem_error) {
    error = "无法读取上传源文件大小：" + filesystem_error.message();
    return false;
  }
  //比较当前获取文件大小和上传前记录的大小
  if (size != upload.file_size) {
    error = "上传源文件大小与保存任务时不一致";
    return false;
  }
  // 新建上传在 prepare_upload() 已经计算过一次 SHA-256。
  // 不再在刚入队时立刻重复扫描整个大文件；只有从 SQLite 恢复的任务才重算。
  if (!upload.source_sha_verified) {
    std::string current_sha;
    if (!fileutil::sha256_file_hex(upload.source_path, current_sha, error)) {
      return false;
    }

    if (current_sha != upload.sha256_hex) {
      error = "上传源文件的 SHA-256 与保存任务时不一致";
      return false;
    }
    upload.source_sha_verified = true;
  }

  return true;
}

static bool send_upload_begin(TlsClientTransport &transport,
                              const PendingUpload &upload) {
  const std::string command =
      upload.scope == "PRIVATE" ? "FILE_BEGIN_PRIVATE " : "FILE_BEGIN_GROUP ";

  return send_all(transport, command + upload.token + " " + upload.target +
                                 " " + encode_text_token(upload.file_name) +
                                 " " + std::to_string(upload.file_size) + " " +
                                 upload.sha256_hex + "\n");
}

static bool start_next_queued_upload(TlsClientTransport &transport,
                                     ClientState &state, SqliteClient &cache) {
  if (!state.active_upload_token.empty()) {
    return true;
  }

  while (!state.upload_queue.empty()) {
    const std::string token = state.upload_queue.front();

    state.upload_queue.pop_front();

    const auto iterator = state.pending_uploads.find(token);

    if (iterator == state.pending_uploads.end()) {
      continue;
    }

    std::string error;

    if (!upload_source_matches(iterator->second, error)) {
      std::cerr << "[断点续传] 丢弃无效的待上传任务 " << token << ": " << error
                << '\n';

      std::string sqlite_error;
      (void)cache.remove_pending_upload(state.active_username, token,
                                        sqlite_error);

      state.pending_uploads.erase(iterator);
      continue;
    }

    state.active_upload_token = token;

    if (!send_upload_begin(transport, iterator->second)) {
      state.active_upload_token.clear();
      state.upload_queue.push_front(token);
      return false;
    }

    std::cout << "[断点续传] 正在向服务端请求断点："
              << iterator->second.file_name << "，令牌=" << token << ".\n";

    return true;
  }

  return true;
}

bool load_and_resume_pending_uploads(TlsClientTransport &transport,
                                     ClientState &state, SqliteClient &cache) {
  std::vector<LocalPendingUpload> saved;

  std::string error;

  if (!cache.list_pending_uploads(state.active_username, saved, error)) {
    std::cerr << "[本地 SQLite 错误] 无法加载待上传任务：" << error
              << '\n';
    return true;
  }

  state.pending_uploads.clear();
  state.upload_queue.clear();
  state.active_upload_token.clear();

  for (const LocalPendingUpload &local : saved) {
    PendingUpload upload = pending_upload_from_local(local);

    state.upload_queue.push_back(upload.token);

    state.pending_uploads[upload.token] = std::move(upload);
  }

  if (!saved.empty()) {
    std::cout << "[断点续传] 找到 " << saved.size()
              << " 个 SQLite 待上传任务。\n";
  }

  return start_next_queued_upload(transport, state, cache);
}

bool prepare_upload(TlsClientTransport &transport, ClientState &state,
                    SqliteClient &cache, const std::string &scope,
                    const std::string &target, const std::string &raw_path) {
  if (!require_local_account(state)) {
    return true;
  }

  const std::filesystem::path source_path =
      std::filesystem::path(trim(raw_path));

  std::error_code filesystem_error;

  if (source_path.empty() ||
      !std::filesystem::is_regular_file(source_path, filesystem_error)) {
    std::cout << "[本地错误] 文件不存在，或者目标不是普通文件。\n";
    return true;
  }

  const std::uint64_t file_size = static_cast<std::uint64_t>(
      std::filesystem::file_size(source_path, filesystem_error));

  if (filesystem_error) {
    std::cout << "[本地错误] 无法读取文件大小："
              << filesystem_error.message() << '\n';
    return true;
  }

  if (file_size > kMaxFileSize) {
    std::cout << "[本地错误] 文件超过客户端允许的最大文件大小。\n";
    return true;
  }

  std::string sha256_hex;
  std::string error;

  if (!fileutil::sha256_file_hex(source_path, sha256_hex, error)) {
    std::cout << "[本地错误] " << error << '\n';
    return true;
  }

  const std::string token = fileutil::make_transfer_token();

  if (token.empty()) {
    std::cout << "[本地错误] 无法生成安全的文件传输令牌。\n";
    return true;
  }

  PendingUpload upload;
  upload.token = token;
  upload.scope = scope;
  upload.target = target;
  upload.source_path = source_path;
  upload.file_name =
      fileutil::sanitize_filename(source_path.filename().string());
  upload.file_size = file_size;
  upload.sha256_hex = sha256_hex;
  upload.source_sha_verified = true;
  upload.created_at_unix_ms = client_now_unix_ms();

  if (!cache.save_pending_upload(
          local_pending_upload(upload, state.active_username), error)) {
    std::cout << "[本地 SQLite 错误] 无法保存断点续传任务："
              << error << '\n';
    return true;
  }

  state.pending_uploads[token] = upload;

  state.upload_queue.push_back(token);

  std::cout << "[本地] 已加入断点续传上传队列：" << scope << " 上传 "
            << upload.file_name << " (" << upload.file_size
            << " 字节，SHA-256 " << upload.sha256_hex << "，传输令牌 " << token
            << "）。\n";

  return start_next_queued_upload(transport, state, cache);
}

static bool send_upload_data(TlsClientTransport &transport,
                             const PendingUpload &upload,
                             std::uint64_t start_offset) {
  if (start_offset > upload.file_size) {
    std::cerr
        << "[本地文件错误] 服务端断点位置超过本地文件大小。\n";
    return false;
  }

  std::uint64_t offset = start_offset;

  bool reported_mode = false;

  while (offset < upload.file_size) {
    const std::uint64_t frame_size =
        std::min<std::uint64_t>(upload.file_size - offset, kFileFrameBytes);

    if (!send_all(transport, "FILE_CHUNK " + upload.token + " " +
                                 std::to_string(offset) + " " +
                                 std::to_string(frame_size) + "\n")) {
      return false;
    }

    bool used_zero_copy = false;
    std::string error;

    if (!transport.send_file(upload.source_path, offset, frame_size,
                             used_zero_copy, error)) {
      std::cerr << "[本地文件错误] 原始文件数据发送失败：" << error << '\n';
      return false;
    }

    if (!reported_mode) {
      std::cout << "[文件传输] "
                << (used_zero_copy ? "已启用 kTLS SSL_sendfile 零拷贝"
                                   : "raw binary streaming fallback "
                                     "(kTLS zero-copy unavailable)")
                << ".\n";
      reported_mode = true;
    }

    offset += frame_size;
  }

  return send_all(transport, "FILE_END " + upload.token + "\n");
}

static bool begin_incoming_download(TlsClientTransport &transport,
                                    const std::vector<std::string> &words,
                                    ClientState &state, SqliteClient &cache) {
  if (state.active_username.empty() || words.size() != 7U) {
    return false;
  }

  std::uint64_t transfer_id = 0U;
  std::uint64_t file_size = 0U;

  if (!parse_uint64(words[0], transfer_id) ||
      (words[1] != "PRIVATE" && words[1] != "GROUP") ||
      !parse_uint64(words[5], file_size) || file_size > kMaxFileSize ||
      !fileutil::is_valid_sha256_hex(words[6])) {
    return false;
  }

  std::string group_name;
  std::string file_name;
  std::string error;

  if (!decode_text_token(words[3], group_name, error) ||
      !decode_text_token(words[4], file_name, error)) {
    std::cerr << "[文件错误] FILE_OFFER 文本字段无效：" << error
              << '\n';
    return false;
  }

  const std::filesystem::path account_dir =
      state.download_root / fileutil::sanitize_filename(state.active_username);

  std::error_code filesystem_error;

  std::filesystem::create_directories(account_dir, filesystem_error);

  if (filesystem_error) {
    std::cerr << "[文件错误] 无法创建下载目录："
              << filesystem_error.message() << '\n';
    return false;
  }

  const std::string safe_name = fileutil::sanitize_filename(file_name);

  IncomingDownload download;
  download.transfer_id = transfer_id;
  download.scope = words[1];
  download.sender_username = words[2];
  download.group_name = group_name;
  download.file_name = safe_name;
  download.expected_size = file_size;
  download.sha256_hex = words[6];

  download.temp_path =
      account_dir / (std::to_string(transfer_id) + "_" + safe_name + ".part");

  download.final_path =
      account_dir / (std::to_string(transfer_id) + "_" + safe_name);

  // If the final file already exists, the previous client may have
  // completed local persistence but lost the TLS acknowledgement.
  // Verify it and acknowledge without re-downloading.
  if (std::filesystem::is_regular_file(download.final_path, filesystem_error)) {
    filesystem_error.clear();

    const std::uint64_t final_size = static_cast<std::uint64_t>(
        std::filesystem::file_size(download.final_path, filesystem_error));

    if (!filesystem_error && final_size == download.expected_size) {
      std::string final_sha;

      if (fileutil::sha256_file_hex(download.final_path, final_sha, error) &&
          final_sha == download.sha256_hex) {
        (void)send_all(transport, "FILE_RECEIVED " +
                                      std::to_string(transfer_id) + " " +
                                      final_sha + "\n");

        std::cout << "[断点续传] #F" << transfer_id
                  << " already exists and verifies locally; "
                     "re-sent FILE_RECEIVED acknowledgement.\n";

        return true;
      }
    }
  }

  std::optional<LocalPartialDownload> saved_partial;

  if (!cache.get_partial_download(state.active_username, transfer_id,
                                  saved_partial, error)) {
    std::cerr << "[本地 SQLite 错误] 无法加载未完成下载 #F"
              << transfer_id << ": " << error << '\n';
    return false;
  }

  bool reusable_partial = false;

  if (saved_partial) {
    reusable_partial =
        saved_partial->scope == download.scope &&
        saved_partial->sender_username == download.sender_username &&
        saved_partial->group_name == download.group_name &&
        saved_partial->file_name == download.file_name &&
        saved_partial->file_size == download.expected_size &&
        saved_partial->sha256_hex == download.sha256_hex &&
        std::filesystem::path(saved_partial->temp_path) == download.temp_path;
  }

  if (!reusable_partial) {
    if (saved_partial) {
      std::error_code ignored;
      std::filesystem::remove(saved_partial->temp_path, ignored);

      std::string sqlite_error;
      (void)cache.remove_partial_download(state.active_username, transfer_id,
                                          sqlite_error);
    }

    std::ofstream output(download.temp_path,
                         std::ios::binary | std::ios::trunc);

    if (!output) {
      std::cerr << "[文件错误] 无法创建 " << download.temp_path << '\n';
      return false;
    }
  } else if (!std::filesystem::exists(download.temp_path, filesystem_error)) {
    std::ofstream output(download.temp_path,
                         std::ios::binary | std::ios::trunc);

    if (!output) {
      std::cerr << "[文件错误] 无法重新创建 " << download.temp_path
                << '\n';
      return false;
    }
  }

  filesystem_error.clear();

  const std::uint64_t local_offset = static_cast<std::uint64_t>(
      std::filesystem::file_size(download.temp_path, filesystem_error));

  if (filesystem_error || local_offset > download.expected_size) {
    std::ofstream reset(download.temp_path, std::ios::binary | std::ios::trunc);

    if (!reset) {
      std::cerr << "[文件错误] 无法重置无效的临时文件。\n";
      return false;
    }

    download.received_size = 0U;
  } else {
    download.received_size = local_offset;
  }

  LocalPartialDownload partial;
  partial.server_transfer_id = transfer_id;
  partial.account_username = state.active_username;
  partial.scope = download.scope;
  partial.sender_username = download.sender_username;
  partial.group_name = download.group_name;
  partial.file_name = download.file_name;
  partial.temp_path = download.temp_path.string();
  partial.file_size = download.expected_size;
  partial.sha256_hex = download.sha256_hex;

  if (!cache.save_partial_download(partial, error)) {
    std::cerr << "[本地 SQLite 错误] 无法保存未完成下载 #F"
              << transfer_id << ": " << error << '\n';
    return false;
  }

  state.downloads[transfer_id] = download;

  if (!send_all(transport, "FILE_RESUME_REQUEST " +
                               std::to_string(transfer_id) + " " +
                               std::to_string(download.received_size) + "\n")) {
    return false;
  }

  std::cout << "[文件 #F" << transfer_id << "] "
            << (download.received_size == 0U ? "starting" : "resuming") << " "
            << download.scope << " file " << download.file_name << " at offset "
            << download.received_size << "/" << download.expected_size << ".\n";

  return true;
}

bool consume_file_binary_payload(TlsClientTransport &transport,
                                 std::string &server_buffer,
                                 ClientState &state) {
  if (state.binary_download.remaining_bytes == 0U) {
    return true;
  }

  const std::uint64_t transfer_id = state.binary_download.transfer_id;

  const auto iterator = state.downloads.find(transfer_id);

  if (iterator == state.downloads.end()) {
    state.binary_download = {};
    return false;
  }

  if (server_buffer.empty()) {
    return true;
  }

  IncomingDownload &download = iterator->second;

  const std::size_t consume = static_cast<std::size_t>(std::min<std::uint64_t>(
      state.binary_download.remaining_bytes,
      static_cast<std::uint64_t>(server_buffer.size())));

  if (download.received_size + static_cast<std::uint64_t>(consume) >
      download.expected_size) {
    state.binary_download = {};

    (void)send_all(transport,
                   "FILE_RECEIVE_FAILED " + std::to_string(transfer_id) + "\n");

    return false;
  }

  std::ofstream output(download.temp_path, std::ios::binary | std::ios::app);

  if (!output) {
    state.binary_download = {};
    return false;
  }

  output.write(server_buffer.data(), static_cast<std::streamsize>(consume));

  if (!output) {
    state.binary_download = {};
    return false;
  }

  server_buffer.erase(0, consume);

  download.received_size += static_cast<std::uint64_t>(consume);

  state.binary_download.remaining_bytes -= static_cast<std::uint64_t>(consume);

  if (state.binary_download.remaining_bytes == 0U) {
    state.binary_download = {};
  }

  return true;
}

static bool finish_incoming_download(TlsClientTransport &transport,
                                     std::uint64_t transfer_id,
                                     ClientState &state, SqliteClient &cache) {
  const auto iterator = state.downloads.find(transfer_id);

  if (iterator == state.downloads.end()) {
    return false;
  }

  IncomingDownload download = iterator->second;

  if (download.received_size != download.expected_size) {
    std::cerr << "[文件错误] #F" << transfer_id << " 接收不完整："
              << download.received_size << "/" << download.expected_size
              << " 字节；临时文件已保留，可用于断点续传。\n";

    (void)send_all(transport,
                   "FILE_RECEIVE_FAILED " + std::to_string(transfer_id) + "\n");

    state.downloads.erase(iterator);

    return true;
  }

  std::string actual_sha256;
  std::string error;

  if (!fileutil::sha256_file_hex(download.temp_path, actual_sha256, error) ||
      actual_sha256 != download.sha256_hex) {
    std::cerr
        << "[文件错误] #F" << transfer_id
        << " SHA-256 校验失败；已删除损坏的临时文件，下次将从 0 开始。\n";

    std::error_code ignored;

    std::filesystem::remove(download.temp_path, ignored);

    std::string sqlite_error;
    (void)cache.remove_partial_download(state.active_username, transfer_id,
                                        sqlite_error);

    state.downloads.erase(iterator);

    (void)send_all(transport,
                   "FILE_RECEIVE_FAILED " + std::to_string(transfer_id) + "\n");

    return true;
  }

  std::error_code filesystem_error;

  std::filesystem::remove(download.final_path, filesystem_error);

  filesystem_error.clear();

  std::filesystem::rename(download.temp_path, download.final_path,
                          filesystem_error);

  if (filesystem_error) {
    std::cerr << "[文件错误] 无法完成文件 #F" << transfer_id << "："
              << filesystem_error.message()
              << "；.part 临时文件已保留，后续可重试。\n";

    state.downloads.erase(iterator);

    (void)send_all(transport,
                   "FILE_RECEIVE_FAILED " + std::to_string(transfer_id) + "\n");

    return true;
  }

  LocalFileTransfer file;
  file.server_transfer_id = transfer_id;
  file.account_username = state.active_username;
  file.scope = download.scope;
  file.peer_username =
      download.scope == "PRIVATE" ? download.sender_username : std::string();
  file.group_name = download.group_name;
  file.sender_username = download.sender_username;
  file.file_name = download.file_name;
  file.local_path = download.final_path.string();
  file.file_size = download.expected_size;
  file.sha256_hex = download.sha256_hex;
  file.received_at_unix_ms = client_now_unix_ms();
  file.outgoing = false;

  if (!cache.cache_file_transfer(file, error)) {
    std::cerr << "[本地 SQLite 错误] 无法缓存文件 #F" << transfer_id
              << "：" << error << '\n';
  }

  std::string sqlite_error;
  (void)cache.remove_partial_download(state.active_username, transfer_id,
                                      sqlite_error);

  state.downloads.erase(iterator);

  if (!send_all(transport, "FILE_RECEIVED " + std::to_string(transfer_id) +
                               " " + actual_sha256 + "\n")) {
    return false;
  }

  std::cout << "[文件 #F" << transfer_id << "] 已接收并通过 SHA-256 校验："
            << download.final_path.string() << '\n';

  return true;
}

bool handle_file_protocol_line(TlsClientTransport &transport,
                               const std::string &line, ClientState &state,
                               SqliteClient &cache) {
  const Command command = parse_command(line);

  if (command.name == "FILE_READY") {
    const std::vector<std::string> words = split_words(command.raw_arguments);

    std::uint64_t start_offset = 0U;

    if (words.size() != 2U || !parse_uint64(words[1], start_offset)) {
      return true;
    }

    const auto iterator = state.pending_uploads.find(words[0]);

    if (iterator == state.pending_uploads.end()) {
      return true;
    }

    state.active_upload_token = words[0];

    std::cout << "[断点续传] 服务端已接受上传断点，文件："
              << iterator->second.file_name << "；从偏移 "
              << start_offset << "/" << iterator->second.file_size << " 开始发送。\n";

    if (!send_upload_data(transport, iterator->second, start_offset)) {
      std::cerr << "[本地文件错误] 上传数据流中断；SQLite 任务已保留，"
                   "下次连接后可以继续断点续传。\n";

      state.active_upload_token.clear();
    }

    return true;
  }

  if (command.name == "FILE_PAUSED") {
    const std::vector<std::string> words = split_words(command.raw_arguments);

    if (words.size() >= 2U) {
      std::string reason;
      std::string error;

      if (!decode_text_token(words[1], reason, error)) {
        reason = "服务端暂停了文件传输";
      }

      if (state.active_upload_token == words[0]) {
        state.active_upload_token.clear();
      }

      std::cout << "[文件已暂停] " << reason << "\n"
                << "[断点续传] task remains in SQLite; "
                   "reconnect/login to resume the saved 上传 task.\n";
    }

    return true;
  }

  if (command.name == "FILE_REJECT") {
    const std::vector<std::string> words = split_words(command.raw_arguments);

    if (words.size() >= 2U) {
      std::string reason;
      std::string error;

      if (!decode_text_token(words[1], reason, error)) {
        reason = "服务端拒绝了文件传输";
      }

      if (!state.active_username.empty()) {
        std::string sqlite_error;
        (void)cache.remove_pending_upload(state.active_username, words[0],
                                          sqlite_error);
      }

      state.pending_uploads.erase(words[0]);

      if (state.active_upload_token == words[0]) {
        state.active_upload_token.clear();
      }

      std::cout << "[文件被拒绝] " << reason << '\n';

      (void)start_next_queued_upload(transport, state, cache);
    }

    return true;
  }

  if (command.name == "FILE_ACCESS_REVOKED") {
    const std::vector<std::string> words = split_words(command.raw_arguments);
    std::uint64_t transfer_id = 0U;
    if (words.empty() || !parse_uint64(words[0], transfer_id)) return true;

    std::string reason = "文件访问权限已被撤销";
    if (words.size() >= 2U) {
      std::string decode_error;
      (void)decode_text_token(words[1], reason, decode_error);
    }

    const auto active = state.downloads.find(transfer_id);
    if (active != state.downloads.end()) {
      std::error_code ignored;
      std::filesystem::remove(active->second.temp_path, ignored);
      ignored.clear();
      std::filesystem::remove(active->second.final_path, ignored);

      if (!state.active_username.empty()) {
        std::string sqlite_error;
        (void)cache.remove_partial_download(state.active_username, transfer_id,
                                            sqlite_error);
      }
      state.downloads.erase(active);
    }

    if (state.binary_download.transfer_id == transfer_id) {
      state.binary_download = {};
    }

    const std::string prefix =
        "FILE_OFFER " + std::to_string(transfer_id) + " ";
    auto remove_queued_offer = [&prefix](auto &offers) {
      for (auto map_it = offers.begin(); map_it != offers.end();) {
        auto &lines = map_it->second;
        lines.erase(std::remove_if(lines.begin(), lines.end(),
                                   [&prefix](const std::string &queued) {
                                     return starts_with(queued, prefix);
                                   }),
                    lines.end());
        if (lines.empty()) map_it = offers.erase(map_it);
        else ++map_it;
      }
    };
    remove_queued_offer(state.pending_private_file_offers);
    remove_queued_offer(state.pending_group_file_offers);

    std::cout << "[文件 #F" << transfer_id << "] 访问权限已撤销：" << reason
              << ". Partial/local copy removed.\n";
    return true;
  }

  if (command.name == "FILE_UPLOAD_OK") {
    const std::vector<std::string> words = split_words(command.raw_arguments);

    std::uint64_t transfer_id = 0U;

    if (words.size() != 2U || !parse_uint64(words[1], transfer_id)) {
      return true;
    }

    const auto iterator = state.pending_uploads.find(words[0]);

    if (iterator == state.pending_uploads.end()) {
      return true;
    }

    const PendingUpload upload = iterator->second;

    LocalFileTransfer file;
    file.server_transfer_id = transfer_id;
    file.account_username = state.active_username;
    file.scope = upload.scope;
    file.peer_username =
        upload.scope == "PRIVATE" ? upload.target : std::string();
    file.group_name = upload.scope == "GROUP" ? upload.target : std::string();
    file.sender_username = state.active_username;
    file.file_name = upload.file_name;
    file.local_path = upload.source_path.string();
    file.file_size = upload.file_size;
    file.sha256_hex = upload.sha256_hex;
    file.received_at_unix_ms = client_now_unix_ms();
    file.outgoing = true;

    std::string error;

    if (!cache.cache_file_transfer(file, error)) {
      std::cerr << "[本地 SQLite 错误] " << error << '\n';
    }

    std::string sqlite_error;
    (void)cache.remove_pending_upload(state.active_username, upload.token,
                                      sqlite_error);

    state.pending_uploads.erase(iterator);

    if (state.active_upload_token == upload.token) {
      state.active_upload_token.clear();
    }

    std::cout << "[文件 #F" << transfer_id
              << "] 上传 persisted on server; resumable task cleared.\n";

    (void)start_next_queued_upload(transport, state, cache);

    return true;
  }

  if (command.name == "FILE_OFFER") {
    const std::vector<std::string> words = split_words(command.raw_arguments);

    // 服务端这里只是在“提供文件”，真正的文件正文要等客户端发送
    // FILE_RESUME_REQUEST 后才开始。利用这一点实现会话隔离：不在对应
    // 私聊/群聊时只保存 offer，不提前下载文件。
    if (words.size() == 7U && (words[1] == "PRIVATE" || words[1] == "GROUP")) {
      std::string group_name;
      std::string decode_error;
      if (!decode_text_token(words[3], group_name, decode_error)) {
        std::cerr << "[文件错误] FILE_OFFER 群字段无效。\n";
        return true;
      }

      if (words[1] == "PRIVATE") {
        const std::string &peer = words[2];
        const bool matching_chat =
            state.chat_scope == ClientChatScope::Private &&
            state.chat_target == peer;
        if (!matching_chat) {
          state.pending_private_file_offers[peer].push_back(line);
          if (state.private_file_unread_notified.insert(peer).second) {
            std::cout << "[未读提示] 好友 " << peer
                      << " 有待接收文件；进入与该好友的私聊后才开始接收。\n";
          }
          return true;
        }
      } else {
        const bool matching_chat =
            state.chat_scope == ClientChatScope::Group &&
            state.chat_target == group_name;
        if (!matching_chat) {
          state.pending_group_file_offers[group_name].push_back(line);
          if (state.group_file_unread_notified.insert(group_name).second) {
            std::cout << "[未读提示] 群 " << group_name
                      << " 有待接收文件；进入该群聊后才开始接收。\n";
          }
          return true;
        }
      }
    }

    if (!begin_incoming_download(transport, words, state, cache)) {
      std::cerr << "[文件错误] FILE_OFFER 无效。\n";
    }

    return true;
  }

  if (command.name == "FILE_RESUME_START") {
    const std::vector<std::string> words = split_words(command.raw_arguments);

    std::uint64_t transfer_id = 0U;
    std::uint64_t offset = 0U;

    if (words.size() == 2U && parse_uint64(words[0], transfer_id) &&
        parse_uint64(words[1], offset)) {
      const auto iterator = state.downloads.find(transfer_id);

      if (iterator != state.downloads.end() &&
          iterator->second.received_size != offset) {
        std::cerr << "[文件错误] 服务端断点位置 " << offset
                  << " does not match local offset "
                  << iterator->second.received_size << " for #F" << transfer_id
                  << ".\n";

        (void)send_all(transport, "FILE_RECEIVE_FAILED " +
                                      std::to_string(transfer_id) + "\n");

        state.downloads.erase(iterator);
      }
    }

    return true;
  }

  if (command.name == "FILE_DATA") {
    const std::vector<std::string> words = split_words(command.raw_arguments);

    std::uint64_t transfer_id = 0U;
    std::uint64_t offset = 0U;
    std::uint64_t byte_count = 0U;

    if (words.size() != 3U || !parse_uint64(words[0], transfer_id) ||
        !parse_uint64(words[1], offset) ||
        !parse_uint64(words[2], byte_count) || byte_count == 0U ||
        byte_count > kFileFrameBytes ||
        state.binary_download.remaining_bytes != 0U) {
      return true;
    }

    const auto iterator = state.downloads.find(transfer_id);

    if (iterator == state.downloads.end() ||
        iterator->second.received_size != offset ||
        offset + byte_count > iterator->second.expected_size) {
      (void)send_all(transport, "FILE_RECEIVE_FAILED " +
                                    std::to_string(transfer_id) + "\n");

      state.downloads.erase(transfer_id);

      return true;
    }

    state.binary_download.transfer_id = transfer_id;

    state.binary_download.remaining_bytes = byte_count;

    return true;
  }

  if (command.name == "FILE_DONE") {
    const std::vector<std::string> words = split_words(command.raw_arguments);

    std::uint64_t transfer_id = 0U;

    if (words.size() == 1U && parse_uint64(words[0], transfer_id)) {
      (void)finish_incoming_download(transport, transfer_id, state, cache);
    }

    return true;
  }

  if (command.name == "FILE_ACK_OK") {
    return true;
  }

  return false;
}

void preserve_partial_downloads(ClientState &state) {
  // Deliberately keep *.part files. SQLite partial_downloads stores their
  // identity, while the actual file size is the resume offset.
  state.downloads.clear();
  state.binary_download = {};
}
