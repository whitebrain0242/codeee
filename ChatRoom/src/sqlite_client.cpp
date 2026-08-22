#include "integration/sqlite_client.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>

namespace {

using StatementPtr = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

bool bind_text(sqlite3_stmt *statement, int index, const std::string &value) {
  return sqlite3_bind_text(statement, index, value.c_str(),
                           static_cast<int>(value.size()),
                           SQLITE_TRANSIENT) == SQLITE_OK;
}

std::string sqlite_error(sqlite3 *database) {
  return database != nullptr ? sqlite3_errmsg(database)
                             : "SQLite database is null";
}

} // namespace

SqliteClient::~SqliteClient() {
  std::lock_guard<std::mutex> lock(mutex_);
  close_locked();
}

bool SqliteClient::open(const std::string &database_path, std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);
  close_locked(); // 如果之前打开过，先关掉

  if (database_path != ":memory:") {
    std::filesystem::path dir =
        std::filesystem::path(database_path).parent_path();
    if (!dir.empty()) {
      std::filesystem::create_directories(dir);
    }
  }

  // 打开数据库
  sqlite3_open_v2(database_path.c_str(), &database_,
                  SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                      SQLITE_OPEN_FULLMUTEX,
                  nullptr);
  database_path_ = database_path;
  sqlite3_busy_timeout(database_, 3000); // 设置超市时间

  return initialize_schema(error); // 自动建表
}
// 存一条私聊消息
bool SqliteClient::cache_private_message(const LocalPrivateMessage &message,
                                         std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  static constexpr const char *sql =
      "INSERT INTO private_messages("
      "account_username,server_message_id,peer_username,"
      "sender_username,recipient_username,content,"
      "received_at_unix_ms,outgoing,offline_delivery"
      ") VALUES(?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(account_username,server_message_id) "
      "DO UPDATE SET "
      "peer_username=excluded.peer_username,"
      "sender_username=excluded.sender_username,"
      "recipient_username=excluded.recipient_username,"
      "content=excluded.content,"
      "received_at_unix_ms=MIN("
      "private_messages.received_at_unix_ms,"
      "excluded.received_at_unix_ms),"
      "outgoing=excluded.outgoing,"
      "offline_delivery=MAX("
      "private_messages.offline_delivery,"
      "excluded.offline_delivery)";

  sqlite3_stmt *raw_statement = nullptr;

  if (sqlite3_prepare_v2(database_, sql, -1, &raw_statement, nullptr) !=
      SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  StatementPtr statement(raw_statement, sqlite3_finalize);

  const bool bound =
      bind_text(statement.get(), 1, message.account_username) &&
      sqlite3_bind_int64(
          statement.get(), 2,
          static_cast<sqlite3_int64>(message.server_message_id)) == SQLITE_OK &&
      bind_text(statement.get(), 3, message.peer_username) &&
      bind_text(statement.get(), 4, message.sender_username) &&
      bind_text(statement.get(), 5, message.recipient_username) &&
      bind_text(statement.get(), 6, message.content) &&
      sqlite3_bind_int64(statement.get(), 7,
                         static_cast<sqlite3_int64>(
                             message.received_at_unix_ms)) == SQLITE_OK &&
      sqlite3_bind_int(statement.get(), 8, message.outgoing ? 1 : 0) ==
          SQLITE_OK &&
      sqlite3_bind_int(statement.get(), 9, message.offline_delivery ? 1 : 0) ==
          SQLITE_OK;

  if (!bound) {
    error = sqlite_error(database_);
    return false;
  }

  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    error = sqlite_error(database_);
    return false;
  }

  return true;
}

