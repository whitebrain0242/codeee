#include "mysql_database.hpp"
#include <deque>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using BindNullFlag = std::remove_pointer_t<decltype(MYSQL_BIND{}.is_null)>;

using Row = std::vector<std::optional<std::string>>;

using Rows = std::vector<Row>;

std::string mysql_error_text(MYSQL *connection) {
  return connection != nullptr ? mysql_error(connection)
                               : "MySQL connection is null";
}

std::string mysql_stmt_error_text(MYSQL_STMT *statement) {
  return statement != nullptr ? mysql_stmt_error(statement)
                              : "MYSQL_STMT is null";
}

class MySqlThreadGuard {
public:
  MySqlThreadGuard() { (void)mysql_thread_init(); }

  ~MySqlThreadGuard() { mysql_thread_end(); }
};

void ensure_mysql_thread() {
  thread_local MySqlThreadGuard guard;
  (void)guard;
}

thread_local MYSQL *g_active_mysql_connection = nullptr;

class ActiveMysqlConnectionScope {
public:
  explicit ActiveMysqlConnectionScope(MYSQL *connection)
      : previous_(g_active_mysql_connection) {
    g_active_mysql_connection = connection;
  }

  ~ActiveMysqlConnectionScope() { g_active_mysql_connection = previous_; }

private:
  MYSQL *previous_ = nullptr;
};

MYSQL *active_mysql_connection() { return g_active_mysql_connection; }

class Statement final {
public:
  explicit Statement(MYSQL *connection)
      : statement_(mysql_stmt_init(connection)) {}

  ~Statement() {
    if (statement_ != nullptr) {
      mysql_stmt_close(statement_);
      statement_ = nullptr;
    }
  }

  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;

  MYSQL_STMT *get() const noexcept { return statement_; }

  bool prepare(const char *sql, std::string &error) {
    if (statement_ == nullptr) {
      error = "mysql_stmt_init failed";
      return false;
    }

    if (mysql_stmt_prepare(statement_, sql,
                           static_cast<unsigned long>(std::strlen(sql))) != 0) {
      error = mysql_stmt_error_text(statement_);
      return false;
    }

    return true;
  }

private:
  MYSQL_STMT *statement_ = nullptr;
};

struct SqlParam {
  enum class Kind {
    Text,
    Blob,
    Unsigned64,
    Signed64,
    Null,
  };

  Kind kind = Kind::Null;
  std::string bytes;
  std::uint64_t unsigned_value = 0;
  std::int64_t signed_value = 0;

  static SqlParam text(std::string value) {
    SqlParam param;
    param.kind = Kind::Text;
    param.bytes = std::move(value);
    return param;
  }

  static SqlParam blob(std::string value) {
    SqlParam param;
    param.kind = Kind::Blob;
    param.bytes = std::move(value);
    return param;
  }

  static SqlParam u64(std::uint64_t value) {
    SqlParam param;
    param.kind = Kind::Unsigned64;
    param.unsigned_value = value;
    return param;
  }

  static SqlParam i64(std::int64_t value) {
    SqlParam param;
    param.kind = Kind::Signed64;
    param.signed_value = value;
    return param;
  }

  static SqlParam nullValue() { return {}; }
};

bool bind_params(MYSQL_STMT *statement, const std::vector<SqlParam> &params,
                 std::vector<MYSQL_BIND> &bindings,
                 std::vector<unsigned long> &lengths,
                 std::deque<BindNullFlag> &null_flags, std::string &error) {
  if (statement == nullptr) {
    error = "MYSQL_STMT is null";
    return false;
  }

  const unsigned long expected = mysql_stmt_param_count(statement);

  if (expected != params.size()) {
    error = "prepared statement parameter count mismatch";
    return false;
  }

  bindings.assign(params.size(), MYSQL_BIND{});

  lengths.assign(params.size(), 0UL);

  null_flags.assign(params.size(), static_cast<BindNullFlag>(0));

  for (std::size_t index = 0; index < params.size(); ++index) {
    MYSQL_BIND &binding = bindings[index];

    const SqlParam &param = params[index];

    switch (param.kind) {
    case SqlParam::Kind::Text:
    case SqlParam::Kind::Blob: {
      lengths[index] = static_cast<unsigned long>(param.bytes.size());

      binding.buffer_type = param.kind == SqlParam::Kind::Blob
                                ? MYSQL_TYPE_BLOB
                                : MYSQL_TYPE_STRING;

      binding.buffer = const_cast<char *>(param.bytes.data());

      binding.buffer_length = lengths[index];

      binding.length = &lengths[index];

      binding.is_null = reinterpret_cast<bool *>(&null_flags[index]);
      break;
    }

    case SqlParam::Kind::Unsigned64:
      binding.buffer_type = MYSQL_TYPE_LONGLONG;
      binding.buffer = const_cast<std::uint64_t *>(&param.unsigned_value);
      binding.is_unsigned = 1;
      binding.is_null = reinterpret_cast<bool *>(&null_flags[index]);
      break;

    case SqlParam::Kind::Signed64:
      binding.buffer_type = MYSQL_TYPE_LONGLONG;
      binding.buffer = const_cast<std::int64_t *>(&param.signed_value);
      binding.is_unsigned = 0;
      binding.is_null = reinterpret_cast<bool *>(&null_flags[index]);
      break;

    case SqlParam::Kind::Null:
      null_flags[index] = static_cast<BindNullFlag>(1);
      binding.buffer_type = MYSQL_TYPE_NULL;
      binding.is_null = reinterpret_cast<bool *>(&null_flags[index]);
      break;
    }
  }

  if (!bindings.empty() &&
      mysql_stmt_bind_param(statement, bindings.data()) != 0) {
    error = mysql_stmt_error_text(statement);
    return false;
  }

  return true;
}

bool execute_prepared(MYSQL *connection, const char *sql,
                      const std::vector<SqlParam> &params,
                      std::uint64_t *insert_id, std::uint64_t *affected_rows,
                      std::string &error) {
  if (connection == nullptr) {
    error = "database is not connected";
    return false;
  }

  Statement statement(connection);

  if (!statement.prepare(sql, error)) {
    return false;
  }

  std::vector<MYSQL_BIND> bindings;
  std::vector<unsigned long> lengths;
  std::deque<BindNullFlag> null_flags;

  if (!bind_params(statement.get(), params, bindings, lengths, null_flags,
                   error)) {
    return false;
  }

  if (mysql_stmt_execute(statement.get()) != 0) {
    error = mysql_stmt_error_text(statement.get());
    return false;
  }

  if (insert_id != nullptr) {
    *insert_id =
        static_cast<std::uint64_t>(mysql_stmt_insert_id(statement.get()));
  }

  if (affected_rows != nullptr) {
    *affected_rows =
        static_cast<std::uint64_t>(mysql_stmt_affected_rows(statement.get()));
  }

  return true;
}

