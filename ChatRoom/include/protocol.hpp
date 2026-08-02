#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace chat
{
    // v3新增：
    struct Command
    {
        std::string name;                   // 命令名字
        std::string raw_arguments;          // 原始参数字符串
    };

    std::string trim(const std::string &text);//去首位空格
    std::string to_upper_ascii(std::string text);//转大写
    std::vector<std::string> split_words(const std::string &text);
    //v4新增：
    bool split_first_token(const std::string& text,std::string& first,std::string& rest);//处理msg命令，在trim后提取第一个人名
    Command parse_command(const std::string &line);
    bool parse_port(const std::string& text, int &port);
    //解析使用历史消息查询的时候的数字，如果没有传入数字---default_value，如果数字超过的maxnum,返回false
    bool parse_count(const std::string& text, std::size_t default_value, std::size_t maximum, std::size_t& count);
    
}