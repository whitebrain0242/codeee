#include "protocol.hpp"

#include <cctype>
#include <sstream>

namespace chat{
    //trim:
//v2新增解析命令函数
//实现：
//目的：用于删除字符串开头和结尾的空格/tab/\r------不同系统的换行格式不同需要处理
std::string trim(const std::string& text){
const auto begin=text.find_first_not_of("  \t\r");
    if(begin==std::string::npos)return "";

    const auto end=text.find_last_not_of(" \t\r");
    return text.substr(begin,end-begin+1);
}


//to_upper_ascii
//v2新增
//实现：使用toupper.。。
//目的：实现转大写操作
std::string to_upper_ascii(std::string text){
    for(char& c:text){
        c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

//split_words
//v3新增函数
//实现：使用C++ 的 operator>> 会自动跳过所有连续的空白字符（空格、换行、回车、制表符），然后把下一个连续的、非空白的字符读进 word 变量
//目的：将一段文本根据空白字符分成一个一个单词，放进一个字符串数组
std::vector<std::string>split_words(const std::string& text){
    std::vector<std::string> words;
    std::istringstream input(text);
    std::string word;
    while(input >> word){
        words.push_back(word);
    }
    return words;
}

//v4：新增功能：将一段文本拆成第一个单词和剩余内容部分
/*实现：trim,分隔符*/
bool split_first_token(const std::string& text,std::string& first,std::string& rest){
    const std::string cleaned=trim(text);
    const std::size_t separator=cleaned.find_first_of(" \t");
    first=cleaned.substr(0,separator);
    rest=trim(cleaned.substr(separator+1));
    return true;
}

//parse_command
//v2新增解析命令函数
//实现：
//目的：去掉首尾空格+提取第一个单词作为命令+把剩余内容作为参数
Command parse_command(const std::string& line){
    const std::string cleaned =trim(line);
    if(cleaned.empty())return {};

    const std::size_t separator=cleaned.find_first_of(" \t");
    if(separator==std::string::npos)return {to_upper_ascii(cleaned),"",{}};

    const std::string raw_arguments=trim(cleaned.substr(separator+1));
    return {to_upper_ascii(cleaned.substr(0,separator)),raw_arguments,split_words(raw_arguments)};
}

//parse_port://v2新增自定义端口函数
bool parse_port(const std::string& text, int& port) {
    try {
        const int value = std::stoi(text);
        if (value < 1 || value > 65535) {
            return false;
        }
        port = value;
        return true;
    } catch (...) {
        return false;
    }
}
}