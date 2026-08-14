#pragma once
#include <functional>
#include <memory>
//回调：A的a方法调用了B的b方法，B的b方法执行完之后调用A的callback方法
//核心就是：回调方将本身的this传给调用方，这样调用方就可以在调用完毕之后告诉回调方他想知道的消息
namespace minimuduo::net
{
class Buffer;
class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*)>;
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
using CloseCallback = std::function<void(const TcpConnectionPtr&)>;   
} // namespace minimuduo::net