bool query_prepared(MYSQL *connection, const char *sql,
                    const std::vector<SqlParam> &params, Rows &rows,
                    std::string &error) {
  if (connection == nullptr) {
    error = "database is not connected";
    return false;
  }

  Statement statement(connection);

  if (!statement.prepare(sql, error)) {
    return false;
  }

  std::vector<MYSQL_BIND> param_bindings;
  std::vector<unsigned long> param_lengths;
  std::deque<BindNullFlag> param_nulls;

  if (!bind_params(statement.get(), params, param_bindings, param_lengths,
                   param_nulls, error)) {
    return false;
  }

  BindNullFlag update_max_length = static_cast<BindNullFlag>(1);

  if (mysql_stmt_attr_set(statement.get(), STMT_ATTR_UPDATE_MAX_LENGTH,
                          &update_max_length) != 0) {
    error = mysql_stmt_error_text(statement.get());
    return false;
  }

  if (mysql_stmt_execute(statement.get()) != 0) {
    error = mysql_stmt_error_text(statement.get());
    return false;
  }

  std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> metadata(
      mysql_stmt_result_metadata(statement.get()), mysql_free_result);

  if (!metadata) {
    error = mysql_stmt_error_text(statement.get());
    return false;
  }

  if (mysql_stmt_store_result(statement.get()) != 0) {
    error = mysql_stmt_error_text(statement.get());
    return false;
  }

  const unsigned int field_count = mysql_num_fields(metadata.get());

  MYSQL_FIELD *fields = mysql_fetch_fields(metadata.get());

  std::vector<std::vector<char>> buffers(field_count);

  std::vector<unsigned long> result_lengths(field_count, 0UL);

  std::deque<BindNullFlag> result_nulls(field_count,
                                        static_cast<BindNullFlag>(0));

  std::deque<BindNullFlag> result_errors(field_count,
                                         static_cast<BindNullFlag>(0));

  std::vector<MYSQL_BIND> result_bindings(field_count, MYSQL_BIND{});

  for (unsigned int index = 0; index < field_count; ++index) {
    const unsigned long capacity =
        std::max<unsigned long>(fields[index].max_length, 1UL);

    buffers[index].resize(static_cast<std::size_t>(capacity));

    MYSQL_BIND &binding = result_bindings[index];

    switch (fields[index].type) {
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
      binding.buffer_type = MYSQL_TYPE_BLOB;
      break;

    default:
      binding.buffer_type = MYSQL_TYPE_STRING;
      break;
    }

    binding.buffer = buffers[index].data();

    binding.buffer_length = capacity;

    binding.length = &result_lengths[index];

    binding.is_null = reinterpret_cast<bool *>(&result_nulls[index]);

    binding.error = reinterpret_cast<bool *>(&result_errors[index]);
  }

  if (field_count > 0U &&
      mysql_stmt_bind_result(statement.get(), result_bindings.data()) != 0) {
    error = mysql_stmt_error_text(statement.get());
    return false;
  }

  rows.clear();

  while (true) {
    const int fetch_result = mysql_stmt_fetch(statement.get());

    if (fetch_result == MYSQL_NO_DATA) {
      break;
    }

    if (fetch_result == 1) {
      error = mysql_stmt_error_text(statement.get());
      return false;
    }

    if (fetch_result == MYSQL_DATA_TRUNCATED) {
      error = "prepared statement result "
              "was unexpectedly truncated";
      return false;
    }

    Row row;
    row.reserve(field_count);

    for (unsigned int index = 0; index < field_count; ++index) {
      if (result_nulls[index] != 0) {
        row.push_back(std::nullopt);
        continue;
      }

      row.emplace_back(
          std::string(buffers[index].data(), result_lengths[index]));
    }

    rows.push_back(std::move(row));
  }

  mysql_stmt_free_result(statement.get());

  return true;
}

const std::string &value_at(const Row &row, std::size_t index) {
  return row.at(index).value();
}

std::uint64_t u64_at(const Row &row, std::size_t index) {
  return static_cast<std::uint64_t>(
      std::strtoull(value_at(row, index).c_str(), nullptr, 10));
}

std::uint32_t u32_at(const Row &row, std::size_t index) {
  return static_cast<std::uint32_t>(
      std::strtoul(value_at(row, index).c_str(), nullptr, 10));
}

} // namespace

MySqlDatabase::~MySqlDatabase() = default;

bool MySqlDatabase::connect(const MySqlConfig &config, std::string &error) {
  ensure_mysql_thread();

  return pool_.initialize(config, error);
}

bool MySqlDatabase::ping(std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  if (mysql_ping(active_mysql_connection()) != 0) {
    error = mysql_error_text(active_mysql_connection());
    return false;
  }

  return true;
}

std::size_t MySqlDatabase::pool_size() const noexcept { return pool_.size(); }

bool MySqlDatabase::begin(std::string &error) {
  if (active_mysql_connection() == nullptr) {
    error = "database is not connected";
    return false;
  }

  if (mysql_autocommit(active_mysql_connection(), 0) != 0) {
    error = mysql_error_text(active_mysql_connection());
    return false;
  }

  return true;
}

bool MySqlDatabase::commit(std::string &error) {
  if (active_mysql_connection() == nullptr) {
    error = "database is not connected";
    return false;
  }

  if (mysql_commit(active_mysql_connection()) != 0) {
    error = mysql_error_text(active_mysql_connection());
    (void)mysql_autocommit(active_mysql_connection(), 1);
    return false;
  }

  if (mysql_autocommit(active_mysql_connection(), 1) != 0) {
    error = mysql_error_text(active_mysql_connection());
    return false;
  }

  return true;
}

void MySqlDatabase::rollback() {
  if (active_mysql_connection() == nullptr) {
    return;
  }

  (void)mysql_rollback(active_mysql_connection());

  (void)mysql_autocommit(active_mysql_connection(), 1);
}

std::pair<std::string, std::string>
MySqlDatabase::normalize_pair(const std::string &left,
                              const std::string &right) {
  if (left <= right) {
    return {left, right};
  }

  return {right, left};
}

bool MySqlDatabase::user_exists(const std::string &username, bool &exists,
                                std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT 1 "
                                     "FROM users "
                                     "WHERE username=? "
                                     "LIMIT 1";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(username)}, rows, error)) {
    return false;
  }

  exists = !rows.empty();
  return true;
}

