#pragma once

#include "database_config.hpp"
#include "user_repository.hpp"

#include <mysql/mysql.h>

namespace chat{
class MysqlUserRepository final 
    :public IUserRepository{
public:
    //构造函数，准备config
    explicit MysqlUserRepository(DatabaseConfig config);
    ~MysqlUserRepository()override;
    MysqlUserRepository(const MysqlUserRepository&)=delete;

    MysqlUserRepository& operator=(const MysqlUserRepository& ) = delete;

    //连接数据库
    bool initialize(std::string& error)override;
    //在数据库里，把所有注册的用户名列出来到usernames
    bool load_usernames(std::vector<std::string>& usernames,std::string&error)override;
    //向数据库的users表插入一条新记录，若名字重复，则返回kalreadyexist
    CreateUserResult create_user(const std::string&username,const std::string& password,std::string& error)override;

    //登陆，验证密码是否正确，正确valid反之invalid
    VerifyUserResult verify_user(
        const std::string& username,
        const std::string& password,
        std::string& error)override;
private:
    //使用成员变量里的IP和密码，调用MYSQL连接服务器，选择服务器
    bool connect(bool select_database,std::string& error);
    //确保一直连接，每次增删改查前都看一下，如果断开连接就使用connect重新连接
    bool ensure_connected(std::string& error);
    //执行SQL语句，只负责建立表格，但不负责插入数据
    bool create_schema(std::string& error);
    //断开连接，把指针释放掉nullptr,析构和出错时用到
    void close_connection();


    DatabaseConfig config_;//配置

    MYSQL*connection_=nullptr;/*指向客户端内存中的某个结构体的指针变量，
    结构体是mysql控制块，里面有与数据库通信所需的所有上下文信息*/
};
}