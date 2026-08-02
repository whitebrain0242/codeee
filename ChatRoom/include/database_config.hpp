#pragma once
#include <string>
//使用mysql的前置声明
namespace chat{
//存储数据库的各种信息
struct DatabaseConfig{
    std::string host = "127.0.0.1";//mysql服务器主机地址
    unsigned int port = 3306;//mysql服务的端口
    std::string user = "chatroom";//连接数据库的默认用户名字
    std::string password;//连接数据库使用的密码
    std::string database = "chatroom";//默认要连接的数据库的名称
    unsigned int connect_timeout_seconds = 5;//连接超时时间
};
//作用：从指定路径读取配置文件，解析之后填充到config中
bool load_database_config(
    const std::string& path,
    DatabaseConfig& config,
    std::string& error
);
//检查环境变量，如果环境变量存在就覆盖config中
void apply_database_environment_overrides(DatabaseConfig& config);

//对于输入的信息，比如说是库名，表名，进行安全检验
bool is_valid_database_identifier(const std::string& value);
}