#include "mysql_user_repository.hpp"
#include "password_hasher.hpp"

#include <cstring>
#include <memory>
#include <sstream>
#include <type_traits>
#include <utility>
namespace chat{
    
namespace{
//匿名命名空间：把内部实现细节隐藏起来，只在这个cpp文件内部可见，不会暴露在其他地方
struct StatementCloser{
    //自动释放MYSQL的预处理语句资源，放置内存泄漏
    void operator()(MYSQL_STMT* statement)const{
        if(statement!=nullptr)//确保及时是空指针也不会崩溃
        mysql_stmt_close(statement);
    }
};
//起别名，它可以自动调用close释放资源
using StatementPtr=std::unique_ptr<MYSQL_STMT,StatementCloser>;

//封装mysql预处理语句和准备过程
StatementPtr prepare_statement(
    MYSQL* connection,//建立连接的预处理语句
    const char* sql,
    std::string& error
){
    //初始化预处理语句---模板
    StatementPtr statement(mysql_stmt_init(connection));
    if(!statement){
        error="mysql_stmt_init failed: " +
              std::string(mysql_error(connection));
        return {};
    }
    /*1. 发送SQL给服务端
    2.服务端解析sql,检查存在与否，缓存执行计划
    3.返回ID保存在statement里面*/
    if(mysql_stmt_prepare(statement.get(),sql,static_cast<unsigned long>(std::strlen(sql)))!=0){
        error="mysql_stmt_prepare failed: " +
            std::string(mysql_stmt_error(statement.get()));
        return {};
    }
    //返回已准备的智能指针，调用者可以绑定，执行～
    return statement;
}
}
MysqlUserRepository::MysqlUserRepository(DatabaseConfig config)
:config_(std::move(config)){}
MysqlUserRepository::~MysqlUserRepository(){
    close_connection();
}
//初始化
//实现：关闭旧连接+建立新连接+建表
bool MysqlUserRepository::initialize(std::string& error){
    close_connection();
    if(!connect(true,error))return false;
    if(!create_schema(error))return false;
    return true;
}
//在mysql的users表中加载所有已经注册的用户名，存在数组里
bool MysqlUserRepository::load_usernames(
    std::vector<std::string>& usernames,
    std::string& error
){
    usernames.clear();
    if (!ensure_connected(error)) {
        return false;
    }
    constexpr const char* kSql =
        "SELECT username "
        "FROM users "
        "ORDER BY id";

        //执行这个语句
    if (mysql_query(connection_, kSql) != 0) {
        error =
            "failed to load usernames: " +
            std::string(mysql_error(connection_));
        return false;
    }
    //将查到的记过从mysql服务端拉取到客户端，返回一个结果及对象、
    //查询+去结果+遍历
    MYSQL_RES* raw_result =
        mysql_store_result(connection_);
    if (raw_result == nullptr) {
        if (mysql_field_count(connection_) == 0) {
            return true;
        }

        error =
            "mysql_store_result failed: " +
            std::string(mysql_error(connection_));
        return false;
    }

    //受到结果集之后把他交给智能指针管理
    std::unique_ptr<
        MYSQL_RES,
        decltype(&mysql_free_result)
    > result(raw_result, &mysql_free_result);

    MYSQL_ROW row = nullptr;

    //mysql_fetch_row：每次调用，它从 result 中取出下一行数据的指针，并内部移动游标指向下一行。当没有更多行时，返回 nullptr。
    while ((row = mysql_fetch_row(result.get())) != nullptr) {
        if (row[0] != nullptr) {
            usernames.emplace_back(row[0]);
        }
    }
    return true;
}


CreateUserResult MysqlUserRepository::create_user(
    const std::string& username,
    const std::string& password,
    std::string&error
){
    if(!ensure_connected(error)){
        return CreateUserResult::Error;
    }
    PasswordRecord password_record;

    //1.密码传进去，生成随即盐和哈希值
    if(!PasswordHasher::create(
        password,password_record,error
    )){
        return CreateUserResult::Error;
    }

    //SQL插入元素预处理语句
    constexpr const char* kSql=
        "INSERT INTO users ("
        "username, "
        "password_salt, "
        "password_hash, "
        "password_iterations"
        ") VALUES (?, ?, ?, ?)";

    //为预处理语句连接智能指针，把占内存和对内存一起处理
    StatementPtr statement=prepare_statement(connection_,kSql,error);
    if(!statement){
        return CreateUserResult::Error;
    }

    //之后是防止SQL注入
    unsigned long username_length =
        static_cast<unsigned long>(
            username.size()
        );

    unsigned long salt_length =
        static_cast<unsigned long>(
            password_record.salt.size()
        );

    unsigned long hash_length =
        static_cast<unsigned long>(
            password_record.hash.size()
        );

    unsigned int iterations =
        password_record.iterations;

    MYSQL_BIND parameters[4]{};

    parameters[0].buffer_type =
        MYSQL_TYPE_STRING;
    parameters[0].buffer =
        const_cast<char*>(username.data());
    parameters[0].buffer_length =
        username_length;
    parameters[0].length =
        &username_length;

    parameters[1].buffer_type =
        MYSQL_TYPE_BLOB;
    parameters[1].buffer =
        password_record.salt.data();
    parameters[1].buffer_length =
        salt_length;
    parameters[1].length =
        &salt_length;

    parameters[2].buffer_type =
        MYSQL_TYPE_BLOB;
    parameters[2].buffer =
        password_record.hash.data();
    parameters[2].buffer_length =
        hash_length;
    parameters[2].length =
        &hash_length;

    parameters[3].buffer_type =
        MYSQL_TYPE_LONG;
    parameters[3].buffer =
        &iterations;
    parameters[3].is_unsigned = 1;

    //把 C++ 变量（username、salt）的内存地址直接告诉 MySQL 服务器
    //，让 MySQL 自己把二进制数据安全地填到 ? 位置
    if (
        mysql_stmt_bind_param(
            statement.get(),
            parameters
        ) != 0
    ) {
        error =
            "mysql_stmt_bind_param failed: " +
            std::string(
                mysql_stmt_error(statement.get())
            );
        return CreateUserResult::Error;
    }

    if (
        mysql_stmt_execute(statement.get()) != 0
    ) {
        if (
            mysql_stmt_errno(statement.get()) ==
            1062
        ) {
            return CreateUserResult::AlreadyExists;
        }

        error =
            "failed to create user: " +
            std::string(
                mysql_stmt_error(statement.get())
            );
        return CreateUserResult::Error;
    }
    return CreateUserResult::Success;
}

VerifyUserResult MysqlUserRepository::verify_user(
    const std::string& username,
    const std::string& password,
    std::string& error)
{
    
    if (!ensure_connected(error)) {
        return VerifyUserResult::Error;
    }
    //根据用户名查找盐，哈希值，迭代次数
    constexpr const char* kSql=
        "SELECT "
        "password_salt, "
        "password_hash, "
        "password_iterations "
        "FROM users "
        "WHERE username = ? "
        "LIMIT 1";
    //绑定智能指针
    StatementPtr statement=prepare_statement(connection_,kSql,error);
    if(!statement)
        return VerifyUserResult::Error;

    unsigned long username_length =
        static_cast<unsigned long>(
            username.size()
        );

    MYSQL_BIND parameter{};

    parameter.buffer_type = MYSQL_TYPE_STRING;
    parameter.buffer =
        const_cast<char*>(username.data());
    parameter.buffer_length = username_length;
    parameter.length = &username_length;


///---根据用户名找数据保存在本地
    //把username绑定到SQL的？部分
    if(mysql_stmt_bind_param(statement.get(),&parameter)!=0){
        error =
            "mysql_stmt_bind_param failed: " +
            std::string(
                mysql_stmt_error(statement.get())
            );
        return VerifyUserResult::Error;
    }
    //执行SQL语句
    if(mysql_stmt_execute(statement.get())!=0){
        error =
            "failed to query user: " +
            std::string(
                mysql_stmt_error(statement.get())
            );
        return VerifyUserResult::Error;
    }
    //查到的结果保存在本地内存
    if(mysql_stmt_store_result(statement.get())!=0){
        error =
            "mysql_stmt_store_result failed: " +
            std::string(
                mysql_stmt_error(statement.get())
            );
        return VerifyUserResult::Error;
    }

///绑定结果并提取
    unsigned char salt_buffer[64]{};
    unsigned char hash_buffer[64]{};
    unsigned int iterations = 0;

    unsigned long lengths[3]{};
    using BindBoolean =
        std::remove_pointer_t<
            decltype(MYSQL_BIND{}.is_null)
        >;

    BindBoolean is_null[3]{};
    BindBoolean result_errors[3]{};

    MYSQL_BIND results[3]{};

    results[0].buffer_type = MYSQL_TYPE_BLOB;
    results[0].buffer = salt_buffer;
    results[0].buffer_length =
        sizeof(salt_buffer);
    results[0].length = &lengths[0];
    results[0].is_null = &is_null[0];
    results[0].error = &result_errors[0];

    results[1].buffer_type = MYSQL_TYPE_BLOB;
    results[1].buffer = hash_buffer;
    results[1].buffer_length =
        sizeof(hash_buffer);
    results[1].length = &lengths[1];
    results[1].is_null = &is_null[1];
    results[1].error = &result_errors[1];

    results[2].buffer_type = MYSQL_TYPE_LONG;
    results[2].buffer = &iterations;
    results[2].is_unsigned = 1;
    results[2].length = &lengths[2];
    results[2].is_null = &is_null[2];
    results[2].error = &result_errors[2];

    if (
        mysql_stmt_bind_result(
            statement.get(),
            results
        ) != 0
    ) {
        error =
            "mysql_stmt_bind_result failed: " +
            std::string(
                mysql_stmt_error(statement.get())
            );
        return VerifyUserResult::Error;
    }

    const int fetch_result =
        mysql_stmt_fetch(statement.get());

    if (fetch_result == MYSQL_NO_DATA) {
        return VerifyUserResult::InvalidCredentials;
    }

    if (
        fetch_result == MYSQL_DATA_TRUNCATED ||
        fetch_result == 1 ||
        result_errors[0] ||
        result_errors[1] ||
        result_errors[2] ||
        is_null[0] ||
        is_null[1] ||
        is_null[2]
    ) {
        error =
            "stored password data is invalid or "
            "could not be read";
        return VerifyUserResult::Error;
    }

    PasswordRecord record;
    record.salt.assign(
        salt_buffer,
        salt_buffer + lengths[0]
    );
    record.hash.assign(
        hash_buffer,
        hash_buffer + lengths[1]
    );
    record.iterations = iterations;

    bool matches = false;

    //比较
    if (
        !PasswordHasher::verify(
            password,
            record,
            matches,
            error
        )
    ) {
        return VerifyUserResult::Error;
    }

    return matches
        ? VerifyUserResult::Success
        : VerifyUserResult::InvalidCredentials;
}


//目的：
//实现：初始化+配置连接参数+连接
bool MysqlUserRepository::connect(
    bool select_database_on_connect,
    std::string& error
){
    close_connection();
    connection_=mysql_init(nullptr);
    if(connection_==nullptr){
        error="mysql_init failed";
        return false;
    }

    //配置超时
    unsigned int timeout =config_.connect_timeout_seconds;
    if(mysql_options(
        connection_,
        MYSQL_OPT_CONNECT_TIMEOUT,
        &timeout
    )!=0){
        error =
            "failed to configure MySQL timeout: " +
            std::string(mysql_error(connection_));
        close_connection();
        return false;
    }
    //配置字符集，那么客户端就会使用"utf8mb4"进行编码
    constexpr const char* kCharset="utf8mb4";
    if(mysql_options(
        connection_,
            MYSQL_SET_CHARSET_NAME,
            kCharset
    )!=0){
        error =
            "failed to configure MySQL charset: " +
            std::string(mysql_error(connection_));
        close_connection();
        return false;
    }

    const char*database=select_database_on_connect
            ? config_.database.c_str()
            : nullptr;
    //连接
    if(mysql_real_connect(
       connection_,
       config_.host.c_str(),      // 主机名或 IP
       config_.user.c_str(),      // 用户名
       config_.password.c_str(),  // 密码
       database,                  // 注意：此处可能为 nullptr
       config_.port,              // 端口（通常是 3306）
       nullptr,                   // Unix socket 路径（Windows 忽略，传空）
       0                          // 客户端标志位（0 为默认行为）
        ) == nullptr)
    {
        error =
            "cannot connect to MySQL: " +
            std::string(mysql_error(connection_));
        close_connection();
        return false;

    }
    return true;
}
//连接存在且存活，否则重新连接
bool  MysqlUserRepository::ensure_connected(
    std::string& error
){
    if(connection_!=nullptr&&mysql_ping(connection_)==0){
        return true;
    }
    return connect(true,error);
}
//在mysql数据库自偶都能够创建users数据表，如果存在不创建，在initalize调用
bool MysqlUserRepository::create_schema(std::string&error){
    constexpr const char* kSql =
        "CREATE TABLE IF NOT EXISTS users ("
        "id BIGINT UNSIGNED NOT NULL "//一个自增的学号
        "AUTO_INCREMENT,"
        "username VARCHAR(20) "//一个不能重名的英文名
        "CHARACTER SET ascii "//强制只允许 ASCII 字符
        "COLLATE ascii_bin NOT NULL,"//采用二进制排序
        "password_salt VARBINARY(16) NOT NULL,"//固定存储 16 字节的二进制盐值
        "password_hash VARBINARY(32) NOT NULL,"//存储 32 字节的哈希值
        "password_iterations INT UNSIGNED "//存储 PBKDF2/Argon2 等算法的迭代次数
        "NOT NULL,"
        "created_at TIMESTAMP NOT NULL "
        "DEFAULT CURRENT_TIMESTAMP,"//插入时自动填充当前服务器时间
        "updated_at TIMESTAMP NOT NULL "
        "DEFAULT CURRENT_TIMESTAMP "//只要该行任何一个字段发生更新（UPDATE），updated_at 会自动更新为当前时间戳，无需你在 C++ 的 UPDATE 语句中手动设置 NOW()
        "ON UPDATE CURRENT_TIMESTAMP,"
        "PRIMARY KEY (id),"//聚簇索引，数据物理按 ID 排序
        "UNIQUE KEY uq_users_username "
        "(username)"
        ") ENGINE=InnoDB";

        //执行一条SQL语句
    if(mysql_query(connection_,kSql)!=0){
        error =
            "failed to create users table: " +
            std::string(mysql_error(connection_));
        return false;
    }
    return true;
}



//资源回收：切断与mysql服务器的连接
void MysqlUserRepository::close_connection(){
    if(connection_!=nullptr){
        //mysql函数，1.向服务器发送停止2.关闭底层TCP,3.释放connection结构体内存
        mysql_close(connection_);
        connection_=nullptr;
    }
}
}