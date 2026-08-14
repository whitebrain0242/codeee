#pragma once

namespace minimuduo::net{
class NonCopyable{
protected:
    NonCopyable()=default;//默认构造函数
    ~NonCopyable()=default;//默认析构函数

    NonCopyable(const NonCopyable&)=delete;//// 拷贝构造函数
    NonCopyable& operator=(const NonCopyable&)=delete;    // 拷贝赋值运算符
};

} // namespace minimuduo::net
