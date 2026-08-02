#include "database_config.hpp"
#include "protocol.hpp"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_set>

namespace chat{
namespace{
//将字符串转化称无符号整数
bool parse_unsigned(
    const std::string& text,
    unsigned int minimum,
    unsigned int maximum,
    unsigned int& value
){
    const std::string cleaned=trim(text);
    if(cleaned.empty())return false;

    unsigned int parsed=0;//存放解析完的数字
    const char* begin=cleaned.data();//拿到数组起始位置的指针
    const char* end=cleaned.data()+cleaned.size();//拿到数组的末尾地址，最后一个字符的后一个位置
    const auto result=std::from_chars(begin,end,parsed);//从begin开始逐个字符的读取，解析为整数，赋值给parsed
    if (
        result.ec != std::errc{} ||
        result.ptr != end ||
        parsed < minimum ||
        parsed > maximum
    ) {
        return false;
    }
    value=parsed;
    return true;
}
//检查环境变量是否存在，储存在target里面
//函数getenv是在操作系统的环境变量列表里面根据名字查找对应的值
void set_if_present(const char* name,std::string& target){
    const char* value=std::getenv(name);
    if(value!=nullptr)
    target=value;
}

}

//作用：从指定路径读取配置文件（数据库连接参数），解析之后填充到config中
bool load_database_config(
    const std::string& path,
    DatabaseConfig& config,
    std::string& error
){
    //打开配置文件
    std::ifstream input(path);
    if(!input.is_open()){
        error =
            "cannot open database config file: " +
            path;
        return false;
    }
    const std::unordered_set<std::string>allowed_keys={
        "host",
        "port",
        "user",
        "password",
        "database",
        "connect_timeout_seconds"
    };
    std::string line;
    std::size_t line_number=0;

    while(std::getline(input,line)){
        ++line_number;
        const std::string cleaned=trim(line);
        if(cleaned.empty()||cleaned.front()=='#')continue;

        const std::size_t seperator=cleaned.find('=');
        if(seperator==std::string::npos){
            error =
                "invalid config line " +
                std::to_string(line_number) +
                ": expected key=value";
            return false;
        }

        const std::string key=trim(cleaned.substr(0,seperator));
        const std::string value=trim(cleaned.substr(seperator+1));

        if(allowed_keys.find(key)==allowed_keys.end()){
            error =
                "unknown database config key on line " +
                std::to_string(line_number) +
                ": " +
                key;
            return false;
        }
        if(key=="host"){
            config.host=value;
        }else if(key=="port"){
            if(!parse_unsigned(value,1,65535,config.port)){
                 error =
                    "invalid MySQL port on line " +
                    std::to_string(line_number);
                return false;
            }
        }else if(key=="user"){
            config.user=value;
        }else if(key=="password"){
            config.password=value;
        }else if(key=="database"){
            config.database=value;
        }else if(key=="connect_timeout_seconds"){
            if(!parse_unsigned(value,1,60,config.connect_timeout_seconds)){
                error =
                    "invalid connect timeout on line " +
                    std::to_string(line_number);
                return false;
            }
        }

        if(config.host.empty()){
            error = "database host cannot be empty";
            return false;
        }
        if(config.user.empty()){
            error = "database user cannot be empty";
            return false;
        }
        if(!is_valid_database_identifier(config.database)){
            error =
            "database name must contain only "
            "letters, digits, and underscores";
            return false;
        }
        
    }
    return true;

}
//检查环境变量，如果环境变量存在就覆盖config默认配置中
void apply_database_environment_overrides(DatabaseConfig& config){

    set_if_present("CHAT_DB_HOST", config.host);
    set_if_present("CHAT_DB_USER", config.user);
    set_if_present(
        "CHAT_DB_PASSWORD",
        config.password
    );
    set_if_present(
        "CHAT_DB_NAME",
        config.database
    );

    const char* port = std::getenv("CHAT_DB_PORT");

    if (port != nullptr) {
        unsigned int parsed = config.port;

        if (
            parse_unsigned(
                port,
                1,
                65535,
                parsed
            )
        ) {
            config.port = parsed;
        }
    }

    const char* timeout =
        std::getenv("CHAT_DB_CONNECT_TIMEOUT");

    if (timeout != nullptr) {
        unsigned int parsed =
            config.connect_timeout_seconds;

        if (
            parse_unsigned(
                timeout,
                1,
                60,
                parsed
            )
        ) {
            config.connect_timeout_seconds = parsed;
        }
    }
}

//对于输入的信息，检查字符串是否可作为 MySQL 数据库名称（仅允许字母、数字、下划线，长度 1~64）
bool is_valid_database_identifier(const std::string& value){
    if (value.empty() || value.size() > 64) {
        return false;
    }

    for (const char ch : value) {
        const bool letter =
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z');

        const bool digit =
            ch >= '0' && ch <= '9';

        if (!letter && !digit && ch != '_') {
            return false;
        }
    }

    return true;
}
}