bool SqliteClient::cache_group_message(const LocalGroupMessage &message,
                                       std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  static constexpr const char *sql =
      "INSERT INTO group_messages("
      "account_username,server_message_id,group_name,"
      "sender_username,content,received_at_unix_ms,"
      "outgoing,offline_delivery"
      ") VALUES(?,?,?,?,?,?,?,?) "
      "ON CONFLICT(account_username,server_message_id) "
      "DO UPDATE SET "
      "group_name=excluded.group_name,"
      "sender_username=excluded.sender_username,"
      "content=excluded.content,"
      "received_at_unix_ms=MIN("
      "group_messages.received_at_unix_ms,"
      "excluded.received_at_unix_ms),"
      "outgoing=excluded.outgoing,"
      "offline_delivery=MAX("
      "group_messages.offline_delivery,"
      "excluded.offline_delivery)";

  sqlite3_stmt *raw_statement = nullptr;

  if (sqlite3_prepare_v2(database_, sql, -1, &raw_statement, nullptr) !=
      SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  StatementPtr statement(raw_statement, sqlite3_finalize);

  const bool bound =
      bind_text(statement.get(), 1, message.account_username) &&
      sqlite3_bind_int64(
          statement.get(), 2,
          static_cast<sqlite3_int64>(message.server_message_id)) == SQLITE_OK &&
      bind_text(statement.get(), 3, message.group_name) &&
      bind_text(statement.get(), 4, message.sender_username) &&
      bind_text(statement.get(), 5, message.content) &&
      sqlite3_bind_int64(statement.get(), 6,
                         static_cast<sqlite3_int64>(
                             message.received_at_unix_ms)) == SQLITE_OK &&
      sqlite3_bind_int(statement.get(), 7, message.outgoing ? 1 : 0) ==
          SQLITE_OK &&
      sqlite3_bind_int(statement.get(), 8, message.offline_delivery ? 1 : 0) ==
          SQLITE_OK;

  if (!bound) {
    error = sqlite_error(database_);
    return false;
  }

  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    error = sqlite_error(database_);
    return false;
  }

  return true;
}
// 查与某人的私聊历史
bool SqliteClient::recent_private_messages(
    const std::string &account_username, const std::string &peer_username,
    std::size_t count, std::vector<LocalPrivateMessage> &messages,
    std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  static constexpr const char *sql =
      "SELECT server_message_id,peer_username,"
      "sender_username,recipient_username,content,"
      "received_at_unix_ms,outgoing,offline_delivery "
      "FROM private_messages "
      "WHERE account_username=? AND peer_username=? "
      "ORDER BY server_message_id DESC LIMIT ?";

  sqlite3_stmt *raw_statement = nullptr;

  if (sqlite3_prepare_v2(database_, sql, -1, &raw_statement, nullptr) !=
      SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  StatementPtr statement(raw_statement, sqlite3_finalize);

  if (!bind_text(statement.get(), 1, account_username) ||
      !bind_text(statement.get(), 2, peer_username) ||
      sqlite3_bind_int64(statement.get(), 3,
                         static_cast<sqlite3_int64>(count)) != SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  messages.clear();

  while (true) {
    const int result = sqlite3_step(statement.get());

    if (result == SQLITE_DONE) {
      break;
    }

    if (result != SQLITE_ROW) {
      error = sqlite_error(database_);
      return false;
    }

    LocalPrivateMessage message;
    message.server_message_id =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
    message.account_username = account_username;
    message.peer_username =
        reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 1));
    message.sender_username =
        reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 2));
    message.recipient_username =
        reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 3));
    message.content =
        reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 4));
    message.received_at_unix_ms =
        static_cast<std::int64_t>(sqlite3_column_int64(statement.get(), 5));
    message.outgoing = sqlite3_column_int(statement.get(), 6) != 0;
    message.offline_delivery = sqlite3_column_int(statement.get(), 7) != 0;

    messages.push_back(std::move(message));
  }

  std::reverse(messages.begin(), messages.end());
  return true;
}

