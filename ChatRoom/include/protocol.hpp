#pragma once

#include <string>
#include <vector>

namespace chat
{
    // v3新增：
    struct ParsedCommand
    {
        std::string name;                   // 命令名字
        std::string raw_arguments;          // 原始参数字符串
        std::vector<std::string> arguments; // 参数列表：删除了空格
    };

    std::string trim(const std::string &text);
    std::string to_upper_ascii(std::string text);
    std::vector<std::string> split_words(const std::string &text);
    //v4新增：
    bool split_first_token(const std::string& text,std::string& first,std::string& rest);
    ParsedCommand parse_command(const std::string &line);
    bool parse_port(const char *text, int &port);
    
}