bool MySqlDatabase::create_user(const std::string &username,
                                const std::string &password_hash,
                                std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "INSERT INTO users("
                                     "username,password_hash"
                                     ") VALUES(?,?)";

  return execute_prepared(
      active_mysql_connection(), sql,
      {SqlParam::text(username), SqlParam::text(password_hash)}, nullptr,
      nullptr, error);
}

bool MySqlDatabase::get_password_hash(const std::string &username,
                                      std::optional<std::string> &password_hash,
                                      std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT password_hash "
                                     "FROM users "
                                     "WHERE username=? "
                                     "LIMIT 1";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(username)}, rows, error)) {
    return false;
  }

  if (rows.empty()) {
    password_hash.reset();
    return true;
  }

  password_hash = value_at(rows[0], 0);
  return true;
}

bool MySqlDatabase::are_friends(const std::string &left,
                                const std::string &right, bool &result_value,
                                std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  const auto pair = normalize_pair(left, right);

  static constexpr const char *sql = "SELECT 1 "
                                     "FROM friendships "
                                     "WHERE user_low=? "
                                     "AND user_high=? "
                                     "LIMIT 1";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(pair.first), SqlParam::text(pair.second)},
                      rows, error)) {
    return false;
  }

  result_value = !rows.empty();
  return true;
}

bool MySqlDatabase::is_friend_blocked(const std::string &blocker,
                                      const std::string &blocked,
                                      bool &result_value, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT 1 "
                                     "FROM friend_blocks "
                                     "WHERE blocker_username=? "
                                     "AND blocked_username=? "
                                     "LIMIT 1";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(blocker), SqlParam::text(blocked)}, rows,
                      error)) {
    return false;
  }

  result_value = !rows.empty();

  return true;
}

bool MySqlDatabase::add_friend_block(const std::string &blocker,
                                     const std::string &blocked, bool &created,
                                     std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "INSERT IGNORE INTO friend_blocks("
                                     "blocker_username,"
                                     "blocked_username"
                                     ") VALUES(?,?)";

  std::uint64_t affected = 0;

  if (!execute_prepared(active_mysql_connection(), sql,
                        {SqlParam::text(blocker), SqlParam::text(blocked)},
                        nullptr, &affected, error)) {
    return false;
  }

  created = affected == 1U;

  return true;
}

bool MySqlDatabase::remove_friend_block(const std::string &blocker,
                                        const std::string &blocked,
                                        bool &removed, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "DELETE FROM friend_blocks "
                                     "WHERE blocker_username=? "
                                     "AND blocked_username=?";

  std::uint64_t affected = 0;

  if (!execute_prepared(active_mysql_connection(), sql,
                        {SqlParam::text(blocker), SqlParam::text(blocked)},
                        nullptr, &affected, error)) {
    return false;
  }

  removed = affected == 1U;

  return true;
}

bool MySqlDatabase::list_blocked_friends(const std::string &blocker,
                                         std::vector<std::string> &users,
                                         std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT blocked_username "
                                     "FROM friend_blocks "
                                     "WHERE blocker_username=? "
                                     "ORDER BY blocked_username";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql, {SqlParam::text(blocker)},
                      rows, error)) {
    return false;
  }

  users.clear();
  users.reserve(rows.size());

  for (const Row &row : rows) {
    users.push_back(value_at(row, 0));
  }

  return true;
}

bool MySqlDatabase::has_friend_request(const std::string &requester,
                                       const std::string &target,
                                       bool &result_value, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT 1 "
                                     "FROM friend_requests "
                                     "WHERE requester_username=? "
                                     "AND target_username=? "
                                     "LIMIT 1";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(requester), SqlParam::text(target)}, rows,
                      error)) {
    return false;
  }

  result_value = !rows.empty();
  return true;
}

bool MySqlDatabase::add_friend_request(const std::string &requester,
                                       const std::string &target,
                                       std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "INSERT INTO friend_requests("
                                     "requester_username,"
                                     "target_username"
                                     ") VALUES(?,?)";

  return execute_prepared(active_mysql_connection(), sql,
                          {SqlParam::text(requester), SqlParam::text(target)},
                          nullptr, nullptr, error);
}

bool MySqlDatabase::accept_friend_request(const std::string &requester,
                                          const std::string &target,
                                          std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  if (!begin(error)) {
    return false;
  }

  static constexpr const char *delete_sql = "DELETE FROM friend_requests "
                                            "WHERE requester_username=? "
                                            "AND target_username=?";

  std::uint64_t affected = 0;

  if (!execute_prepared(active_mysql_connection(), delete_sql,
                        {SqlParam::text(requester), SqlParam::text(target)},
                        nullptr, &affected, error) ||
      affected != 1U) {
    if (error.empty()) {
      error = "friend request does not exist";
    }

    rollback();
    return false;
  }

  const auto pair = normalize_pair(requester, target);

  static constexpr const char *insert_sql = "INSERT INTO friendships("
                                            "user_low,user_high"
                                            ") VALUES(?,?)";

  if (!execute_prepared(
          active_mysql_connection(), insert_sql,
          {SqlParam::text(pair.first), SqlParam::text(pair.second)}, nullptr,
          nullptr, error)) {
    rollback();
    return false;
  }

  if (!commit(error)) {
    rollback();
    return false;
  }

  return true;
}

bool MySqlDatabase::reject_friend_request(const std::string &requester,
                                          const std::string &target,
                                          bool &removed, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "DELETE FROM friend_requests "
                                     "WHERE requester_username=? "
                                     "AND target_username=?";

  std::uint64_t affected = 0;

  if (!execute_prepared(active_mysql_connection(), sql,
                        {SqlParam::text(requester), SqlParam::text(target)},
                        nullptr, &affected, error)) {
    return false;
  }

  removed = affected == 1U;
  return true;
}

bool MySqlDatabase::remove_friendship(const std::string &left,
                                      const std::string &right, bool &removed,
                                      std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  const auto pair = normalize_pair(left, right);

  if (!begin(error)) {
    return false;
  }

  static constexpr const char *friendship_sql = "DELETE FROM friendships "
                                                "WHERE user_low=? "
                                                "AND user_high=?";

  std::uint64_t affected = 0;

  if (!execute_prepared(
          active_mysql_connection(), friendship_sql,
          {SqlParam::text(pair.first), SqlParam::text(pair.second)}, nullptr,
          &affected, error)) {
    rollback();
    return false;
  }

  removed = affected == 1U;

  if (!removed) {
    if (!commit(error)) {
      rollback();
      return false;
    }

    return true;
  }

  // Blocking is defined only for an active friendship. Removing the
  // friendship therefore removes both directional block settings.
  static constexpr const char *block_cleanup_sql = "DELETE FROM friend_blocks "
                                                   "WHERE "
                                                   "(blocker_username=? "
                                                   "AND blocked_username=?) "
                                                   "OR "
                                                   "(blocker_username=? "
                                                   "AND blocked_username=?)";

  if (!execute_prepared(active_mysql_connection(), block_cleanup_sql,
                        {SqlParam::text(left), SqlParam::text(right),
                         SqlParam::text(right), SqlParam::text(left)},
                        nullptr, nullptr, error)) {
    rollback();
    return false;
  }

  if (!commit(error)) {
    rollback();
    return false;
  }

  return true;
}