bool SqliteClient::recent_group_messages(
    const std::string &account_username, const std::string &group_name,
    std::size_t count, std::vector<LocalGroupMessage> &messages,
    std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  static constexpr const char *sql =
      "SELECT server_message_id,group_name,"
      "sender_username,content,received_at_unix_ms,"
      "outgoing,offline_delivery "
      "FROM group_messages "
      "WHERE account_username=? AND group_name=? "
      "ORDER BY server_message_id DESC LIMIT ?";

  sqlite3_stmt *raw_statement = nullptr;

  if (sqlite3_prepare_v2(database_, sql, -1, &raw_statement, nullptr) !=
      SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  StatementPtr statement(raw_statement, sqlite3_finalize);

  if (!bind_text(statement.get(), 1, account_username) ||
      !bind_text(statement.get(), 2, group_name) ||
      sqlite3_bind_int64(statement.get(), 3,
                         static_cast<sqlite3_int64>(count)) != SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  messages.clear();

  while (true) {
    const int result = sqlite3_step(statement.get());

    if (result == SQLITE_DONE) {
      break;
    }

    if (result != SQLITE_ROW) {
      error = sqlite_error(database_);
      return false;
    }

    LocalGroupMessage message;
    message.server_message_id =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
    message.account_username = account_username;
    message.group_name =
        reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 1));
    message.sender_username =
        reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 2));
    message.content =
        reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 3));
    message.received_at_unix_ms =
        static_cast<std::int64_t>(sqlite3_column_int64(statement.get(), 4));
    message.outgoing = sqlite3_column_int(statement.get(), 5) != 0;
    message.offline_delivery = sqlite3_column_int(statement.get(), 6) != 0;

    messages.push_back(std::move(message));
  }

  std::reverse(messages.begin(), messages.end());
  return true;
}

bool SqliteClient::cache_file_transfer(const LocalFileTransfer &file,
                                       std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  static constexpr const char *sql =
      "INSERT INTO file_transfers("
      "account_username,server_transfer_id,scope,"
      "peer_username,group_name,sender_username,"
      "file_name,local_path,file_size,sha256_hex,"
      "received_at_unix_ms,outgoing"
      ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(account_username,server_transfer_id) "
      "DO UPDATE SET "
      "scope=excluded.scope,"
      "peer_username=excluded.peer_username,"
      "group_name=excluded.group_name,"
      "sender_username=excluded.sender_username,"
      "file_name=excluded.file_name,"
      "local_path=excluded.local_path,"
      "file_size=excluded.file_size,"
      "sha256_hex=excluded.sha256_hex,"
      "received_at_unix_ms=excluded.received_at_unix_ms,"
      "outgoing=excluded.outgoing";

  sqlite3_stmt *raw_statement = nullptr;

  if (sqlite3_prepare_v2(database_, sql, -1, &raw_statement, nullptr) !=
      SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  StatementPtr statement(raw_statement, sqlite3_finalize);

  const bool bound =
      bind_text(statement.get(), 1, file.account_username) &&
      sqlite3_bind_int64(statement.get(), 2,
                         static_cast<sqlite3_int64>(file.server_transfer_id)) ==
          SQLITE_OK &&
      bind_text(statement.get(), 3, file.scope) &&
      bind_text(statement.get(), 4, file.peer_username) &&
      bind_text(statement.get(), 5, file.group_name) &&
      bind_text(statement.get(), 6, file.sender_username) &&
      bind_text(statement.get(), 7, file.file_name) &&
      bind_text(statement.get(), 8, file.local_path) &&
      sqlite3_bind_int64(statement.get(), 9,
                         static_cast<sqlite3_int64>(file.file_size)) ==
          SQLITE_OK &&
      bind_text(statement.get(), 10, file.sha256_hex) &&
      sqlite3_bind_int64(
          statement.get(), 11,
          static_cast<sqlite3_int64>(file.received_at_unix_ms)) == SQLITE_OK &&
      sqlite3_bind_int(statement.get(), 12, file.outgoing ? 1 : 0) == SQLITE_OK;

  if (!bound) {
    error = sqlite_error(database_);
    return false;
  }

  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    error = sqlite_error(database_);
    return false;
  }

  return true;
}

