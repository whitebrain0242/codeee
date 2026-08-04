#include "mysql_friend_repository.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

namespace chat{
namespace{
    //运算符重载
struct StatementCloser {
    void operator()(MYSQL_STMT* statement) const {
        if (statement != nullptr) {
            mysql_stmt_close(statement);
        }
    }
};
using StatementPtr =
    std::unique_ptr<MYSQL_STMT, StatementCloser>;

//预处理SQL语句---放置SQl注入
StatementPtr prepare_statement(
    MYSQL* connection,
    const char* sql,
    std::string& error
) {//初始化：创建一个预处理句柄，保存在智能指针中，使用get获取句柄
    StatementPtr statement(
        mysql_stmt_init(connection)
    );

    if (!statement) {
        error =
            "mysql_stmt_init failed: " +
            std::string(mysql_error(connection));
        return {};
    }
//把输入的SQL和句柄发送给MYSQL服务器，服务起解析生成语法书，执行计划，
//存在元数据包中，发过去客户端内部自动接收与解析
    if (
        mysql_stmt_prepare(
            statement.get(),
            sql,
            static_cast<unsigned long>(
                std::strlen(sql)
            )
        ) != 0
    ) {
        error =
            "mysql_stmt_prepare failed: " +
            std::string(
                mysql_stmt_error(statement.get())
            );
        return {};
    }

    return statement;
}
//作为字符串参数绑定的封装
//使用在用户名，密码
void bind_string(
    MYSQL_BIND& binding,//空
    const std::string& value,//答案
    unsigned long& length//存储字符串的实际长度
) {
    length = static_cast<unsigned long>(
        value.size()
    );

    binding = {};
    //VARCHAR、CHAR、TEXT--MYSQL对应类型
    binding.buffer_type = MYSQL_TYPE_STRING;
    binding.buffer =
        const_cast<char*>(value.data());
    binding.buffer_length = length;
    binding.length = &length;
}
//把二进制数据绑定到预处理语句的占位符（mysql的blob列）上面：盐和哈希
void bind_blob(
    MYSQL_BIND& binding,
    const std::string& value,
    unsigned long& length
) {
    length = static_cast<unsigned long>(
        value.size()
    );

    binding = {};
    //BLOB、VARBINARY、BINARY对应的SQL类型
    binding.buffer_type = MYSQL_TYPE_BLOB;
    binding.buffer =
        const_cast<char*>(value.data());
    binding.buffer_length = length;
    binding.length = &length;
}
//将执行预处理语句和错误处理打包在一起，把打包好的文件发送给服务端
bool execute_statement(
    MYSQL_STMT* statement,
    std::string& error
) {
    if (mysql_stmt_execute(statement) != 0) {
        error =
            "mysql_stmt_execute failed: " +
            std::string(
                mysql_stmt_error(statement)
            );
        return false;
    }

    return true;
}
}


MySqlFriendRepository::MySqlFriendRepository(
    DatabaseConfig config
)
    : config_(std::move(config)) {
}

MySqlFriendRepository::~MySqlFriendRepository() {
    close_connection();
}

//初始化，为好友关系
bool MySqlFriendRepository::initialize(
    std::string& error
) {
    close_connection();
//建立新的MYSQL连接
    if (!connect(error)) {
        return false;
    }
//执行SQL语句，创建好友关系需要的数据库表，确保表存在
    return create_schema(error);
}

//服务起启动时
//从数据库加载所有已经存在的好友关系和待处理的好友请求，填充到friendstate结构体中
bool MySqlFriendRepository::load_state(
    FriendState& state,
    std::string& error
) {
    state = {};//清空输出结构提

    if (!ensure_connected(error)) {
        return false;
    }
//查找所有的好友关系
    constexpr const char* kFriendshipsSql =
        "SELECT "
        "left_user.username, "
        "right_user.username "
        "FROM friendships friendship "
        "JOIN users left_user "//去users表查询，起名为leftuser
        "ON left_user.id = "
        "friendship.user_id_low "//通过这个
        "JOIN users right_user "
        "ON right_user.id = "
        "friendship.user_id_high "
        "ORDER BY friendship.user_id_low, "
        "friendship.user_id_high";

        //给服务器发送一个SQL命令，检查执行是否成功
    if (
        mysql_query(
            connection_,
            kFriendshipsSql
        ) != 0
    ) {
        error =
            "failed to load friendships: " +
            std::string(mysql_error(connection_));
        return false;
    }
//输入：活动的MYSQL连接句柄，输出存放着所有查询结果行的结构体
//发给服务器的SQL语句后，返回的数据在服务端的网络缓冲区，要显示调用存回本地内存
    MYSQL_RES* raw_friendships =
        mysql_store_result(connection_);

    if (raw_friendships == nullptr) {
        error =
            "mysql_store_result failed for "
            "friendships: " +
            std::string(mysql_error(connection_));
        return false;
    }
//创建一个智能指针，用来管理之前的结果行，确保退出作用于的时候会自己释放内存
    std::unique_ptr<MYSQL_RES,decltype(&mysql_free_result)> friendships(
        raw_friendships,
        &mysql_free_result
    );
//从MYSQL结果及中逐行取出数据，转换称数据结构
    MYSQL_ROW row = nullptr;

    while (
        (row = mysql_fetch_row(
            friendships.get()
        )) != nullptr
    ) {
        if (
            row[0] != nullptr &&
            row[1] != nullptr
        ) {
            state.friendships.emplace_back(
                row[0],
                row[1]
            );
        }
    }

    constexpr const char* kRequestsSql =
        "SELECT "
        "sender.username, "
        "receiver.username "
        "FROM friend_requests request "
        "JOIN users sender "
        "ON sender.id = request.sender_user_id "
        "JOIN users receiver "
        "ON receiver.id = "
        "request.receiver_user_id "
        "ORDER BY request.created_at, "
        "request.sender_user_id, "
        "request.receiver_user_id";

    if (
        mysql_query(
            connection_,
            kRequestsSql
        ) != 0
    ) {
        error =
            "failed to load friend requests: " +
            std::string(mysql_error(connection_));
        return false;
    }

    MYSQL_RES* raw_requests =
        mysql_store_result(connection_);

    if (raw_requests == nullptr) {
        error =
            "mysql_store_result failed for "
            "friend requests: " +
            std::string(mysql_error(connection_));
        return false;
    }

    std::unique_ptr<
        MYSQL_RES,
        decltype(&mysql_free_result)
    > requests(
        raw_requests,
        &mysql_free_result
    );

    while (
        (row = mysql_fetch_row(
            requests.get()
        )) != nullptr
    ) {
        if (
            row[0] != nullptr &&
            row[1] != nullptr
        ) {
            state.pending_requests.emplace_back(
                row[0],
                row[1]
            );
        }
    }

    return true;
}

//把发送好友请求通过SQL写入MYSQL数据库
FriendMutationResult
MySqlFriendRepository::create_request(
    const std::string& sender_username,
    const std::string& receiver_username,
    const std::string& protobuf_event,
    std::string& error
) {//检查mysql连接是否有效
    if (!ensure_connected(error)) {
        return FriendMutationResult::Error;
    }
//发送 START TRANSACTION SQL。这是 事务开始 的标志
    if (!begin_transaction(error)) {
        return FriendMutationResult::Error;
    }

    constexpr const char* kSql =
        "INSERT INTO friend_requests ("
        "sender_user_id, "
        "receiver_user_id"
        ") "
        "SELECT sender.id, receiver.id "
        "FROM users sender "
        "JOIN users receiver ON 1 = 1 "
        "WHERE sender.username = ? "
        "AND receiver.username = ? "
        "AND sender.id <> receiver.id";

    StatementPtr statement =
        prepare_statement(
            connection_,
            kSql,
            error
        );

    if (!statement) {
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    MYSQL_BIND parameters[2]{};
    unsigned long lengths[2]{};

    bind_string(
        parameters[0],
        sender_username,
        lengths[0]
    );

    bind_string(
        parameters[1],
        receiver_username,
        lengths[1]
    );

    if (
        mysql_stmt_bind_param(
            statement.get(),
            parameters
        ) != 0
    ) {
        error =
            "failed to bind friend request: " +
            std::string(
                mysql_stmt_error(statement.get())
            );
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    if (
        mysql_stmt_execute(statement.get()) != 0
    ) {
        if (
            mysql_stmt_errno(statement.get()) ==
            1062
        ) {
            rollback_transaction();
            return FriendMutationResult::AlreadyExists;
        }

        error =
            "failed to create friend request: " +
            std::string(
                mysql_stmt_error(statement.get())
            );
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    if (
        mysql_stmt_affected_rows(
            statement.get()
        ) != 1
    ) {
        rollback_transaction();
        return FriendMutationResult::NotFound;
    }

    if (
        !insert_event(
            sender_username,
            receiver_username,
            protobuf_event,
            error
        )
    ) {
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    if (!commit_transaction(error)) {
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    return FriendMutationResult::Success;
}

//同意，在申请表中删除，在好友列表中添加，事件记录添加
FriendMutationResult
MySqlFriendRepository::accept_request(
    const std::string& requester_username,
    const std::string& accepter_username,
    const std::string& protobuf_event,
    std::string& error
) {
    if (!ensure_connected(error)) {
        return FriendMutationResult::Error;
    }
//事务开启，确保后续两个都要成功，要么全部撤销
    if (!begin_transaction(error)) {
        return FriendMutationResult::Error;
    }

    constexpr const char* kDeleteSql =
        "DELETE request "
        "FROM friend_requests request "
        "JOIN users requester "
        "ON requester.id = "
        "request.sender_user_id "
        "JOIN users accepter "
        "ON accepter.id = "
        "request.receiver_user_id "
        "WHERE requester.username = ? "
        "AND accepter.username = ?";

    StatementPtr delete_statement =
        prepare_statement(
            connection_,
            kDeleteSql,
            error
        );

    if (!delete_statement) {
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    MYSQL_BIND delete_parameters[2]{};
    unsigned long delete_lengths[2]{};

    bind_string(
        delete_parameters[0],
        requester_username,
        delete_lengths[0]
    );

    bind_string(
        delete_parameters[1],
        accepter_username,
        delete_lengths[1]
    );

    if (
        mysql_stmt_bind_param(
            delete_statement.get(),
            delete_parameters
        ) != 0 ||
        !execute_statement(
            delete_statement.get(),
            error
        )
    ) {
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    if (
        mysql_stmt_affected_rows(
            delete_statement.get()
        ) != 1
    ) {
        rollback_transaction();
        return FriendMutationResult::NotFound;
    }

    constexpr const char* kInsertSql =
        "INSERT INTO friendships ("
        "user_id_low, "
        "user_id_high"
        ") "
        "SELECT "
        "LEAST(requester.id, accepter.id), "
        "GREATEST(requester.id, accepter.id) "
        "FROM users requester "
        "JOIN users accepter ON 1 = 1 "
        "WHERE requester.username = ? "
        "AND accepter.username = ? "
        "AND requester.id <> accepter.id";

    StatementPtr insert_statement =
        prepare_statement(
            connection_,
            kInsertSql,
            error
        );

    if (!insert_statement) {
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    MYSQL_BIND insert_parameters[2]{};
    unsigned long insert_lengths[2]{};

    bind_string(
        insert_parameters[0],
        requester_username,
        insert_lengths[0]
    );

    bind_string(
        insert_parameters[1],
        accepter_username,
        insert_lengths[1]
    );

    if (
        mysql_stmt_bind_param(
            insert_statement.get(),
            insert_parameters
        ) != 0
    ) {
        error =
            "failed to bind friendship insert: " +
            std::string(
                mysql_stmt_error(
                    insert_statement.get()
                )
            );
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    if (
        mysql_stmt_execute(
            insert_statement.get()
        ) != 0
    ) {
        if (
            mysql_stmt_errno(
                insert_statement.get()
            ) == 1062
        ) {
            rollback_transaction();
            return FriendMutationResult::AlreadyExists;
        }

        error =
            "failed to create friendship: " +
            std::string(
                mysql_stmt_error(
                    insert_statement.get()
                )
            );
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    if (
        !insert_event(
            accepter_username,
            requester_username,
            protobuf_event,
            error
        )
    ) {
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    if (!commit_transaction(error)) {
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    return FriendMutationResult::Success;
}




//拒绝：只需要在friendrequest中删除一条记录，生成一个事件日志
FriendMutationResult
MySqlFriendRepository::reject_request(
    const std::string& requester_username,
    const std::string& rejecter_username,
    const std::string& protobuf_event,
    std::string& error
) {
    if (!ensure_connected(error)) {
        return FriendMutationResult::Error;
    }

    if (!begin_transaction(error)) {
        return FriendMutationResult::Error;
    }

    constexpr const char* kSql =
        "DELETE request "
        "FROM friend_requests request "
        "JOIN users requester "
        "ON requester.id = "
        "request.sender_user_id "
        "JOIN users rejecter "
        "ON rejecter.id = "
        "request.receiver_user_id "
        "WHERE requester.username = ? "
        "AND rejecter.username = ?";

    StatementPtr statement =
        prepare_statement(
            connection_,
            kSql,
            error
        );

    if (!statement) {
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    MYSQL_BIND parameters[2]{};
    unsigned long lengths[2]{};

    bind_string(
        parameters[0],
        requester_username,
        lengths[0]
    );

    bind_string(
        parameters[1],
        rejecter_username,
        lengths[1]
    );

    if (
        mysql_stmt_bind_param(
            statement.get(),
            parameters
        ) != 0 ||
        !execute_statement(
            statement.get(),
            error
        )
    ) {
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    if (
        mysql_stmt_affected_rows(
            statement.get()
        ) != 1
    ) {
        rollback_transaction();
        return FriendMutationResult::NotFound;
    }

    if (
        !insert_event(
            rejecter_username,
            requester_username,
            protobuf_event,
            error
        )
    ) {
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    if (!commit_transaction(error)) {
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    return FriendMutationResult::Success;
}




//删除好友，双方的好友表都进行操作，记录审计事件
FriendMutationResult
MySqlFriendRepository::remove_friend(
    const std::string& actor_username,
    const std::string& target_username,
    const std::string& protobuf_event,
    std::string& error
) {
    if (!ensure_connected(error)) {
        return FriendMutationResult::Error;
    }

    if (!begin_transaction(error)) {
        return FriendMutationResult::Error;
    }
//根据两个用户名查询ID,并且绑定ID
    constexpr const char* kSql =
        "DELETE friendship "//用后面找的id去定位他们好友的哪一行，删除那个行
        "FROM friendships friendship "
        "JOIN users actor ON 1 = 1 "
        "JOIN users target ON 1 = 1 "
        "WHERE actor.username = ? "
        "AND target.username = ? "
        "AND friendship.user_id_low = "
        "LEAST(actor.id, target.id) "
        "AND friendship.user_id_high = "
        "GREATEST(actor.id, target.id)";
//创建一个句柄
    StatementPtr statement =
        prepare_statement(
            connection_,
            kSql,
            error
        );

    if (!statement) {
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    MYSQL_BIND parameters[2]{};
    unsigned long lengths[2]{};

    bind_string(
        parameters[0],
        actor_username,
        lengths[0]
    );

    bind_string(
        parameters[1],
        target_username,
        lengths[1]
    );

    if (//填空
        mysql_stmt_bind_param(
            statement.get(),
            parameters
        ) != 0 ||
        !execute_statement(
            statement.get(),
            error
        )
    ) {
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    if (//行数检查，确认只操作了一行
        mysql_stmt_affected_rows(
            statement.get()
        ) != 1
    ) {
        rollback_transaction();//出现错误就回滚，撤销这个操作
        return FriendMutationResult::NotFound;
    }
//插入日志
    if (
        !insert_event(
            actor_username,
            target_username,
            protobuf_event,
            error
        )
    ) {
        rollback_transaction();
        return FriendMutationResult::Error;
    }
//事务提交，任何一个操作失误都会回滚
    if (!commit_transaction(error)) {
        rollback_transaction();
        return FriendMutationResult::Error;
    }

    return FriendMutationResult::Success;
}


//从好友事件表中，加载某个用户的n个好友事件记录，客户端查看好友动态

bool MySqlFriendRepository::load_recent_events(
    const std::string& username,
    std::size_t count,
    std::vector<StoredFriendEvent>& events,//输出：二进制字节流，需要反序列化
    std::string& error
) {//清空输出容器
    events.clear();

    if (!ensure_connected(error)) {
        return false;
    }

    constexpr const char* kSql =
    "SELECT event.id, event.payload "
    "FROM friend_events event "
    "JOIN users cu "          // 别名改为 cu
    "ON cu.username = ? "
    "WHERE event.actor_user_id = cu.id "
    "OR event.target_user_id = cu.id "
    "ORDER BY event.id DESC "
    "LIMIT ?";

    StatementPtr statement =
        prepare_statement(
            connection_,
            kSql,
            error
        );

    if (!statement) {
        return false;
    }

    unsigned long username_length = 0;
    unsigned long long limit =
        static_cast<unsigned long long>(count);

    MYSQL_BIND parameters[2]{};

    bind_string(
        parameters[0],
        username,
        username_length
    );

    parameters[1].buffer_type =
        MYSQL_TYPE_LONGLONG;
    parameters[1].buffer = &limit;
    parameters[1].is_unsigned = 1;

    if (
        mysql_stmt_bind_param(
            statement.get(),
            parameters
        ) != 0 ||
        !execute_statement(
            statement.get(),
            error
        )
    ) {
        return false;
    }

    if (
        mysql_stmt_store_result(
            statement.get()
        ) != 0
    ) {
        error =
            "failed to buffer friend events: " +
            std::string(
                mysql_stmt_error(statement.get())
            );
        return false;
    }
//绑定结果列
    constexpr std::size_t kMaxEventPayload = 4096;
//分配内存
    unsigned long long database_id = 0;
    char payload_buffer[kMaxEventPayload]{};
    unsigned long payload_length = 0;

    using BindBoolean =
        std::remove_pointer_t<
            decltype(MYSQL_BIND{}.is_null)
        >;

    BindBoolean is_null[2]{};
    BindBoolean result_errors[2]{};
    unsigned long lengths[2]{};

    MYSQL_BIND results[2]{};

    results[0].buffer_type =
        MYSQL_TYPE_LONGLONG;
    results[0].buffer = &database_id;//第一列放id
    results[0].is_unsigned = 1;
    results[0].length = &lengths[0];
    results[0].is_null = &is_null[0];
    results[0].error = &result_errors[0];

    results[1].buffer_type = MYSQL_TYPE_BLOB;
    results[1].buffer = payload_buffer;//第二列
    results[1].buffer_length =
        static_cast<unsigned long>(
            sizeof(payload_buffer)//缓冲区大小上限
        );
    results[1].length = &payload_length;//实际长度
    results[1].is_null = &is_null[1];
    results[1].error = &result_errors[1];
//存在result中
    if (
        mysql_stmt_bind_result(
            statement.get(),
            results
        ) != 0
    ) {
        error =
            "failed to bind friend event "
            "results: " +
            std::string(
                mysql_stmt_error(statement.get())
            );
        return false;
    }
//循环取行
    while (true) {
        const int fetch =
            mysql_stmt_fetch(statement.get());

        if (fetch == MYSQL_NO_DATA) {
            break;
        }
//检查异常
        if (
            fetch == 1 ||
            fetch == MYSQL_DATA_TRUNCATED ||
            is_null[0] ||
            is_null[1] ||
            result_errors[0] ||
            result_errors[1] ||
            payload_length >
                sizeof(payload_buffer)
        ) {
            error =
                "invalid or oversized friend "
                "event row";
            return false;
        }
//把数据转换成c++对象
        events.push_back(
            StoredFriendEvent{
                static_cast<std::uint64_t>(
                    database_id
                ),
                std::string(
                    payload_buffer,
                    payload_length
                )
            }
        );
    }
//反转顺序，这样就就是从旧到新
    std::reverse(
        events.begin(),
        events.end()
    );

    return true;
}




//建立MYSQL数据库连接
bool MySqlFriendRepository::connect(
    std::string& error
) {
    close_connection();
//初始化mysql句柄
    connection_ = mysql_init(nullptr);

    if (connection_ == nullptr) {
        error = "mysql_init failed";
        return false;
    }
//设置连接超时
    unsigned int timeout =
        config_.connect_timeout_seconds;

    if (
        mysql_options(
            connection_,
            MYSQL_OPT_CONNECT_TIMEOUT,
            &timeout
        ) != 0
    ) {
        error =
            "failed to configure MySQL "
            "timeout: " +
            std::string(mysql_error(connection_));
        close_connection();
        return false;
    }
//设置字符集
    constexpr const char* kCharset = "utf8mb4";

    if (
        mysql_options(
            connection_,
            MYSQL_SET_CHARSET_NAME,
            kCharset
        ) != 0
    ) {
        error =
            "failed to configure MySQL "
            "charset: " +
            std::string(mysql_error(connection_));
        close_connection();
        return false;
    }
//真正建立连接，成功返回connnection,失败nullptr
    if (
        mysql_real_connect(
            connection_,
            config_.host.c_str(),
            config_.user.c_str(),
            config_.password.c_str(),
            config_.database.c_str(),
            config_.port,
            nullptr,
            0
        ) == nullptr
    ) {
        error =
            "cannot connect to MySQL: " +
            std::string(mysql_error(connection_));
        close_connection();
        return false;
    }

    return true;
}



//确保当前MYSQL连接有效，如果断开就自动重新连接
bool MySqlFriendRepository::ensure_connected(
    std::string& error
) {
    if (
        connection_ != nullptr &&
        mysql_ping(connection_) == 0
    ) {
        return true;
    }

    return connect(error);
}



//在MYSQL中创建好友系统所需的三张核心数据表
bool MySqlFriendRepository::create_schema(
    std::string& error
) {
    constexpr const char* kFriendRequestsSql =
        "CREATE TABLE IF NOT EXISTS "
        "friend_requests ("
        "sender_user_id BIGINT UNSIGNED "
        "NOT NULL,"
        "receiver_user_id BIGINT UNSIGNED "
        "NOT NULL,"
        "created_at TIMESTAMP(3) NOT NULL "
        "DEFAULT CURRENT_TIMESTAMP(3),"
        "PRIMARY KEY ("
        "sender_user_id, receiver_user_id"
        "),"
        "KEY idx_friend_requests_receiver "
        "(receiver_user_id, created_at),"
        "CONSTRAINT fk_friend_requests_sender "
        "FOREIGN KEY (sender_user_id) "
        "REFERENCES users(id) "
        "ON DELETE CASCADE,"
        "CONSTRAINT fk_friend_requests_receiver "
        "FOREIGN KEY (receiver_user_id) "
        "REFERENCES users(id) "
        "ON DELETE CASCADE,"
        "CONSTRAINT chk_friend_request_users "
        "CHECK ("
        "sender_user_id <> receiver_user_id"
        ")"
        ") ENGINE=InnoDB";

    constexpr const char* kFriendshipsSql =
        "CREATE TABLE IF NOT EXISTS "
        "friendships ("
        "user_id_low BIGINT UNSIGNED NOT NULL,"
        "user_id_high BIGINT UNSIGNED NOT NULL,"
        "created_at TIMESTAMP(3) NOT NULL "
        "DEFAULT CURRENT_TIMESTAMP(3),"
        "PRIMARY KEY ("
        "user_id_low, user_id_high"
        "),"
        "KEY idx_friendships_high "
        "(user_id_high),"
        "CONSTRAINT fk_friendships_low "
        "FOREIGN KEY (user_id_low) "
        "REFERENCES users(id) "
        "ON DELETE CASCADE,"
        "CONSTRAINT fk_friendships_high "
        "FOREIGN KEY (user_id_high) "
        "REFERENCES users(id) "
        "ON DELETE CASCADE,"
        "CONSTRAINT chk_friendship_order "
        "CHECK (user_id_low < user_id_high)"
        ") ENGINE=InnoDB";

    constexpr const char* kFriendEventsSql =
        "CREATE TABLE IF NOT EXISTS "
        "friend_events ("
        "id BIGINT UNSIGNED NOT NULL "
        "AUTO_INCREMENT,"
        "actor_user_id BIGINT UNSIGNED "
        "NOT NULL,"
        "target_user_id BIGINT UNSIGNED "
        "NOT NULL,"
        "payload BLOB NOT NULL,"
        "created_at TIMESTAMP(3) NOT NULL "
        "DEFAULT CURRENT_TIMESTAMP(3),"
        "PRIMARY KEY (id),"
        "KEY idx_friend_events_actor "
        "(actor_user_id, id),"
        "KEY idx_friend_events_target "
        "(target_user_id, id),"
        "CONSTRAINT fk_friend_events_actor "
        "FOREIGN KEY (actor_user_id) "
        "REFERENCES users(id) "
        "ON DELETE CASCADE,"
        "CONSTRAINT fk_friend_events_target "
        "FOREIGN KEY (target_user_id) "
        "REFERENCES users(id) "
        "ON DELETE CASCADE"
        ") ENGINE=InnoDB";

    const char* statements[] = {
        kFriendRequestsSql,
        kFriendshipsSql,
        kFriendEventsSql
    };

    for (const char* statement : statements) {
        if (
            mysql_query(
                connection_,
                statement
            ) != 0
        ) {
            error =
                "failed to create friend "
                "schema: " +
                std::string(
                    mysql_error(connection_)
                );
            return false;
        }
    }

    return true;
}



//向MYSQL发送指令，开启一个事务上下文，确保后续所有SQL操作在同一个源自工作单元中执行
bool MySqlFriendRepository::begin_transaction(
    std::string& error
) {
    if (//服务器给MYSQL发送SQL指令
        mysql_query(
            connection_,
            "START TRANSACTION"
        ) != 0
    ) {
        error =
            "failed to start transaction: " +
            std::string(mysql_error(connection_));
        return false;
    }

    return true;
}


//把所有已经执行成功的SQL修改，持久化到硬盘文件里，让数据尘埃落地
bool MySqlFriendRepository::commit_transaction(
    std::string& error
) {//服务起向MYSQL发送一各COMMIT指令包，0提交成功，非0失败
    if (mysql_commit(connection_) != 0) {
        error =
            "failed to commit transaction: " +
            std::string(mysql_error(connection_));
        return false;
    }

    return true;
}

//取消当前事务中从BEGIN以来执行d所有未提交的数据库修改，让数据库回到事务开始的状态
void MySqlFriendRepository::rollback_transaction() {
    if (connection_ != nullptr) {
        mysql_rollback(connection_);//服务器向MYSQL发送一个ROLLBACK包
    }
}

//审计日志记录器：把每一次的好友操作以二进制proto格式写进friendevent表
//传入用户名和二进制字节流：输出：将两人的ID和二进制字节流一起写进事件表
//1.写SQL语句2.初始化，传给MYSQL3.绑定返回的元数据表4.发送给MYSQL服务器保存
bool MySqlFriendRepository::insert_event(
    const std::string& actor_username,
    const std::string& target_username,
    const std::string& protobuf_event,//protobuf二进制数据字节流
    std::string& error
) {//核心SQL语句：实现：用户名-ID转换——BLOB数据插入
    constexpr const char* kSql =
        "INSERT INTO friend_events ("
        "actor_user_id, "
        "target_user_id, "
        "payload"
        ") "
        "SELECT actor.id, target.id, ? "
        "FROM users actor "
        "JOIN users target ON 1 = 1 "
        "WHERE actor.username = ? "
        "AND target.username = ?";

    StatementPtr statement =
        prepare_statement(
            connection_,
            kSql,
            error
        );

    if (!statement) {
        return false;
    }

    MYSQL_BIND parameters[3]{};
    unsigned long lengths[3]{};
//因为protobuf_event是二进制字节流，
    bind_blob(
        parameters[0],
        protobuf_event,
        lengths[0]
    );
//因为用户名`是纯文本
    bind_string(
        parameters[1],
        actor_username,
        lengths[1]
    );

    bind_string(
        parameters[2],
        target_username,
        lengths[2]
    );

    if (
        mysql_stmt_bind_param(
            statement.get(),
            parameters
        ) != 0 ||
        !execute_statement(
            statement.get(),
            error
        )
    ) {
        return false;
    }
//获取上一条预处理语句执行后，实际被修改插入删除的行数，这里SQL语句期望巧好插入一条记录
    if (
        mysql_stmt_affected_rows(
            statement.get()
        ) != 1
    ) {
        error =
            "friend event participants do not "
            "exist";
        return false;
    }

    return true;
}

//资源清理函数，关闭与MYSQL的连接并且释放相关资源
void MySqlFriendRepository::close_connection() {
    if (connection_ != nullptr) {
        mysql_close(connection_);
        connection_ = nullptr;
    }
}

}