bool MySqlDatabase::list_friends(const std::string &username,
                                 std::vector<std::string> &friends,
                                 std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT CASE "
                                     "WHEN user_low=? "
                                     "THEN user_high "
                                     "ELSE user_low END AS friend_name "
                                     "FROM friendships "
                                     "WHERE user_low=? "
                                     "OR user_high=? "
                                     "ORDER BY friend_name";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(username), SqlParam::text(username),
                       SqlParam::text(username)},
                      rows, error)) {
    return false;
  }

  friends.clear();
  friends.reserve(rows.size());

  for (const Row &row : rows) {
    friends.push_back(value_at(row, 0));
  }

  return true;
}

bool MySqlDatabase::list_incoming_requests(const std::string &username,
                                           std::vector<std::string> &users,
                                           std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT requester_username "
                                     "FROM friend_requests "
                                     "WHERE target_username=? "
                                     "ORDER BY requester_username";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(username)}, rows, error)) {
    return false;
  }

  users.clear();
  users.reserve(rows.size());

  for (const Row &row : rows) {
    users.push_back(value_at(row, 0));
  }

  return true;
}

bool MySqlDatabase::list_outgoing_requests(const std::string &username,
                                           std::vector<std::string> &users,
                                           std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT target_username "
                                     "FROM friend_requests "
                                     "WHERE requester_username=? "
                                     "ORDER BY target_username";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(username)}, rows, error)) {
    return false;
  }

  users.clear();
  users.reserve(rows.size());

  for (const Row &row : rows) {
    users.push_back(value_at(row, 0));
  }

  return true;
}

bool MySqlDatabase::add_friend_event(const FriendEventPayload &event,
                                     std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  std::string payload;

  if (!event.SerializeToString(&payload)) {
    error = "official protobuf failed to serialize "
            "FriendEventPayload";
    return false;
  }

  static constexpr const char *sql = "INSERT INTO friend_events("
                                     "event_type,"
                                     "actor_username,"
                                     "target_username,"
                                     "occurred_at_unix_ms,"
                                     "payload"
                                     ") VALUES(?,?,?,?,?)";

  return execute_prepared(
      active_mysql_connection(), sql,
      {SqlParam::u64(static_cast<std::uint64_t>(event.type())),
       SqlParam::text(event.actor_username()),
       SqlParam::text(event.target_username()),
       SqlParam::i64(event.occurred_at_unix_ms()),
       SqlParam::blob(std::move(payload))},
      nullptr, nullptr, error);
}

bool MySqlDatabase::insert_message_locked(const ChatMessagePayload &payload,
                                          std::uint64_t &message_id,
                                          std::string &error) {
  std::string bytes;

  if (!payload.SerializeToString(&bytes)) {
    error = "official protobuf failed to serialize "
            "ChatMessagePayload";
    return false;
  }

  static constexpr const char *sql = "INSERT INTO messages("
                                     "message_type,"
                                     "sender_username,"
                                     "recipient_username,"
                                     "created_at_unix_ms,"
                                     "payload"
                                     ") VALUES(?,?,?,?,?)";

  const SqlParam recipient = payload.recipient_username().empty()
                                 ? SqlParam::nullValue()
                                 : SqlParam::text(payload.recipient_username());

  return execute_prepared(
      active_mysql_connection(), sql,
      {SqlParam::u64(static_cast<std::uint64_t>(payload.type())),
       SqlParam::text(payload.sender_username()), recipient,
       SqlParam::i64(payload.created_at_unix_ms()),
       SqlParam::blob(std::move(bytes))},
      &message_id, nullptr, error);
}

bool MySqlDatabase::add_message(const ChatMessagePayload &payload,
                                std::uint64_t &message_id, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  return insert_message_locked(payload, message_id, error);
}

bool MySqlDatabase::add_private_message_with_delivery(
    const ChatMessagePayload &payload, std::uint64_t &message_id,
    std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  if (payload.type() != chatroom::v7::PRIVATE ||
      payload.recipient_username().empty()) {
    error = "private delivery requires "
            "a private message recipient";
    return false;
  }

  if (!begin(error)) {
    return false;
  }

  if (!insert_message_locked(payload, message_id, error)) {
    rollback();
    return false;
  }

  static constexpr const char *delivery_sql = "INSERT INTO "
                                              "private_message_deliveries("
                                              "message_id,"
                                              "recipient_username,"
                                              "delivered_at_unix_ms"
                                              ") VALUES(?,?,NULL)";

  if (!execute_prepared(active_mysql_connection(), delivery_sql,
                        {SqlParam::u64(message_id),
                         SqlParam::text(payload.recipient_username())},
                        nullptr, nullptr, error)) {
    rollback();
    return false;
  }

  if (!commit(error)) {
    rollback();
    return false;
  }

  return true;
}

bool MySqlDatabase::recent_public_messages(std::size_t count,
                                           std::vector<StoredMessage> &messages,
                                           std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT id,payload "
                                     "FROM messages "
                                     "WHERE message_type=1 "
                                     "ORDER BY id DESC "
                                     "LIMIT ?";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::u64(static_cast<std::uint64_t>(count))}, rows,
                      error)) {
    return false;
  }

  messages.clear();
  messages.reserve(rows.size());

  for (const Row &row : rows) {
    StoredMessage message;
    message.id = u64_at(row, 0);

    const std::string &bytes = value_at(row, 1);

    if (!message.payload.ParseFromArray(bytes.data(),
                                        static_cast<int>(bytes.size()))) {
      error = "failed to decode a public "
              "message payload";
      return false;
    }

    messages.push_back(std::move(message));
  }

  std::reverse(messages.begin(), messages.end());

  return true;
}