bool SqliteClient::recent_file_transfers(const std::string &account_username,
                                         std::size_t count,
                                         std::vector<LocalFileTransfer> &files,
                                         std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  static constexpr const char *sql =
      "SELECT server_transfer_id,scope,peer_username,"
      "group_name,sender_username,file_name,local_path,"
      "file_size,sha256_hex,received_at_unix_ms,outgoing "
      "FROM file_transfers "
      "WHERE account_username=? "
      "ORDER BY server_transfer_id DESC LIMIT ?";

  sqlite3_stmt *raw_statement = nullptr;

  if (sqlite3_prepare_v2(database_, sql, -1, &raw_statement, nullptr) !=
      SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  StatementPtr statement(raw_statement, sqlite3_finalize);

  if (!bind_text(statement.get(), 1, account_username) ||
      sqlite3_bind_int64(statement.get(), 2,
                         static_cast<sqlite3_int64>(count)) != SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  files.clear();

  while (true) {
    const int result = sqlite3_step(statement.get());

    if (result == SQLITE_DONE) {
      break;
    }

    if (result != SQLITE_ROW) {
      error = sqlite_error(database_);
      return false;
    }

    LocalFileTransfer file;
    file.server_transfer_id =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
    file.account_username = account_username;

    auto column_text = [&](int column) {
      const unsigned char *value = sqlite3_column_text(statement.get(), column);

      return value == nullptr
                 ? std::string()
                 : std::string(reinterpret_cast<const char *>(value));
    };

    file.scope = column_text(1);
    file.peer_username = column_text(2);
    file.group_name = column_text(3);
    file.sender_username = column_text(4);
    file.file_name = column_text(5);
    file.local_path = column_text(6);
    file.file_size =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 7));
    file.sha256_hex = column_text(8);
    file.received_at_unix_ms =
        static_cast<std::int64_t>(sqlite3_column_int64(statement.get(), 9));
    file.outgoing = sqlite3_column_int(statement.get(), 10) != 0;

    files.push_back(std::move(file));
  }

  std::reverse(files.begin(), files.end());

  return true;
}

bool SqliteClient::save_pending_upload(const LocalPendingUpload &upload,
                                       std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  static constexpr const char *sql =
      "INSERT INTO pending_uploads("
      "account_username,transfer_token,scope,target,"
      "source_path,file_name,file_size,sha256_hex,"
      "created_at_unix_ms"
      ") VALUES(?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(account_username,transfer_token) "
      "DO UPDATE SET "
      "scope=excluded.scope,"
      "target=excluded.target,"
      "source_path=excluded.source_path,"
      "file_name=excluded.file_name,"
      "file_size=excluded.file_size,"
      "sha256_hex=excluded.sha256_hex,"
      "created_at_unix_ms=excluded.created_at_unix_ms";

  sqlite3_stmt *raw_statement = nullptr;

  if (sqlite3_prepare_v2(database_, sql, -1, &raw_statement, nullptr) !=
      SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  StatementPtr statement(raw_statement, sqlite3_finalize);

  const bool bound =
      bind_text(statement.get(), 1, upload.account_username) &&
      bind_text(statement.get(), 2, upload.transfer_token) &&
      bind_text(statement.get(), 3, upload.scope) &&
      bind_text(statement.get(), 4, upload.target) &&
      bind_text(statement.get(), 5, upload.source_path) &&
      bind_text(statement.get(), 6, upload.file_name) &&
      sqlite3_bind_int64(statement.get(), 7,
                         static_cast<sqlite3_int64>(upload.file_size)) ==
          SQLITE_OK &&
      bind_text(statement.get(), 8, upload.sha256_hex) &&
      sqlite3_bind_int64(
          statement.get(), 9,
          static_cast<sqlite3_int64>(upload.created_at_unix_ms)) == SQLITE_OK;

  if (!bound) {
    error = sqlite_error(database_);
    return false;
  }

  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    error = sqlite_error(database_);
    return false;
  }

  return true;
}

