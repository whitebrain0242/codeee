#pragma once

#include <cstdint>
#include <string>
#include <vector>
//密码哈希
namespace chat{
//密码记录：存储盐，哈希值，迭代次数
struct PasswordRecord{
    std::vector<unsigned char> salt;
    std::vector<unsigned char> hash;
    std::uint32_t iterations=0;
};
//密码哈希
class PasswordHasher{
public:
    static constexpr std::size_t kSaltSize=16;//盐之
    static constexpr std::size_t kHashSize = 32;//输出哈希
    static constexpr std::uint32_t kDefaultIterations =210000;//迭代次数

    //创建密码记录，生成盐，将盐和迭代次数传给derive
    static bool create(
        const std::string& password,
        PasswordRecord& record,
        std::string& error
    );
    //验证密码--登录使用，取出record里的salt,iterations,passwords+盐--哈希迭代计算
    //比较算出来的哈希和record.hash是否一样，结果赋值给match
    static bool verify(
        const std::string& password,//此时输入密码
        const PasswordRecord& record,//存储的盐，迭代次数，哈希值
        bool& matches,//是否一致`
        std::string& error
    );

private:
    //哈希私有化：外部不能直接调用，生成哈希值
    static bool derive(
        const std::string&password,
        const std::vector<unsigned char>&salt,//盐值
        std::uint32_t iterations,//迭代次数
        std::vector<unsigned char>&output,//哈希值
        std::string& error
    );
};
    
}// namespace chat