bool MySqlDatabase::recent_private_messages(
    const std::string &user_a, const std::string &user_b, std::size_t count,
    std::vector<StoredMessage> &messages, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT id,payload "
                                     "FROM messages "
                                     "WHERE message_type=2 "
                                     "AND ("
                                     "(sender_username=? "
                                     "AND recipient_username=?) "
                                     "OR "
                                     "(sender_username=? "
                                     "AND recipient_username=?)"
                                     ") "
                                     "ORDER BY id DESC "
                                     "LIMIT ?";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(user_a), SqlParam::text(user_b),
                       SqlParam::text(user_b), SqlParam::text(user_a),
                       SqlParam::u64(static_cast<std::uint64_t>(count))},
                      rows, error)) {
    return false;
  }

  messages.clear();
  messages.reserve(rows.size());

  for (const Row &row : rows) {
    StoredMessage message;
    message.id = u64_at(row, 0);

    const std::string &bytes = value_at(row, 1);

    if (!message.payload.ParseFromArray(bytes.data(),
                                        static_cast<int>(bytes.size()))) {
      error = "failed to decode a private "
              "message payload";
      return false;
    }

    messages.push_back(std::move(message));
  }

  std::reverse(messages.begin(), messages.end());

  return true;
}

bool MySqlDatabase::pending_private_messages(
    const std::string &recipient, std::size_t count,
    std::vector<StoredMessage> &messages, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT m.id,m.payload "
                                     "FROM private_message_deliveries d "
                                     "JOIN messages m "
                                     "ON m.id=d.message_id "
                                     "WHERE d.recipient_username=? "
                                     "AND d.delivered_at_unix_ms IS NULL "
                                     "AND NOT EXISTS ("
                                     "SELECT 1 "
                                     "FROM friend_blocks b "
                                     "WHERE b.blocker_username=? "
                                     "AND b.blocked_username=m.sender_username"
                                     ") "
                                     "ORDER BY m.id ASC "
                                     "LIMIT ?";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(recipient), SqlParam::text(recipient),
                       SqlParam::u64(static_cast<std::uint64_t>(count))},
                      rows, error)) {
    return false;
  }

  messages.clear();
  messages.reserve(rows.size());

  for (const Row &row : rows) {
    StoredMessage message;
    message.id = u64_at(row, 0);

    const std::string &bytes = value_at(row, 1);

    if (!message.payload.ParseFromArray(bytes.data(),
                                        static_cast<int>(bytes.size()))) {
      error = "failed to decode an offline "
              "private message";
      return false;
    }

    messages.push_back(std::move(message));
  }

  return true;
}

bool MySqlDatabase::mark_private_message_delivered(
    std::uint64_t message_id, const std::string &recipient,
    std::int64_t delivered_at_unix_ms, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "UPDATE private_message_deliveries "
                                     "SET delivered_at_unix_ms=? "
                                     "WHERE message_id=? "
                                     "AND recipient_username=? "
                                     "AND delivered_at_unix_ms IS NULL";

  return execute_prepared(active_mysql_connection(), sql,
                          {SqlParam::i64(delivered_at_unix_ms),
                           SqlParam::u64(message_id),
                           SqlParam::text(recipient)},
                          nullptr, nullptr, error);
}

bool MySqlDatabase::group_id_by_name(const std::string &group_name,
                                     std::optional<std::uint64_t> &group_id,
                                     std::string &error) {
  static constexpr const char *sql = "SELECT group_id "
                                     "FROM chat_groups "
                                     "WHERE group_name=? "
                                     "LIMIT 1";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(group_name)}, rows, error)) {
    return false;
  }

  if (rows.empty()) {
    group_id.reset();
    return true;
  }

  group_id = u64_at(rows[0], 0);

  return true;
}

bool MySqlDatabase::create_group(const std::string &group_name,
                                 const std::string &owner_username,
                                 std::uint64_t &group_id, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  if (!begin(error)) {
    return false;
  }

  static constexpr const char *group_sql = "INSERT INTO chat_groups("
                                           "group_name,owner_username"
                                           ") VALUES(?,?)";

  if (!execute_prepared(
          active_mysql_connection(), group_sql,
          {SqlParam::text(group_name), SqlParam::text(owner_username)},
          &group_id, nullptr, error)) {
    rollback();
    return false;
  }

  static constexpr const char *member_sql = "INSERT INTO group_members("
                                            "group_id,username,member_role"
                                            ") VALUES(?,?,1)";

  if (!execute_prepared(
          active_mysql_connection(), member_sql,
          {SqlParam::u64(group_id), SqlParam::text(owner_username)}, nullptr,
          nullptr, error)) {
    rollback();
    return false;
  }

  if (!commit(error)) {
    rollback();
    return false;
  }

  return true;
}

bool MySqlDatabase::get_group(const std::string &group_name,
                              std::optional<GroupInfo> &group,
                              std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT "
                                     "group_id,"
                                     "group_name,"
                                     "owner_username "
                                     "FROM chat_groups "
                                     "WHERE group_name=? "
                                     "LIMIT 1";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(group_name)}, rows, error)) {
    return false;
  }

  if (rows.empty()) {
    group.reset();
    return true;
  }

  GroupInfo info;
  info.id = u64_at(rows[0], 0);
  info.name = value_at(rows[0], 1);
  info.owner_username = value_at(rows[0], 2);

  group = std::move(info);
  return true;
}

bool MySqlDatabase::dissolve_group(const std::string &group_name,
                                   const std::string &owner_username,
                                   bool &removed, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "DELETE FROM chat_groups "
                                     "WHERE group_name=? "
                                     "AND owner_username=?";

  std::uint64_t affected = 0;

  if (!execute_prepared(
          active_mysql_connection(), sql,
          {SqlParam::text(group_name), SqlParam::text(owner_username)}, nullptr,
          &affected, error)) {
    return false;
  }

  removed = affected == 1U;
  return true;
}

bool MySqlDatabase::get_group_role(const std::string &group_name,
                                   const std::string &username,
                                   std::optional<GroupRole> &role,
                                   std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT gm.member_role "
                                     "FROM group_members gm "
                                     "JOIN chat_groups g "
                                     "ON g.group_id=gm.group_id "
                                     "WHERE g.group_name=? "
                                     "AND gm.username=? "
                                     "LIMIT 1";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(group_name), SqlParam::text(username)},
                      rows, error)) {
    return false;
  }

  if (rows.empty()) {
    role.reset();
    return true;
  }

  role = static_cast<GroupRole>(u32_at(rows[0], 0));

  return true;
}