bool SqliteClient::list_pending_uploads(
    const std::string &account_username,
    std::vector<LocalPendingUpload> &uploads, std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  static constexpr const char *sql =
      "SELECT transfer_token,scope,target,source_path,"
      "file_name,file_size,sha256_hex,created_at_unix_ms "
      "FROM pending_uploads "
      "WHERE account_username=? "
      "ORDER BY created_at_unix_ms,transfer_token";

  sqlite3_stmt *raw_statement = nullptr;

  if (sqlite3_prepare_v2(database_, sql, -1, &raw_statement, nullptr) !=
      SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  StatementPtr statement(raw_statement, sqlite3_finalize);

  if (!bind_text(statement.get(), 1, account_username)) {
    error = sqlite_error(database_);
    return false;
  }

  uploads.clear();

  auto column_text = [&](int column) {
    const unsigned char *value = sqlite3_column_text(statement.get(), column);

    return value == nullptr
               ? std::string()
               : std::string(reinterpret_cast<const char *>(value));
  };

  while (true) {
    const int result = sqlite3_step(statement.get());

    if (result == SQLITE_DONE) {
      break;
    }

    if (result != SQLITE_ROW) {
      error = sqlite_error(database_);
      return false;
    }

    LocalPendingUpload upload;
    upload.account_username = account_username;
    upload.transfer_token = column_text(0);
    upload.scope = column_text(1);
    upload.target = column_text(2);
    upload.source_path = column_text(3);
    upload.file_name = column_text(4);
    upload.file_size =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 5));
    upload.sha256_hex = column_text(6);
    upload.created_at_unix_ms =
        static_cast<std::int64_t>(sqlite3_column_int64(statement.get(), 7));

    uploads.push_back(std::move(upload));
  }

  return true;
}

bool SqliteClient::remove_pending_upload(const std::string &account_username,
                                         const std::string &transfer_token,
                                         std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  static constexpr const char *sql = "DELETE FROM pending_uploads "
                                     "WHERE account_username=? "
                                     "AND transfer_token=?";

  sqlite3_stmt *raw_statement = nullptr;

  if (sqlite3_prepare_v2(database_, sql, -1, &raw_statement, nullptr) !=
      SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  StatementPtr statement(raw_statement, sqlite3_finalize);

  if (!bind_text(statement.get(), 1, account_username) ||
      !bind_text(statement.get(), 2, transfer_token)) {
    error = sqlite_error(database_);
    return false;
  }

  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    error = sqlite_error(database_);
    return false;
  }

  return true;
}

bool SqliteClient::save_partial_download(const LocalPartialDownload &download,
                                         std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  static constexpr const char *sql =
      "INSERT INTO partial_downloads("
      "account_username,server_transfer_id,scope,"
      "sender_username,group_name,file_name,temp_path,"
      "file_size,sha256_hex"
      ") VALUES(?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(account_username,server_transfer_id) "
      "DO UPDATE SET "
      "scope=excluded.scope,"
      "sender_username=excluded.sender_username,"
      "group_name=excluded.group_name,"
      "file_name=excluded.file_name,"
      "temp_path=excluded.temp_path,"
      "file_size=excluded.file_size,"
      "sha256_hex=excluded.sha256_hex";

  sqlite3_stmt *raw_statement = nullptr;

  if (sqlite3_prepare_v2(database_, sql, -1, &raw_statement, nullptr) !=
      SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  StatementPtr statement(raw_statement, sqlite3_finalize);

  const bool bound =
      bind_text(statement.get(), 1, download.account_username) &&
      sqlite3_bind_int64(statement.get(), 2,
                         static_cast<sqlite3_int64>(
                             download.server_transfer_id)) == SQLITE_OK &&
      bind_text(statement.get(), 3, download.scope) &&
      bind_text(statement.get(), 4, download.sender_username) &&
      bind_text(statement.get(), 5, download.group_name) &&
      bind_text(statement.get(), 6, download.file_name) &&
      bind_text(statement.get(), 7, download.temp_path) &&
      sqlite3_bind_int64(statement.get(), 8,
                         static_cast<sqlite3_int64>(download.file_size)) ==
          SQLITE_OK &&
      bind_text(statement.get(), 9, download.sha256_hex);

  if (!bound) {
    error = sqlite_error(database_);
    return false;
  }

  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    error = sqlite_error(database_);
    return false;
  }

  return true;
}

