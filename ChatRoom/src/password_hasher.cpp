#include "password_hasher.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <limits>

namespace chat{
    
//创建密码记录，生成盐，将盐和迭代次数传给derive
//实现：初始化，调用函数生成盐，传给drive
 bool PasswordHasher::create(
    const std::string& password,
    PasswordRecord& record,
    std::string& error
){
    record.salt.assign(kSaltSize,0);
    record.hash.clear();
    record.iterations=kDefaultIterations;

    if(RAND_bytes(record.salt.data(),static_cast<int>(record.salt.size()))!=1){
        error="OpenSSL RAND_bytes failed while creating password salt";
        return false;
    }

    return derive(password,record.salt,record.iterations,record.hash,error);
}

//验证密码，将输入的密码和盐一起传入derive中，然后比较
//实现：验证内容是否存在，调用derive生成哈希值，比较
bool PasswordHasher::verify(
    const std::string& password,
    const PasswordRecord& record,
    bool& matches,
    std::string& error
){
    matches=false;
    if(record.salt.empty()||record.hash.empty()||record.iterations==0){
        error="stored password record is invalid";
        return false;
    }
    std::vector<unsigned char> candidate;

    if(!derive(password,record.salt,record.iterations,candidate,error)){
        return false;
    }

if(candidate.size()!=record.hash.size()){
    matches=false;
    return true;
}
matches=CRYPTO_memcmp(candidate.data(),record.hash.data(),candidate.size())==0;
return true;
}

//生成哈希值的函数
//实现：验证，分配空间,调用函数
bool PasswordHasher::derive(
    const std::string& password,
    const std::vector<unsigned char>& salt,
    std::uint32_t iterations,
    std::vector<unsigned char>& output,
    std::string& error
){
    if(
        password.size() >
        static_cast<std::size_t>(
            std::numeric_limits<int>::max()
        ) ||
        salt.size() >
        static_cast<std::size_t>(
            std::numeric_limits<int>::max()
        ) ||
        iterations >
        static_cast<std::uint32_t>(
            std::numeric_limits<int>::max()
        )
    ){
        error="password hashing input exceeds OpenSSL limits";
        return false;
    }

    output.assign(kHashSize,0);
    const int result=
    PKCS5_PBKDF2_HMAC(
        password.data(),
        static_cast<int>(password.size()),
        salt.data(),
        static_cast<int>(salt.size()),
        static_cast<int>(iterations),
        EVP_sha256(),
        static_cast<int>(output.size()),
        output.data()
    );
    if(result!=1){
        error = "OpenSSL PBKDF2 failed";
        output.clear();
        return false;
    }

    return true;

}

}