bool MySqlDatabase::list_user_groups(const std::string &username,
                                     std::vector<GroupMembership> &groups,
                                     std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT "
                                     "g.group_id,"
                                     "g.group_name,"
                                     "g.owner_username,"
                                     "gm.member_role "
                                     "FROM group_members gm "
                                     "JOIN chat_groups g "
                                     "ON g.group_id=gm.group_id "
                                     "WHERE gm.username=? "
                                     "ORDER BY g.group_name";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(username)}, rows, error)) {
    return false;
  }

  groups.clear();
  groups.reserve(rows.size());

  for (const Row &row : rows) {
    GroupMembership membership;
    membership.group.id = u64_at(row, 0);
    membership.group.name = value_at(row, 1);
    membership.group.owner_username = value_at(row, 2);
    membership.role = static_cast<GroupRole>(u32_at(row, 3));

    groups.push_back(std::move(membership));
  }

  return true;
}

bool MySqlDatabase::list_group_members(const std::string &group_name,
                                       std::vector<GroupMemberInfo> &members,
                                       std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT "
                                     "gm.username,"
                                     "gm.member_role "
                                     "FROM group_members gm "
                                     "JOIN chat_groups g "
                                     "ON g.group_id=gm.group_id "
                                     "WHERE g.group_name=? "
                                     "ORDER BY "
                                     "gm.member_role,"
                                     "gm.username";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(group_name)}, rows, error)) {
    return false;
  }

  members.clear();
  members.reserve(rows.size());

  for (const Row &row : rows) {
    GroupMemberInfo member;
    member.username = value_at(row, 0);
    member.role = static_cast<GroupRole>(u32_at(row, 1));

    members.push_back(std::move(member));
  }

  return true;
}

bool MySqlDatabase::list_group_member_usernames(
    const std::string &group_name, std::vector<std::string> &members,
    std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT gm.username "
                                     "FROM group_members gm "
                                     "JOIN chat_groups g "
                                     "ON g.group_id=gm.group_id "
                                     "WHERE g.group_name=? "
                                     "ORDER BY gm.username";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(group_name)}, rows, error)) {
    return false;
  }

  members.clear();
  members.reserve(rows.size());

  for (const Row &row : rows) {
    members.push_back(value_at(row, 0));
  }

  return true;
}

bool MySqlDatabase::has_group_join_request(const std::string &group_name,
                                           const std::string &username,
                                           bool &exists, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT 1 "
                                     "FROM group_join_requests r "
                                     "JOIN chat_groups g "
                                     "ON g.group_id=r.group_id "
                                     "WHERE g.group_name=? "
                                     "AND r.requester_username=? "
                                     "LIMIT 1";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(group_name), SqlParam::text(username)},
                      rows, error)) {
    return false;
  }

  exists = !rows.empty();
  return true;
}

bool MySqlDatabase::add_group_join_request(const std::string &group_name,
                                           const std::string &username,
                                           std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  std::optional<std::uint64_t> group_id;

  if (!group_id_by_name(group_name, group_id, error)) {
    return false;
  }

  if (!group_id) {
    error = "group does not exist";
    return false;
  }

  static constexpr const char *sql = "INSERT INTO group_join_requests("
                                     "group_id,requester_username"
                                     ") VALUES(?,?)";

  return execute_prepared(active_mysql_connection(), sql,
                          {SqlParam::u64(*group_id), SqlParam::text(username)},
                          nullptr, nullptr, error);
}

bool MySqlDatabase::list_group_join_requests(const std::string &group_name,
                                             std::vector<std::string> &users,
                                             std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT r.requester_username "
                                     "FROM group_join_requests r "
                                     "JOIN chat_groups g "
                                     "ON g.group_id=r.group_id "
                                     "WHERE g.group_name=? "
                                     "ORDER BY "
                                     "r.created_at,"
                                     "r.requester_username";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(group_name)}, rows, error)) {
    return false;
  }

  users.clear();
  users.reserve(rows.size());

  for (const Row &row : rows) {
    users.push_back(value_at(row, 0));
  }

  return true;
}

bool MySqlDatabase::list_group_managers(const std::string &group_name,
                                        std::vector<std::string> &users,
                                        std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT gm.username "
                                     "FROM group_members gm "
                                     "JOIN chat_groups g "
                                     "ON g.group_id=gm.group_id "
                                     "WHERE g.group_name=? "
                                     "AND gm.member_role IN (1,2) "
                                     "ORDER BY "
                                     "gm.member_role,"
                                     "gm.username";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(group_name)}, rows, error)) {
    return false;
  }

  users.clear();
  users.reserve(rows.size());

  for (const Row &row : rows) {
    users.push_back(value_at(row, 0));
  }

  return true;
}

bool MySqlDatabase::list_managed_group_request_counts(
    const std::string &username,
    std::vector<ManagedGroupRequestCount> &requests, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT "
                                     "g.group_name,"
                                     "COUNT(r.requester_username) "
                                     "FROM group_members gm "
                                     "JOIN chat_groups g "
                                     "ON g.group_id=gm.group_id "
                                     "LEFT JOIN group_join_requests r "
                                     "ON r.group_id=g.group_id "
                                     "WHERE gm.username=? "
                                     "AND gm.member_role IN (1,2) "
                                     "GROUP BY "
                                     "g.group_id,"
                                     "g.group_name "
                                     "HAVING COUNT(r.requester_username)>0 "
                                     "ORDER BY g.group_name";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(username)}, rows, error)) {
    return false;
  }

  requests.clear();
  requests.reserve(rows.size());

  for (const Row &row : rows) {
    ManagedGroupRequestCount item;
    item.group_name = value_at(row, 0);
    item.pending_count = static_cast<std::size_t>(u64_at(row, 1));

    requests.push_back(std::move(item));
  }

  return true;
}

bool MySqlDatabase::approve_group_join_request(const std::string &group_name,
                                               const std::string &username,
                                               std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  std::optional<std::uint64_t> group_id;

  if (!group_id_by_name(group_name, group_id, error)) {
    return false;
  }

  if (!group_id) {
    error = "group does not exist";
    return false;
  }

  if (!begin(error)) {
    return false;
  }

  static constexpr const char *delete_sql = "DELETE FROM group_join_requests "
                                            "WHERE group_id=? "
                                            "AND requester_username=?";

  std::uint64_t affected = 0;

  if (!execute_prepared(active_mysql_connection(), delete_sql,
                        {SqlParam::u64(*group_id), SqlParam::text(username)},
                        nullptr, &affected, error) ||
      affected != 1U) {
    if (error.empty()) {
      error = "group join request does not exist";
    }

    rollback();
    return false;
  }

  static constexpr const char *insert_sql = "INSERT INTO group_members("
                                            "group_id,username,member_role"
                                            ") VALUES(?,?,3)";

  if (!execute_prepared(active_mysql_connection(), insert_sql,
                        {SqlParam::u64(*group_id), SqlParam::text(username)},
                        nullptr, nullptr, error)) {
    rollback();
    return false;
  }

  if (!commit(error)) {
    rollback();
    return false;
  }

  return true;
}