bool SqliteClient::get_partial_download(
    const std::string &account_username, std::uint64_t transfer_id,
    std::optional<LocalPartialDownload> &download, std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  static constexpr const char *sql =
      "SELECT scope,sender_username,group_name,file_name,"
      "temp_path,file_size,sha256_hex "
      "FROM partial_downloads "
      "WHERE account_username=? "
      "AND server_transfer_id=? "
      "LIMIT 1";

  sqlite3_stmt *raw_statement = nullptr;

  if (sqlite3_prepare_v2(database_, sql, -1, &raw_statement, nullptr) !=
      SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  StatementPtr statement(raw_statement, sqlite3_finalize);

  if (!bind_text(statement.get(), 1, account_username) ||
      sqlite3_bind_int64(statement.get(), 2,
                         static_cast<sqlite3_int64>(transfer_id)) !=
          SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  const int result = sqlite3_step(statement.get());

  if (result == SQLITE_DONE) {
    download.reset();
    return true;
  }

  if (result != SQLITE_ROW) {
    error = sqlite_error(database_);
    return false;
  }

  auto column_text = [&](int column) {
    const unsigned char *value = sqlite3_column_text(statement.get(), column);

    return value == nullptr
               ? std::string()
               : std::string(reinterpret_cast<const char *>(value));
  };

  LocalPartialDownload item;
  item.server_transfer_id = transfer_id;
  item.account_username = account_username;
  item.scope = column_text(0);
  item.sender_username = column_text(1);
  item.group_name = column_text(2);
  item.file_name = column_text(3);
  item.temp_path = column_text(4);
  item.file_size =
      static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 5));
  item.sha256_hex = column_text(6);

  download = std::move(item);
  return true;
}

