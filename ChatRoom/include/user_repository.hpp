#pragma once

#include <string>
#include <vector>

namespace chat
{
//让服务器只需要直接调用函数就好，而不是看到具体的实现，方便换数据库之类的
enum class CreateUserResult{
    Success,
    AlreadyExists,
    Error
};

enum class VerifyUserResult {
    Success,
    InvalidCredentials,
    Error
};


class IUserRepository{
public:
    virtual ~IUserRepository()=default;
    //初始化
    virtual bool initialize(std::string& error)=0;
    //加载所有用户名：把已注册列表加载到内存，方便查询
    virtual bool load_usernames(
        std::vector<std::string>& usernames,
        std::string& error)=0;
    //创建注册用户、
    virtual CreateUserResult create_user(
        const std::string& username,
        const std::string& password,
        std::string& error
    )=0;

    //验证用户
    virtual VerifyUserResult verify_user(
        const std::string& username,
        const std::string& password,
        std::string& error
    )=0;
};
    
} // namespace chat