bool MySqlDatabase::reject_group_join_request(const std::string &group_name,
                                              const std::string &username,
                                              bool &removed,
                                              std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  std::optional<std::uint64_t> group_id;

  if (!group_id_by_name(group_name, group_id, error)) {
    return false;
  }

  if (!group_id) {
    error = "group does not exist";
    return false;
  }

  static constexpr const char *sql = "DELETE FROM group_join_requests "
                                     "WHERE group_id=? "
                                     "AND requester_username=?";

  std::uint64_t affected = 0;

  if (!execute_prepared(active_mysql_connection(), sql,
                        {SqlParam::u64(*group_id), SqlParam::text(username)},
                        nullptr, &affected, error)) {
    return false;
  }

  removed = affected == 1U;
  return true;
}

bool MySqlDatabase::set_group_member_role(const std::string &group_name,
                                          const std::string &username,
                                          GroupRole role, bool &changed,
                                          std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  std::optional<std::uint64_t> group_id;

  if (!group_id_by_name(group_name, group_id, error)) {
    return false;
  }

  if (!group_id) {
    error = "group does not exist";
    return false;
  }

  static constexpr const char *sql = "UPDATE group_members "
                                     "SET member_role=? "
                                     "WHERE group_id=? "
                                     "AND username=?";

  std::uint64_t affected = 0;

  if (!execute_prepared(active_mysql_connection(), sql,
                        {SqlParam::u64(static_cast<std::uint64_t>(role)),
                         SqlParam::u64(*group_id), SqlParam::text(username)},
                        nullptr, &affected, error)) {
    return false;
  }

  changed = affected == 1U;
  return true;
}

bool MySqlDatabase::remove_group_member(const std::string &group_name,
                                        const std::string &username,
                                        bool &removed, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  std::optional<std::uint64_t> group_id;

  if (!group_id_by_name(group_name, group_id, error)) {
    return false;
  }

  if (!group_id) {
    error = "group does not exist";
    return false;
  }

  static constexpr const char *sql = "DELETE FROM group_members "
                                     "WHERE group_id=? "
                                     "AND username=?";

  std::uint64_t affected = 0;

  if (!execute_prepared(active_mysql_connection(), sql,
                        {SqlParam::u64(*group_id), SqlParam::text(username)},
                        nullptr, &affected, error)) {
    return false;
  }

  removed = affected == 1U;
  return true;
}

bool MySqlDatabase::add_group_message(
    const GroupMessagePayload &payload,
    const std::vector<std::string> &recipients, std::uint64_t &message_id,
    std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  if (!begin(error)) {
    return false;
  }

  std::string bytes;

  if (!payload.SerializeToString(&bytes)) {
    error = "official protobuf failed to serialize "
            "GroupMessagePayload";
    rollback();
    return false;
  }

  static constexpr const char *message_sql = "INSERT INTO group_messages("
                                             "group_id,"
                                             "sender_username,"
                                             "created_at_unix_ms,"
                                             "payload"
                                             ") VALUES(?,?,?,?)";

  if (!execute_prepared(active_mysql_connection(), message_sql,
                        {SqlParam::u64(payload.group_id()),
                         SqlParam::text(payload.sender_username()),
                         SqlParam::i64(payload.created_at_unix_ms()),
                         SqlParam::blob(std::move(bytes))},
                        &message_id, nullptr, error)) {
    rollback();
    return false;
  }

  static constexpr const char *delivery_sql = "INSERT INTO "
                                              "group_message_deliveries("
                                              "message_id,"
                                              "recipient_username,"
                                              "delivered_at_unix_ms"
                                              ") VALUES(?,?,NULL)";

  for (const std::string &recipient : recipients) {
    if (!execute_prepared(
            active_mysql_connection(), delivery_sql,
            {SqlParam::u64(message_id), SqlParam::text(recipient)}, nullptr,
            nullptr, error)) {
      rollback();
      return false;
    }
  }

  if (!commit(error)) {
    rollback();
    return false;
  }

  return true;
}

bool MySqlDatabase::recent_group_messages(
    const std::string &group_name, std::size_t count,
    std::vector<StoredGroupMessage> &messages, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT gm.id,gm.payload "
                                     "FROM group_messages gm "
                                     "JOIN chat_groups g "
                                     "ON g.group_id=gm.group_id "
                                     "WHERE g.group_name=? "
                                     "ORDER BY gm.id DESC "
                                     "LIMIT ?";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(group_name),
                       SqlParam::u64(static_cast<std::uint64_t>(count))},
                      rows, error)) {
    return false;
  }

  messages.clear();
  messages.reserve(rows.size());

  for (const Row &row : rows) {
    StoredGroupMessage message;
    message.id = u64_at(row, 0);

    const std::string &bytes = value_at(row, 1);

    if (!message.payload.ParseFromArray(bytes.data(),
                                        static_cast<int>(bytes.size()))) {
      error = "failed to decode a group "
              "message payload";
      return false;
    }

    messages.push_back(std::move(message));
  }

  std::reverse(messages.begin(), messages.end());

  return true;
}

bool MySqlDatabase::pending_group_messages(
    const std::string &recipient, std::size_t count,
    std::vector<StoredGroupMessage> &messages, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT gm.id,gm.payload "
                                     "FROM group_message_deliveries d "
                                     "JOIN group_messages gm "
                                     "ON gm.id=d.message_id "
                                     "WHERE d.recipient_username=? "
                                     "AND d.delivered_at_unix_ms IS NULL "
                                     "ORDER BY gm.id ASC "
                                     "LIMIT ?";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(recipient),
                       SqlParam::u64(static_cast<std::uint64_t>(count))},
                      rows, error)) {
    return false;
  }

  messages.clear();
  messages.reserve(rows.size());

  for (const Row &row : rows) {
    StoredGroupMessage message;
    message.id = u64_at(row, 0);

    const std::string &bytes = value_at(row, 1);

    if (!message.payload.ParseFromArray(bytes.data(),
                                        static_cast<int>(bytes.size()))) {
      error = "failed to decode an offline "
              "group message";
      return false;
    }

    messages.push_back(std::move(message));
  }

  return true;
}