bool SqliteClient::remove_partial_download(const std::string &account_username,
                                           std::uint64_t transfer_id,
                                           std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  static constexpr const char *sql = "DELETE FROM partial_downloads "
                                     "WHERE account_username=? "
                                     "AND server_transfer_id=?";

  sqlite3_stmt *raw_statement = nullptr;

  if (sqlite3_prepare_v2(database_, sql, -1, &raw_statement, nullptr) !=
      SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  StatementPtr statement(raw_statement, sqlite3_finalize);

  if (!bind_text(statement.get(), 1, account_username) ||
      sqlite3_bind_int64(statement.get(), 2,
                         static_cast<sqlite3_int64>(transfer_id)) !=
          SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    error = sqlite_error(database_);
    return false;
  }

  return true;
}

bool SqliteClient::stats(const std::string &account_username,
                         LocalCacheStats &stats_value, std::string &error) {
  std::lock_guard<std::mutex> lock(mutex_);

  static constexpr const char *sql = "SELECT "
                                     "(SELECT COUNT(*) FROM private_messages "
                                     "WHERE account_username=?),"
                                     "(SELECT COUNT(*) FROM group_messages "
                                     "WHERE account_username=?),"
                                     "(SELECT COUNT(*) FROM file_transfers "
                                     "WHERE account_username=?)";

  sqlite3_stmt *raw_statement = nullptr;

  if (sqlite3_prepare_v2(database_, sql, -1, &raw_statement, nullptr) !=
      SQLITE_OK) {
    error = sqlite_error(database_);
    return false;
  }

  StatementPtr statement(raw_statement, sqlite3_finalize);

  if (!bind_text(statement.get(), 1, account_username) ||
      !bind_text(statement.get(), 2, account_username) ||
      !bind_text(statement.get(), 3, account_username)) {
    error = sqlite_error(database_);
    return false;
  }

  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    error = sqlite_error(database_);
    return false;
  }

  stats_value.private_messages =
      static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 0));

  stats_value.group_messages =
      static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 1));

  stats_value.files =
      static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 2));

  return true;
}
// 执行不需要返回数据的SQL（建表，删除
bool SqliteClient::execute(const std::string &sql, std::string &error) {
  char *raw_error = nullptr;

  int result =
      sqlite3_exec(database_, sql.c_str(), nullptr, nullptr, &raw_error);

  if (result == SQLITE_OK) {
    return true;
  }

  error = raw_error ? raw_error : sqlite_error(database_);

  sqlite3_free(raw_error);
  return false;
}
// 初始话表结构
bool SqliteClient::initialize_schema(std::string &error) {
  static constexpr const char *schema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA foreign_keys=ON;

CREATE TABLE IF NOT EXISTS private_messages (
    account_username TEXT NOT NULL,
    server_message_id INTEGER NOT NULL,
    peer_username TEXT NOT NULL,
    sender_username TEXT NOT NULL,
    recipient_username TEXT NOT NULL,
    content TEXT NOT NULL,
    received_at_unix_ms INTEGER NOT NULL,
    outgoing INTEGER NOT NULL CHECK (outgoing IN (0,1)),
    offline_delivery INTEGER NOT NULL
        CHECK (offline_delivery IN (0,1)),
    PRIMARY KEY (account_username, server_message_id)
);

CREATE INDEX IF NOT EXISTS
idx_local_private_peer
ON private_messages(
    account_username,
    peer_username,
    server_message_id
);

CREATE TABLE IF NOT EXISTS group_messages (
    account_username TEXT NOT NULL,
    server_message_id INTEGER NOT NULL,
    group_name TEXT NOT NULL,
    sender_username TEXT NOT NULL,
    content TEXT NOT NULL,
    received_at_unix_ms INTEGER NOT NULL,
    outgoing INTEGER NOT NULL CHECK (outgoing IN (0,1)),
    offline_delivery INTEGER NOT NULL
        CHECK (offline_delivery IN (0,1)),
    PRIMARY KEY (account_username, server_message_id)
);

CREATE INDEX IF NOT EXISTS
idx_local_group_name
ON group_messages(
    account_username,
    group_name,
    server_message_id
);

CREATE TABLE IF NOT EXISTS file_transfers (
    account_username TEXT NOT NULL,
    server_transfer_id INTEGER NOT NULL,
    scope TEXT NOT NULL,
    peer_username TEXT NOT NULL DEFAULT '',
    group_name TEXT NOT NULL DEFAULT '',
    sender_username TEXT NOT NULL,
    file_name TEXT NOT NULL,
    local_path TEXT NOT NULL,
    file_size INTEGER NOT NULL,
    sha256_hex TEXT NOT NULL,
    received_at_unix_ms INTEGER NOT NULL,
    outgoing INTEGER NOT NULL CHECK (outgoing IN (0,1)),
    PRIMARY KEY (account_username, server_transfer_id)
);

CREATE INDEX IF NOT EXISTS
idx_local_files_account
ON file_transfers(
    account_username,
    server_transfer_id
);

CREATE TABLE IF NOT EXISTS pending_uploads (
    account_username TEXT NOT NULL,
    transfer_token TEXT NOT NULL,
    scope TEXT NOT NULL,
    target TEXT NOT NULL,
    source_path TEXT NOT NULL,
    file_name TEXT NOT NULL,
    file_size INTEGER NOT NULL,
    sha256_hex TEXT NOT NULL,
    created_at_unix_ms INTEGER NOT NULL,
    PRIMARY KEY (account_username, transfer_token)
);

CREATE INDEX IF NOT EXISTS
idx_pending_uploads_account
ON pending_uploads(
    account_username,
    created_at_unix_ms,
    transfer_token
);

CREATE TABLE IF NOT EXISTS partial_downloads (
    account_username TEXT NOT NULL,
    server_transfer_id INTEGER NOT NULL,
    scope TEXT NOT NULL,
    sender_username TEXT NOT NULL,
    group_name TEXT NOT NULL DEFAULT '',
    file_name TEXT NOT NULL,
    temp_path TEXT NOT NULL,
    file_size INTEGER NOT NULL,
    sha256_hex TEXT NOT NULL,
    PRIMARY KEY (account_username, server_transfer_id)
);
)SQL";

  return execute(schema, error);
}

void SqliteClient::close_locked() {
  if (database_ != nullptr) {
    sqlite3_close(database_);
    database_ = nullptr;
  }

  database_path_.clear();
}