bool MySqlDatabase::mark_group_message_delivered(
    std::uint64_t message_id, const std::string &recipient,
    std::int64_t delivered_at_unix_ms, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "UPDATE group_message_deliveries "
                                     "SET delivered_at_unix_ms=? "
                                     "WHERE message_id=? "
                                     "AND recipient_username=? "
                                     "AND delivered_at_unix_ms IS NULL";

  return execute_prepared(active_mysql_connection(), sql,
                          {SqlParam::i64(delivered_at_unix_ms),
                           SqlParam::u64(message_id),
                           SqlParam::text(recipient)},
                          nullptr, nullptr, error);
}

bool MySqlDatabase::add_file_transfer(
    const FileTransferMetadata &metadata,
    const std::vector<std::string> &recipients, std::uint64_t &transfer_id,
    std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  if (recipients.empty()) {
    error = "file transfer requires "
            "at least one recipient";
    return false;
  }

  std::string bytes;

  if (!metadata.SerializeToString(&bytes)) {
    error = "official protobuf failed to serialize "
            "FileTransferMetadata";
    return false;
  }

  if (!begin(error)) {
    return false;
  }

  const SqlParam recipient =
      metadata.recipient_username().empty()
          ? SqlParam::nullValue()
          : SqlParam::text(metadata.recipient_username());

  const SqlParam group_id = metadata.group_id() == 0U
                                ? SqlParam::nullValue()
                                : SqlParam::u64(metadata.group_id());

  static constexpr const char *insert_sql = "INSERT INTO file_transfers("
                                            "transfer_token,"
                                            "scope_type,"
                                            "sender_username,"
                                            "recipient_username,"
                                            "group_id,"
                                            "file_name,"
                                            "file_size,"
                                            "sha256_hex,"
                                            "stored_relative_path,"
                                            "created_at_unix_ms,"
                                            "metadata"
                                            ") VALUES(?,?,?,?,?,?,?,?,?,?,?)";

  if (!execute_prepared(
          active_mysql_connection(), insert_sql,
          {SqlParam::text(metadata.transfer_token()),
           SqlParam::u64(static_cast<std::uint64_t>(metadata.scope())),
           SqlParam::text(metadata.sender_username()), recipient, group_id,
           SqlParam::text(metadata.file_name()),
           SqlParam::u64(metadata.file_size()),
           SqlParam::text(metadata.sha256_hex()),
           SqlParam::text(metadata.stored_relative_path()),
           SqlParam::i64(metadata.created_at_unix_ms()),
           SqlParam::blob(std::move(bytes))},
          &transfer_id, nullptr, error)) {
    rollback();
    return false;
  }

  static constexpr const char *delivery_sql = "INSERT INTO "
                                              "file_transfer_deliveries("
                                              "transfer_id,"
                                              "recipient_username,"
                                              "delivered_at_unix_ms"
                                              ") VALUES(?,?,NULL)";

  for (const std::string &recipient_name : recipients) {
    if (!execute_prepared(
            active_mysql_connection(), delivery_sql,
            {SqlParam::u64(transfer_id), SqlParam::text(recipient_name)},
            nullptr, nullptr, error)) {
      rollback();
      return false;
    }
  }

  if (!commit(error)) {
    rollback();
    return false;
  }

  return true;
}

bool MySqlDatabase::pending_file_transfers(
    const std::string &recipient, std::size_t count,
    std::vector<StoredFileTransfer> &transfers, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT f.id,f.metadata "
                                     "FROM file_transfer_deliveries d "
                                     "JOIN file_transfers f "
                                     "ON f.id=d.transfer_id "
                                     "WHERE d.recipient_username=? "
                                     "AND d.delivered_at_unix_ms IS NULL "
                                     "AND ("
                                     "f.scope_type<>1 "
                                     "OR NOT EXISTS ("
                                     "SELECT 1 "
                                     "FROM friend_blocks b "
                                     "WHERE b.blocker_username=? "
                                     "AND b.blocked_username=f.sender_username"
                                     ")"
                                     ") "
                                     "ORDER BY f.id ASC "
                                     "LIMIT ?";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::text(recipient), SqlParam::text(recipient),
                       SqlParam::u64(static_cast<std::uint64_t>(count))},
                      rows, error)) {
    return false;
  }

  transfers.clear();
  transfers.reserve(rows.size());

  for (const Row &row : rows) {
    StoredFileTransfer transfer;
    transfer.id = u64_at(row, 0);

    const std::string &bytes = value_at(row, 1);

    if (!transfer.metadata.ParseFromArray(bytes.data(),
                                          static_cast<int>(bytes.size()))) {
      error = "official protobuf failed to parse "
              "FileTransferMetadata";
      return false;
    }

    transfers.push_back(std::move(transfer));
  }

  return true;
}

bool MySqlDatabase::file_transfer_for_recipient(
    std::uint64_t transfer_id, const std::string &recipient,
    std::optional<StoredFileTransfer> &transfer, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "SELECT f.id,f.metadata "
                                     "FROM file_transfer_deliveries d "
                                     "JOIN file_transfers f "
                                     "ON f.id=d.transfer_id "
                                     "WHERE d.transfer_id=? "
                                     "AND d.recipient_username=? "
                                     "AND d.delivered_at_unix_ms IS NULL "
                                     "LIMIT 1";

  Rows rows;

  if (!query_prepared(active_mysql_connection(), sql,
                      {SqlParam::u64(transfer_id), SqlParam::text(recipient)},
                      rows, error)) {
    return false;
  }

  if (rows.empty()) {
    transfer.reset();
    return true;
  }

  StoredFileTransfer stored;
  stored.id = u64_at(rows[0], 0);

  const std::string &bytes = value_at(rows[0], 1);

  if (!stored.metadata.ParseFromArray(bytes.data(),
                                      static_cast<int>(bytes.size()))) {
    error = "official protobuf failed to parse "
            "FileTransferMetadata";
    return false;
  }

  transfer = std::move(stored);
  return true;
}

bool MySqlDatabase::mark_file_transfer_delivered(
    std::uint64_t transfer_id, const std::string &recipient,
    std::int64_t delivered_at_unix_ms, std::string &error) {
  ensure_mysql_thread();

  auto lease = pool_.acquire(error);

  if (!lease) {
    return false;
  }

  ActiveMysqlConnectionScope scope(lease.get());

  static constexpr const char *sql = "UPDATE file_transfer_deliveries "
                                     "SET delivered_at_unix_ms=? "
                                     "WHERE transfer_id=? "
                                     "AND recipient_username=? "
                                     "AND delivered_at_unix_ms IS NULL";

  return execute_prepared(active_mysql_connection(), sql,
                          {SqlParam::i64(delivered_at_unix_ms),
                           SqlParam::u64(transfer_id),
                           SqlParam::text(recipient)},
                          nullptr, nullptr, error);
}
